#include "cross_thread_slot.h"
#include "streamline_gpu_handoff.h"
#include "streamline_discovery.h"
#include "streamline_hooks.h"
#include "native_present_gate.h"
#include "gpu_readiness.h"
#include "feedback_device_identity.h"
#include "swapchain_manager.h"
#include "runtime_lifecycle.h"
#include "gpu_queue_probe.h"
#include "fg_present_order.h"
#include "gpu_hold_order.h"
#include "logger.h"
#include "pacing_resolver.h"
#include "bounded_mpsc_queue.h"
#include <d3d12.h>
#include <dxgi.h>
#include <cstdio>
#include <array>
#include <atomic>

namespace streamline_handoff {
constexpr unsigned RequiredCycles = 4;
std::atomic<uint64_t> epoch{1}, authorized{0}, sequence{0};
// The source queue whose fence-chain proof the authorized binding's hold stands on (a separate presenter's binding,
// see Authorize), or null when it stands on none (a worker-local Present). Stored before `authorized`, read only
// behind it.
std::atomic<const void*> authorized_proof{nullptr};
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
    // The back Signal came without a worker-local Present: frame generation presents from its own thread, and
    // present-first ordering holds the source queue for that Present (fg_present_order.h).
    bool separate = false;
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
    bool separate = false; // the kind of the binding's completed cycles (Cycle::separate); one kind per binding
    Cycle cycle{};
} route;
struct Marker { GpuPresentFgInput input{}; int64_t release = 0; };
thread_local Marker marker;
thread_local GpuPresentFgInput last_marker;
// gpu_queue_probe: the real frame's marker, recorded whatever the gate's demand or policy (the
// probe measures the source queue whenever its key is on). Validated like an input (frame, producer,
// controller, frame generation observed, producer epoch and controller of the current context)
// except the gate's policy and binding epoch; and, for a Present on another thread than the marker's
// (Gears), through its own hand-off slot (cross_thread_slot.h CrossThreadSlot: the newest marker, taken
// exactly once by the game's own outermost proxy Present, which frame generation never calls). That slot
// keeps its latest-wins hand-off, unlike the route's (marker_slot below, taken by frame id), because the
// probe is telemetry: its key is the one the Present's pacing row takes from the same newest marker
// (PacingTelemetry_TakePresentAttribution), so a frame is attributed to the row the pipeline itself
// attributes that Present to. What that leaves unverified is what the native gate verifies with ETW frame
// ids: that Present(n) takes marker n and not n - 1.
thread_local Marker probe_marker;
thread_local uint64_t probe_sequence = 0;
thread_local uint64_t probe_last_frame = 0;
thread_local uint32_t probe_last_producer = 0;
CrossThreadSlot<Marker> probe_slot;
bool ProbeInputCurrent(const GpuPresentFgInput& input) noexcept {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const auto context = NativeGate_GetContext(now.QuadPart);
    return context.observe_frame_generation && context.native_swapchain == input.native &&
        context.swapchain_generation == input.swapchain && context.producer_epoch == input.producer &&
        context.controller_generation == input.controller && input.observer == GpuReadiness_Settings() &&
        input.prediction == NativeGate_PredictionEpoch();
}
void ProbeRecordMarker(uint64_t frame, uint32_t producer, uint32_t controller, int64_t release) noexcept {
    // A duplicate of the accepted frame keeps what is recorded and published.
    if (frame && probe_last_producer == producer && frame == probe_last_frame) return;
    probe_marker = {};
    bool valid = false;
    if (frame && producer && controller) {
        const auto context = NativeGate_GetContext(release);
        if (context.observe_frame_generation && context.native_swapchain && context.producer_epoch == producer &&
            context.controller_generation == controller) {
            if (!(probe_last_producer == producer && frame < probe_last_frame)) { // one input per frame, in order
                probe_last_producer = producer; probe_last_frame = frame;
                probe_marker.input = {0, frame, context.native_swapchain, context.swapchain_generation,
                    GpuReadiness_Settings(), NativeGate_PredictionEpoch(), producer, controller};
                probe_marker.release = release;
                valid = true;
            }
        }
    }
    // The newest marker (an empty one withdraws the previous) for a Present on another thread.
    probe_sequence = probe_slot.Publish(probe_marker, valid);
}
// Cross-thread hand-off of the route's markers (cross_thread_slot.h): the newest eight, each under its frame id
// (Marker::input.source). A proxy Present on a thread without a marker of its own (Gears sends PRESENT_START on the
// render thread and presents on another) claims a marker only by identity: the frame id that an async PRESENT_START
// states on that thread, inside that outermost scope and before the scope's first source fence call
// (StreamlineHandoff_RecordInbandFrame; Streamline's NvAPI_D3D12_SetAsyncFrameMarker names the frame being
// presented, in the game's own NvAPI frame-id domain). No identity, no claim. "The newest marker" is not an
// identity: while a scene loads about one Present in five begins after the next frame's marker, and the newest
// marker is then the wrong frame; the ETW check that used to guard that (NativeGate_CrossThreadVerified) comes late
// and cannot see frames with frame generation on, so the route no longer consults it. A rejected marker leaves a
// tombstone that withdraws no older marker. The probe's own slot (probe_slot) keeps its latest-wins hand-off.
CrossThreadRing<Marker> marker_slot;
thread_local uint64_t marker_sequence = 0;
// The in-band claims, for the admission report and the qualified line (relaxed counters, formatted by the installer
// worker only). claims: a marker claimed by the in-band frame id of a Present without a marker of its own. unknown:
// such a Present whose id no marker in the ring has (never recorded, rejected, evicted, consumed, or lost to
// contention). late: one that reached a fence call of the handoff before its id. absent: one that ended without any.
// match, mismatch: for a Present that has claimed a marker (this thread's own or by id), a later in-band frame id of
// the same Present that equals the claimed frame or not (counters only: a claim is never changed by them).
std::atomic<uint64_t> inband_claims{0}, inband_unknown{0}, inband_late{0}, inband_absent{0}, inband_match{0},
    inband_mismatch{0};
