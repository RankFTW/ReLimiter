#include "startup_breadcrumb.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

// The single mapped record. Published (release) only after the file is reset for this launch; null disables
// every mark. The mapping is never released in production (late detours still call Breadcrumb_Mark).
std::atomic<BreadcrumbRecord*> s_record{nullptr};
HANDLE s_file = INVALID_HANDLE_VALUE;
HANDLE s_mapping = nullptr;

// Bit per first-wins stage that was stored.
std::atomic<uint32_t> s_seen{0};
// 0, Running or Closed: the stage after which only Closed is stored.
std::atomic<uint8_t> s_terminal{0};
std::atomic<uint32_t> s_presents{0};
// At most one writer at a time (the record is a seqlock); a contending mark is dropped.
std::atomic<bool> s_writer{false};
// Bit per stage whose first mark is in the trail (every batch mark is, so those bits stay clear).
std::atomic<uint32_t> s_trail_seen{0};

constexpr uint32_t kRunningPresents = 32;

class WriterClaim {
public:
    WriterClaim() noexcept : held_(!s_writer.exchange(true, std::memory_order_acquire)) {}
    ~WriterClaim() { if (held_) s_writer.store(false, std::memory_order_release); }
    WriterClaim(const WriterClaim&) = delete;
    WriterClaim& operator=(const WriterClaim&) = delete;
    bool Held() const noexcept { return held_; }
private:
    bool held_;
};

bool IsBatchStage(StartupStage stage) noexcept {
    return stage == StartupStage::BatchApplyEntered || stage == StartupStage::BatchApplyLeft;
}

// Seqlock write: seq_begin odd while the fields change, then seq_end and seq_begin even and equal. Volatile stores
// keep the order in the compiler; x64 and x86 keep store order in hardware. The claim is held by the caller.
// `fresh` also writes the launch constants.
void StoreRecord(BreadcrumbRecord* record, StartupStage stage, uint8_t site, bool fresh) noexcept {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    volatile BreadcrumbRecord* r = record;
    const uint32_t before = r->seq_begin;
    const uint32_t odd = before | 1u;
    const uint32_t after = (before & ~1u) + 2u;
    r->seq_begin = odd;
    std::atomic_thread_fence(std::memory_order_release);
    if (fresh) {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        r->magic = kBreadcrumbMagic;
        r->version = kBreadcrumbVersion;
        r->size = static_cast<uint16_t>(sizeof(BreadcrumbRecord));
        r->process_id = GetCurrentProcessId();
        r->launch_filetime = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        r->reserved = 0;
    }
    r->stage_qpc = static_cast<uint64_t>(qpc.QuadPart);
    r->stage = static_cast<uint8_t>(stage);
    r->site = site;
    std::atomic_thread_fence(std::memory_order_release);
    r->seq_end = after;
    r->seq_begin = after;
}

void CopyRecord(BreadcrumbRecord& out, const BreadcrumbRecord* in) noexcept {
    const volatile uint8_t* source = reinterpret_cast<const volatile uint8_t*>(in);
    uint8_t* destination = reinterpret_cast<uint8_t*>(&out);
    for (size_t i = 0; i < sizeof(BreadcrumbRecord); ++i) destination[i] = source[i];
}

TrailHeader* TrailHeaderOf(BreadcrumbRecord* record) noexcept {
    return reinterpret_cast<TrailHeader*>(reinterpret_cast<uint8_t*>(record) + kTrailOffset);
}

// The ring starts empty for this launch. A late detour of the previous session may still append while this runs:
// it writes its own slot at worst, which the next reset or the wrap overwrites.
void ResetTrail(BreadcrumbRecord* record) noexcept {
    TrailHeader* header = TrailHeaderOf(record);
    volatile TrailHeader* h = header;
    h->magic = 0;
    std::atomic_thread_fence(std::memory_order_release);
    std::memset(header + 1, 0, kTrailCapacity * sizeof(TrailEntry));
    h->version = kTrailVersion;
    h->entry_size = static_cast<uint16_t>(sizeof(TrailEntry));
    h->capacity = kTrailCapacity;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&header->write_index), 0);
    std::atomic_thread_fence(std::memory_order_release);
    h->magic = kTrailMagic;
}

