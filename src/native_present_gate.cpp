#include "native_present_gate.h"
#include "cross_thread_slot.h"
#include <atomic>
#include <cstdio>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace {
std::atomic<uint64_t> s_config{0}; // mode in low two bits, generation above
std::atomic<uint32_t> s_qualified{0};
std::atomic<uint32_t> s_no_fg_controller{0};
std::atomic<uint64_t> s_prediction_epoch{1};
// The route's qualification (NativeGateChain), shared by every thread that presents its frames. A call that
// reaches it takes `s_chain_lock` with a bounded try-lock, as the hand-off slots do, so contention never blocks a
// Present hook. A call that does not get it runs on the thread's detached chain, which selects nothing, and marks
// the shared chain lost: its next holder starts it again.
NativeGateChain s_chain;
std::atomic_flag s_chain_lock = ATOMIC_FLAG_INIT;
std::atomic<bool> s_chain_lost{false};
uint32_t s_previous_qualified = 0;     // under s_chain_lock: the qualified count published last
bool s_last_completed_adopted = false; // under s_chain_lock: the newest completed frame came from another thread
thread_local NativeGateChain t_detached_chain;
thread_local NativePresentGateRoute s_route{&s_chain};
thread_local uint64_t s_marker_prediction_epoch = 0;
class ChainLease {
public:
    ChainLease() noexcept {
        for (int attempt = 0; attempt < kAttempts && !held_; ++attempt)
            held_ = !s_chain_lock.test_and_set(std::memory_order_acquire);
        if (held_) {
            if (s_chain_lost.exchange(false, std::memory_order_acq_rel)) s_chain.qualified = 0;
        } else {
            s_chain_lost.store(true, std::memory_order_release);
            t_detached_chain = {};
            s_route.Attach(&t_detached_chain);
        }
    }
    ~ChainLease() {
        if (held_) s_chain_lock.clear(std::memory_order_release);
        else s_route.Attach(&s_chain);
    }
    ChainLease(const ChainLease&) = delete;
    ChainLease& operator=(const ChainLease&) = delete;
    bool Held() const noexcept { return held_; }
private:
    static constexpr int kAttempts = 64;
    bool held_ = false;
};

