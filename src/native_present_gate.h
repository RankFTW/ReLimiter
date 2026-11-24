#pragma once

#include "pacing_core_types.h"
#include <cstddef>
#include <cstdint>

// Native Present route: PRESENT_START -> native Present
// attribution and qualification for every NvAPI frame, carrying the exact
// plan key to the native Present for H3 (GPU queue wait). Qualification is published as
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
    // PacingPlanStatus of the native-Present executor for this call (the H3
    // decline when the GPU gate did not take it); 0 when none ran.
    uint8_t plan_status = 0;
    uint32_t present_flags = 0; // DXGI Present flags of this call (NativeGateCall)
    // The key came from another thread's PRESENT_START (cross-thread hand-off);
    // telemetry verifies it against the ETW frame ID (NativeGateCall).
    bool cross_thread = false;
};

// The qualification of one native route: the chain of consecutive frames it completed. Every thread that
// presents the route's frames shares it, because a game may present each frame from another thread (Cyberpunk
// 2077 runs a frame's PRESENT_START and Present on whichever job thread is free; record N.197): consecutive
// frames belong to the route, not to a thread. Not synchronized here: the production route
// (native_present_gate.cpp) holds the chain's lock around every call that reaches it.
struct NativeGateChain {
    PacingPlanKey last_complete{};          // the newest frame completed (EndScope valid)
    uint64_t last_swapchain_generation = 0; // that frame's swapchain generation
    PacingPlanKey begun{};                  // the newest frame whose marker a presenting thread took
    uint64_t settings_generation = 0;       // of that marker
    bool no_frame_generation = false;       // of that marker's context
    bool observe_frame_generation = false;  // of that marker's context
    uint32_t qualified = 0;
};