// One append: a time, the thread and the kind. The slot is claimed by one interlocked increment; its time is cleared
// first and stored last, so a reader never takes a half-written slot for a whole one (a writer overtaken by 96 later
// appends can still mix two entries of one slot, which only costs that slot).
void TrailAppend(BreadcrumbRecord* record, TrailKind kind, uint8_t site, uint32_t extra, uint16_t aux) noexcept {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    TrailHeader* header = TrailHeaderOf(record);
    const uint32_t index = static_cast<uint32_t>(
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&header->write_index))) - 1u;
    volatile TrailEntry* slot = reinterpret_cast<volatile TrailEntry*>(header + 1) + (index % kTrailCapacity);
    slot->qpc = 0;
    std::atomic_thread_fence(std::memory_order_release);
    slot->thread_id = GetCurrentThreadId();
    slot->extra = extra;
    slot->kind = static_cast<uint8_t>(kind);
    slot->site = site;
    slot->aux = aux;
    std::atomic_thread_fence(std::memory_order_release);
    slot->qpc = static_cast<uint64_t>(qpc.QuadPart);
}

// The newest `capacity` complete slots, oldest first, from a page (the live mapping or a copy of the file).
size_t ParseTrail(const volatile uint8_t* page, size_t page_size, TrailEntry* out, size_t capacity) noexcept {
    if (!out || capacity == 0 || page_size < kTrailOffset + sizeof(TrailHeader) + kTrailCapacity * sizeof(TrailEntry)) return 0;
    const volatile TrailHeader* header = reinterpret_cast<const volatile TrailHeader*>(page + kTrailOffset);
    if (header->magic != kTrailMagic || header->version != kTrailVersion ||
        header->entry_size != sizeof(TrailEntry) || header->capacity != kTrailCapacity) return 0;
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t written = header->write_index;
    const uint32_t available = written < kTrailCapacity ? written : kTrailCapacity;
    const uint32_t wanted = available < capacity ? available : static_cast<uint32_t>(capacity);
    const volatile TrailEntry* slots = reinterpret_cast<const volatile TrailEntry*>(header + 1);
    size_t count = 0;
    for (uint32_t i = written - wanted; i != written; ++i) {
        const volatile TrailEntry* slot = slots + (i % kTrailCapacity);
        TrailEntry entry;
        entry.qpc = slot->qpc;
        entry.thread_id = slot->thread_id;
        entry.extra = slot->extra;
        entry.kind = slot->kind;
        entry.site = slot->site;
        entry.aux = slot->aux;
        std::atomic_thread_fence(std::memory_order_acquire);
        // The time again: a slot that was cleared or rewritten while it was read is not taken (its time changed).
        if (entry.qpc == 0 || slot->qpc != entry.qpc || entry.kind == 0 || entry.kind > kTrailKindLast) continue;
        out[count++] = entry;
    }
    return count;
}

// Starts this launch's record at stage None. Waits a bounded time for a writer left over from the previous
// session (a late detour); gives up without a record change otherwise.
bool ResetForLaunch(BreadcrumbRecord* record) noexcept {
    for (int spin = 0; spin < 10000; ++spin) {
        WriterClaim claim;
        if (claim.Held()) {
            s_seen.store(0, std::memory_order_relaxed);
            s_terminal.store(0, std::memory_order_relaxed);
            s_presents.store(0, std::memory_order_relaxed);
            s_trail_seen.store(0, std::memory_order_relaxed);
            ResetTrail(record);
            StoreRecord(record, StartupStage::None, 0, true);
            return true;
        }
        YieldProcessor();
    }
    return false;
}

const char* StageName(StartupStage stage) noexcept {
    switch (stage) {
    case StartupStage::None: return "None";
    case StartupStage::InitBegun: return "InitBegun";
    case StartupStage::InitEnded: return "InitEnded";
    case StartupStage::BatchApplyEntered: return "BatchApplyEntered";
    case StartupStage::BatchApplyLeft: return "BatchApplyLeft";
    case StartupStage::FirstPresentEvent: return "FirstPresentEvent";
    case StartupStage::FirstNativePresentEntered: return "FirstNativePresentEntered";
    case StartupStage::FirstNativePresentReturned: return "FirstNativePresentReturned";
    case StartupStage::FirstGateWaitEnqueued: return "FirstGateWaitEnqueued";
    case StartupStage::FirstGateSignal: return "FirstGateSignal";
    case StartupStage::Running: return "Running";
    case StartupStage::Closed: return "Closed";
    }
    return "Unknown";
}

