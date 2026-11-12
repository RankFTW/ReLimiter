#include "fg_input_observer.h"
#include "fg_queue_observer.h"
#include "gpu_present_gate.h"
#include "streamline_gpu_handoff.h"
#include "bounded_mpsc_queue.h"
#include "hooks.h"
#include "native_present_gate.h"
#include "gpu_readiness.h"
#include "runtime_lifecycle.h"
#include "logger.h"
#include "frame_timeline.h"
#include <dxgi1_2.h>
#include <wincrypt.h>
#include <array>
#include <atomic>
#include <cstring>

namespace {
// Internal Streamline callback ABI, NOT a COM method. Only reviewed x64 build.
using Present = HRESULT(*)(IDXGISwapChain*, UINT, UINT, bool&);
using Present1 = HRESULT(*)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*, bool&);
using Resolver = void*(*)(const char*);
std::atomic<Present> original_present{nullptr};
std::atomic<Present1> original_present1{nullptr};
std::atomic<bool> installed{false};
std::atomic<uint64_t> record_sequence{0};
SRWLOCK drain_lock = SRWLOCK_INIT;
HMODULE reviewed_module = nullptr; // pinned; one reviewed implementation per process
bool rejected = false; // installer owned; restart after replacing an unsupported DLL
bool present_enabled = false, present1_enabled = false;

struct Marker {
    uint64_t frame = 0, native = 0, generation = 0, prediction = 0, settings = 0;
    uint32_t producer = 0, controller = 0;
    int64_t release = 0;
};
thread_local Marker marker;
thread_local Marker last_marker;
std::atomic<uint64_t> observed_calls{0}, candidate_calls{0};
std::atomic<uint64_t> diagnostic_examples{0};
thread_local uint32_t depth = 0;
thread_local uint64_t nesting_serial = 0;
struct Record {
    uint64_t id = 0, base = 0, selected = 0, generation = 0;
    uint64_t transaction = 0, parent = 0;
    uint32_t nesting_depth = 0, children = 0;
    bool delegation = false;
    Marker provisional{}; // raw evidence, never an accepted candidate
    uint64_t frame = 0;
    uint32_t producer = 0, controller = 0, thread = 0, method = 0;
    uint32_t flags = 0, sync = 0, reasons = 0, fg_flags = 0;
    int64_t begin = 0, end = 0, release = 0, frequency = 0;
    HRESULT result = E_FAIL;
    bool skip_before = false, skip_after = false;
};
// reasons: 1=no marker, 2=stale/lifetime, 4=nested, 8=special Present,
// 16=base differs from selected, 32=failed result, 64=exit context changed.
// reasons=0 is only a same-thread candidate, never proven input ownership.
relimiter::BoundedMpscQueue<Record, 2048> records;

