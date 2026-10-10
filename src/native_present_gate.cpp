#include "native_present_gate.h"
#include "cross_thread_slot.h"
#include <atomic>

namespace {
std::atomic<uint64_t> s_config{0}; // mode in low two bits, generation above
std::atomic<uint32_t> s_qualified{0};
std::atomic<uint32_t> s_no_fg_controller{0};
std::atomic<uint64_t> s_prediction_epoch{1};
thread_local NativePresentGateRoute s_route;
thread_local uint32_t s_previous_qualified = 0;
thread_local uint64_t s_marker_prediction_epoch = 0;

// Cross-thread hand-off (phase4_design.md section 15, R8; cross_thread_slot.h).
// The marker thread publishes its frame; the next native Present scope on a
// thread without its own pending frame takes it exactly once and runs the
// unchanged per-thread route with it, so every route check (swapchain,
// generation, consecutive IDs, 0.5 s age, one call) still applies.
//
// Interleaving alone cannot tell Present(n) from a Present(n - 1) that runs
// one frame behind the markers, so a taken frame is only observed (Shadow)
// and never counts toward qualification until telemetry has verified the
// mapping: the ETW Present joined to its Reflex frame ID (PC latency
// collection) must agree with the taken key for kCrossThreadVerifyFrames
// Presents in a row, and any disagreement revokes it. Without that evidence
// the route stays unqualified (fail closed). Only frames without frame
// generation in a selecting mode are offered, and a thread that presents its
// own markers offers nothing, so DLSS-G's or any other Present thread cannot
// take a same-thread game's frame.
constexpr uint32_t kCrossThreadVerifyFrames = 64;
struct MarkerHandoff {
    NativeGateContext context{};
    PacingPlanKey key{};
    NativeGateMode mode = NativeGateMode::Native;
    uint64_t settings_generation = 0;
    uint64_t prediction_epoch = 0;
    int64_t release_qpc = 0;
};
CrossThreadSlot<MarkerHandoff> s_handoff;
std::atomic<uint64_t> s_handoff_adoptions{0};
std::atomic<uint32_t> s_cross_agreements{0};   // consecutive verified Presents
std::atomic<bool> s_cross_verified{false};
std::atomic<uint64_t> s_cross_disagreements{0};
thread_local uint64_t s_local_marker_sequence = 0; // published by this thread's newest marker
thread_local bool s_presents_own_markers = false;  // this thread completed a scope with its own marker
thread_local bool s_pending_adopted = false;       // the route's pending frame came from another thread
thread_local bool s_last_completed_adopted = false;

// Present thread, at the start of a native Present scope.
void ClaimOrAdoptMarker() noexcept {
    if (s_route.Pending() && !s_pending_adopted) {
        // Same-thread marker: claim it so no other thread takes it.
        s_handoff.Claim(s_local_marker_sequence);
        return;
    }
    MarkerHandoff record{};
    if (!s_handoff.Take(&record)) return;
    // Unverified mappings are observed only (Shadow never selects).
    const NativeGateMode mode = s_cross_verified.load(std::memory_order_acquire)
        ? record.mode : NativeGateMode::Shadow;
    s_route.Marker(record.context, record.key, mode, record.settings_generation);
    if (record.release_qpc != 0) s_route.MarkerReleased(record.release_qpc);
    s_marker_prediction_epoch = record.prediction_epoch;
    s_local_marker_sequence = 0;
    s_pending_adopted = true;
    s_handoff_adoptions.fetch_add(1, std::memory_order_relaxed);
}

// The route runs in every mode. Shadow observes FG and never selects native
// carriage; Marker (the default) and Native select once qualified.
NativeGateMode RouteMode(uint64_t config) noexcept {
    return (config & 3u) == static_cast<uint64_t>(NativeGateMode::Shadow)
        ? NativeGateMode::Shadow : NativeGateMode::Native;
}

void PublishStatus() noexcept {
    // A route built on cross-thread frames counts only once verified.
    const uint32_t qualified = s_last_completed_adopted && !s_cross_verified.load(std::memory_order_acquire)
        ? 0u : s_route.QualifiedFrames();
    if (s_previous_qualified >= NativePresentGateRoute::kQualificationFrames &&
        qualified < NativePresentGateRoute::kQualificationFrames)
        NativeGate_InvalidatePrediction();
    s_previous_qualified = qualified;
    s_qualified.store(qualified, std::memory_order_relaxed);
}
}

