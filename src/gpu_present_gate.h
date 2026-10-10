#pragma once
// H3 output executor (phase2_timing_core_design.md 2.6): GPU time-fence waits
// on the present queue (GpuQueueWait) or before the frame-generation input
// fence (FgInputWait), to the output target the cadence grid resolves for the
// frame's exact plan. Registry, tickets, fences, the deadline worker and its
// drain live in gpu_present_gate_d3d12.cpp; readiness timestamps come from
// gpu_readiness_d3d12.cpp.
#include "gpu_present_types.h"
#include <algorithm> // kept for includers that relied on it transitively
#include <array>
#include <cstdint>

// Exported to CSV as raw values: retired outcomes stay reserved, never reused.
// Warming, Shadow, Planned, AdmissionWait, RecoveryLimit and TransientMiss
// belonged to the retired deadline models and are never produced.
enum class GpuPresentStatus : uint8_t {
    Off, Unsupported, Warming, Shadow, Planned, Queued, Late, Budget,
    Unavailable, Released, Drained, Failed, Rebased,
    NoQueue, QueuePending, QueueFailed, RegistryFull, RegistrationBusy,
    QueueBusy, Capacity, WorkerStale, AdmissionClosed, IdentityChanged,
    AdmissionWait, AdmissionCancelled, RecoveryLimit, TransientMiss,
    PlanMissing, PlanDuplicate, PlanStale
};
// Budget: the target lies beyond the H3 horizon (Undeliverable).
struct GpuPresentTrace {
    ReadyPresentMode mode=ReadyPresentMode::Off;
    GpuPresentStatus status=GpuPresentStatus::Off;
    uint64_t settings=0,epoch=0,ticket=0,queue=0,swapchain=0,present_sequence=0;
    int64_t target_qpc=0,interval_qpc=0,enqueue_before_qpc=0,enqueue_after_qpc=0;
    int64_t signal_before_qpc=0,signal_after_qpc=0,ready_qpc=0;
    int32_t wait_result=INT32_MIN,signal_result=INT32_MIN,retire_result=INT32_MIN;
    // nominal_target_qpc: the plan's T_n at planning. phase_adjust_qpc: the
    // resolved target minus that (grid epochs and the output spacing floor).
    int64_t nominal_target_qpc=0,phase_adjust_qpc=0;
    int64_t worker_wake_qpc=0,worker_service_qpc=0;
    int64_t worker_arm_before_qpc=0,worker_arm_after_qpc=0,worker_requested_wake_qpc=0;
    uint32_t worker_wait_result=UINT32_MAX,worker_flags=0,worker_policy=4;
    int64_t begin_qpc=0; // executor entry: native Present or FG input begin
    // Exact plan of this output.
    uint64_t grid_index=0;
    uint8_t plan_status=0; // PacingPlanStatus
};
// H3 horizon: a target more than clamp(3 × interval, 2 ms, 100 ms) ahead
// is Undeliverable. A planned frame's target is at most its lead (at most
// 3 I, lead_controller.h) after its source release, plus the spacing floor,
// and the executor runs at the Present call or FG input, after the source,
// so the horizon only rejects implausible targets (phase4_design.md R2) up
// to a real-frame interval of 33 ms. The 100 ms ceiling bounds one GPU wait
// (the worker's 50 ms poll keeps its heartbeat inside it); above 33 ms
// (for example 4× MFG at 60 presented FPS) a lead beyond 100 ms is
// Undeliverable and the frame is presented unheld.
constexpr double GPU_PRESENT_HORIZON_INTERVALS = 3.0; // R15: the lead's own limit
int64_t GpuPresent_HorizonTicks(double interval_us,int64_t frequency) noexcept;
// Settings word: probes bit and generation only. mode is derived: Active
// while an H3 rung is resolved or H3 obligations exist, otherwise Off; slack
// and max_hold report the retired defaults.
ReadyPresentConfig GpuPresent_GetConfig() noexcept;
// True while the resolved pipeline rung is GpuQueueWait/FgInputWait, H3
// obligations (tickets, FG prefix readbacks) exist, or the resolver waits for
// the FG input route to qualify (FgUnqualified, or override gpu under FG).
bool GpuPresent_RequiresReadiness() noexcept;
// IsSupported/IsFgSupported report the GpuQueue/FgInputRoute capabilities for
// the controller generation; the gate publishes them (GpuQueue: the selected
// swapchain's present queue is Ready; FgInputRoute: an FG input reached an
// FG-ready queue within 250 ms).
bool GpuPresent_IsSupported(uint32_t controller) noexcept;
bool GpuPresent_IsFgSupported(uint32_t controller) noexcept;
// Probes come from PacingPipeline_GetConfig().gpu_probes.
bool GpuPresent_ProbesFromSettings(uint64_t word) noexcept;
const char* GpuPresent_StatusText(GpuPresentStatus status) noexcept;
class GpuReadinessCall;
class NativeGateCall;
struct PresentTimelineSpan;
enum class GpuPresentExecution : uint8_t {
    Fallback, // No ownership: the plan belongs to another rung; the native CPU gate may run.
    Handled, // The H3 executor decided this output without inserting a wait.
    WaitCommitted // A signal obligation exists, even if native submission failed.
};
bool GpuPresent_Start() noexcept;
bool GpuPresent_Stop() noexcept; // closes admission, discharges time gates, THEN joins
bool GpuPresent_ReleaseAfterRundown() noexcept;
// Called with a D3D12 native queue identity; never releases resources in the callback.
void GpuPresent_OnQueueDestroy(uint64_t native_queue) noexcept;
GpuPresentExecution GpuPresent_Execute(NativeGateCall*,GpuReadinessCall*,PresentTimelineSpan*) noexcept;
bool GpuPresent_ReadStatus(GpuPresentTrace*) noexcept;
struct NativeGateContext;
// Admission diagnostics, not worker completion or enforcement authority. Recent
// counts cover up to 128 observed inputs in this exact route/settings lifetime.
struct GpuPresentStatusSnapshot {
    GpuPresentTrace trace{};
    ReadyPresentIdentity identity{};
    uint64_t native_swapchain=0,source_frame=0,binding=0;
    int64_t publication_qpc=0;
    bool frame_generation=false;
    uint32_t inputs=0,queued=0;
};
bool GpuPresent_ReadStatusSnapshot(GpuPresentStatusSnapshot*) noexcept;
bool GpuPresent_StatusMatches(const GpuPresentStatusSnapshot&,const NativeGateContext&,
    uint64_t observer,uint64_t prediction) noexcept;