int64_t Now() noexcept { LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc); return qpc.QuadPart; }
bool Enabled() noexcept {
    return !RuntimeLifecycle_IsStopping() && NativeGate_GetMode() == NativeGateMode::Shadow &&
        (GpuReadiness_IsEnabled() || GpuPresent_RequiresReadiness());
}
bool ReadSkip(const bool* value, bool* output) noexcept {
    __try { *output = *value; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

class Observation {
public:
    static bool TakeHandoff(uint64_t caller,GpuPresentFgInput* input) noexcept {
        if (StreamlineHandoff_ClaimsInput()) return false;
        if (!input || !reviewed_module || caller != reinterpret_cast<uint64_t>(reviewed_module)+0x4817d ||
            !current_ || !Enabled() || !GpuPresent_RequiresReadiness()) return false;
        auto* root=current_;
        if (root->previous_) {
            if (!root->record_.delegation || root->record_.children || root->previous_->previous_ ||
                root->previous_->record_.children!=1 || root->record_.skip_before ||
                (root->record_.reasons & ~(1u|4u|16u))) return false;
            root=root->previous_;
        } else if (root->record_.method!=1 || root->record_.children) return false;
        const auto& r=root->record_;
        const auto& s=r.provisional;
        // Base mismatch and the reviewed Present->Present1 delegation are
        // expected here, not permission to forgive other rejection reasons.
        if (root->handoff_ || r.nesting_depth!=1 || (r.reasons & ~16u) || r.sync || r.skip_before ||
            !s.frame || !s.release || Now()-s.release>r.frequency/4) return false;
        const auto c=NativeGate_GetContext(Now());
        if (!c.observe_frame_generation || c.native_swapchain!=s.native || c.swapchain_generation!=s.generation ||
            c.producer_epoch!=s.producer || c.controller_generation!=s.controller ||
            s.settings!=GpuReadiness_Settings() || s.prediction!=NativeGate_PredictionEpoch()) return false;
        root->handoff_=true;
        StreamlineHandoff_SuppressInput(); // reviewed path owns this input; no duplicate generic measurement
        *input={r.transaction,s.frame,s.native,s.generation,s.settings,s.prediction,s.producer,s.controller};
        return true;
    }
    static FgInputTransaction Transaction() noexcept {
        auto* root = current_;
        while (root && root->previous_) root = root->previous_;
        if (!root) return {};
        const auto& source = root->record_.provisional;
        return {root->record_.transaction, source.frame, source.native, source.generation,
            source.producer, source.controller};
    }

    Observation(IDXGISwapChain* base, UINT sync, UINT flags, uint32_t method, bool* skip) noexcept {
        ++depth;
        if (depth > 1) ++nesting_serial;
        serial_ = nesting_serial;
        active_ = Enabled() && (FrameTimeline_IsEnabled() || GpuPresent_RequiresReadiness());
        if (!active_) { marker = {}; return; }
        const Marker source = marker;
        marker = {}; // consume once, even when rejected; never tag a second call
        record_.begin = Now();
        context_ = NativeGate_GetContext(record_.begin);
        record_.frequency = context_.qpc_frequency;
        record_.id = record_sequence.fetch_add(1) + 1;
        previous_ = current_;
        record_.parent = previous_ ? previous_->record_.id : 0;
        record_.transaction = previous_ ? previous_->record_.transaction : record_.id;
        record_.nesting_depth = depth;
        record_.provisional = source;
        current_ = this;
        record_.base = reinterpret_cast<uint64_t>(base);
        record_.selected = context_.native_swapchain;
        record_.generation = context_.swapchain_generation;
        record_.fg_flags = context_.fg_diagnostic_flags;
        record_.thread = GetCurrentThreadId();
        record_.method = method;
        record_.sync = sync;
        record_.flags = flags;
        if (previous_) {
            ++previous_->record_.children;
            // A structural observation only. Do not inherit the parent's marker
            // or forgive its strict nested/base-mismatch rejection.
            record_.delegation = depth == 2 && previous_->record_.method == 0 &&
                method == 1 && previous_->record_.base == record_.base &&
                previous_->record_.sync == sync && previous_->record_.flags == flags;
        }
        settings_ = GpuReadiness_Settings();
        prediction_ = NativeGate_PredictionEpoch();
        if (!source.frame) record_.reasons |= 1;
        if (source.frame && (!context_.observe_frame_generation || context_.qpc_frequency <= 0 ||
            source.release <= 0 || record_.begin < source.release ||
            record_.begin - source.release > context_.qpc_frequency / 4 ||
            source.native != context_.native_swapchain || source.generation != context_.swapchain_generation ||
            source.producer != context_.producer_epoch || source.controller != context_.controller_generation ||
            source.settings != settings_ || source.prediction != prediction_)) record_.reasons |= 2;
        if (depth != 1) record_.reasons |= 4;
        if (flags & (DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_WAIT)) record_.reasons |= 8;
        if (!record_.selected || record_.base != record_.selected) record_.reasons |= 16;
        if (!ReadSkip(skip, &record_.skip_before)) record_.reasons |= 64;
        if (!record_.reasons) {
            record_.frame = source.frame;
            record_.producer = source.producer;
            record_.controller = source.controller;
            record_.release = source.release;
        }
    }
    ~Observation() {
        if (handoff_ && !completed_) GpuPresent_FinishFgInput(record_.transaction,false);
        if (active_) current_ = previous_;
        --depth;
    }
    void Complete(HRESULT result, bool* skip) noexcept {
        if (!active_) return;
        record_.end = Now();
        record_.result = result;
        if (FAILED(result)) record_.reasons |= 32;
        if (serial_ != nesting_serial) record_.reasons |= 4;
        const auto current = NativeGate_GetContext(record_.end);
        if (!ReadSkip(skip, &record_.skip_after) || !Enabled() ||
            current.native_swapchain != context_.native_swapchain ||
            current.swapchain_generation != context_.swapchain_generation ||
            current.producer_epoch != context_.producer_epoch ||
            current.controller_generation != context_.controller_generation ||
            !current.observe_frame_generation || settings_ != GpuReadiness_Settings() ||
            prediction_ != NativeGate_PredictionEpoch()) record_.reasons |= 64;
        if (record_.reasons) {
            record_.frame = 0; record_.producer = record_.controller = 0; record_.release = 0;
        }
        if (handoff_) {
            const bool valid=!(record_.reasons & ~(4u|16u)) && record_.skip_after &&
                record_.children<=1 && nesting_serial-serial_==record_.children;
            GpuPresent_FinishFgInput(record_.transaction,valid);
            completed_=true;
        }
        observed_calls.fetch_add(1, std::memory_order_relaxed);
        if (!record_.reasons) candidate_calls.fetch_add(1, std::memory_order_relaxed);
        // Sparse bounded examples, not a per-frame log stream. Counts cover all
        // observed calls; exact downstream correlation needs the timeline CSV.
        if (FrameTimeline_IsEnabled() && diagnostic_examples.fetch_add(1,std::memory_order_relaxed)<128) records.Push(record_);
    }
private:
    static thread_local Observation* current_;
    Observation* previous_ = nullptr;
    Record record_{};
    NativeGateContext context_{};
    uint64_t serial_ = 0, settings_ = 0, prediction_ = 0;
    bool active_ = false;
    bool handoff_=false,completed_=false;
};
thread_local Observation* Observation::current_ = nullptr;

HRESULT HookPresent(IDXGISwapChain* base, UINT sync, UINT flags, bool& skip) {
    const DWORD incoming = GetLastError();
    RuntimeCallbackGuard callback;
    const auto original = original_present.load(std::memory_order_acquire);
    if (callback.IsStopping()) return original(base, sync, flags, skip);
    Observation observation(base, sync, flags, 0, &skip);
    SetLastError(incoming);
    const HRESULT result = original(base, sync, flags, skip);
    const DWORD error = GetLastError();
    observation.Complete(result, &skip);
    SetLastError(error);
    return result;
}
HRESULT HookPresent1(IDXGISwapChain* base, UINT sync, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters, bool& skip) {
    const DWORD incoming = GetLastError();
    RuntimeCallbackGuard callback;
    const auto original = original_present1.load(std::memory_order_acquire);
    if (callback.IsStopping()) return original(base, sync, flags, parameters, skip);
    Observation observation(base, sync, flags, 1, &skip);
    SetLastError(incoming);
    const HRESULT result = original(base, sync, flags, parameters, skip);
    const DWORD error = GetLastError();
    observation.Complete(result, &skip);
    SetLastError(error);
    return result;
}

constexpr std::array<BYTE, 32> reviewed_hash = {
    0xf4,0xa6,0xb2,0xb1,0x4d,0xcc,0x0b,0x14,0x85,0x98,0x9e,0x43,0x0d,0x3b,0x4e,0x3a,
    0x44,0xac,0x18,0x00,0xb9,0x2b,0xa1,0xad,0x74,0xf4,0x76,0xe6,0x4f,0xb2,0xb0,0x9c};
bool MatchesLoadedEntries(HMODULE module, const void* image) noexcept {
    __try {
        const auto* disk = static_cast<const BYTE*>(image);
        const auto* loaded = reinterpret_cast<const BYTE*>(module);
        return std::memcmp(disk + 0x5a2a0, loaded + 0x5a2a0, 0x500) == 0 &&
            std::memcmp(disk + 0x54690, loaded + 0x54690, 16) == 0 &&
            std::memcmp(disk + 0x543b0, loaded + 0x543b0, 16) == 0 &&
            std::memcmp(disk + 0x48160, loaded + 0x48160, 0xb0) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReviewedFile(HMODULE module, const std::array<BYTE,32>& expected = reviewed_hash,
    bool (*matches)(HMODULE,const void*) noexcept = MatchesLoadedEntries) noexcept {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool good = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash);
    BYTE buffer[16384];
    DWORD count = 0;
    while (good) {
        if (!ReadFile(file, buffer, sizeof(buffer), &count, nullptr)) { good = false; break; }
        if (!count) break;
        good = CryptHashData(hash, buffer, count, 0) != FALSE;
    }
    std::array<BYTE, 32> digest{};
    DWORD size = static_cast<DWORD>(digest.size());
    good = good && CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &size, 0) &&
        size == digest.size() && digest == expected;
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    if (good) {
        HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr);
        const void* image = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
        good = image && matches(module, image);
        if (image) UnmapViewOfFile(image);
        if (mapping) CloseHandle(mapping);
    }
    CloseHandle(file);
    return good;
}
bool ResolveReviewed(HMODULE module, void** present, void** present1) noexcept {
    __try {
        auto* base = reinterpret_cast<BYTE*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt->FileHeader.TimeDateStamp != 0x6a9afd6b || nt->OptionalHeader.SizeOfImage != 0x9c000) return false;
        auto resolver = reinterpret_cast<Resolver>(GetProcAddress(module, "slGetPluginFunction"));
        if (reinterpret_cast<BYTE*>(resolver) != base + 0x5a2a0) return false;
        // Reviewed resolver is a string-to-function dispatch; no feature call.
        *present = resolver("slHookPresent");
        *present1 = resolver("slHookPresent1");
        return *present == base + 0x54690 && *present1 == base + 0x543b0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

FgInputTransaction FgInput_CurrentTransaction() noexcept {
    return Observation::Transaction();
}
bool FgInput_TakeGpuHandoff(uint64_t caller,GpuPresentFgInput* input) noexcept {
    return Observation::TakeHandoff(caller,input);
}

bool FgInput_ReviewModule(HMODULE module, const std::array<BYTE,32>& digest,
    bool (*matches)(HMODULE,const void*) noexcept) noexcept {
    return ReviewedFile(module, digest, matches);
}

void FgInput_RecordMarker(uint64_t frame, uint32_t producer, uint32_t controller, int64_t release) noexcept {
    StreamlineHandoff_RecordMarker(frame, producer, controller, release);
    marker = {};
    if (!installed.load(std::memory_order_acquire) || !Enabled() ||
        (!FrameTimeline_IsEnabled() && !GpuPresent_RequiresReadiness()) || depth) return;
    const auto context = NativeGate_GetContext(release);
    if (!context.observe_frame_generation || !frame || !producer || !controller ||
        producer != context.producer_epoch || controller != context.controller_generation) return;
    if (last_marker.frame == frame && last_marker.producer == producer &&
        last_marker.controller == controller) return;
    marker = {frame, context.native_swapchain, context.swapchain_generation,
        NativeGate_PredictionEpoch(), GpuReadiness_Settings(), producer, controller, release};
    last_marker = marker;
}

void FgInput_Drain() noexcept {
    FgQueue_Drain();
    if (!TryAcquireSRWLockExclusive(&drain_lock)) return;
    GpuPresent_DrainDiagnostics();
    Record r;
    // Bounded worker work; no COM references or borrowed pointers to dereference.
    for (size_t i = 0; i < 8 && records.Pop(&r); ++i) {
        LOG_INFO("FG input: id=%llu method=%u begin=%lld end=%lld frequency=%lld thread=%u "
            "base=%llu selected=%llu generation=%llu sync=%u flags=%u skip=%u/%u result=%ld "
            "candidate=%llu producer=%u controller=%u marker_release=%lld reasons=%u fg_flags=%u",
            r.id,r.method,r.begin,r.end,r.frequency,r.thread,r.base,r.selected,r.generation,r.sync,r.flags,
            unsigned(r.skip_before),unsigned(r.skip_after),r.result,r.frame,r.producer,r.controller,r.release,r.reasons,r.fg_flags);
        LOG_INFO("FG input identity: id=%llu transaction=%llu parent=%llu depth=%u children=%u delegation=%u "
            "provisional=%llu producer=%u controller=%u marker_release=%lld marker_selected=%llu "
            "marker_generation=%llu marker_settings=%llu marker_prediction=%llu",
            r.id,r.transaction,r.parent,r.nesting_depth,r.children,unsigned(r.delegation),
            r.provisional.frame,r.provisional.producer,r.provisional.controller,r.provisional.release,
            r.provisional.native,r.provisional.generation,r.provisional.settings,r.provisional.prediction);
    }
    static uint64_t last_drops = 0;
    static int64_t last_report = 0;
    static uint64_t last_calls = 0;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const auto now = Now();
    const auto calls = observed_calls.load(std::memory_order_relaxed);
    if (calls != last_calls && now - last_report >= frequency.QuadPart) {
        LOG_INFO("FG input: cumulative calls=%llu candidates=%llu examples_limit=128 dropped=%llu",
            calls, candidate_calls.load(std::memory_order_relaxed), records.DroppedValues());
        last_calls = calls; last_report = now;
    }
    const auto drops = records.DroppedValues();
    if (drops != last_drops) { LOG_WARN("FG input: cumulative dropped=%llu", drops); last_drops = drops; }
    ReleaseSRWLockExclusive(&drain_lock);
}

void FgInput_InstallAndDrain() noexcept {
    FgInput_Drain();
    if (Enabled() && installed.load(std::memory_order_acquire)) FgQueue_InstallAndDrain();
    HookInstallGuard admission;
    if (!admission) return;
    if (!Enabled() || rejected || (present_enabled && present1_enabled)) return;
    if (!reviewed_module) {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(0, L"sl.dlss_g.dll", &module)) return;
        void* present = nullptr;
        void* present1 = nullptr;
        const bool valid = ReviewedFile(module) && ResolveReviewed(module, &present, &present1) &&
            Hooks_PinTargetModule(present) && Hooks_PinTargetModule(present1);
        if (valid) reviewed_module = module;
        FreeLibrary(module);
        if (!valid) {
            rejected = true;
            LOG_INFO("FG input: unavailable; DLSS-G SHA256/loaded image/resolver not reviewed (restart required)");
            return;
        }
        LOG_INFO("FG input: reviewed DLSS-G 2.14.1 build accepted; GPU gate opt-in available; base=%llu",
            reinterpret_cast<uint64_t>(reviewed_module));
    }
    auto* base = reinterpret_cast<BYTE*>(reviewed_module);
    void* const present_target = base + 0x54690;
    void* const present1_target = base + 0x543b0;
    MH_STATUS present_status = MH_OK, present1_status = MH_OK;
    if (!present_enabled)
        present_status = InstallHook(present_target, reinterpret_cast<void*>(&HookPresent), &original_present);
    if (!present1_enabled)
        present1_status = InstallHook(present1_target, reinterpret_cast<void*>(&HookPresent1), &original_present1);
    // Inside a batch the hooks are only queued: enable them now, the flags below read the result.
    Hooks_FlushBatch();
    if (!present_enabled) {
        present_enabled = (present_status == MH_OK || present_status == MH_ERROR_ENABLED) &&
            Hooks_TargetEnabled(present_target);
        LOG_INFO("FG input: Present installation status=%d enabled=%d", present_status, present_enabled ? 1 : 0);
    }
    if (!present1_enabled) {
        present1_enabled = (present1_status == MH_OK || present1_status == MH_ERROR_ENABLED) &&
            Hooks_TargetEnabled(present1_target);
        LOG_INFO("FG input: Present1 installation status=%d enabled=%d", present1_status, present1_enabled ? 1 : 0);
    }
    installed.store(present_enabled || present1_enabled, std::memory_order_release);
}

void FgInput_BeginWorkerLifetime() noexcept {
    FgQueue_BeginWorkerLifetime();
    present_enabled = present1_enabled = false;
    installed.store(false, std::memory_order_release);
}