bool NativePresentGateRoute::Matches(const NativeGateContext& c) const noexcept {
    return c.supported && c.native_swapchain != 0 && c.swapchain_generation != 0 &&
        c.native_swapchain == context_.native_swapchain &&
        c.swapchain_generation == context_.swapchain_generation &&
        c.producer_epoch == trace_.producer_epoch &&
        c.controller_generation == trace_.controller_generation &&
        c.now_qpc >= trace_.marker_arrival_qpc && c.qpc_frequency > 0 &&
        static_cast<double>(c.now_qpc - trace_.marker_arrival_qpc) /
            static_cast<double>(c.qpc_frequency) <= 0.5;
}

bool NativePresentGateRoute::Marker(const NativeGateContext& c, const PacingPlanKey& key,
    NativeGateMode mode, uint64_t settings_generation) noexcept {
    const bool consecutive = last_complete_.identity_domain == key.identity_domain &&
        last_complete_.producer_epoch == key.producer_epoch &&
        last_complete_.controller_generation == key.controller_generation &&
        key.frame_id > last_complete_.frame_id && key.frame_id - last_complete_.frame_id == 1;
    if (pending_ || depth_ != 0 || !consecutive || settings_generation != settings_generation_ ||
        c.swapchain_generation != last_swapchain_generation_ ||
        c.no_frame_generation != context_.no_frame_generation ||
        c.observe_frame_generation != context_.observe_frame_generation) qualified_ = 0;
    trace_ = {};
    trace_.mode = mode;
    trace_.settings_generation = settings_generation;
    trace_.swapchain_generation = c.swapchain_generation;
    trace_.source_frame_id = key.frame_id;
    trace_.producer_epoch = key.producer_epoch;
    trace_.controller_generation = key.controller_generation;
    trace_.marker_arrival_qpc = c.now_qpc;
    context_ = c;
    settings_generation_ = settings_generation;
    scope_ = completed_ = native_selected_ = false;
    invalid_ = depth_ != 0;
    calls_ = 0;
    pending_ = mode != NativeGateMode::Marker;
    if (mode == NativeGateMode::Marker) status_ = NativeGateStatus::Off;
    else if (!c.supported || key.identity_domain != PacingIdentityDomain::NvApiFrame ||
             key.frame_id == 0 || key.producer_epoch == 0 || key.controller_generation == 0 ||
             key.producer_epoch != c.producer_epoch || key.controller_generation != c.controller_generation ||
             c.swapchain_generation == 0 || c.native_swapchain == 0 || c.now_qpc <= 0)
        status_ = NativeGateStatus::Unsupported;
    else if (!c.no_frame_generation) status_ =
        mode == NativeGateMode::Shadow && c.observe_frame_generation
            ? NativeGateStatus::FrameGenerationShadow : NativeGateStatus::FrameGeneration;
    else if (mode == NativeGateMode::Shadow) status_ = NativeGateStatus::Shadow;
    else if (qualified_ < kQualificationFrames || invalid_) status_ = NativeGateStatus::Warming;
    else { status_ = NativeGateStatus::Native; native_selected_ = true; }
    if (status_ == NativeGateStatus::Unsupported || status_ == NativeGateStatus::FrameGeneration)
        qualified_ = 0;
    trace_.status = status_;
    return native_selected_;
}