// The hook batch sites (HookBatchSite in hooks.h); other stages carry no site name.
const char* SiteName(StartupStage stage, uint8_t site) noexcept {
    if (!IsBatchStage(stage)) return nullptr;
    switch (site) {
    case 1: return "Init";
    case 2: return "FirstPresent";
    case 3: return "LoaderScan";
    case 4: return "FocusLock";
    case 5: return "DxgiFallback";
    default: return nullptr;
    }
}

} // namespace

void Breadcrumb_Trail(TrailKind kind, uint8_t site, uint32_t extra, uint16_t aux) noexcept {
    if (!HangReport_Active()) return;
    BreadcrumbRecord* record = s_record.load(std::memory_order_acquire);
    if (!record) return;
    const uint8_t index = static_cast<uint8_t>(kind);
    if (index == 0 || index > kTrailKindLast) return;
    TrailAppend(record, kind, site, extra, aux);
}

size_t Breadcrumb_TrailSnapshot(TrailEntry* out, size_t capacity) noexcept {
    const BreadcrumbRecord* record = s_record.load(std::memory_order_acquire);
    if (!record) return 0;
    return ParseTrail(reinterpret_cast<const volatile uint8_t*>(record), kBreadcrumbFileSize, out, capacity);
}

size_t Breadcrumb_TrailParse(const void* page, size_t page_size, TrailEntry* out, size_t capacity) noexcept {
    if (!page) return 0;
    return ParseTrail(static_cast<const volatile uint8_t*>(page), page_size, out, capacity);
}

const char* Breadcrumb_TrailKindName(uint8_t kind) noexcept {
    switch (static_cast<TrailKind>(kind)) {
    case TrailKind::None: return "None";
    case TrailKind::Stage: return "Stage";
    case TrailKind::FreezeEnter: return "FreezeEnter";
    case TrailKind::FreezeLeave: return "FreezeLeave";
    case TrailKind::InitDeviceEnter: return "InitDeviceEnter";
    case TrailKind::InitDeviceLeave: return "InitDeviceLeave";
    case TrailKind::DestroyDeviceEnter: return "DestroyDeviceEnter";
    case TrailKind::DestroyDeviceLeave: return "DestroyDeviceLeave";
    case TrailKind::InitSwapchainEnter: return "InitSwapchainEnter";
    case TrailKind::InitSwapchainLeave: return "InitSwapchainLeave";
    case TrailKind::DestroySwapchainEnter: return "DestroySwapchainEnter";
    case TrailKind::DestroySwapchainLeave: return "DestroySwapchainLeave";
    case TrailKind::DestroyQueueEnter: return "DestroyQueueEnter";
    case TrailKind::DestroyQueueLeave: return "DestroyQueueLeave";
    case TrailKind::FirstReflexSleep: return "FirstReflexSleep";
    case TrailKind::FirstLatencyMarker: return "FirstLatencyMarker";
    case TrailKind::FirstQueueExecute: return "FirstQueueExecute";
    case TrailKind::FirstQueueSignal: return "FirstQueueSignal";
    case TrailKind::FirstQueueWait: return "FirstQueueWait";
    case TrailKind::QueueEnsureHooksEnter: return "QueueEnsureHooksEnter";
    case TrailKind::QueueEnsureHooksLeave: return "QueueEnsureHooksLeave";
    case TrailKind::QueueOfferEnter: return "QueueOfferEnter";
    case TrailKind::QueueOfferLeave: return "QueueOfferLeave";
    case TrailKind::ProbeSessionStart: return "ProbeSessionStart";
    case TrailKind::ProbeSessionEnd: return "ProbeSessionEnd";
    }
    return "Unknown";
}