// The route of one Present thread: a frame's marker, its scope and its call. A token must pass through the
// selected ReShade present/finish_present scope and its exact native swapchain. The marker may come from another
// thread through the cross-thread hand-off (native_present_gate.cpp); nesting, retries, missing scopes and a
// Present without a newly published marker reset the chain's qualification. History never proves FG
// real/generated identity, so FG can only be observed in shadow. A route made without a chain keeps its own.
class NativePresentGateRoute {
public:
    static constexpr uint32_t kQualificationFrames = 32;
    NativePresentGateRoute() noexcept = default;
    // constexpr: the production route is a thread_local, which then needs no dynamic initialization on threads
    // the game created before the add-on loaded.
    constexpr explicit NativePresentGateRoute(NativeGateChain* chain) noexcept : chain_(chain) {}
    NativePresentGateRoute(const NativePresentGateRoute&) = delete;
    NativePresentGateRoute& operator=(const NativePresentGateRoute&) = delete;
    // `presents`: this thread presents the marker's frame (its own marker, or one it took from another
    // thread). False for the marker of a thread that hands its frames to another (Gears' render thread): the
    // chain is left alone and the frame is never selected.
    bool Marker(const NativeGateContext& context, const PacingPlanKey& key,
                NativeGateMode mode, uint64_t settings_generation, bool presents = true) noexcept;
    void MarkerReleased(int64_t qpc) noexcept;
    void BeginScope(uint64_t native_swapchain, uint64_t generation) noexcept;
    bool EndScope(uint64_t native_swapchain, uint64_t generation,
                  NativeGateTrace* completed = nullptr) noexcept;
    NativeGateTrace BeginCall(const NativeGateContext& context, uint32_t flags,
                             uint64_t settings_generation) noexcept;
    void EndCall(bool success, bool retried,
                 const NativeGateTrace* trace = nullptr) noexcept;
    uint32_t QualifiedFrames() const noexcept { return chain_->qualified; }
    bool Pending() const noexcept { return pending_; }
    NativeGateStatus Status() const noexcept { return status_; }
    // The chain the next calls reach (the production route swaps in a detached one when its lock is taken).
    void Attach(NativeGateChain* chain) noexcept { chain_ = chain; }

private:
    bool Matches(const NativeGateContext& context) const noexcept;
    NativeGateChain own_chain_{};
    NativeGateChain* chain_ = &own_chain_;
    NativeGateTrace trace_{};
    NativeGateContext context_{};
    uint64_t settings_generation_ = 0;
    uint32_t depth_ = 0;
    uint32_t calls_ = 0;
    bool pending_ = false;
    bool scope_ = false;
    bool invalid_ = false;
    bool completed_ = false;
    bool native_selected_ = false;
    bool carried_ = false; // this thread completed a frame of the route
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
// Native route diagnostics, cumulative: game Presents rejected at the identity guard of the hooked
// Present (present1 false) or Present1 body (self is not the swapchain the swap manager holds), and
// scopes that ended with no counted Present call (calls_ == 0). Relaxed counters, no other effect.
void NativeGate_NotePresentRejected(bool present1) noexcept;
uint64_t NativeGate_PresentsRejected(bool present1) noexcept;
uint64_t NativeGate_ScopesWithoutCalls() noexcept;
// Route diagnostics (log text only; no decision reads them): cumulative relaxed counters of where the
// Present path of the native route is entered and what each gate step saw, so a route that never
// qualifies can be told apart from the log. TypedPresent/TypedPresent1 count entries of the inline
// patch's detours, VtablePresent/VtablePresent1 entries of the private vtable copy's detours (the route's
// normal entry); InnerPassThrough counts the entries of either kind that passed straight to their
// original because the same logical swapchain was already inside a detour on that thread. AliasPassThrough
// counts input entries that forward unchanged because the native body runs at Streamline's base instead.
enum class NativeGateEntry : uint8_t {
    OnPresent, TypedPresent, TypedPresent1, BeginScope, EndScope, GateCall,
    VtablePresent, VtablePresent1, InnerPassThrough, AliasPassThrough, Count };
// One increment, and the thread id when it differs from the last seen.
void NativeGate_NoteEntry(NativeGateEntry entry) noexcept;
enum class NativeGateCallOutcome : uint8_t {
    Mapped, UnmappedNoPending, UnmappedNoScope, UnmappedInvalid, Nested, Stale, SpecialPresent, MultipleCalls, Count };
enum class NativeGateScopeEvent : uint8_t {
    NoPending, NativeMismatch, GenerationMismatch, AlreadyOpen, ZeroGeneration,
    EndNativeMismatch, EndGenerationMismatch, Count };
struct NativeGateDiagnostics {
    uint64_t entries[static_cast<size_t>(NativeGateEntry::Count)];
    uint32_t entry_thread[static_cast<size_t>(NativeGateEntry::Count)];
    uint64_t call[static_cast<size_t>(NativeGateCallOutcome::Count)]; // BeginCall outcomes
    uint64_t flags_zero, flags_test, flags_do_not_wait, flags_other;  // Present flags at the gate
    uint64_t scope[static_cast<size_t>(NativeGateScopeEvent::Count)];
    uint64_t present_rejected, present1_rejected, scopes_without_call;
    // The scope (native, generation) against the marker-time context at the last change of that pair.
    uint64_t mismatch_sequence, mismatch_scope_native, mismatch_scope_generation,
        mismatch_marker_native, mismatch_marker_generation;
};
NativeGateDiagnostics NativeGate_Diagnostics() noexcept;
// The counters as one line of text (no newline); returns the length written (truncated to size).
size_t NativeGate_DiagnosticsText(char* out, size_t size) noexcept;
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
// Native Present executor: a plan on the GPU queue wait rung that the GPU gate did not
// handle is declined here (decided without a hold, plan_status stamped in call->trace);
// no other plan is touched. The H2 NativeHold hold no longer exists (the rung stays an
// enumerator of the telemetry).
void NativeGate_Execute(NativeGateCall* call, uint64_t native_swapchain) noexcept;
// Publishes PacingCapability::NativeRoute for controller_generation when the
// route qualification or the generation changed since the last publication.
void NativeGate_PublishRouteCapability(uint32_t controller_generation) noexcept;