// Cross-thread hand-off (cross_thread_slot.h).
// The marker thread publishes its frame; the next native Present scope on a
// thread without its own pending frame takes it exactly once and runs its
// own route with it against the shared chain, so every route check (swapchain,
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
// Native route diagnostics (cumulative, relaxed): Presents rejected at the identity guard of each
// hooked Present body, and scopes that ended with no counted Present call.
std::atomic<uint64_t> s_present_rejected{0};
std::atomic<uint64_t> s_present1_rejected{0};
std::atomic<uint64_t> s_scopes_without_calls{0};
// Route diagnostics (log text only; relaxed, no decision reads them).
struct RouteDiag {
    std::atomic<uint64_t> entries[static_cast<size_t>(NativeGateEntry::Count)]{};
    std::atomic<uint32_t> entry_thread[static_cast<size_t>(NativeGateEntry::Count)]{};
    std::atomic<uint64_t> call[static_cast<size_t>(NativeGateCallOutcome::Count)]{};
    std::atomic<uint64_t> flags[4]{}; // zero, TEST, DO_NOT_WAIT, other
    std::atomic<uint64_t> scope[static_cast<size_t>(NativeGateScopeEvent::Count)]{};
    std::atomic<uint64_t> mismatch[4]{}; // scope native/generation, marker-time native/generation
    std::atomic<uint64_t> mismatch_sequence{0};
};
RouteDiag s_diag;
void NoteScopeEvent(NativeGateScopeEvent event) noexcept {
    s_diag.scope[static_cast<size_t>(event)].fetch_add(1, std::memory_order_relaxed);
}
// The sequence changes only when the (scope, marker-time) pair changes, so a log keyed on it prints
// the first mismatch and then each change, not every frame.
void NoteScopeMismatch(uint64_t scope_native, uint64_t scope_generation, uint64_t marker_native,
    uint64_t marker_generation) noexcept {
    const uint64_t values[4] = {scope_native, scope_generation, marker_native, marker_generation};
    bool same = s_diag.mismatch_sequence.load(std::memory_order_relaxed) != 0;
    for (size_t i = 0; i < 4 && same; ++i) same = s_diag.mismatch[i].load(std::memory_order_relaxed) == values[i];
    if (same) return;
    for (size_t i = 0; i < 4; ++i) s_diag.mismatch[i].store(values[i], std::memory_order_relaxed);
    s_diag.mismatch_sequence.fetch_add(1, std::memory_order_release);
}
void NoteCallOutcome(NativeGateStatus status, bool pending, bool scope, uint32_t flags) noexcept {
    NativeGateCallOutcome outcome = NativeGateCallOutcome::Mapped;
    switch (status) {
    case NativeGateStatus::Unmapped:
        outcome = !pending ? NativeGateCallOutcome::UnmappedNoPending
                : !scope ? NativeGateCallOutcome::UnmappedNoScope : NativeGateCallOutcome::UnmappedInvalid;
        break;
    case NativeGateStatus::Nested: outcome = NativeGateCallOutcome::Nested; break;
    case NativeGateStatus::Stale: outcome = NativeGateCallOutcome::Stale; break;
    case NativeGateStatus::SpecialPresent: outcome = NativeGateCallOutcome::SpecialPresent; break;
    case NativeGateStatus::MultipleCalls: outcome = NativeGateCallOutcome::MultipleCalls; break;
    default: break;
    }
    s_diag.call[static_cast<size_t>(outcome)].fetch_add(1, std::memory_order_relaxed);
    s_diag.flags[flags == 0 ? 0 : (flags & 0x1u) ? 1 : (flags & 0x8u) ? 2 : 3].fetch_add(1, std::memory_order_relaxed);
}
thread_local uint64_t s_local_marker_sequence = 0; // published by this thread's newest marker
// Whether this thread presents the frames of its own markers: Unknown until it completes a scope with its own
// marker (Presents) or a marker of its own goes unpresented by it (HandsOff: Gears' render thread hands every
// frame to its Present thread).
enum class MarkerThread : uint8_t { Unknown, Presents, HandsOff };
thread_local MarkerThread s_marker_thread = MarkerThread::Unknown;
thread_local bool s_pending_adopted = false;       // the route's pending frame came from another thread

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

// Under the lease: the chain's qualified count, which every Present thread of the route publishes alike. A route
// built on cross-thread frames counts only once verified.
void PublishStatus(const ChainLease& lease) noexcept {
    if (!lease.Held()) return; // the chain is lost; its next holder starts it again
    const uint32_t qualified = s_last_completed_adopted && !s_cross_verified.load(std::memory_order_acquire)
        ? 0u : s_chain.qualified;
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
    NativeGateMode mode, uint64_t settings_generation, bool presents) noexcept {
    if (presents) {
        NativeGateChain& chain = *chain_;
        const PacingPlanKey& last = chain.last_complete;
        const bool same_route = last.identity_domain == key.identity_domain &&
            last.producer_epoch == key.producer_epoch && last.controller_generation == key.controller_generation;
        // The frame right after the newest completed one, or the one after that while the frame between is still
        // being presented on another thread (its marker was taken, its scope has not ended).
        const bool consecutive = same_route && key.frame_id > last.frame_id &&
            (key.frame_id - last.frame_id == 1 || (key.frame_id - last.frame_id == 2 &&
                chain.begun == PacingPlanKey{key.identity_domain, key.producer_epoch, key.controller_generation,
                    key.frame_id - 1}));
        // This thread's previous marker that no Present took: a frame without a Present, unless the route has
        // completed that frame's successor since (a thread that presents a frame only now and then).
        const bool orphan = pending_ && !(last.identity_domain == PacingIdentityDomain::NvApiFrame &&
            last.producer_epoch == trace_.producer_epoch && last.controller_generation == trace_.controller_generation &&
            trace_.source_frame_id <= last.frame_id);
        if (orphan || depth_ != 0 || !consecutive || settings_generation != chain.settings_generation ||
            c.swapchain_generation != chain.last_swapchain_generation ||
            c.no_frame_generation != chain.no_frame_generation ||
            c.observe_frame_generation != chain.observe_frame_generation) chain.qualified = 0;
        chain.begun = key;
        chain.settings_generation = settings_generation;
        chain.no_frame_generation = c.no_frame_generation;
        chain.observe_frame_generation = c.observe_frame_generation;
    }
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
    else if (!presents || chain_->qualified < kQualificationFrames || invalid_) status_ = NativeGateStatus::Warming;
    else { status_ = NativeGateStatus::Native; native_selected_ = true; }
    if (presents && (status_ == NativeGateStatus::Unsupported || status_ == NativeGateStatus::FrameGeneration))
        chain_->qualified = 0;
    trace_.status = status_;
    return native_selected_;
}