const char* Breadcrumb_StageText(uint8_t stage) noexcept {
    return StageName(static_cast<StartupStage>(stage));
}

const char* Breadcrumb_BatchSiteText(uint8_t site) noexcept {
    const char* name = SiteName(StartupStage::BatchApplyEntered, static_cast<uint8_t>(site & kBreadcrumbSiteMask));
    return name ? name : "";
}

const char* Breadcrumb_TrailApiName(uint8_t api) noexcept {
    switch (static_cast<TrailApi>(api)) {
    case TrailApi::Unknown: return "unknown";
    case TrailApi::D3D9: return "d3d9";
    case TrailApi::D3D10: return "d3d10";
    case TrailApi::D3D11: return "d3d11";
    case TrailApi::D3D12: return "d3d12";
    case TrailApi::OpenGL: return "opengl";
    case TrailApi::Vulkan: return "vulkan";
    }
    return "unknown";
}

void Breadcrumb_Mark(StartupStage stage, uint8_t site) noexcept {
    BreadcrumbRecord* record = s_record.load(std::memory_order_acquire);
    if (!record) return;
    const uint8_t index = static_cast<uint8_t>(stage);
    if (index == 0 || index > kStartupStageLast) return;
    const uint32_t bit = 1u << index;
    const bool first_wins = !IsBatchStage(stage);
    // The trail records every batch mark and the first mark of every other stage, also after Running (which stops
    // the record, not the trail). Once a first-wins stage is in the trail a call is this one relaxed load and a
    // branch (FirstPresentEvent is called at every present event).
    // With the stall report off none of this runs: one relaxed load of the report's flag.
    if (HangReport_Active() && (!first_wins || !(s_trail_seen.load(std::memory_order_relaxed) & bit))) {
        if (!first_wins || !(s_trail_seen.fetch_or(bit, std::memory_order_relaxed) & bit))
            TrailAppend(record, TrailKind::Stage, index, 0, site);
    }
    const uint8_t terminal = s_terminal.load(std::memory_order_relaxed);
    if (terminal == static_cast<uint8_t>(StartupStage::Closed)) return;
    if (terminal != 0 && stage != StartupStage::Closed) return;
    if (first_wins && (s_seen.load(std::memory_order_relaxed) & bit)) return;

    WriterClaim claim;
    if (!claim.Held()) return;
    // The state may have changed between the pre-check and the claim.
    const uint8_t terminal_now = s_terminal.load(std::memory_order_relaxed);
    if (terminal_now == static_cast<uint8_t>(StartupStage::Closed)) return;
    if (terminal_now != 0 && stage != StartupStage::Closed) return;
    if (first_wins && (s_seen.load(std::memory_order_relaxed) & bit)) return;

    StoreRecord(record, stage, site, false);
    if (first_wins) s_seen.fetch_or(bit, std::memory_order_relaxed);
    if (stage == StartupStage::Running || stage == StartupStage::Closed)
        s_terminal.store(index, std::memory_order_relaxed);
}

void Breadcrumb_PresentCompleted() noexcept {
    if (!s_record.load(std::memory_order_acquire)) return;
    if (s_terminal.load(std::memory_order_relaxed) != 0) return;
    const uint32_t completed = s_presents.fetch_add(1, std::memory_order_relaxed) + 1;
    if (completed < kRunningPresents) return;
    if (s_seen.load(std::memory_order_relaxed) & (1u << static_cast<uint8_t>(StartupStage::FirstPresentEvent)))
        Breadcrumb_Mark(StartupStage::Running);
}

BreadcrumbVerdict Breadcrumb_Judge(const BreadcrumbRecord& record) noexcept {
    BreadcrumbVerdict verdict;
    if (record.magic != kBreadcrumbMagic || record.version != kBreadcrumbVersion ||
        record.size != sizeof(BreadcrumbRecord)) return verdict;
    if (record.seq_begin != record.seq_end || (record.seq_begin & 1u) != 0) return verdict; // torn write
    if (record.process_id == 0 || record.launch_filetime == 0) return verdict;
    if (record.stage > kStartupStageLast) return verdict;
    if (record.stage != 0 && record.stage_qpc == 0) return verdict;
    const StartupStage stage = static_cast<StartupStage>(record.stage);
    if (stage == StartupStage::Running || stage == StartupStage::Closed) return verdict;
    verdict.report = true;
    verdict.stage = stage;
    verdict.site = static_cast<uint8_t>(record.site & kBreadcrumbSiteMask);
    verdict.flags = static_cast<uint8_t>(record.site & ~kBreadcrumbSiteMask);
    return verdict;
}