void NativePresentGateRoute::MarkerReleased(int64_t qpc) noexcept {
    if (pending_) trace_.marker_release_qpc = qpc;
}
void NativePresentGateRoute::BeginScope(uint64_t native, uint64_t generation) noexcept {
    if (!pending_) return;
    if (scope_ || native != context_.native_swapchain || generation != context_.swapchain_generation) {
        invalid_ = true;
        qualified_ = 0;
        status_ = NativeGateStatus::Unmapped;
        return;
    }
    scope_ = true;
}
bool NativePresentGateRoute::EndScope(uint64_t native, uint64_t generation,
    NativeGateTrace* completed) noexcept {
    if (!pending_) return false;
    bool valid = false;
    if (native != context_.native_swapchain || generation != context_.swapchain_generation ||
        !scope_ || invalid_ || !completed_ || calls_ != 1 || depth_ != 0 ||
        !context_.supported ||
        (!context_.no_frame_generation && !(context_.observe_frame_generation &&
            trace_.mode == NativeGateMode::Shadow && trace_.status == NativeGateStatus::FrameGenerationShadow))) {
        qualified_ = 0;
        if (status_ != NativeGateStatus::FrameGeneration && status_ != NativeGateStatus::Unsupported)
            status_ = NativeGateStatus::Unmapped;
    } else {
        if (qualified_ < kQualificationFrames) ++qualified_;
        last_complete_ = {PacingIdentityDomain::NvApiFrame, trace_.producer_epoch,
            trace_.controller_generation, trace_.source_frame_id};
        last_swapchain_generation_ = generation;
        valid = true;
    }
    if (valid && completed) *completed = trace_;
    pending_ = scope_ = native_selected_ = false;
    return valid;
}

NativeGateTrace NativePresentGateRoute::BeginCall(const NativeGateContext& c,
    uint32_t flags, uint64_t settings_generation) noexcept {
    const bool nested = depth_++ != 0;
    NativeGateTrace result = pending_ ? trace_ : NativeGateTrace{};
    result.native_arrival_qpc = c.now_qpc;
    result.native_release_qpc = c.now_qpc;
    if (!pending_ || !scope_) result.status = NativeGateStatus::Unmapped;
    else if (nested) result.status = NativeGateStatus::Nested;
    else if (invalid_) result.status = NativeGateStatus::Unmapped;
    else if (!Matches(c) || settings_generation != settings_generation_) result.status = NativeGateStatus::Stale;
    else if ((flags & (0x1u | 0x8u)) != 0) result.status = NativeGateStatus::SpecialPresent; // TEST / DO_NOT_WAIT
    else if (++calls_ != 1) result.status = NativeGateStatus::MultipleCalls;
    else if (c.no_frame_generation != context_.no_frame_generation ||
             c.observe_frame_generation != context_.observe_frame_generation) result.status = NativeGateStatus::Stale;
    else if (!c.no_frame_generation) result.status =
        trace_.mode == NativeGateMode::Shadow && c.observe_frame_generation
            ? NativeGateStatus::FrameGenerationShadow : NativeGateStatus::FrameGeneration;
    else if (native_selected_) result.status = NativeGateStatus::Native;
    if (result.status == NativeGateStatus::Unmapped || result.status == NativeGateStatus::Nested ||
        result.status == NativeGateStatus::Stale || result.status == NativeGateStatus::SpecialPresent ||
        result.status == NativeGateStatus::MultipleCalls || result.status == NativeGateStatus::FrameGeneration) {
        qualified_ = 0;
        invalid_ = true;
    }
    native_selected_ = false; // one native hold maximum, including failures/retries
    status_ = result.status;
    return result;
}
void NativePresentGateRoute::EndCall(bool success, bool retried,
    const NativeGateTrace* trace) noexcept {
    if (depth_ != 0) --depth_;
    if (pending_ && scope_ && depth_ == 0) {
        completed_ = success && !retried;
        if (completed_ && trace && !invalid_ && calls_ == 1)
            trace_ = *trace;
        if (!completed_) { invalid_ = true; qualified_ = 0; status_ = NativeGateStatus::Failed; }
    }
}

