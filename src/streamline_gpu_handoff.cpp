#include "cross_thread_slot.h"
#include "streamline_gpu_handoff.h"
#include "streamline_discovery.h"
#include "streamline_hooks.h"
#include "native_present_gate.h"
#include "gpu_readiness.h"
#include "feedback_device_identity.h"
#include "swapchain_manager.h"
#include "runtime_lifecycle.h"
#include "logger.h"
#include "bounded_mpsc_queue.h"
#include <d3d12.h>
#include <dxgi.h>
#include <array>
#include <atomic>

namespace streamline_handoff {
constexpr unsigned RequiredCycles = 4;
std::atomic<uint64_t> epoch{1}, authorized{0}, sequence{0};
std::atomic<unsigned> pins{0};
std::atomic<bool> tracking{false};
std::atomic<bool> faulted{false};
std::atomic<const char*> rejection{"none"};
std::atomic<uint64_t> retained_epoch{0}, maintenance_deferrals{0}, callback_contentions{0};
// The source owns one pinned cycle until its scope returns. Publish that exact
// transaction, not a reusable done flag; only locked full-cycle validation may
// consume it. Cleanup cannot clear/reuse the slot until the source drops its pin.
static_assert(std::atomic<uint64_t>::is_always_lock_free, "Source completion publication must be lock-free");
std::atomic<uint64_t> completed_source_transaction{0};
struct Invalidation {
    uint64_t epoch = 0;
    const char* reason = nullptr;
    int64_t qpc = 0;
    DWORD thread = 0;
    uint64_t object = 0;
};
// One first-revocation record per retained binding, not a per-frame trace.
relimiter::BoundedMpscQueue<Invalidation, 32> invalidations;
std::atomic<ID3D12Fence*> forward_filter{nullptr}, back_filter{nullptr};
SRWLOCK mutex = SRWLOCK_INIT;
struct Cycle {
    GpuPresentFgInput input{};
    int64_t begin = 0;
    uint64_t forward = 0, back = 0;
    std::array<uint64_t, 4> sites{};
    unsigned begun = 0, finished = 0, executions = 0, callbacks = 0;
    bool outer_done = false, complete = false;
    uint32_t worker_thread = 0;
};
struct Route {
    uint64_t epoch = 0, config = 0, previous_frame = 0, previous_forward = 0, previous_back = 0;
    int multiplier = 0;
    uint32_t source_thread = 0;
    UINT sync_interval = 0;
    IDXGISwapChain* proxy = nullptr;
    ID3D12CommandQueue* source = nullptr;
    ID3D12CommandQueue* worker = nullptr;
    ID3D12Fence* forward = nullptr;
    ID3D12Fence* back = nullptr;
    std::array<uint64_t, 4> sites{};
    unsigned successes = 0;
    bool device_checked = false;
    Cycle cycle{};
} route;
struct Marker { GpuPresentFgInput input{}; int64_t release = 0; };
thread_local Marker marker;
thread_local GpuPresentFgInput last_marker;
// Cross-thread hand-off (R8, cross_thread_slot.h): the newest marker (empty
// when rejected) for a proxy Present on another thread. Taken only by the
// game's own outermost proxy Present, which frame generation never calls,
// so a generated-frame Present cannot take a real frame's marker, and only
// once ETW frame IDs have verified this game's cross-thread mapping on the
// native route (NativeGate_CrossThreadVerified): interleaving alone cannot
// tell Present(n) from a Present(n - 1) running a frame behind.
CrossThreadSlot<Marker> marker_slot;
thread_local uint64_t marker_sequence = 0;
std::atomic<uint64_t> cross_thread_takes{0};
struct WorkerScope { uint64_t epoch = 0, transaction = 0; ID3D12CommandQueue* queue = nullptr; };
thread_local WorkerScope worker_scope;
// Independent cumulative stage snapshots, not per-frame correlations. Producers
// only update scalars; the existing installer worker formats at most once / 5s.
struct AdmissionStage {
    std::atomic<uint64_t> seen{0}, accepted{0};
    std::atomic<const char*> last_failure{"none"};
    std::atomic<DWORD> last_thread{0};
    void Record(const char* failure) noexcept {
        seen.fetch_add(1, std::memory_order_relaxed);
        last_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        if (failure) last_failure.store(failure, std::memory_order_relaxed);
        else accepted.fetch_add(1, std::memory_order_relaxed);
    }
};
AdmissionStage marker_admission, present_admission, fence_admission;
std::atomic<UINT> last_sync{0}, last_flags{0};
int64_t last_admission_report = 0; // installer worker only
int64_t Now() noexcept { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart; }
const char* PolicyRejection() noexcept {
    if (faulted.load(std::memory_order_acquire)) return "retained_com_fault";
    if (RuntimeLifecycle_IsStopping()) return "stopping";
    if (!GpuPresent_RequiresReadiness()) return "gpu_deadline_off";
    if (g_fg_mode.load(std::memory_order_acquire) != 1) return "mode_not_static_on";
    if (!g_fg_presenting.load(std::memory_order_acquire)) return "fg_not_presenting";
    return nullptr;
}
bool Enabled() noexcept {
    return !PolicyRejection();
}
const char* InputRejection(const GpuPresentFgInput& input) noexcept {
    const auto current = NativeGate_GetContext(Now());
    if (const auto* reason = PolicyRejection()) return reason;
    if (input.binding != epoch.load(std::memory_order_acquire)) return "binding_epoch_changed";
    if (!current.observe_frame_generation) return "fg_context_not_observable";
    // FgInputRoute is published by the GPU gate from inputs this route
    // delivers, so it is never an admission requirement here.
    if (current.native_swapchain != input.native) return "native_swapchain_changed";
    if (current.swapchain_generation != input.swapchain) return "swapchain_generation_changed";
    if (current.producer_epoch != input.producer) return "marker_producer_changed";
    if (current.controller_generation != input.controller) return "controller_changed";
    if (input.observer != GpuReadiness_Settings()) return "observer_settings_changed";
    if (input.prediction != NativeGate_PredictionEpoch()) return "prediction_epoch_changed";
    return nullptr;
}
bool Current(const GpuPresentFgInput& input) noexcept { return !InputRejection(input); }
void ReportAdmission(bool final) noexcept {
    const auto context = NativeGate_GetContext(Now());
    // R12b: also report while the binding is current but the FG input route
    // capability is missing (it stayed fg_unqualified after FG was re-enabled
    // in Gears of War: E-Day, and nothing was logged).
    if (!final && StreamlineHandoff_IsCurrent(epoch.load(std::memory_order_acquire)) &&
        GpuPresent_IsFgSupported(context.controller_generation)) return;
    if (!final && (g_fg_mode.load(std::memory_order_acquire) != 1 || context.qpc_frequency <= 0 ||
        context.now_qpc - last_admission_report < context.qpc_frequency * 5)) return;
    if (!marker_admission.seen.load(std::memory_order_relaxed) && !present_admission.seen.load(std::memory_order_relaxed)) return;
    last_admission_report = context.now_qpc;
    const auto* policy = PolicyRejection();
    LOG_INFO("Streamline GPU: admission policy=%s native_mode=%u context_supported=%d fg_observable=%d fg_flags=%u scheduler_fg=%d binding=%llu authorized=%llu last_rejection=%s cross_thread_markers=%llu",
        policy ? policy : "eligible", unsigned(NativeGate_GetMode()), context.supported, context.observe_frame_generation,
        context.fg_diagnostic_flags, GpuPresent_IsFgSupported(context.controller_generation), epoch.load(), authorized.load(), rejection.load(),
        static_cast<unsigned long long>(cross_thread_takes.load(std::memory_order_relaxed)));
    const auto route = GpuPresent_FgRouteDiagnostics();
    LOG_INFO("Streamline GPU: FG route inputs seen=%llu pending=%llu invalid=%llu stale_binding=%llu queue_missing=%llu "
             "queue_unready=%llu fg_queue_unready=%llu identity=%llu",
        route.seen, route.pending, route.invalid_input, route.stale_binding, route.queue_missing,
        route.queue_unready, route.fg_unready, route.identity);
    const char* names[] = {"marker", "present", "fence_attribution"};
    AdmissionStage* stages[] = {&marker_admission, &present_admission, &fence_admission};
    for (unsigned i = 0; i < 3; ++i) {
        const auto admitted = stages[i]->accepted.load(std::memory_order_relaxed);
        const auto seen = stages[i]->seen.load(std::memory_order_relaxed);
        LOG_INFO("Streamline GPU: admission stage=%s accepted=%llu seen=%llu last_failure=%s last_thread=%lu sync=%u flags=%u",
            names[i], admitted, seen, stages[i]->last_failure.load(std::memory_order_relaxed),
            stages[i]->last_thread.load(std::memory_order_relaxed), last_sync.load(), last_flags.load());
    }
}
struct Lock {
    bool held = false;
    explicit Lock(const char* critical_failure) noexcept {
        held = TryAcquireSRWLockExclusive(&mutex) != FALSE;
        if (held) return;
        if (critical_failure) {
            callback_contentions.fetch_add(1, std::memory_order_relaxed);
            StreamlineHandoff_Invalidate(critical_failure);
        } else maintenance_deferrals.fetch_add(1, std::memory_order_relaxed);
    }
    ~Lock() { Unlock(); }
    void Unlock() noexcept {
        if (held) {
            ReleaseSRWLockExclusive(&mutex); held = false;
        }
    }
};
template<class T> bool Retain(T* object, T** output) noexcept {
    if (!object) return false;
    __try { object->AddRef(); *output = object; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { faulted.store(true, std::memory_order_release); return false; }
}
bool Release(IUnknown* object) noexcept {
    if (!object) return true;
    __try { object->Release(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { faulted.store(true, std::memory_order_release); return false; }
}
void Reject(StreamlineInputScope* scope = nullptr, const char* reason = "handoff_mismatch") noexcept {
    if (scope) scope->invalid = true;
    StreamlineHandoff_Invalidate(reason);
}
bool SamePolicy(const StreamlineInputScope& scope) noexcept {
    const auto& a = route.cycle.input;
    const auto& b = scope.input;
    return route.epoch == b.binding && route.config == scope.config && route.multiplier == scope.multiplier &&
        route.sync_interval == scope.sync_interval &&
        a.native == b.native && a.swapchain == b.swapchain && a.producer == b.producer && a.controller == b.controller &&
        a.observer == b.observer && a.prediction == b.prediction;
}
bool WorkerMatches() noexcept {
    return worker_scope.epoch == route.epoch && worker_scope.transaction == route.cycle.input.transaction &&
        worker_scope.queue == route.worker && route.cycle.worker_thread == GetCurrentThreadId();
}
void CompleteCycle(const char* validated_reason = nullptr, bool validated = false) noexcept {
    auto& cycle = route.cycle;
    if (!cycle.outer_done && cycle.input.transaction &&
        completed_source_transaction.load(std::memory_order_acquire) == cycle.input.transaction)
        cycle.outer_done = true;
    if (cycle.complete || !cycle.outer_done || cycle.finished != 15 || cycle.begun != 15 ||
        !cycle.executions || !cycle.callbacks || route.epoch != epoch.load(std::memory_order_acquire)) return;
    if (const auto* reason = validated ? validated_reason : InputRejection(cycle.input)) { Reject(nullptr, reason); return; }
    if (!route.successes) route.sites = cycle.sites;
    else if (route.sites != cycle.sites) { Reject(); return; }
    // Distinct input and consumer sites are learned from exact fence edges,
    // not fixed RVAs, signal ordinals, thread/FPS ratios or module ownership alone.
    if (cycle.sites[0] == cycle.sites[3] || cycle.sites[1] == cycle.sites[2]) { Reject(); return; }
    cycle.complete = true;
    route.previous_frame = cycle.input.source;
    route.previous_forward = cycle.forward;
    route.previous_back = cycle.back;
    if (route.successes < RequiredCycles) ++route.successes;
    if (route.successes >= RequiredCycles && route.device_checked)
        authorized.store(route.epoch, std::memory_order_release);
}
bool CheckDevices() noexcept {
    auto* expected = SwapMgr_AcquirePresentingDevice();
    if (!expected) return false;
    bool valid = true;
    IUnknown* devices[5]{};
    __try {
        valid = route.source->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT &&
            route.worker->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT &&
            SUCCEEDED(route.source->GetDevice(IID_PPV_ARGS(&devices[0]))) &&
            SUCCEEDED(route.worker->GetDevice(IID_PPV_ARGS(&devices[1]))) &&
            SUCCEEDED(route.forward->GetDevice(IID_PPV_ARGS(&devices[2]))) &&
            SUCCEEDED(route.back->GetDevice(IID_PPV_ARGS(&devices[3]))) &&
            SUCCEEDED(route.proxy->GetDevice(IID_PPV_ARGS(&devices[4])));
    } __except (EXCEPTION_EXECUTE_HANDLER) { valid = false; }
    for (auto* device : devices) {
        valid = valid && device && FeedbackDeviceIdentity_IsSame(device, expected);
        if (!Release(device)) valid = false;
    }
    if (!Release(expected)) valid = false;
    return valid;
}
}

void StreamlineHandoff_Invalidate(const char* reason, uint64_t object) noexcept {
    using namespace streamline_handoff;
    rejection.store(reason, std::memory_order_release);
    authorized.store(0, std::memory_order_release);
    const auto previous = epoch.fetch_add(1, std::memory_order_acq_rel);
    if (previous == retained_epoch.load(std::memory_order_acquire))
        invalidations.Push({previous, reason, Now(), GetCurrentThreadId(), object});
}
StreamlineHandoffStatus StreamlineHandoff_ReadStatus() noexcept {
    using namespace streamline_handoff;
    const auto value = epoch.load(std::memory_order_acquire);
    return {value, maintenance_deferrals.load(std::memory_order_relaxed),
        callback_contentions.load(std::memory_order_relaxed), StreamlineHandoff_IsCurrent(value),
        tracking.load(std::memory_order_acquire), rejection.load(std::memory_order_acquire)};
}
bool StreamlineHandoff_IsCurrent(uint64_t value) noexcept {
    return value && value == streamline_handoff::epoch.load(std::memory_order_acquire) &&
        value == streamline_handoff::authorized.load(std::memory_order_acquire) && streamline_handoff::Enabled();
}
bool StreamlineHandoff_Tracking() noexcept { return streamline_handoff::tracking.load(std::memory_order_acquire); }
bool StreamlineHandoff_ClaimsInput() noexcept {
    const auto* scope = StreamlineInputScope::Current();
    return scope && scope->input.transaction && !scope->invalid && StreamlineHandoff_IsCurrent(scope->input.binding);
}
void StreamlineHandoff_SuppressInput() noexcept {
    auto* scope = StreamlineInputScope::Current();
    if (scope) scope->invalid = true; // preserve any in-flight token for Finish/rundown
    if (streamline_handoff::tracking.load(std::memory_order_acquire)) StreamlineHandoff_Invalidate("reviewed_input_owner");
}
bool StreamlineHandoff_Interested(ID3D12Fence* fence) noexcept {
    const auto* scope = StreamlineInputScope::Current();
    if (scope && scope->input.transaction && !scope->invalid) return true;
    return fence && streamline_handoff::tracking.load(std::memory_order_acquire) &&
        (fence == streamline_handoff::forward_filter.load(std::memory_order_acquire) ||
         fence == streamline_handoff::back_filter.load(std::memory_order_acquire));
}
void StreamlineHandoff_RecordMarker(uint64_t frame, uint32_t producer, uint32_t controller, int64_t release) noexcept {
    using namespace streamline_handoff;
    marker = {};
    // Publish the outcome on every exit: a rejected marker withdraws the
    // previous one, so a Present elsewhere never takes an older frame.
    struct PublishOnExit {
        bool skip = false; // a duplicate of the accepted frame keeps it published
        ~PublishOnExit() { if (!skip) marker_sequence = marker_slot.Publish(marker, marker.input.source != 0); }
    } publish;
    if (const auto* reason = PolicyRejection()) { marker_admission.Record(reason); return; }
    if (StreamlineInputScope::Current()) { marker_admission.Record("marker_inside_proxy_scope"); return; }
    const auto context = NativeGate_GetContext(release);
    GpuPresentFgInput input{0, frame, context.native_swapchain, context.swapchain_generation,
        GpuReadiness_Settings(), NativeGate_PredictionEpoch(), producer, controller};
    input.binding = epoch.load(std::memory_order_acquire);
    if (!frame || !producer || !controller) { marker_admission.Record("invalid_marker_key"); return; }
    if (const auto* reason = InputRejection(input)) { marker_admission.Record(reason); return; }
    if (last_marker.producer == producer && last_marker.controller == controller && frame <= last_marker.source) {
        publish.skip = frame == last_marker.source;
        marker_admission.Record("nonmonotonic_marker"); return;
    }
    last_marker = input;
    marker = {input, release};
    marker_admission.Record(nullptr);
}
thread_local StreamlineInputScope* StreamlineInputScope::current_ = nullptr;
StreamlineInputScope::StreamlineInputScope(uint64_t object, UINT sync, UINT flags) noexcept : sync_interval(sync), previous_(current_) {
    using namespace streamline_handoff;
    const DWORD error = GetLastError();
    current_ = this; // invalid and nested calls mask, never inherit, the parent
    auto source = marker; marker = {};
    if (source.input.source) {
        marker_slot.Claim(marker_sequence); // same-thread marker
    } else if (!previous_ && NativeGate_CrossThreadVerified() && marker_slot.Take(&source)) {
        cross_thread_takes.fetch_add(1, std::memory_order_relaxed); // marker from another thread
    }
    last_sync.store(sync, std::memory_order_relaxed); last_flags.store(flags, std::memory_order_relaxed);
    if (previous_) { present_admission.Record("nested_present"); Reject(previous_, "nested_present"); SetLastError(error); return; }
    const auto* policy = PolicyRejection();
    // This is the outer Streamline request, not the downstream output Present.
    // DXGI accepts intervals 0..4; GPU ordering comes from the qualified fences,
    // not from forcing this argument to zero. The wrapper forwards it unchanged.
    if (!object || sync > 4 || (flags & (DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_WAIT)) || !source.input.source || policy) {
        present_admission.Record(!object ? "proxy_vtable_mismatch" : sync > 4 ? "invalid_sync_interval" :
            (flags & (DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_WAIT)) ? "special_present_flags" :
            !source.input.source ? "missing_same_thread_marker" : policy);
        if (tracking.load(std::memory_order_acquire)) Reject(nullptr, "ineligible_present");
        SetLastError(error); return;
    }
    const auto context = NativeGate_GetContext(Now());
    const char* failure = "stale_marker_release";
    if (source.release > 0 && context.now_qpc >= source.release && context.qpc_frequency > 0 &&
        context.now_qpc - source.release <= context.qpc_frequency / 4) {
        failure = InputRejection(source.input);
        if (!failure) {
            input = source.input; input.transaction = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
            proxy = object; config = GpuPresent_GetConfig().word;
            multiplier = g_fg_multiplier.load(std::memory_order_acquire);
            if (multiplier < 1 || multiplier > 3 || !input.transaction) {
                failure = !input.transaction ? "transaction_exhausted" : "unsupported_multiplier"; input = {};
            }
        }
    }
    if (input.transaction && tracking.load(std::memory_order_acquire)) {
        Lock lock("source_scope_lock_busy");
        if (!lock.held) { failure = "qualification_busy"; invalid = true; }
        else if (route.epoch != input.binding) { failure = "binding_epoch_changed"; invalid = true; }
        else if (route.sync_interval != sync_interval) {
            failure = "sync_interval_changed";
            Reject(this, failure); // cancels old tickets even if this Present submits no fence
        }
    }
    if (failure) input = {};
    present_admission.Record(failure);
    if (!input.transaction && !invalid && tracking.load(std::memory_order_acquire)) Reject(nullptr, failure);
    SetLastError(error);
}
StreamlineInputScope::~StreamlineInputScope() {
    using namespace streamline_handoff;
    const DWORD error = GetLastError();
    bool valid = result_ == S_OK && !invalid && input.transaction && streamline_handoff::Current(input) &&
        config == GpuPresent_GetConfig().word && multiplier == g_fg_multiplier.load(std::memory_order_acquire);
    if (owns_cycle) {
        // Never lose a successful outer completion to a competing callback or
        // maintenance lock. The two source calls completed on this owning thread;
        // worker edges remain independently required by CompleteCycle under mutex.
        valid = valid && successful_source_fences == 3;
        if (valid) completed_source_transaction.store(input.transaction, std::memory_order_release);
        else Reject();
        // Finish only our own successfully begun measurement. An exact-hash
        // private integration may already own this call; never finish its token.
        if (handoff) GpuPresent_FinishFgInput(input.transaction, valid);
        pins.fetch_sub(1, std::memory_order_release);
    } else if (input.transaction && authorized.load(std::memory_order_acquire)) Reject();
    current_ = previous_;
    SetLastError(error);
}

StreamlineFenceCall StreamlineHandoff_BeforeFence(ID3D12CommandQueue* queue, ID3D12Fence* fence,
    uint64_t value, uint64_t site, bool signal) noexcept {
    using namespace streamline_handoff;
    auto* scope = StreamlineInputScope::Current();
    const bool source = scope && scope->input.transaction && !scope->invalid;
    if (!source && !StreamlineHandoff_Interested(fence)) return {};
    fence_admission.Record(site ? nullptr : "selected_plugin_site_missing");
    Lock lock("fence_before_lock_busy");
    if (!lock.held) return {};
    // The worker may have finished before the outer source scope published its
    // return. Resolve that pending completion before considering a new input.
    if (source && signal) CompleteCycle();
    if (source && (!Current(scope->input) || scope->config != GpuPresent_GetConfig().word ||
        scope->multiplier != g_fg_multiplier.load(std::memory_order_acquire))) { Reject(scope, "source_policy_changed"); return {}; }
    unsigned role = 0;
    if (source) {
        if (!queue || !fence || !site || !value || value == UINT64_MAX) { Reject(scope); return {}; }
        if (signal) {
            if (scope->owns_cycle) { Reject(scope); return {}; } // ambiguous, not "first Signal wins"
            if (!route.epoch) {
                route.epoch = scope->input.binding; route.config = scope->config; route.multiplier = scope->multiplier;
                route.sync_interval = scope->sync_interval;
                route.source_thread = GetCurrentThreadId(); route.cycle.input = scope->input;
                if (!Retain(reinterpret_cast<IDXGISwapChain*>(scope->proxy), &route.proxy) ||
                    !Retain(queue, &route.source) || !Retain(fence, &route.forward)) { Reject(scope); return {}; }
            } else if (!SamePolicy(*scope) || !route.cycle.complete || route.proxy != reinterpret_cast<IDXGISwapChain*>(scope->proxy) ||
                route.source != queue || route.forward != fence || route.source_thread != GetCurrentThreadId() ||
                scope->input.source <= route.previous_frame || value <= route.previous_forward ||
                (route.successes && route.sites[0] != site)) { Reject(scope); return {}; }
            completed_source_transaction.store(0, std::memory_order_relaxed);
            route.cycle = {}; route.cycle.input = scope->input; route.cycle.forward = value; route.cycle.begin = Now();
            scope->owns_cycle = true; pins.fetch_add(1, std::memory_order_acq_rel);
            retained_epoch.store(route.epoch, std::memory_order_release);
            tracking.store(true, std::memory_order_release);
            forward_filter.store(route.forward, std::memory_order_release);
            role = 1;
        } else {
            if (!scope->owns_cycle || route.epoch != scope->input.binding || route.source != queue ||
                route.cycle.input.transaction != scope->input.transaction || (route.cycle.begun & 2) ||
                fence == route.forward || value <= route.previous_back) { Reject(scope); return {}; }
            if (!route.back) { if (!Retain(fence, &route.back)) { Reject(scope); return {}; } }
            else if (route.back != fence) { Reject(scope); return {}; }
            route.cycle.back = value; role = 2;
            back_filter.store(route.back, std::memory_order_release);
        }
    } else {
        if (route.epoch != epoch.load(std::memory_order_acquire)) return {};
        if (route.cycle.complete) { Reject(); return {}; }
        if (!signal && fence == route.forward && value == route.cycle.forward) {
            if (!site || queue == route.source || (route.cycle.begun & 4)) { Reject(); return {}; }
            if (!route.worker) { if (!Retain(queue, &route.worker)) { Reject(); return {}; } }
            else if (route.worker != queue) { Reject(); return {}; }
            route.cycle.worker_thread = GetCurrentThreadId();
            worker_scope = {route.epoch, route.cycle.input.transaction, queue}; role = 3;
        } else if (signal && fence == route.back && value == route.cycle.back) {
            if (!site || !WorkerMatches() || queue != route.worker || (route.cycle.begun & 8) ||
                !route.cycle.executions || !route.cycle.callbacks) { Reject(); return {}; }
            role = 4;
        } else { Reject(); return {}; }
    }
    if (route.successes && route.sites[role - 1] != site) { Reject(scope, "callsite_changed"); return {}; }
    route.cycle.sites[role - 1] = site;
    route.cycle.begun |= 1u << (role - 1);
    const StreamlineFenceCall call{route.epoch, route.cycle.input.transaction, role};
    const bool gate = role == 1 && StreamlineHandoff_IsCurrent(call.epoch);
    lock.Unlock();
    // The hook's recursion guard masks our timestamp/Wait/retirement submissions.
    // Source/proxy/fences remain retained until this outer scope drops its pin.
    if (gate) scope->handoff = GpuPresent_BeginFgInput(reinterpret_cast<uint64_t>(queue), scope->input);
    return call;
}
void StreamlineHandoff_AfterFence(const StreamlineFenceCall& call, HRESULT result) noexcept {
    using namespace streamline_handoff;
    if (!call.role) return;
    Lock lock("fence_after_lock_busy");
    if (!lock.held || call.epoch != epoch.load(std::memory_order_acquire) || route.epoch != call.epoch ||
        route.cycle.input.transaction != call.transaction) return;
    if (result != S_OK) { Reject(nullptr, "fence_call_failed"); return; }
    if (call.role <= 2) {
        auto* scope = StreamlineInputScope::Current();
        if (!scope || !scope->owns_cycle || scope->input.binding != call.epoch || scope->input.transaction != call.transaction) {
            Reject(nullptr, "source_completion_mismatch"); return;
        }
        scope->successful_source_fences |= 1u << (call.role - 1);
    }
    route.cycle.finished |= 1u << (call.role - 1);
    if (call.role == 4) worker_scope = {};
    CompleteCycle();
}
void StreamlineHandoff_Execute(ID3D12CommandQueue* queue) noexcept {
    using namespace streamline_handoff;
    if (!tracking.load(std::memory_order_acquire) || !worker_scope.transaction ||
        worker_scope.epoch != epoch.load(std::memory_order_acquire)) return;
    Lock lock("worker_execute_lock_busy");
    if (lock.held && route.epoch == epoch.load(std::memory_order_acquire) && WorkerMatches() &&
        queue == route.worker && (route.cycle.finished & 4) && !(route.cycle.begun & 8))
        route.cycle.executions = 1; // presence, not a generated-frame count
}
void StreamlineHandoff_Boundary(const StreamlineBoundaryEvidence& evidence) noexcept {
    using namespace streamline_handoff;
    if (evidence.stage != StreamlineBoundaryStage::Present) {
        if (evidence.stage == StreamlineBoundaryStage::DestroyQueue) {
            Lock lock("queue_lifecycle_lock_busy");
            if (!lock.held) return;
            // Both roles must already be retained. Missing identity, incomplete
            // discovery or contention cannot prove this queue is unrelated.
            if (route.epoch == epoch.load(std::memory_order_acquire) && route.source && route.worker && evidence.queue &&
                evidence.queue != reinterpret_cast<uint64_t>(route.source) &&
                evidence.queue != reinterpret_cast<uint64_t>(route.worker)) return;
        }
        const char* reason = evidence.stage == StreamlineBoundaryStage::DestroyQueue ? "queue_destroyed" :
            evidence.stage == StreamlineBoundaryStage::CreateSwapchain ? "swapchain_created" :
            evidence.stage == StreamlineBoundaryStage::InitSwapchain ? "swapchain_initialized" : "swapchain_destroyed";
        StreamlineHandoff_Invalidate(reason, evidence.stage == StreamlineBoundaryStage::DestroyQueue ? evidence.queue : evidence.swapchain);
        return;
    }
    if (!tracking.load(std::memory_order_acquire) || !worker_scope.transaction) return;
    Lock lock("worker_present_lock_busy");
    if (!lock.held || route.epoch != epoch.load(std::memory_order_acquire) || !WorkerMatches()) return;
    if (evidence.swapchain != route.cycle.input.native || evidence.generation != route.cycle.input.swapchain ||
        evidence.queue != reinterpret_cast<uint64_t>(route.worker) || !route.cycle.executions ||
        (route.cycle.begun & 8)) { Reject(); return; }
    route.cycle.callbacks = 1; // no real/generated inference
}
void StreamlineHandoff_Service(bool final) noexcept {
    using namespace streamline_handoff;
    if (final) StreamlineHandoff_Invalidate("final_drain");
    else if (const auto* reason = PolicyRejection()) StreamlineHandoff_Invalidate(reason);
    Invalidation invalidation;
    for (unsigned i = 0; i < 32 && invalidations.Pop(&invalidation); ++i)
        LOG_INFO("Streamline GPU: binding invalidated epoch=%llu reason=%s qpc=%lld thread=%lu object=%llu maintenance_deferrals=%llu callback_contentions=%llu record_drops=%llu",
            invalidation.epoch, invalidation.reason, invalidation.qpc, invalidation.thread, invalidation.object,
            maintenance_deferrals.load(), callback_contentions.load(), invalidations.DroppedValues());
    ReportAdmission(final); // no formatting or I/O from marker/Present/queue callbacks
    // Snapshot only scalars while locked. Registry/context and policy lookups
    // can overlap a critical fence callback and must not hold its SRW lock.
    Lock snapshot_lock(nullptr);
    if (!snapshot_lock.held || !route.epoch) return;
    const auto binding = route.epoch;
    const auto transaction = route.cycle.input.transaction;
    const auto input = route.cycle.input;
    const auto begin = route.cycle.begin;
    const bool complete = route.cycle.complete;
    snapshot_lock.Unlock();
    const bool current = binding == epoch.load(std::memory_order_acquire);
    const char* failure = current ? InputRejection(input) : nullptr;
    const auto context = current && !complete && begin > 0 ? NativeGate_GetContext(Now()) : NativeGateContext{};
    // If a source replaced this cycle while validation was in flight, the
    // next service pass will inspect it. Never apply old policy to a new input.
    Lock lock(nullptr);
    if (!lock.held || route.epoch != binding || route.cycle.input.transaction != transaction) return;
    if (current && route.epoch == epoch.load(std::memory_order_acquire)) {
        CompleteCycle(failure, true); // consumes worker-before-source completion
        if (failure && route.epoch == epoch.load(std::memory_order_acquire)) Reject(nullptr, failure);
        if (route.epoch == epoch.load(std::memory_order_acquire) && !route.cycle.complete &&
            route.cycle.begin == begin && begin > 0 && context.qpc_frequency > 0 &&
            context.now_qpc - begin > context.qpc_frequency / 4) Reject(nullptr, "incomplete_cycle_timeout");
    }
    if (route.epoch != epoch.load(std::memory_order_acquire)) {
        if (pins.load(std::memory_order_acquire)) return;
        const auto old = route;
        const auto* reason = rejection.load(std::memory_order_acquire);
        route = {}; retained_epoch.store(0, std::memory_order_release); tracking.store(false, std::memory_order_release);
        completed_source_transaction.store(0, std::memory_order_relaxed);
        forward_filter.store(nullptr, std::memory_order_release); back_filter.store(nullptr, std::memory_order_release);
        lock.Unlock();
        // R14: the source queue outlives the binding, so the gate keeps it
        // registered and only re-reads the FG swapchain identity. Destroying a
        // queue is reported by ReShade (GpuPresent_OnQueueDestroy). No
        // plugin-owned fence is destroyed or signalled here.
        if (old.source) GpuPresent_OnFgBindingRetired(reinterpret_cast<uint64_t>(old.source));
        const bool released = Release(old.proxy) & Release(old.source) & Release(old.worker) & Release(old.forward) & Release(old.back);
        LOG_INFO("Streamline GPU: binding retired epoch=%llu last_reason=%s retained_refs_released=%d", old.epoch, reason, released);
        return;
    }
    if (route.cycle.complete && !route.device_checked) {
        const auto checked_epoch = route.epoch;
        pins.fetch_add(1, std::memory_order_acq_rel);
        lock.Unlock();
        const bool valid = CheckDevices(); // retained interfaces, installer worker only
        Lock publish(nullptr); // retained pins survive a deferred device-check publication
        if (publish.held && route.epoch == checked_epoch && checked_epoch == epoch.load(std::memory_order_acquire)) {
            if (!valid) Reject(nullptr, "device_validation_failed");
            else {
                route.device_checked = true;
                if (route.successes >= RequiredCycles) authorized.store(checked_epoch, std::memory_order_release);
            }
        }
        pins.fetch_sub(1, std::memory_order_release);
        return;
    }
    static uint64_t reported = 0;
    const auto accepted = authorized.load(std::memory_order_acquire);
    if (accepted && accepted != reported) {
        reported = accepted;
        const auto snapshot = route; // scalar diagnostics only after unlocking; never dereference copied pointers
        lock.Unlock();
        LOG_INFO("Streamline GPU: qualified epoch=%llu cycles=%u source=%p worker=%p native=%llu generation=%llu sync=%u; deadline before input Signal",
            accepted, snapshot.successes, snapshot.source, snapshot.worker, snapshot.cycle.input.native, snapshot.cycle.input.swapchain, snapshot.sync_interval);
    }
}
bool StreamlineHandoff_ReleaseAfterRundown() noexcept {
    StreamlineHandoff_Service(true);
    return !streamline_handoff::route.epoch && !streamline_handoff::pins.load(std::memory_order_acquire) &&
        !streamline_handoff::faulted.load(std::memory_order_acquire);
}
void StreamlineHandoff_RejectQueueMethods() noexcept {
    streamline_handoff::fence_admission.Record("queue_vtable_methods_mismatch");
    streamline_handoff::Reject(nullptr, "queue_vtable_methods_mismatch");
}