int Breadcrumb_Describe(const BreadcrumbVerdict& verdict, char* out, size_t cap) noexcept {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!verdict.report) return 0;
    char site_text[48] = "";
    if (verdict.site != 0) {
        const char* name = SiteName(verdict.stage, verdict.site);
        if (name) std::snprintf(site_text, sizeof(site_text), " (site %s)", name);
        else std::snprintf(site_text, sizeof(site_text), " (site %u)", static_cast<unsigned>(verdict.site));
    }
    char installer_text[80] = "";
    if (IsBatchStage(verdict.stage)) {
        if (verdict.flags & kBreadcrumbInstallerRan)
            std::snprintf(installer_text, sizeof(installer_text), "; the apply ran on the installer thread");
        else if (verdict.flags & kBreadcrumbInstallerProtected)
            std::snprintf(installer_text, sizeof(installer_text), "; installer thread protected, the apply ran on the requester");
        else if (verdict.flags & kBreadcrumbInstallerUnprotected)
            std::snprintf(installer_text, sizeof(installer_text), "; installer thread unprotected, the apply ran on the requester");
    }
    const int written = std::snprintf(out, cap,
        "the previous launch ended during start-up; last stage: %s%s%s", StageName(verdict.stage), site_text, installer_text);
    out[cap - 1] = '\0';
    return written < 0 ? 0 : (static_cast<size_t>(written) < cap ? written : static_cast<int>(cap - 1));
}

BreadcrumbVerdict Breadcrumb_Attach(const char* path) noexcept {
    if (BreadcrumbRecord* mapped = s_record.load(std::memory_order_acquire)) {
        // A reload in the same process: the mapping stays, its record is the previous session's.
        BreadcrumbRecord previous{};
        CopyRecord(previous, mapped);
        ResetForLaunch(mapped);
        return Breadcrumb_Judge(previous);
    }
    BreadcrumbVerdict verdict;
    if (!path || !*path) return verdict;

    // Exclusive writer: a second live launch of the same exe, or any process still holding the file, makes this
    // open fail with a sharing violation, so a live launch's record is never reported or trampled.
    const HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return verdict;

    BreadcrumbRecord previous{};
    DWORD got = 0;
    if (ReadFile(file, &previous, sizeof(previous), &got, nullptr) && got == sizeof(previous))
        verdict = Breadcrumb_Judge(previous);

    const HANDLE mapping = CreateFileMappingA(file, nullptr, PAGE_READWRITE, 0,
                                              static_cast<DWORD>(kBreadcrumbFileSize), nullptr);
    void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, kBreadcrumbFileSize) : nullptr;
    if (!view) {
        if (mapping) CloseHandle(mapping);
        CloseHandle(file);
        return verdict;
    }
    std::memset(view, 0, kBreadcrumbFileSize);
    BreadcrumbRecord* record = static_cast<BreadcrumbRecord*>(view);
    if (!ResetForLaunch(record)) {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        CloseHandle(file);
        return verdict;
    }
    s_file = file;
    s_mapping = mapping;
    s_record.store(record, std::memory_order_release);
    return verdict;
}

void Breadcrumb_Detach() noexcept {
    BreadcrumbRecord* record = s_record.exchange(nullptr, std::memory_order_acq_rel);
    if (!record) return;
    UnmapViewOfFile(record);
    CloseHandle(s_mapping);
    CloseHandle(s_file);
    s_mapping = nullptr;
    s_file = INVALID_HANDLE_VALUE;
    s_seen.store(0, std::memory_order_relaxed);
    s_terminal.store(0, std::memory_order_relaxed);
    s_presents.store(0, std::memory_order_relaxed);
    s_trail_seen.store(0, std::memory_order_relaxed);
}