#ifdef _WIN64
// Existing diagnostic worker only; caller serializes drains. No capture required.
void GpuPresent_DrainDiagnostics() noexcept;
// Present thread of the selected swapchain (ReShade present event): registers
// its D3D12 queue so the GpuQueue capability can be published before any H3
// plan exists. Nonblocking; the worker creates the fences.
void GpuPresent_ObservePresentQueue(uint64_t native_queue,uint64_t native_swapchain) noexcept;
// R14: a Streamline FG binding on native_queue retired. The queue stays
// registered; the worker re-reads the FG swapchain identity when it is idle.
// Nonblocking (shared registration lock only).
void GpuPresent_OnFgBindingRetired(uint64_t native_queue) noexcept;
// Single-consumption input from a reviewed private callsite or a retained,
// dynamically qualified standard-queue handoff. Nonzero binding is revocable.
struct GpuPresentFgInput {
    uint64_t transaction=0,source=0,native=0,swapchain=0,observer=0,prediction=0;
    uint32_t producer=0,controller=0;
    uint64_t binding=0;
};
bool GpuPresent_BeginFgInput(uint64_t queue,const GpuPresentFgInput& input) noexcept;
// FG input route diagnostics (R12b, cumulative): why FG inputs did not mark
// the route seen. Reported with the Streamline admission counters.
struct GpuPresentFgRouteDiagnostics {
    uint64_t pending=0;        // a measurement of this thread was still pending
    uint64_t invalid_input=0;  // no queue, marker source or transaction
    uint64_t stale_binding=0;  // the Streamline binding was no longer current
    uint64_t queue_missing=0;  // the input queue is not registered
    uint64_t queue_unready=0;  // registered but not Ready, or retired
    uint64_t fg_unready=0;     // the FG prefix queue is not ready (requested)
    uint64_t identity=0;       // FG queue bound to another swapchain or generation
    uint64_t seen=0;           // inputs that marked the route seen
};
GpuPresentFgRouteDiagnostics GpuPresent_FgRouteDiagnostics() noexcept;
void GpuPresent_FinishFgInput(uint64_t transaction,bool valid) noexcept;
#endif