void NativePresentGateRoute::MarkerReleased(int64_t qpc) noexcept {
    if (pending_) trace_.marker_release_qpc = qpc;
}
void NativePresentGateRoute::BeginScope(uint64_t native, uint64_t generation) noexcept {
    if (generation == 0) NoteScopeEvent(NativeGateScopeEvent::ZeroGeneration);
    if (!pending_) { NoteScopeEvent(NativeGateScopeEvent::NoPending); return; }
    const bool native_mismatch = native != context_.native_swapchain;
    const bool generation_mismatch = generation != context_.swapchain_generation;
    if (scope_ || native_mismatch || generation_mismatch) {
        if (scope_) NoteScopeEvent(NativeGateScopeEvent::AlreadyOpen);
        if (native_mismatch) NoteScopeEvent(NativeGateScopeEvent::NativeMismatch);
        if (generation_mismatch) NoteScopeEvent(NativeGateScopeEvent::GenerationMismatch);
        if (native_mismatch || generation_mismatch)
            NoteScopeMismatch(native, generation, context_.native_swapchain, context_.swapchain_generation);
        invalid_ = true;
        chain_->qualified = 0;
        status_ = NativeGateStatus::Unmapped;
        return;
    }
    scope_ = true;
}
bool NativePresentGateRoute::EndScope(uint64_t native, uint64_t generation,
    NativeGateTrace* completed) noexcept {
    if (!pending_) return false;
    if (native != context_.native_swapchain) NoteScopeEvent(NativeGateScopeEvent::EndNativeMismatch);
    if (generation != context_.swapchain_generation) NoteScopeEvent(NativeGateScopeEvent::EndGenerationMismatch);
    if (calls_ == 0) s_scopes_without_calls.fetch_add(1, std::memory_order_relaxed);
    bool valid = false;
    if (native != context_.native_swapchain || generation != context_.swapchain_generation ||
        !scope_ || invalid_ || !completed_ || calls_ != 1 || depth_ != 0 ||
        !context_.supported ||
        (!context_.no_frame_generation && !(context_.observe_frame_generation &&
            trace_.mode == NativeGateMode::Shadow && trace_.status == NativeGateStatus::FrameGenerationShadow))) {
        chain_->qualified = 0;
        if (status_ != NativeGateStatus::FrameGeneration && status_ != NativeGateStatus::Unsupported)
            status_ = NativeGateStatus::Unmapped;
    } else {
        // The chain grows only by the frame right after its newest completed one. A frame whose marker the chain did
        // not see (the first after its thread handed its markers on) is judged here as its marker would have been,
        // and brings that marker's settings to the chain, so the next marker is judged against its own predecessor.
        NativeGateChain& chain = *chain_;
        const PacingPlanKey frame{PacingIdentityDomain::NvApiFrame, trace_.producer_epoch,
            trace_.controller_generation, trace_.source_frame_id};
        const PacingPlanKey& last = chain.last_complete;
        const PacingPlanKey& begun = chain.begun;
        const bool seen = begun.identity_domain == frame.identity_domain && begun.producer_epoch == frame.producer_epoch &&
            begun.controller_generation == frame.controller_generation && frame.frame_id <= begun.frame_id;
        const bool next = last.identity_domain == frame.identity_domain && last.producer_epoch == frame.producer_epoch &&
            last.controller_generation == frame.controller_generation && frame.frame_id > last.frame_id &&
            frame.frame_id - last.frame_id == 1 &&
            (seen || (settings_generation_ == chain.settings_generation &&
                context_.swapchain_generation == chain.last_swapchain_generation &&
                context_.no_frame_generation == chain.no_frame_generation &&
                context_.observe_frame_generation == chain.observe_frame_generation));
        if (!next) chain.qualified = 1;
        else if (chain.qualified < kQualificationFrames) ++chain.qualified;
        chain.last_complete = frame;
        chain.last_swapchain_generation = generation;
        if (!seen) {
            chain.begun = frame;
            chain.settings_generation = settings_generation_;
            chain.no_frame_generation = context_.no_frame_generation;
            chain.observe_frame_generation = context_.observe_frame_generation;
        }
        carried_ = true;
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
        // A Present on a thread that has no frame of the route and never carried one (a loading screen's
        // thread, DLSS-G's presenting thread) is not the route's: it is unmapped without breaking the chain.
        if (pending_ || carried_) chain_->qualified = 0;
        invalid_ = true;
    }
    native_selected_ = false; // one native hold maximum, including failures/retries
    status_ = result.status;
    NoteCallOutcome(result.status, pending_, scope_, flags);
    return result;
}
void NativePresentGateRoute::EndCall(bool success, bool retried,
    const NativeGateTrace* trace) noexcept {
    if (depth_ != 0) --depth_;
    if (pending_ && scope_ && depth_ == 0) {
        completed_ = success && !retried;
        if (completed_ && trace && !invalid_ && calls_ == 1)
            trace_ = *trace;
        if (!completed_) { invalid_ = true; chain_->qualified = 0; status_ = NativeGateStatus::Failed; }
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
// Automatic route mode, as the legacy default pacing method selected:
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
    const MarkerThread before = s_marker_thread;
    if (s_route.Pending() && !s_pending_adopted) s_marker_thread = MarkerThread::HandsOff;
    // The chain sees the marker of a thread that presents its own frames (with the frame it left unpresented, if
    // any) and a thread's first marker; a thread that hands its frames to another never reaches it.
    const bool presents = before == MarkerThread::Presents || s_marker_thread == MarkerThread::Unknown;
    bool selected = false;
    if (presents) {
        ChainLease lease;
        selected = s_route.Marker(c, key, RouteMode(config), config >> 2);
        // Same-thread route: publish its state now, as before the hand-off
        // (a gap or an unsupported context drops qualification at the marker).
        if (s_marker_thread == MarkerThread::Presents) PublishStatus(lease);
    } else {
        selected = s_route.Marker(c, key, RouteMode(config), config >> 2, false);
    }
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
    const bool offer = s_marker_thread != MarkerThread::Presents && record.mode == NativeGateMode::Native &&
        c.no_frame_generation && c.supported;
    s_local_marker_sequence = s_handoff.Publish(record, offer);
    return selected;
}
void NativeGate_OnMarkerReleased(int64_t qpc) noexcept {
    s_route.MarkerReleased(qpc);
    s_handoff.Amend(s_local_marker_sequence, [qpc](MarkerHandoff* record) { record->release_qpc = qpc; });
}
void NativeGate_BeginScope(uint64_t native, uint64_t generation) noexcept {
    NativeGate_NoteEntry(NativeGateEntry::BeginScope);
    ChainLease lease;
    ClaimOrAdoptMarker();
    s_route.BeginScope(native, generation);
}
uint64_t NativeGate_CrossThreadAdoptions() noexcept {
    return s_handoff_adoptions.load(std::memory_order_relaxed);
}
void NativeGate_EndScope(uint64_t native, uint64_t generation) noexcept {
    NativeGate_NoteEntry(NativeGateEntry::EndScope);
    const bool adopted = s_pending_adopted;
    NativeGateTrace completed{};
    ChainLease lease;
    const bool valid = s_route.EndScope(native, generation, &completed);
    if (valid) {
        if (lease.Held()) s_last_completed_adopted = adopted;
        if (!adopted) s_marker_thread = MarkerThread::Presents;
    }
    s_pending_adopted = false;
    PublishStatus(lease);
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
void NativeGate_NotePresentRejected(bool present1) noexcept {
    (present1 ? s_present1_rejected : s_present_rejected).fetch_add(1, std::memory_order_relaxed);
}
uint64_t NativeGate_PresentsRejected(bool present1) noexcept {
    return (present1 ? s_present1_rejected : s_present_rejected).load(std::memory_order_relaxed);
}
uint64_t NativeGate_ScopesWithoutCalls() noexcept {
    return s_scopes_without_calls.load(std::memory_order_relaxed);
}
void NativeGate_NoteEntry(NativeGateEntry entry) noexcept {
    const size_t index = static_cast<size_t>(entry);
    if (index >= static_cast<size_t>(NativeGateEntry::Count)) return;
    s_diag.entries[index].fetch_add(1, std::memory_order_relaxed);
    const uint32_t thread = GetCurrentThreadId();
    if (s_diag.entry_thread[index].load(std::memory_order_relaxed) != thread)
        s_diag.entry_thread[index].store(thread, std::memory_order_relaxed);
}
NativeGateDiagnostics NativeGate_Diagnostics() noexcept {
    NativeGateDiagnostics out{};
    for (size_t i = 0; i < static_cast<size_t>(NativeGateEntry::Count); ++i) {
        out.entries[i] = s_diag.entries[i].load(std::memory_order_relaxed);
        out.entry_thread[i] = s_diag.entry_thread[i].load(std::memory_order_relaxed);
    }
    for (size_t i = 0; i < static_cast<size_t>(NativeGateCallOutcome::Count); ++i)
        out.call[i] = s_diag.call[i].load(std::memory_order_relaxed);
    out.flags_zero = s_diag.flags[0].load(std::memory_order_relaxed);
    out.flags_test = s_diag.flags[1].load(std::memory_order_relaxed);
    out.flags_do_not_wait = s_diag.flags[2].load(std::memory_order_relaxed);
    out.flags_other = s_diag.flags[3].load(std::memory_order_relaxed);
    for (size_t i = 0; i < static_cast<size_t>(NativeGateScopeEvent::Count); ++i)
        out.scope[i] = s_diag.scope[i].load(std::memory_order_relaxed);
    out.present_rejected = s_present_rejected.load(std::memory_order_relaxed);
    out.present1_rejected = s_present1_rejected.load(std::memory_order_relaxed);
    out.scopes_without_call = s_scopes_without_calls.load(std::memory_order_relaxed);
    out.mismatch_sequence = s_diag.mismatch_sequence.load(std::memory_order_acquire);
    out.mismatch_scope_native = s_diag.mismatch[0].load(std::memory_order_relaxed);
    out.mismatch_scope_generation = s_diag.mismatch[1].load(std::memory_order_relaxed);
    out.mismatch_marker_native = s_diag.mismatch[2].load(std::memory_order_relaxed);
    out.mismatch_marker_generation = s_diag.mismatch[3].load(std::memory_order_relaxed);
    return out;
}
size_t NativeGate_DiagnosticsText(char* out, size_t size) noexcept {
    if (!out || size == 0) return 0;
    const NativeGateDiagnostics d = NativeGate_Diagnostics();
    using E = NativeGateEntry;
    using C = NativeGateCallOutcome;
    using S = NativeGateScopeEvent;
    auto ull = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    auto entry = [&](E e) { return d.entries[static_cast<size_t>(e)]; };
    auto thread = [&](E e) { return static_cast<unsigned long>(d.entry_thread[static_cast<size_t>(e)]); };
    auto call = [&](C c) { return ull(d.call[static_cast<size_t>(c)]); };
    auto scope = [&](S s) { return ull(d.scope[static_cast<size_t>(s)]); };
    const int written = std::snprintf(out, size,
        "entries on_present=%llu(t%lu) typed_present=%llu(t%lu) typed_present1=%llu(t%lu) "
        "begin_scope=%llu(t%lu) end_scope=%llu(t%lu) gate_call=%llu(t%lu) "
        "vtable_present=%llu(t%lu) vtable_present1=%llu(t%lu) inner_pass_through=%llu(t%lu) alias_pass_through=%llu(t%lu); "
        "present_rejected=%llu present1_rejected=%llu scopes_without_call=%llu; "
        "begin_call mapped=%llu unmapped_no_pending=%llu unmapped_no_scope=%llu unmapped_invalid=%llu "
        "nested=%llu stale=%llu special=%llu multiple=%llu; "
        "flags zero=%llu test=%llu do_not_wait=%llu other=%llu; "
        "begin_scope no_pending=%llu native_mismatch=%llu generation_mismatch=%llu already_open=%llu "
        "zero_generation=%llu; end_scope native_mismatch=%llu generation_mismatch=%llu",
        ull(entry(E::OnPresent)), thread(E::OnPresent), ull(entry(E::TypedPresent)), thread(E::TypedPresent),
        ull(entry(E::TypedPresent1)), thread(E::TypedPresent1), ull(entry(E::BeginScope)), thread(E::BeginScope),
        ull(entry(E::EndScope)), thread(E::EndScope), ull(entry(E::GateCall)), thread(E::GateCall),
        ull(entry(E::VtablePresent)), thread(E::VtablePresent), ull(entry(E::VtablePresent1)), thread(E::VtablePresent1),
        ull(entry(E::InnerPassThrough)), thread(E::InnerPassThrough),
        ull(entry(E::AliasPassThrough)), thread(E::AliasPassThrough),
        ull(d.present_rejected), ull(d.present1_rejected), ull(d.scopes_without_call),
        call(C::Mapped), call(C::UnmappedNoPending), call(C::UnmappedNoScope), call(C::UnmappedInvalid),
        call(C::Nested), call(C::Stale), call(C::SpecialPresent), call(C::MultipleCalls),
        ull(d.flags_zero), ull(d.flags_test), ull(d.flags_do_not_wait), ull(d.flags_other),
        scope(S::NoPending), scope(S::NativeMismatch), scope(S::GenerationMismatch), scope(S::AlreadyOpen),
        scope(S::ZeroGeneration), scope(S::EndNativeMismatch), scope(S::EndGenerationMismatch));
    if (written < 0) { out[0] = '\0'; return 0; }
    return static_cast<size_t>(written) < size ? static_cast<size_t>(written) : size - 1;
}
NativeGateCall::NativeGateCall(const NativeGateContext& c, uint32_t flags) noexcept {
    NativeGate_NoteEntry(NativeGateEntry::GateCall);
    const uint64_t config = s_config.load(std::memory_order_acquire);
    ChainLease lease;
    trace = s_route.BeginCall(c, flags, config >> 2);
    trace.mode = RouteMode(config);
    trace.present_flags = flags;
    PublishStatus(lease);
    trace.prediction_epoch = s_marker_prediction_epoch;
    trace.cross_thread = s_pending_adopted && trace.source_frame_id != 0;
}
NativeGateCall::~NativeGateCall() {
    ChainLease lease;
    s_route.EndCall(success_, retried_, &trace);
    PublishStatus(lease);
}
void NativeGateCall::Complete(bool success, bool retried) noexcept {
    success_ = success && trace.status != NativeGateStatus::Cancelled;
    retried_ = retried;
}