void NativeGate_SetMode(NativeGateMode mode) noexcept {
    uint64_t before = s_config.load(std::memory_order_acquire);
    while ((before & 3u) != static_cast<uint64_t>(mode)) {
        const uint64_t after = ((before >> 2) + 1) * 4 + static_cast<uint64_t>(mode);
        if (s_config.compare_exchange_weak(before, after, std::memory_order_acq_rel)) {
            NativeGate_InvalidatePrediction();
            s_qualified.store(0);
            s_no_fg_controller.store(0, std::memory_order_release);
            return;
        }
    }
}
NativeGateMode NativeGate_GetMode() noexcept { return static_cast<NativeGateMode>(s_config.load(std::memory_order_acquire) & 3u); }
uint64_t NativeGate_PredictionEpoch() noexcept {
    return s_prediction_epoch.load(std::memory_order_acquire);
}
void NativeGate_InvalidatePrediction() noexcept {
    s_prediction_epoch.fetch_add(1, std::memory_order_acq_rel);
}
bool NativeGate_IsRouteQualified() noexcept {
    return RouteMode(s_config.load(std::memory_order_acquire)) == NativeGateMode::Native &&
        s_qualified.load(std::memory_order_relaxed) >= NativePresentGateRoute::kQualificationFrames;
}
void NativeGate_PublishNoFrameGeneration(bool no_fg, uint32_t controller_generation) noexcept {
    s_no_fg_controller.store(no_fg ? controller_generation : 0, std::memory_order_release);
}
namespace {
// Automatic route mode, as the default pacing method selected before Phase 2:
// while FG presents, Shadow observes the FG input route (the observers,
// Streamline admission and FgInputRoute need it) and never selects native
// carriage; without FG, Native qualifies the route for H2/H3. The resolver
// still decides the rung. A change bumps the settings generation, resets
// qualification and clears the no-FG token, like NativeGate_SetMode.
void SelectRouteMode(bool frame_generation) noexcept {
    uint64_t before = s_config.load(std::memory_order_acquire);
    const auto mode = frame_generation ? NativeGateMode::Shadow : NativeGateMode::Native;
    if ((before & 3u) == static_cast<uint64_t>(mode)) return;
    const uint64_t after = ((before >> 2) + 1) * 4 + static_cast<uint64_t>(mode);
    if (!s_config.compare_exchange_strong(before, after, std::memory_order_acq_rel)) return;
    NativeGate_InvalidatePrediction();
    s_qualified.store(0);
    s_no_fg_controller.store(0, std::memory_order_release);
}
}
void NativeGate_ObserveFrameGeneration(int divisor, uint32_t controller_generation) noexcept {
    // Selection may clear the token; publish the observation afterwards.
    SelectRouteMode(divisor > 1);
    NativeGate_PublishNoFrameGeneration(divisor == 1, controller_generation);
}
bool NativeGate_HasNoFrameGeneration(uint32_t controller_generation) noexcept {
    return controller_generation != 0 && s_no_fg_controller.load(std::memory_order_acquire) == controller_generation;
}
bool NativeGate_IsSettingsCurrent(uint64_t generation) noexcept {
    return (s_config.load(std::memory_order_acquire) >> 2) == generation;
}
uint32_t NativeGate_GetQualifiedFrames() noexcept { return s_qualified.load(std::memory_order_relaxed); }
bool NativeGate_OnMarker(const NativeGateContext& c, const PacingPlanKey& key) noexcept {
    const uint64_t config = s_config.load(std::memory_order_acquire);
    // A previous own marker that no scope on this thread took means this
    // thread no longer presents its own frames.
    if (s_route.Pending() && !s_pending_adopted) s_presents_own_markers = false;
    const bool selected = s_route.Marker(c, key, RouteMode(config), config >> 2);
    s_pending_adopted = false;
    s_marker_prediction_epoch = NativeGate_PredictionEpoch();
    MarkerHandoff record{};
    record.context = c;
    record.key = key;
    record.mode = RouteMode(config);
    record.settings_generation = config >> 2;
    record.prediction_epoch = s_marker_prediction_epoch;
    // Offered to another Present thread only when this thread does not
    // present its own markers, the frame has no frame generation and the
    // mode selects; otherwise the offer withdraws the previous one.
    const bool offer = !s_presents_own_markers && record.mode == NativeGateMode::Native &&
        c.no_frame_generation && c.supported;
    s_local_marker_sequence = s_handoff.Publish(record, offer);
    if (s_presents_own_markers) {
        // Same-thread route: publish its state now, as before the hand-off
        // (a gap or an unsupported context drops qualification at the marker).
        PublishStatus();
    }
    return selected;
}
void NativeGate_OnMarkerReleased(int64_t qpc) noexcept {
    s_route.MarkerReleased(qpc);
    s_handoff.Amend(s_local_marker_sequence, [qpc](MarkerHandoff* record) { record->release_qpc = qpc; });
}
void NativeGate_BeginScope(uint64_t native, uint64_t generation) noexcept {
    ClaimOrAdoptMarker();
    s_route.BeginScope(native, generation);
}
uint64_t NativeGate_CrossThreadAdoptions() noexcept {
    return s_handoff_adoptions.load(std::memory_order_relaxed);
}
void NativeGate_EndScope(uint64_t native, uint64_t generation) noexcept {
    const bool adopted = s_pending_adopted;
    NativeGateTrace completed{};
    const bool valid = s_route.EndScope(native, generation, &completed);
    if (valid) {
        s_last_completed_adopted = adopted;
        if (!adopted) s_presents_own_markers = true;
    }
    s_pending_adopted = false;
    PublishStatus();
}
void NativeGate_WithdrawMarker() noexcept {
    s_local_marker_sequence = s_handoff.Publish(MarkerHandoff{}, false);
}
void NativeGate_ReportCrossThreadEvidence(uint64_t agreements, uint64_t disagreements) noexcept {
    if (disagreements != 0) {
        s_cross_disagreements.fetch_add(disagreements, std::memory_order_relaxed);
        s_cross_agreements.store(0, std::memory_order_relaxed);
        if (s_cross_verified.exchange(false, std::memory_order_acq_rel)) NativeGate_InvalidatePrediction();
        return;
    }
    if (agreements == 0) return;
    const uint32_t total = s_cross_agreements.fetch_add(static_cast<uint32_t>(agreements),
        std::memory_order_relaxed) + static_cast<uint32_t>(agreements);
    if (total >= kCrossThreadVerifyFrames) s_cross_verified.store(true, std::memory_order_release);
}
bool NativeGate_CrossThreadVerified() noexcept { return s_cross_verified.load(std::memory_order_acquire); }
uint64_t NativeGate_CrossThreadDisagreements() noexcept {
    return s_cross_disagreements.load(std::memory_order_relaxed);
}
NativeGateCall::NativeGateCall(const NativeGateContext& c, uint32_t flags) noexcept {
    const uint64_t config = s_config.load(std::memory_order_acquire);
    trace = s_route.BeginCall(c, flags, config >> 2);
    trace.mode = RouteMode(config);
    trace.present_flags = flags;
    PublishStatus();
    trace.prediction_epoch = s_marker_prediction_epoch;
    trace.cross_thread = s_pending_adopted && trace.source_frame_id != 0;
}
NativeGateCall::~NativeGateCall() {
    s_route.EndCall(success_, retried_, &trace);
    PublishStatus();
}
void NativeGateCall::Complete(bool success, bool retried) noexcept {
    success_ = success && trace.status != NativeGateStatus::Cancelled;
    retried_ = retried;
}
