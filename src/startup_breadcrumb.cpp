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

// Starts this launch's record at stage None. Waits a bounded time for a writer left over from the previous
// session (a late detour); gives up without a record change otherwise.
bool ResetForLaunch(BreadcrumbRecord* record) noexcept {
    for (int spin = 0; spin < 10000; ++spin) {
        WriterClaim claim;
        if (claim.Held()) {
            s_seen.store(0, std::memory_order_relaxed);
            s_terminal.store(0, std::memory_order_relaxed);
            s_presents.store(0, std::memory_order_relaxed);
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

void Breadcrumb_Mark(StartupStage stage, uint8_t site) noexcept {
    BreadcrumbRecord* record = s_record.load(std::memory_order_acquire);
    if (!record) return;
    const uint8_t index = static_cast<uint8_t>(stage);
    if (index == 0 || index > kStartupStageLast) return;
    const uint8_t terminal = s_terminal.load(std::memory_order_relaxed);
    if (terminal == static_cast<uint8_t>(StartupStage::Closed)) return;
    if (terminal != 0 && stage != StartupStage::Closed) return;
    const uint32_t bit = 1u << index;
    const bool first_wins = !IsBatchStage(stage);
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
}
