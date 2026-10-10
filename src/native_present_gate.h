#pragma once

#include "pacing_core_types.h"
#include <cstdint>

// Native Present route: PRESENT_START -> native Present
// attribution and qualification for every NvAPI frame, carrying the exact
// plan key to the native Present for H2 (NativeHold, developer override) and
// H3 (GPU queue wait). Qualification is published as
// PacingCapability::NativeRoute by NativeGate_PublishRouteCapability.
//
// Route mode: Marker (the initial value) and Native run the route with native
// selection; Shadow runs it for FG observation only and never selects. The
// scheduler's FG observation (NativeGate_ObserveFrameGeneration) selects
// Shadow while FG presents and Native otherwise; the FG input observers and
// Streamline admission run only in Shadow. The hold itself is chosen by the
// plan's rung, not by the mode.
enum class NativeGateMode : uint8_t { Marker, Shadow, Native };
enum class NativeGateStatus : uint8_t {
    Off, Warming, Shadow, Native, FrameGeneration, Unsupported, NoDeadline,
    Unmapped, Stale, MultipleCalls, Nested, SpecialPresent, Failed, Cancelled,
    FrameGenerationShadow
};

// No native handles are dereferenced by routing. Each call site supplies an
// immutable snapshot; CSV capture is not involved in runtime admission.
struct NativeGateContext {
    uint64_t native_swapchain = 0;
    uint64_t swapchain_generation = 0;
    uint32_t producer_epoch = 0;
    uint32_t controller_generation = 0;
    int64_t now_qpc = 0;
    int64_t qpc_frequency = 0;
    bool supported = false;
    bool no_frame_generation = false;
    // Diagnostic mapping only. Never grants native/GPU enforcement authority.
    bool observe_frame_generation = false;
    // Diagnostic snapshot: mode data, static mode, active, presenting.
    uint32_t fg_diagnostic_flags = 0;
};

struct NativeGateTrace {
    uint64_t source_frame_id = 0;
    uint32_t producer_epoch = 0;
    uint32_t controller_generation = 0;
    int64_t marker_arrival_qpc = 0;
    int64_t marker_release_qpc = 0;
    int64_t native_arrival_qpc = 0;
    int64_t target_qpc = 0;
    int64_t native_release_qpc = 0;
    NativeGateMode mode = NativeGateMode::Marker;
    NativeGateStatus status = NativeGateStatus::Off;
    uint64_t settings_generation = 0;
    uint64_t swapchain_generation = 0;
    uint64_t prediction_epoch = 0;
    // PacingPlanStatus of the native-Present executor for this call (H2, or
    // the H3 decline when the GPU gate did not take it); 0 when none ran.
    uint8_t plan_status = 0;
    uint32_t present_flags = 0; // DXGI Present flags of this call (NativeGateCall)
    // The key came from another thread's PRESENT_START (cross-thread hand-off);
    // telemetry verifies it against the ETW frame ID (NativeGateCall).
    bool cross_thread = false;
};

// One instance per Present thread. A token must pass through the selected
// ReShade present/finish_present scope and its exact native swapchain. The
// marker may come from another thread through the cross-thread hand-off
// (native_present_gate.cpp); nesting, retries, missing scopes and a
// Present without a newly published marker reset qualification. History
// never proves FG real/generated identity, so FG can only be observed in shadow.
class NativePresentGateRoute {
public:
    static constexpr uint32_t kQualificationFrames = 32;
    bool Marker(const NativeGateContext& context, const PacingPlanKey& key,
                NativeGateMode mode, uint64_t settings_generation) noexcept;
    void MarkerReleased(int64_t qpc) noexcept;
    void BeginScope(uint64_t native_swapchain, uint64_t generation) noexcept;
    bool EndScope(uint64_t native_swapchain, uint64_t generation,
                  NativeGateTrace* completed = nullptr) noexcept;
    NativeGateTrace BeginCall(const NativeGateContext& context, uint32_t flags,
                             uint64_t settings_generation) noexcept;
    void EndCall(bool success, bool retried,
                 const NativeGateTrace* trace = nullptr) noexcept;
    uint32_t QualifiedFrames() const noexcept { return qualified_; }
    bool Pending() const noexcept { return pending_; }
    NativeGateStatus Status() const noexcept { return status_; }

private:
    bool Matches(const NativeGateContext& context) const noexcept;
    NativeGateTrace trace_{};
    NativeGateContext context_{};
    PacingPlanKey last_complete_{};
    uint64_t settings_generation_ = 0;
    uint64_t last_swapchain_generation_ = 0;
    uint32_t qualified_ = 0;
    uint32_t depth_ = 0;
    uint32_t calls_ = 0;
    bool pending_ = false;
    bool scope_ = false;
    bool invalid_ = false;
    bool completed_ = false;
    bool native_selected_ = false;
    NativeGateStatus status_ = NativeGateStatus::Off;
};