unsigned long long Counted(const std::atomic<uint64_t>& counter) noexcept { return counter.load(std::memory_order_relaxed); }
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
    // Also report while the binding is current but the FG input route
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
        Counted(inband_claims));
    const auto route = GpuPresent_FgRouteDiagnostics();
    LOG_INFO("Streamline GPU: FG route inputs seen=%llu pending=%llu invalid=%llu stale_binding=%llu queue_missing=%llu "
             "queue_unready=%llu fg_queue_unready=%llu identity=%llu",
        route.seen, route.pending, route.invalid_input, route.stale_binding, route.queue_missing,
        route.queue_unready, route.fg_unready, route.identity);
    LOG_INFO("Streamline GPU: in-band frame claims=%llu unknown=%llu late=%llu absent=%llu match=%llu mismatch=%llu",
        Counted(inband_claims), Counted(inband_unknown), Counted(inband_late), Counted(inband_absent),
        Counted(inband_match), Counted(inband_mismatch));
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
// Separate presenter (fg_route_unsupported). Without hardware flip metering,
// DLSS-G presents the native swapchain from its own thread and queue after the
// worker's return Signal, so the worker-local Present this route requires never
// comes (Valor Mortis with SetFlipConfig hidden: every Present on one other
// thread, none on the worker's queue). Positive evidence only: during the
// current cycle (its token), a native Present of the bound swapchain outside
// any source scope, on a thread other than the known worker's and a known queue
// other than its queue. The worker identity outlives a binding, so each
// binding's first cycle can be judged; Presents are seen lock-free.
constexpr const char* OtherQueuePresentReason = "worker_present_on_other_queue";
constexpr const char* PresentQueueNotWorkerReason = "present_queue_not_worker";
struct SeparatePresenter {
    std::atomic<uint64_t> native{0}, generation{0};         // the cycle's swapchain, published at the source Signal
    std::atomic<ID3D12CommandQueue*> worker_queue{nullptr}; // published at the worker Wait
    std::atomic<DWORD> worker_thread{0};
    // Cycle token: advanced at each source Signal (under the lock). A Present
    // records the token it loaded before its checks, so a stale observation can
    // only miss a cycle, never mark the next one.
    std::atomic<uint64_t> cycle{0}, seen_cycle{0};
    // The unsupported route's identity: written under the lock, read and
    // re-judged by Service every pass without it.
    std::atomic<uint64_t> tag_native{0}, tag_generation{0};
    std::atomic<uint32_t> tag_producer{0}, tag_controller{0};
    std::atomic<int> tag_multiplier{0};
    std::atomic<bool> evident{false};
    bool published = false; // installer worker only
} separate;
void RecordChainPresenter(uint64_t queue) noexcept; // fence chain, below
// Lock-free: a native Present outside any source scope.
void RecordSeparatePresent(const StreamlineBoundaryEvidence& evidence) noexcept {
    constexpr auto relaxed = std::memory_order_relaxed;
    const auto cycle = separate.cycle.load(std::memory_order_acquire);
    const auto native = separate.native.load(relaxed);
    if (!cycle || !native || evidence.swapchain != native || evidence.generation != separate.generation.load(relaxed) ||
        StreamlineInputScope::Current() || !tracking.load(std::memory_order_acquire)) return;
    const auto* worker = separate.worker_queue.load(std::memory_order_acquire);
    // An unknown queue (0) is no evidence of another queue.
    if (!worker || !evidence.queue || GetCurrentThreadId() == separate.worker_thread.load(relaxed) ||
        evidence.queue == reinterpret_cast<uint64_t>(worker)) return;
    separate.seen_cycle.store(cycle, relaxed);
    RecordChainPresenter(evidence.queue);
}
// Fence chain: which fences the FG worker's queue and the separate presenter's queue signal and wait on, and for a
// wait on a fence the other side signals, whether the waited value is at, below or above that side's newest Signal
// so far. The counts are the report lines'. The facts are also the one proof a hold on a separate presenter's route
// stands on (ReportChain, ChainProven, Authorize): the presenter's queue is not the game's source queue, and its
// Presents depend on the worker's work.
// Only the installer worker publishes the two queues, from observation records; the
// hooks push records for published queues only. Destroying any queue advances one
// epoch; every record carries the epoch it read before the queues. The installer
// worker aggregates in push order: older-epoch records are dropped, and a newer epoch
// reports, resets and unpublishes both sides until observations of that epoch
// publish them again. So a Wait is judged only against Signals of its own epoch
// pushed before it, and nothing crosses a queue's destruction. A record the queue
// dropped and a native call that failed advance the same epoch (VoidChain): what the
// hooks saw is then incomplete or untrue, and nothing before it counts. A side also
// restarts, after being reported, when its queue changes. The tables keep the first 8
// fences per queue and direction.
struct ChainRecord {
    enum class Kind : uint8_t { WorkerSignal, WorkerWait, PresenterSignal, PresenterWait, WorkerSeen, PresenterSeen };
    Kind kind = Kind::WorkerSignal;
    bool presenter_is_source = false; // PresenterSeen
    const void* queue = nullptr;
    const void* fence = nullptr;
    uint64_t value = 0, epoch = 0;
};
struct ChainDiagnostic {
    // Queues: written by the installer worker only. Epoch: advanced by any queue destroy, lost record or failed call (VoidChain).
    std::atomic<ID3D12CommandQueue*> worker_queue{nullptr}, presenter_queue{nullptr}, source_queue{nullptr};
    std::atomic<uint64_t> epoch{1};
    std::atomic<ID3D12Fence*> forward_fence{nullptr}, back_fence{nullptr};
    std::atomic<DWORD> presenter_thread{0};
    // Installer worker: the fence-chain proof of its last pass (ReportChain): the source queue it holds for, or null,
    // and the epoch of the aggregate it was computed from. Read through ChainProven only.
    std::atomic<const void*> proof_source{nullptr};
    std::atomic<uint64_t> proof_epoch{0};
    relimiter::BoundedMpscQueue<ChainRecord, 1024> records;
} chain;
// Installer worker only.
struct ChainTally {
    const void* fence = nullptr;
    uint64_t calls = 0, last = 0, at = 0, below = 0, above = 0;
    // A presenter wait entry's evidence of the current report pass (ReportChain): its waits at or above, and below,
    // the worker's newest Signal. `proves` is what the last pass that saw a wait on it decided.
    uint64_t pass_ok = 0, pass_below = 0;
    bool proves = false;
};
struct ChainTable {
    std::array<ChainTally, 8> entries{};
    uint64_t overflow = 0;
    const ChainTally* Find(const void* fence) const noexcept {
        for (const auto& entry : entries) if (entry.fence == fence) return &entry;
        return nullptr;
    }
    ChainTally* Claim(const void* fence) noexcept {
        for (auto& entry : entries) {
            if (entry.fence == fence) return &entry;
            if (!entry.fence) { entry.fence = fence; return &entry; }
        }
        ++overflow;
        return nullptr;
    }
};
struct ChainSide { const void* queue = nullptr; ChainTable signals, waits; };
struct ChainAggregate {
    ChainSide worker, presenter;
    uint64_t epoch = 1, presenter_is_source = 0, presenter_other = 0, dropped = 0, version = 0, reported = 0;
    int64_t last_report = 0;
} aggregate;
void ReportChainTable(const char* side, const char* direction, const ChainTable& table, const void* queue) noexcept {
    char line[1024]; int used = 0;
    const void* forward = chain.forward_fence.load(std::memory_order_relaxed);
    const void* back = chain.back_fence.load(std::memory_order_relaxed);
    for (const auto& entry : table.entries) {
        if (!entry.fence || used >= static_cast<int>(sizeof(line))) break;
        const int wrote = std::snprintf(line + used, sizeof(line) - used, " [%p%s calls=%llu last=%llu at=%llu below=%llu above=%llu]",
            entry.fence, entry.fence == forward ? " forward" : entry.fence == back ? " back" : "",
            static_cast<unsigned long long>(entry.calls), static_cast<unsigned long long>(entry.last),
            static_cast<unsigned long long>(entry.at), static_cast<unsigned long long>(entry.below),
            static_cast<unsigned long long>(entry.above));
        if (wrote < 0) break;
        used += wrote;
    }
    if (used == 0) return;
    LOG_INFO("Streamline GPU: chain %s %s queue=%p overflow=%llu:%s", side, direction, queue,
        static_cast<unsigned long long>(table.overflow), line);
}
void ReportChainSide(const char* name, const ChainSide& side) noexcept {
    ReportChainTable(name, "signals", side.signals, side.queue);
    ReportChainTable(name, "waits", side.waits, side.queue);
}
// Reports the previous queue's counts, then publishes `queue` (nullptr unpublishes).
void RestartChainSide(const char* name, ChainSide& side, std::atomic<ID3D12CommandQueue*>& published, const void* queue) noexcept {
    if (side.queue) ReportChainSide(name, side);
    side = {};
    side.queue = queue;
    published.store(const_cast<ID3D12CommandQueue*>(static_cast<const ID3D12CommandQueue*>(queue)), std::memory_order_release);
    ++aggregate.version;
}
// A queue was destroyed: nothing of the previous epoch is counted or published again.
void AdvanceChainEpoch(uint64_t epoch) noexcept {
    RestartChainSide("worker", aggregate.worker, chain.worker_queue, nullptr);
    RestartChainSide("presenter", aggregate.presenter, chain.presenter_queue, nullptr);
    aggregate.epoch = epoch;
}
void AggregateChain(const ChainRecord& record) noexcept {
    using Kind = ChainRecord::Kind;
    auto& a = aggregate;
    if (record.epoch < a.epoch) { ++a.dropped; return; }
    if (record.epoch > a.epoch) AdvanceChainEpoch(record.epoch);
    const bool worker = record.kind == Kind::WorkerSignal || record.kind == Kind::WorkerWait || record.kind == Kind::WorkerSeen;
    auto& side = worker ? a.worker : a.presenter;
    if (record.kind == Kind::WorkerSeen || record.kind == Kind::PresenterSeen) {
        if (record.kind == Kind::PresenterSeen) { ++(record.presenter_is_source ? a.presenter_is_source : a.presenter_other); ++a.version; }
        if (side.queue != record.queue)
            RestartChainSide(worker ? "worker" : "presenter", side, worker ? chain.worker_queue : chain.presenter_queue, record.queue);
        return;
    }
    if (side.queue != record.queue) { ++a.dropped; return; }
    ++a.version;
    const bool signal = record.kind == Kind::WorkerSignal || record.kind == Kind::PresenterSignal;
    auto* entry = (signal ? side.signals : side.waits).Claim(record.fence);
    if (!entry) return;
    ++entry->calls;
    entry->last = record.value;
    if (signal) return;
    if (const auto* signalled = (worker ? a.presenter : a.worker).signals.Find(record.fence)) {
        const bool below = record.value < signalled->last;
        ++(record.value == signalled->last ? entry->at : below ? entry->below : entry->above);
        if (!worker) ++(below ? entry->pass_below : entry->pass_ok); // the presenter's evidence of this pass
    }
}
// Nothing recorded so far counts. A destroyed queue's address can be reused by a later queue, and a record the queue
// dropped or a call that failed leaves what the hooks saw incomplete or untrue. The proof is void at once
// (ChainProven reads this epoch); the installer worker drops the history at its next pass.
void VoidChain() noexcept { chain.epoch.fetch_add(1, std::memory_order_acq_rel); }
// Every record goes through here: a lost one voids the chain.
void PushChain(const ChainRecord& record) noexcept {
    if (!chain.records.Push(record)) VoidChain();
}
void PushChainSeen(ChainRecord::Kind kind, const void* queue, bool presenter_is_source) noexcept {
    ChainRecord record;
    record.kind = kind;
    record.queue = queue;
    record.presenter_is_source = presenter_is_source;
    record.epoch = chain.epoch.load(std::memory_order_acquire);
    PushChain(record);
}
// A separate-presenter Present passed RecordSeparatePresent's checks.
void RecordChainPresenter(uint64_t queue) noexcept {
    auto* presenter = reinterpret_cast<ID3D12CommandQueue*>(queue);
    chain.presenter_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    PushChainSeen(ChainRecord::Kind::PresenterSeen, presenter, presenter == chain.source_queue.load(std::memory_order_acquire));
}
// Installer worker: publishes the proof of this pass (null source: none) with the epoch of the aggregate it was
// computed from. ChainProven loads the epoch, then the source, then the chain's epoch; so the source is retracted
// before the epoch changes and published after it, and a reader can only match a pair published together.
void PublishChainProof(uint64_t aggregate_epoch, const void* source) noexcept {
    if (chain.proof_epoch.load(std::memory_order_relaxed) == aggregate_epoch &&
        chain.proof_source.load(std::memory_order_relaxed) == source) return;
    chain.proof_source.store(nullptr, std::memory_order_release);
    chain.proof_epoch.store(aggregate_epoch, std::memory_order_release);
    chain.proof_source.store(source, std::memory_order_release);
}
// Any thread, no lock: the fence-chain proof holds for `source` now. A proof holds only in the epoch of the aggregate
// it was computed from, so a queue destroy, a lost record or a failed call (VoidChain) ends it before the installer
// worker's next pass.
bool ChainProven(const void* source) noexcept {
    const uint64_t proved = chain.proof_epoch.load(std::memory_order_acquire);
    return source && chain.proof_source.load(std::memory_order_acquire) == source &&
        chain.epoch.load(std::memory_order_acquire) == proved;
}
void ReportChain(bool final) noexcept {
    auto& a = aggregate;
    ChainRecord record;
    for (unsigned i = 0; i < 4096 && chain.records.Pop(&record); ++i) AggregateChain(record);
    // A destroy without records since: unpublish now, so no reused address is matched.
    if (const uint64_t epoch = chain.epoch.load(std::memory_order_acquire); epoch > a.epoch) AdvanceChainEpoch(epoch);
    // The presenter's waits of this pass, per entry: waits only at or above the worker's newest Signal prove it, a
    // wait below it unproves it, and a pass that saw none changes nothing. The cumulative counts stay for the report
    // lines. The records are in the order the threads pushed them, not the order of the GPU's work, so one late
    // record (a wait pushed behind the worker's next Signal) voids the proof for its pass, not for the epoch.
    for (auto& wait : a.presenter.waits.entries) {
        if (wait.pass_below) wait.proves = false;
        else if (wait.pass_ok) wait.proves = true;
        wait.pass_ok = wait.pass_below = 0;
    }
    // The fence-chain proof a hold on a separate presenter's route stands on: in this epoch the presenter's queue
    // is neither the game's source queue nor the worker's, and it waits on a fence the worker's queue signals, at or
    // above the worker's newest Signal as of the last pass that saw a wait on it. It is published with the epoch of
    // this aggregate, so a destroy after the epoch read above (or a lost record, or a failed call) voids it at once.
    {
        const void* source = chain.source_queue.load(std::memory_order_acquire);
        bool proven = source && a.presenter.queue && a.worker.queue && a.presenter.queue != source &&
            a.presenter.queue != a.worker.queue;
        if (proven) {
            proven = false;
            for (const auto& wait : a.presenter.waits.entries)
                if (wait.fence && wait.proves && a.worker.signals.Find(wait.fence)) { proven = true; break; }
        }
        PublishChainProof(a.epoch, proven ? source : nullptr);
    }
    const auto context = NativeGate_GetContext(Now());
    if (a.version == a.reported || (!final && (context.qpc_frequency <= 0 ||
        context.now_qpc - a.last_report < context.qpc_frequency * 5))) return;
    a.reported = a.version; a.last_report = context.now_qpc;
    constexpr auto relaxed = std::memory_order_relaxed;
    LOG_INFO("Streamline GPU: chain presenter_queue=%p thread=%lu source_queue=%p worker_queue=%p "
             "presenter_is_source=%llu presenter_other=%llu forward=%p back=%p record_drops=%llu stale_records=%llu",
        a.presenter.queue, static_cast<unsigned long>(chain.presenter_thread.load(relaxed)),
        static_cast<void*>(chain.source_queue.load(relaxed)), a.worker.queue,
        static_cast<unsigned long long>(a.presenter_is_source), static_cast<unsigned long long>(a.presenter_other),
        static_cast<void*>(chain.forward_fence.load(relaxed)), static_cast<void*>(chain.back_fence.load(relaxed)),
        static_cast<unsigned long long>(chain.records.DroppedValues()), static_cast<unsigned long long>(a.dropped));
    ReportChainSide("worker", a.worker);
    ReportChainSide("presenter", a.presenter);
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
void Reject(StreamlineInputScope* scope, const char* reason) noexcept {
    if (scope) scope->invalid = true;
    StreamlineHandoff_Invalidate(reason);
}
// The route's identity as a separate presenter's. Caller holds the lock.
void TagSeparate() noexcept {
    constexpr auto relaxed = std::memory_order_relaxed;
    const auto& input = route.cycle.input;
    separate.tag_native.store(input.native, relaxed); separate.tag_generation.store(input.swapchain, relaxed);
    separate.tag_producer.store(input.producer, relaxed); separate.tag_controller.store(input.controller, relaxed);
    separate.tag_multiplier.store(route.multiplier, relaxed);
    separate.evident.store(true, std::memory_order_release);
}
// A structural reason marks this route identity as a separate presenter's. Caller holds the lock.
void RejectRoute(const char* reason) noexcept {
    if (reason == OtherQueuePresentReason || reason == PresentQueueNotWorkerReason) TagSeparate();
    Reject(nullptr, reason);
}
// Installer worker: the separate-presenter capability for the current controller,
// while the evidence matches the current route identity and FG presents. Cleared
// by a cycle completed with a worker-local Present or by such a Present, never by time.
// The route still qualifies while present-first ordering holds the source queue for the presenter (Cycle::separate;
// the developer switch fg_separate_presenter_hold, on by default); the resolver reads the capability only while it does not.
void PublishSeparatePresenter() noexcept {
    constexpr auto relaxed = std::memory_order_relaxed;
    const auto context = NativeGate_GetContext(Now());
    const bool fg = g_fg_mode.load(std::memory_order_acquire) == 1 && g_fg_presenting.load(std::memory_order_acquire);
    auto& s = separate;
    bool evident = s.evident.load(std::memory_order_acquire);
    if (evident && !(fg && s.tag_native.load(relaxed) == context.native_swapchain &&
            s.tag_generation.load(relaxed) == context.swapchain_generation &&
            s.tag_producer.load(relaxed) == context.producer_epoch &&
            s.tag_controller.load(relaxed) == context.controller_generation &&
            s.tag_multiplier.load(relaxed) == g_fg_multiplier.load(std::memory_order_acquire))) {
        s.evident.store(false, relaxed);
        evident = false;
    }
    PacingPipeline_PublishCapability(PacingCapability::FgSeparatePresenter, evident, context.controller_generation);
    if (evident == s.published) return;
    s.published = evident;
    if (evident) {
        if (g_fg_separate_presenter_hold.load(relaxed))
            LOG_INFO("Streamline GPU: frame generation presents from its own thread and queue (no hardware flip "
                     "metering); the FG input wait holds it while present-first ordering is established, otherwise "
                     "the cap applies at the source (fg_route_unsupported)");
        else
            LOG_INFO("Streamline GPU: frame generation presents from its own thread and queue (no hardware flip "
                     "metering); fg_separate_presenter_hold is off, so the FG input wait does not hold it and the cap "
                     "applies at the source (fg_route_unsupported)");
    } else {
        LOG_INFO("Streamline GPU: the separate presenter is no longer current; the FG input route is judged again");
    }
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
// Each rejection names the check that failed; the first one of a binding is
// logged (`binding invalidated ... reason=`). These read route state under the lock.
constexpr const char* SiteChangedReasons[4] = {"source_signal_site_changed", "source_wait_site_changed",
    "worker_wait_site_changed", "worker_signal_site_changed"};
const char* SourceSignalRejection(const StreamlineInputScope& scope, ID3D12CommandQueue* queue, ID3D12Fence* fence,
    uint64_t value, uint64_t site) noexcept {
    if (!SamePolicy(scope)) return "route_policy_mismatch";
    if (!route.cycle.complete) return "previous_cycle_incomplete";
    if (route.proxy != reinterpret_cast<IDXGISwapChain*>(scope.proxy)) return "proxy_changed";
    if (route.source != queue) return "source_queue_changed";
    if (route.forward != fence) return "forward_fence_changed";
    if (route.source_thread != GetCurrentThreadId()) return "source_thread_changed";
    if (scope.input.source <= route.previous_frame) return "source_frame_not_newer";
    if (value <= route.previous_forward) return "forward_value_not_newer";
    if (route.successes && route.sites[0] != site) return SiteChangedReasons[0];
    return nullptr;
}
const char* SourceWaitRejection(const StreamlineInputScope& scope, ID3D12CommandQueue* queue, ID3D12Fence* fence,
    uint64_t value) noexcept {
    if (!scope.owns_cycle) return "source_wait_before_signal";
    if (route.epoch != scope.input.binding) return "source_wait_binding_changed";
    if (route.source != queue) return "source_wait_queue_changed";
    if (route.cycle.input.transaction != scope.input.transaction) return "source_wait_transaction_changed";
    if (route.cycle.begun & 2) return "source_wait_repeated";
    if (fence == route.forward) return "source_wait_on_forward_fence";
    if (value <= route.previous_back) return "back_value_not_newer";
    return nullptr;
}
const char* WorkerWaitRejection(ID3D12CommandQueue* queue, uint64_t site) noexcept {
    if (!site) return "worker_wait_site_missing";
    if (queue == route.source) return "worker_wait_on_source_queue";
    if (route.cycle.begun & 4) return "worker_wait_repeated";
    return nullptr;
}
// WorkerMatches(), split so the name says which part differed.
const char* WorkerSignalRejection(ID3D12CommandQueue* queue, uint64_t site) noexcept {
    if (!site) return "worker_signal_site_missing";
    if (worker_scope.epoch != route.epoch || worker_scope.transaction != route.cycle.input.transaction)
        return "worker_signal_without_wait";
    if (route.cycle.worker_thread != GetCurrentThreadId()) return "worker_signal_thread_changed";
    if (worker_scope.queue != route.worker || queue != route.worker) return "worker_signal_queue_changed";
    if (route.cycle.begun & 8) return "worker_signal_repeated";
    if (!route.cycle.executions) return "worker_signal_before_execute";
    // No worker-local Present. With a separate presenter that is the rule. Its cycle stands while present-first
    // ordering holds this source queue for the presenter's Present (fg_present_order.h); the binding is first
    // authorized only with the fence-chain proof as well, and holds only while it holds (Authorize). Otherwise the
    // cycle fails, and the failure names a separate presenter on positive evidence only: a Present of another
    // thread and queue inside this cycle, or the ordering unit's (which does not depend on a Present falling into
    // a cycle's span, a matter of the phase between two threads).
    if (!route.cycle.callbacks && !FgPresentOrder_ProvesSeparatePresenter(route.source))
        return (separate.seen_cycle.load(std::memory_order_relaxed) == separate.cycle.load(std::memory_order_relaxed) &&
                separate.worker_queue.load(std::memory_order_relaxed) == route.worker) ||
            FgPresentOrder_Orders(route.source)
            ? OtherQueuePresentReason : "worker_signal_before_present";
    return nullptr;
}
// Outside the input scope, Interested() admits the forward and back fences before
// the lock; a binding replaced since then can leave a fence of neither.
const char* UnexpectedFenceRejection(ID3D12Fence* fence, bool signal) noexcept {
    if (fence == route.forward) return signal ? "forward_signal_outside_input" : "forward_wait_value_mismatch";
    if (fence == route.back) return signal ? "back_signal_value_mismatch" : "back_wait_outside_input";
    return "fence_not_in_route";
}
const char* WorkerPresentRejection(const StreamlineBoundaryEvidence& evidence) noexcept {
    if (evidence.swapchain != route.cycle.input.native) return "worker_present_swapchain_changed";
    if (evidence.generation != route.cycle.input.swapchain) return "worker_present_generation_changed";
    if (!evidence.queue) return "worker_present_queue_unknown";
    if (evidence.queue != reinterpret_cast<uint64_t>(route.worker)) return PresentQueueNotWorkerReason;
    if (!route.cycle.executions) return "worker_present_before_execute";
    if (route.cycle.begun & 8) return "worker_present_after_back_signal";
    return nullptr;
}
// Caller holds the lock. A binding with a separate presenter is first authorized while the fence chain is proven for
// its source queue (ReportChain): the presenter's queue is not the source queue, which the hold blocks, and waits
// on a fence the worker's queue signals. Until then its cycles complete without the hold, which keeps the binding
// tracked so that the presenter's Presents and waits are recorded. Once authorized, the proof is enforced where a
// hold is admitted and validated (StreamlineHandoff_IsCurrent, through authorized_proof): a proof lost afterwards
// (a queue destroy, a wait below the worker's newest Signal, a lost record, a failed call) pauses the hold and ends
// nothing; the hold resumes when the chain is proven again.
void Authorize() noexcept {
    if (route.successes < RequiredCycles || !route.device_checked) return;
    if (route.separate && authorized.load(std::memory_order_acquire) != route.epoch && !ChainProven(route.source)) return;
    authorized_proof.store(route.separate ? route.source : nullptr, std::memory_order_release);
    authorized.store(route.epoch, std::memory_order_release);
}
void CompleteCycle(const char* validated_reason = nullptr, bool validated = false) noexcept {
    auto& cycle = route.cycle;
    if (!cycle.outer_done && cycle.input.transaction &&
        completed_source_transaction.load(std::memory_order_acquire) == cycle.input.transaction)
        cycle.outer_done = true;
    if (cycle.complete || !cycle.outer_done || cycle.finished != 15 || cycle.begun != 15 ||
        !cycle.executions || (!cycle.callbacks && !cycle.separate) || route.epoch != epoch.load(std::memory_order_acquire)) return;
    if (const auto* reason = validated ? validated_reason : InputRejection(cycle.input)) { Reject(nullptr, reason); return; }
    if (!route.successes) { route.sites = cycle.sites; route.separate = cycle.separate; }
    else if (route.sites != cycle.sites) { Reject(nullptr, "cycle_sites_changed"); return; }
    else if (route.separate != cycle.separate) { Reject(nullptr, "cycle_presenter_changed"); return; }
    // Distinct input and consumer sites are learned from exact fence edges,
    // not fixed RVAs, signal ordinals, thread/FPS ratios or module ownership alone.
    if (cycle.sites[0] == cycle.sites[3] || cycle.sites[1] == cycle.sites[2]) { Reject(nullptr, "cycle_sites_not_distinct"); return; }
    cycle.complete = true;
    if (cycle.separate) TagSeparate();
    else separate.evident.store(false, std::memory_order_relaxed); // a completed cycle proves the worker-local route
    route.previous_frame = cycle.input.source;
    route.previous_forward = cycle.forward;
    route.previous_back = cycle.back;
    if (route.successes < RequiredCycles) ++route.successes;
    Authorize();
}
// `separate_route`: the worker does not present, so a compute queue is accepted for it.
bool CheckDevices(bool separate_route) noexcept {
    auto* expected = SwapMgr_AcquirePresentingDevice();
    if (!expected) return false;
    bool valid = true;
    IUnknown* devices[5]{};
    __try {
        const auto worker_type = route.worker->GetDesc().Type;
        valid = route.source->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT &&
            (worker_type == D3D12_COMMAND_LIST_TYPE_DIRECT ||
                (separate_route && worker_type == D3D12_COMMAND_LIST_TYPE_COMPUTE)) &&
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
    using namespace streamline_handoff;
    if (!value || value != epoch.load(std::memory_order_acquire) || value != authorized.load(std::memory_order_acquire) ||
        !Enabled()) return false;
    // A separate presenter's binding holds only while the fence chain is proven for its source queue (Authorize
    // stored it before `authorized`); while it is not, the hold is paused, not ended.
    const void* proof = authorized_proof.load(std::memory_order_acquire);
    return !proof || ChainProven(proof);
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
    // A Present still waiting for its frame id is interested in its first fence call, tracked or not: that call is where
    // it is judged (BeforeFence, RejectQueueMethods), so an id that comes after it claims nothing.
    if (scope && (scope->pending || (scope->input.transaction && !scope->invalid))) return true;
    return fence && streamline_handoff::tracking.load(std::memory_order_acquire) &&
        (fence == streamline_handoff::forward_filter.load(std::memory_order_acquire) ||
         fence == streamline_handoff::back_filter.load(std::memory_order_acquire));
}
void StreamlineHandoff_RecordMarker(uint64_t frame, uint32_t producer, uint32_t controller, int64_t release) noexcept {
    using namespace streamline_handoff;
    if (g_gpu_queue_probe.load(std::memory_order_relaxed)) ProbeRecordMarker(frame, producer, controller, release);
    marker = {};
    // Publish the outcome on every exit: a rejected marker withdraws the
    // previous one, so a Present elsewhere never takes an older frame.
    struct PublishOnExit {
        uint64_t frame; // the key: the marker's frame id, also for a tombstone
        bool skip = false; // a duplicate of the accepted frame keeps it published
        ~PublishOnExit() { if (!skip) marker_sequence = marker_slot.Publish(marker, frame, marker.input.source != 0); }
    } publish{frame};
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
namespace streamline_handoff {
// The admission of a proxy Present that has a source marker, whichever thread recorded it: the stale-release window,
// the input's validity, the transaction and the multiplier, and the checks under the lock. The constructor runs it for
// the marker of its own thread, StreamlineHandoff_RecordInbandFrame for the marker an in-band frame id claimed.
// Records the Present in present_admission, once.
void AdmitSource(StreamlineInputScope& scope, const Marker& source, uint64_t object) noexcept {
    const auto context = NativeGate_GetContext(Now());
    const char* failure = "stale_marker_release";
    if (source.release > 0 && context.now_qpc >= source.release && context.qpc_frequency > 0 &&
        context.now_qpc - source.release <= context.qpc_frequency / 4) {
        failure = InputRejection(source.input);
        if (!failure) {
            scope.input = source.input; scope.input.transaction = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
            scope.proxy = object; scope.config = GpuPresent_GetConfig().word;
            scope.multiplier = g_fg_multiplier.load(std::memory_order_acquire);
            if (scope.multiplier < 1 || scope.multiplier > 3 || !scope.input.transaction) {
                failure = !scope.input.transaction ? "transaction_exhausted" : "unsupported_multiplier"; scope.input = {};
            }
        }
    }
    if (scope.input.transaction && tracking.load(std::memory_order_acquire)) {
        Lock lock("source_scope_lock_busy");
        if (!lock.held) { failure = "qualification_busy"; scope.invalid = true; }
        else if (route.epoch != scope.input.binding) { failure = "binding_epoch_changed"; scope.invalid = true; }
        else if (route.sync_interval != scope.sync_interval) {
            failure = "sync_interval_changed";
            Reject(&scope, failure); // cancels old tickets even if this Present submits no fence
        }
    }
    if (failure) scope.input = {};
    present_admission.Record(failure);
    if (!scope.input.transaction && !scope.invalid && tracking.load(std::memory_order_acquire)) Reject(nullptr, failure);
}
// A pending scope's Present fails without a claim: recorded once, as the constructor records a Present without a
// marker, and the binding is rejected while it is tracked (unless the caller has rejected it for its own cause).
void ResolveFailed(StreamlineInputScope& scope, const char* reason, bool reject = true) noexcept {
    scope.pending = false;
    present_admission.Record(reason);
    if (reject && tracking.load(std::memory_order_acquire)) Reject(nullptr, "ineligible_present");
}
// NV_LATENCY_MARKER_TYPE (nvapi_types.h): the markers whose frame id names the frame being presented.
constexpr uint32_t kPresentStart = 4, kOutOfBandPresentStart = 11;
}
void StreamlineHandoff_RecordInbandFrame(uint32_t marker_type, uint64_t frame_id) noexcept {
    using namespace streamline_handoff;
    auto* scope = StreamlineInputScope::Current();
    if (!scope || !frame_id) return; // no Present of ours on this thread, or no identity yet (the first calls state 0)
    if (scope->claimed_frame) {
        (frame_id == scope->claimed_frame ? inband_match : inband_mismatch).fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!scope->pending || (marker_type != kPresentStart && marker_type != kOutOfBandPresentStart)) return;
    const DWORD error = GetLastError();
    Marker taken;
    if (marker_slot.TakeKey(frame_id, &taken)) {
        scope->pending = false;
        scope->claimed_frame = frame_id;
        inband_claims.fetch_add(1, std::memory_order_relaxed);
        AdmitSource(*scope, taken, scope->proxy);
    } else {
        inband_unknown.fetch_add(1, std::memory_order_relaxed);
        ResolveFailed(*scope, "inband_frame_unknown");
    }
    SetLastError(error);
}
thread_local StreamlineInputScope* StreamlineInputScope::current_ = nullptr;
StreamlineInputScope::StreamlineInputScope(uint64_t object, UINT sync, UINT flags) noexcept : sync_interval(sync), previous_(current_) {
    using namespace streamline_handoff;
    const DWORD error = GetLastError();
    current_ = this; // invalid and nested calls mask, never inherit, the parent
    auto source = marker; marker = {};
    if (g_gpu_queue_probe.load(std::memory_order_relaxed) && !previous_) {
        // The probe's input of this outermost proxy Present: this thread's own marker (claimed, so no
        // other thread takes it), else the newest marker another thread published (taken exactly once).
        // Gears hands its markers over across threads. Fresh and still current, or dropped.
        static const int64_t frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f.QuadPart; }();
        Marker taken;
        bool have = false;
        if (probe_marker.input.source) {
            taken = probe_marker; probe_slot.Claim(probe_sequence); have = true;
        } else if (probe_slot.Take(&taken)) have = true;
        probe_marker = {};
        const auto now = Now();
        if (have && taken.release > 0 && now >= taken.release && now - taken.release <= frequency / 4 &&
            ProbeInputCurrent(taken.input)) {
            probe_input = taken.input; probe_pending = true;
        }
    }
    if (source.input.source) {
        marker_slot.Claim(marker_sequence); // same-thread marker
        if (!previous_) claimed_frame = source.input.source; // a nested scope is rejected below and counts nothing
    }
    last_sync.store(sync, std::memory_order_relaxed); last_flags.store(flags, std::memory_order_relaxed);
    if (previous_) {
        present_admission.Record("nested_present");
        // A parent still waiting for its frame id ends here: its Present is recorded once, with this cause.
        if (previous_->pending) { previous_->pending = false; present_admission.Record("nested_present"); }
        Reject(previous_, "nested_present"); SetLastError(error); return;
    }
    const auto* policy = PolicyRejection();
    // This is the outer Streamline request, not the downstream output Present.
    // DXGI accepts intervals 0..4; GPU ordering comes from the qualified fences,
    // not from forcing this argument to zero. The wrapper forwards it unchanged.
    // Without a marker of this thread's own, and with nothing else against it, the Present waits for the frame id
    // that an in-band PRESENT_START states on this thread (StreamlineHandoff_RecordInbandFrame) and claims that
    // frame's marker by it; no id, no claim. It is judged when the id comes, at its first fence call that reaches the
    // handoff, or when it ends (BeforeFence, RejectQueueMethods, the destructor).
    if (!source.input.source && object && sync <= 4 && !(flags & (DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_WAIT)) && !policy) {
        proxy = object; pending = true;
        SetLastError(error); return;
    }
    if (!object || sync > 4 || (flags & (DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_WAIT)) || !source.input.source || policy) {
        present_admission.Record(!object ? "proxy_vtable_mismatch" : sync > 4 ? "invalid_sync_interval" :
            (flags & (DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_WAIT)) ? "special_present_flags" :
            !source.input.source ? "missing_same_thread_marker" : policy);
        if (tracking.load(std::memory_order_acquire)) Reject(nullptr, "ineligible_present");
        SetLastError(error); return;
    }
    AdmitSource(*this, source, object);
    SetLastError(error);
}
StreamlineInputScope::~StreamlineInputScope() {
    using namespace streamline_handoff;
    const DWORD error = GetLastError();
    // A Present that ends still waiting for its in-band frame id is recorded as failed, once.
    if (pending) { inband_absent.fetch_add(1, std::memory_order_relaxed); ResolveFailed(*this, "inband_frame_absent"); }
    const char* failure = result_ != S_OK ? "present_failed" : invalid ? "input_invalidated" :
        !input.transaction ? "input_missing" : streamline_handoff::InputRejection(input);
    if (!failure && (config != GpuPresent_GetConfig().word || multiplier != g_fg_multiplier.load(std::memory_order_acquire)))
        failure = "input_policy_changed";
    if (owns_cycle) {
        // Never lose a successful outer completion to a competing callback or
        // maintenance lock. The two source calls completed on this owning thread;
        // worker edges remain independently required by CompleteCycle under mutex.
        if (!failure && successful_source_fences != 3) failure = "source_fences_incomplete";
        const bool valid = !failure;
        if (valid) completed_source_transaction.store(input.transaction, std::memory_order_release);
        else Reject(nullptr, failure);
        // Finish only our own successfully begun measurement. An exact-hash
        // private integration may already own this call; never finish its token.
        if (handoff) GpuPresent_FinishFgInput(input.transaction, valid);
        pins.fetch_sub(1, std::memory_order_release);
    } else if (input.transaction && authorized.load(std::memory_order_acquire)) Reject(nullptr, "present_without_input_signal");
    current_ = previous_;
    SetLastError(error);
}

StreamlineFenceCall StreamlineHandoff_BeforeFence(ID3D12CommandQueue* queue, ID3D12Fence* fence,
    uint64_t value, uint64_t site, bool signal) noexcept {
    using namespace streamline_handoff;
    auto* scope = StreamlineInputScope::Current();
    // A Present still waiting for its in-band frame id reaches a fence call without it: late. The call is then handled
    // as any call of a scope that has no input.
    if (scope && scope->pending) { inband_late.fetch_add(1, std::memory_order_relaxed); ResolveFailed(*scope, "inband_frame_late"); }
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
        if (!queue || !fence || !site || !value || value == UINT64_MAX) { Reject(scope, "source_fence_args_invalid"); return {}; }
        if (signal) {
            if (scope->owns_cycle) { Reject(scope, "source_signal_repeated"); return {}; } // ambiguous, not "first Signal wins"
            if (!route.epoch) {
                route.epoch = scope->input.binding; route.config = scope->config; route.multiplier = scope->multiplier;
                route.sync_interval = scope->sync_interval;
                route.source_thread = GetCurrentThreadId(); route.cycle.input = scope->input;
                if (!Retain(reinterpret_cast<IDXGISwapChain*>(scope->proxy), &route.proxy) ||
                    !Retain(queue, &route.source) || !Retain(fence, &route.forward)) { Reject(scope, "source_retain_failed"); return {}; }
                chain.source_queue.store(queue, std::memory_order_release);
                chain.forward_fence.store(fence, std::memory_order_relaxed);
            } else if (const auto* reason = SourceSignalRejection(*scope, queue, fence, value, site)) { Reject(scope, reason); return {}; }
            completed_source_transaction.store(0, std::memory_order_relaxed);
            route.cycle = {}; route.cycle.input = scope->input; route.cycle.forward = value; route.cycle.begin = Now();
            separate.generation.store(scope->input.swapchain, std::memory_order_relaxed);
            separate.native.store(scope->input.native, std::memory_order_relaxed);
            separate.cycle.fetch_add(1, std::memory_order_release); // opens this cycle's evidence
            scope->owns_cycle = true; pins.fetch_add(1, std::memory_order_acq_rel);
            retained_epoch.store(route.epoch, std::memory_order_release);
            tracking.store(true, std::memory_order_release);
            forward_filter.store(route.forward, std::memory_order_release);
            role = 1;
        } else {
            if (const auto* reason = SourceWaitRejection(*scope, queue, fence, value)) { Reject(scope, reason); return {}; }
            if (!route.back) {
                if (!Retain(fence, &route.back)) { Reject(scope, "back_retain_failed"); return {}; }
                chain.back_fence.store(fence, std::memory_order_relaxed);
            }
            else if (route.back != fence) { Reject(scope, "back_fence_changed"); return {}; }
            route.cycle.back = value; role = 2;
            back_filter.store(route.back, std::memory_order_release);
        }
    } else {
        if (route.epoch != epoch.load(std::memory_order_acquire)) return {};
        if (route.cycle.complete) { Reject(nullptr, "fence_call_after_cycle_complete"); return {}; }
        if (!signal && fence == route.forward && value == route.cycle.forward) {
            if (const auto* reason = WorkerWaitRejection(queue, site)) { Reject(nullptr, reason); return {}; }
            if (!route.worker) { if (!Retain(queue, &route.worker)) { Reject(nullptr, "worker_retain_failed"); return {}; } }
            else if (route.worker != queue) { Reject(nullptr, "worker_queue_changed"); return {}; }
            route.cycle.worker_thread = GetCurrentThreadId();
            separate.worker_thread.store(route.cycle.worker_thread, std::memory_order_relaxed);
            separate.worker_queue.store(queue, std::memory_order_release);
            PushChainSeen(ChainRecord::Kind::WorkerSeen, queue, false);
            worker_scope = {route.epoch, route.cycle.input.transaction, queue}; role = 3;
        } else if (signal && fence == route.back && value == route.cycle.back) {
            if (const auto* reason = WorkerSignalRejection(queue, site)) { RejectRoute(reason); return {}; }
            route.cycle.separate = !route.cycle.callbacks;
            role = 4;
        } else { Reject(nullptr, UnexpectedFenceRejection(fence, signal)); return {}; }
    }
    if (route.successes && route.sites[role - 1] != site) { Reject(scope, SiteChangedReasons[role - 1]); return {}; }
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
// gpu_queue_probe: the first queue Signal inside an outermost proxy Present scope that has a probe
// input is the source Signal of that real frame; its queue is the source queue. It validates what
// the marker validated (see ProbeRecordMarker) and that the queue is present; the gate's fence
// qualification (site, forward fence, plugin) is the gate's demand and is not required.
void StreamlineHandoff_ProbeSourceSignal(ID3D12CommandQueue* queue) noexcept {
    auto* scope = StreamlineInputScope::Current();
    if (!scope || !scope->probe_pending || !queue) return;
    scope->probe_pending = false;
    const auto& input = scope->probe_input;
    GpuQueueProbe_OnFgInputBoundary(queue, input.swapchain,
        PacingFrameKey{PacingIdentityDomain::NvApiFrame, input.producer, input.source});
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
        if (evidence.stage == StreamlineBoundaryStage::DestroyQueue) VoidChain();
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
    RecordSeparatePresent(evidence);
    if (!tracking.load(std::memory_order_acquire) || !worker_scope.transaction) return;
    Lock lock("worker_present_lock_busy");
    if (!lock.held || route.epoch != epoch.load(std::memory_order_acquire) || !WorkerMatches()) return;
    if (const auto* reason = WorkerPresentRejection(evidence)) { RejectRoute(reason); return; }
    route.cycle.callbacks = 1; // no real/generated inference
    separate.evident.store(false, std::memory_order_relaxed); // a worker-local Present proves the route
}
void StreamlineHandoff_Service(bool final) noexcept {
    using namespace streamline_handoff;
    if (final) StreamlineHandoff_Invalidate("final_drain");
    else if (const auto* reason = PolicyRejection()) StreamlineHandoff_Invalidate(reason);
    Invalidation invalidation;
    for (unsigned i = 0; i < 32 && invalidations.Pop(&invalidation); ++i) {
        // While frame generation presents from its own thread every binding fails
        // the same way; the state is logged once instead (PublishSeparatePresenter).
        if (separate.published && (invalidation.reason == OtherQueuePresentReason ||
                                   invalidation.reason == PresentQueueNotWorkerReason)) continue;
        LOG_INFO("Streamline GPU: binding invalidated epoch=%llu reason=%s qpc=%lld thread=%lu object=%llu maintenance_deferrals=%llu callback_contentions=%llu record_drops=%llu",
            invalidation.epoch, invalidation.reason, invalidation.qpc, invalidation.thread, invalidation.object,
            maintenance_deferrals.load(), callback_contentions.load(), invalidations.DroppedValues());
    }
    // fg_present_order: its binding stands only while frame generation presents at one multiplier.
    FgPresentOrder_Service(g_fg_presenting.load(std::memory_order_acquire), g_fg_multiplier.load(std::memory_order_acquire));
    // gpu_hold_order: its binding stands while the key is on, frame generation does not present and the rung is the
    // GPU queue wait; the same pass says its ten-second line.
    GpuHoldOrder_Service(g_fg_presenting.load(std::memory_order_acquire));
    PublishSeparatePresenter();
    ReportChain(final);
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
        // The source queue outlives the binding, so the gate keeps it
        // registered and only re-reads the FG swapchain identity. Destroying a
        // queue is reported by ReShade (GpuPresent_OnQueueDestroy). No
        // plugin-owned fence is destroyed or signalled here.
        if (old.source) GpuPresent_OnFgBindingRetired(reinterpret_cast<uint64_t>(old.source));
        const bool released = Release(old.proxy) & Release(old.source) & Release(old.worker) & Release(old.forward) & Release(old.back);
        if (!separate.published)
            LOG_INFO("Streamline GPU: binding retired epoch=%llu last_reason=%s retained_refs_released=%d", old.epoch, reason, released);
        return;
    }
    if (route.cycle.complete && !route.device_checked) {
        const auto checked_epoch = route.epoch;
        const bool separate_route = route.separate;
        pins.fetch_add(1, std::memory_order_acq_rel);
        lock.Unlock();
        const bool valid = CheckDevices(separate_route); // retained interfaces, installer worker only
        Lock publish(nullptr); // retained pins survive a deferred device-check publication
        if (publish.held && route.epoch == checked_epoch && checked_epoch == epoch.load(std::memory_order_acquire)) {
            if (!valid) Reject(nullptr, "device_validation_failed");
            else {
                route.device_checked = true;
                Authorize();
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
        LOG_INFO("Streamline GPU: qualified epoch=%llu cycles=%u source=%p worker=%p native=%llu generation=%llu sync=%u presenter=%s; deadline before input Signal inband=%llu/%llu/%llu/%llu/%llu/%llu",
            accepted, snapshot.successes, snapshot.source, snapshot.worker, snapshot.cycle.input.native, snapshot.cycle.input.swapchain, snapshot.sync_interval,
            snapshot.separate ? "separate" : "worker", Counted(inband_claims), Counted(inband_unknown), Counted(inband_late),
            Counted(inband_absent), Counted(inband_match), Counted(inband_mismatch));
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
    // A Present still waiting for its frame id has reached its first fence call here, which never reaches
    // StreamlineHandoff_BeforeFence: late, as there. The binding is rejected above already.
    auto* scope = StreamlineInputScope::Current();
    if (scope && scope->pending) {
        streamline_handoff::inband_late.fetch_add(1, std::memory_order_relaxed);
        streamline_handoff::ResolveFailed(*scope, "inband_frame_late", false);
    }
}
void StreamlineHandoff_RecordQueueFence(ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value, bool signal) noexcept {
    using namespace streamline_handoff;
    using Kind = ChainRecord::Kind;
    if (!queue || !fence) return;
    // The epoch before the queues: a match after a destroy carries the destroy's epoch.
    const uint64_t epoch = chain.epoch.load(std::memory_order_acquire);
    ChainRecord record;
    if (queue == chain.worker_queue.load(std::memory_order_acquire)) record.kind = signal ? Kind::WorkerSignal : Kind::WorkerWait;
    else if (queue == chain.presenter_queue.load(std::memory_order_acquire)) record.kind = signal ? Kind::PresenterSignal : Kind::PresenterWait;
    else return;
    record.queue = queue;
    record.fence = fence;
    record.value = value;
    record.epoch = epoch;
    PushChain(record);
}
// The native Signal or Wait the hooks forwarded for `queue` (an outermost call) failed: its record, pushed before the
// call, is no evidence. If `queue` is the chain's worker or presenter queue, the proof and its history are void from
// this call on (VoidChain).
void StreamlineHandoff_ChainCallFailed(ID3D12CommandQueue* queue) noexcept {
    using namespace streamline_handoff;
    if (queue && (queue == chain.worker_queue.load(std::memory_order_acquire) ||
                  queue == chain.presenter_queue.load(std::memory_order_acquire))) VoidChain();
}