void NativeGate_SetMode(NativeGateMode mode) noexcept;
void NativeGate_PublishNoFrameGeneration(bool no_fg, uint32_t controller_generation) noexcept;
void NativeGate_ObserveFrameGeneration(int divisor, uint32_t controller_generation) noexcept;
bool NativeGate_HasNoFrameGeneration(uint32_t controller_generation) noexcept;
bool NativeGate_IsSettingsCurrent(uint64_t generation) noexcept;
NativeGateMode NativeGate_GetMode() noexcept;
uint32_t NativeGate_GetQualifiedFrames() noexcept;
// PRESENT_START of every NvAPI frame: starts the route for the exact key.
// True when the route selected native carriage (qualified, no FG).
bool NativeGate_OnMarker(const NativeGateContext&, const PacingPlanKey&) noexcept;
void NativeGate_OnMarkerReleased(int64_t qpc) noexcept;
void NativeGate_BeginScope(uint64_t native_swapchain, uint64_t generation) noexcept;
void NativeGate_EndScope(uint64_t native_swapchain, uint64_t generation) noexcept;
// Native Present scopes that took their PRESENT_START from another thread
// (cross-thread hand-off); cumulative, for the runtime log.
uint64_t NativeGate_CrossThreadAdoptions() noexcept;
// A PRESENT_START that does not reach NativeGate_OnMarker (stale token):
// withdraws the published frame so no Present takes an older one.
void NativeGate_WithdrawMarker() noexcept;
// Telemetry evidence for cross-thread keys: Presents whose ETW frame ID
// (PC latency collection) agreed or disagreed with the taken key. 64
// agreements in a row verify the mapping; any disagreement revokes it.
// Unverified taken frames never count toward qualification.
void NativeGate_ReportCrossThreadEvidence(uint64_t agreements, uint64_t disagreements) noexcept;
bool NativeGate_CrossThreadVerified() noexcept;
uint64_t NativeGate_CrossThreadDisagreements() noexcept;
uint64_t NativeGate_PredictionEpoch() noexcept;
void NativeGate_InvalidatePrediction() noexcept;
// True when the route mode may select native carriage (not Shadow) and the
// last published route status has kQualificationFrames qualified frames.
bool NativeGate_IsRouteQualified() noexcept;

// Holds nesting state only, never a mutex/scheduler owner. Retry attempts use
// this same scope and never perform a second hold.
class NativeGateCall {
public:
    NativeGateCall(const NativeGateContext&, uint32_t flags) noexcept;
    ~NativeGateCall();
    void Complete(bool success, bool retried) noexcept;
    NativeGateTrace trace;
    NativeGateCall(const NativeGateCall&) = delete;
    NativeGateCall& operator=(const NativeGateCall&) = delete;
private:
    bool success_ = false;
    bool retried_ = false;
};

// Windows runtime adapter, separate from the directly tested routing policy.
NativeGateContext NativeGate_GetContext(int64_t now_qpc) noexcept;
// H2 executor (developer rung override `native`): holds a NativeHold plan's
// output at the native Present (presentation_gate.h contract), after pushing
// the NativeArrival stage event. Other plans are left untouched. Stamps
// target_qpc, native_release_qpc and plan_status in call->trace.
void NativeGate_Execute(NativeGateCall* call, uint64_t native_swapchain) noexcept;
// Publishes PacingCapability::NativeRoute for controller_generation when the
// route qualification or the generation changed since the last publication.
void NativeGate_PublishRouteCapability(uint32_t controller_generation) noexcept;