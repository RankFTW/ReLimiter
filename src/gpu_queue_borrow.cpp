#include "gpu_queue_borrow.h"

namespace gpu_queue_borrow {

// ── Pending word ──────────────────────────────────────────────────────────────────────────────
uint64_t PackPending(const Pending& p) noexcept
{
    return ((p.k & kMaxK) << 33) | (p.borrowed ? (uint64_t(1) << 32) : 0) | uint64_t(p.thread);
}

Pending UnpackPending(uint64_t word) noexcept
{
    Pending p;
    p.k = word >> 33;
    p.borrowed = ((word >> 32) & 1) != 0;
    p.thread = static_cast<uint32_t>(word & 0xFFFFFFFFu);
    return p;
}

// ── Pattern ───────────────────────────────────────────────────────────────────────────────────
void PatternTracker::OnMarker(uint64_t fence, uint64_t value) noexcept
{
    // The frame's fence: claimed by a compare-exchange only by the FIRST marker (while it still is 0); every later marker of
    // the frame compares what it loads (the common case: no locked instruction). Same outcome as an unconditional
    // compare-exchange: the first claim wins, a marker of another fence marks the frame mixed.
    uint64_t seen = fence_.load(std::memory_order_acquire);
    if (seen == 0) {
        uint64_t expected = 0;
        if (fence_.compare_exchange_strong(expected, fence, std::memory_order_acq_rel, std::memory_order_acquire)) {
            // The first marker of the frame: another fence than the last frame's has its own value space.
            if (known_fence_.load(std::memory_order_relaxed) != fence) last_value_.store(0, std::memory_order_relaxed);
            seen = fence;
        } else {
            seen = expected;
        }
    }
    if (seen != fence) mixed_.store(true, std::memory_order_relaxed);
    // Strictly increasing: a value that does not rise above the last one is not a marker for a new
    // submission (the fence was reset or signalled by someone else).
    uint64_t last = last_value_.load(std::memory_order_relaxed);
    if (value <= last) {
        regressed_.store(true, std::memory_order_relaxed);
    } else {
        last_value_.compare_exchange_strong(last, value, std::memory_order_relaxed);
    }
    markers_.store(markers_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

FramePattern PatternTracker::EndFrame() noexcept
{
    FramePattern p;
    p.submissions = submissions_.exchange(0, std::memory_order_acq_rel);
    p.markers = markers_.exchange(0, std::memory_order_acq_rel);
    const uint32_t breaks = breaks_.exchange(0, std::memory_order_acq_rel);
    p.fence = fence_.load(std::memory_order_acquire);
    const bool mixed = mixed_.exchange(false, std::memory_order_acq_rel);
    const bool regressed = regressed_.exchange(false, std::memory_order_acq_rel);
    const bool foreign = foreign_.exchange(false, std::memory_order_acq_rel);
    const bool unclean = unclean_.exchange(false, std::memory_order_acq_rel);
    p.whole = p.submissions > 0 && p.markers == p.submissions && breaks == 0 && !mixed && !regressed && !foreign &&
              !unclean && p.fence != 0;
    // The last value carries over (values keep rising across frames); the frame's fence is remembered for the
    // other-queue test and the next frame names its own.
    if (p.fence != 0) known_fence_.store(p.fence, std::memory_order_release);
    fence_.store(0, std::memory_order_release);
    return p;
}

void PatternTracker::Reset() noexcept
{
    submissions_.store(0, std::memory_order_relaxed);
    markers_.store(0, std::memory_order_relaxed);
    breaks_.store(0, std::memory_order_relaxed);
    fence_.store(0, std::memory_order_relaxed);
    known_fence_.store(0, std::memory_order_relaxed);
    last_value_.store(0, std::memory_order_relaxed);
    mixed_.store(false, std::memory_order_relaxed);
    regressed_.store(false, std::memory_order_relaxed);
    foreign_.store(false, std::memory_order_relaxed);
    unclean_.store(false, std::memory_order_relaxed);
}

// ── Gate ──────────────────────────────────────────────────────────────────────────────────────
const char* OffReasonName(OffReason r) noexcept
{
    switch (r) {
    case OffReason::None: return "none";
    case OffReason::FenceChanged: return "fence changed";
    case OffReason::ValueRegressed: return "value regressed";
    case OffReason::StampBeforeIssue: return "stamp before issue";
    case OffReason::ReferenceFailed: return "reference failed";
    case OffReason::RegistrationFailed: return "registration failed";
    case OffReason::DeviceRemoved: return "device removed";
    case OffReason::ForeignSignal: return "foreign signal of the bound fence";
    }
    return "?";
}

FrameEnd Gate::OnFrameEnd(const FramePattern& p, uint64_t bound_fence) noexcept
{
    FrameEnd out;
    if (disabled()) {
        active_.store(false, std::memory_order_release);
        return out;
    }
    if (!p.whole) {
        if (active_.exchange(false, std::memory_order_acq_rel)) out.deactivated = true;
        return out;
    }
    if (bound_fence != 0 && p.fence != bound_fence) {
        // A whole frame on another fence than the one the reference is held on: the session does not
        // rebind (one reference, one fence).
        if (Disable(OffReason::FenceChanged)) {
            out.disabled_now = true;
            out.reason = OffReason::FenceChanged;
        }
        if (active_.exchange(false, std::memory_order_acq_rel)) out.deactivated = true;
        return out;
    }
    candidate_.store(p.fence, std::memory_order_release);
    if (bound_fence == 0) return out; // nominated only: no reference yet, the next whole frame decides
    if (!active_.exchange(true, std::memory_order_acq_rel)) {
        out.activated = true;
        out.first_activation = !ever_active_;
        ever_active_ = true;
    }
    return out;
}

bool Gate::Disable(OffReason r) noexcept
{
    uint8_t expected = 0;
    const bool first = reason_.compare_exchange_strong(expected, static_cast<uint8_t>(r), std::memory_order_acq_rel);
    active_.store(false, std::memory_order_release);
    return first;
}

bool Gate::OnForeignSignal(uint64_t fence, uint64_t bound_fence) noexcept
{
    if (fence == 0 || (fence != bound_fence && fence != candidate_.load(std::memory_order_acquire))) return false;
    foreign_signals_.fetch_add(1, std::memory_order_relaxed);
    return Disable(OffReason::ForeignSignal);
}

void Gate::Reset() noexcept
{
    foreign_signals_.store(0, std::memory_order_relaxed);
    active_.store(false, std::memory_order_relaxed);
    reason_.store(0, std::memory_order_relaxed);
    candidate_.store(0, std::memory_order_relaxed);
    ever_active_ = false;
}

// ── Slot mapping ──────────────────────────────────────────────────────────────────────────────
KInfo InfoFromSlot(uint64_t slot_k, uint64_t slot_value, uint64_t k, int64_t leave, int64_t issue) noexcept
{
    KInfo info;
    if (slot_k > k) { info.mode = KMode::Overwritten; return info; }
    if (slot_k != k || slot_value == 0) return info; // never borrowed, or settled to the probe's own Signal
    info.leave_qpc = leave;
    info.issue_qpc = issue;
    if (slot_value == kValuePending) {
        info.mode = KMode::Pending;
    } else if (slot_value == kValueLost) {
        info.mode = KMode::Lost;
    } else {
        info.mode = KMode::Borrowed;
        info.game_value = slot_value;
    }
    return info;
}

// ── The worker step ───────────────────────────────────────────────────────────────────────────
bool WorkerStep::Covered(const Observation& o, uint8_t fence, uint64_t value) noexcept
{
    if (!o.valid) return false;
    return value <= (fence == kFenceGame ? o.game_completed : o.own_completed);
}

void WorkerStep::Reset(uint64_t session_generation) noexcept
{
    session_ = session_generation;
    reg_[0] = Reg{};
    reg_[1] = Reg{};
    rounds_ = 0;
    game_refused_ = false;
    registrations_ = 0;
    boundary_registrations_ = 0;
}

uint64_t WorkerStep::armed_k() const noexcept
{
    uint64_t k = 0;
    for (const Reg& r : reg_)
        if (r.armed.valid && r.armed.k > k) k = r.armed.k;
    return k;
}

StepActions WorkerStep::OnWake(const WakeInput& in) noexcept
{
    StepActions out;
    rounds_ = in.immediate_round ? rounds_ + 1 : 0;
    const Observation& o = in.obs;

    // 3. The armed marker is awaited only when this is the event of its own registration and the registration's value is
    // reached. Every other wake (calibration, timeout, stale, an event with nothing outstanding or not reached) certifies
    // nothing: the armed marker then gets an observation interval like the others (the core's OnObservation).
    if (in.cause == WakeCause::ArmedEvent && o.valid) {
        Reg& r = reg_[in.event_fence & 1];
        if (r.outstanding && Covered(o, in.event_fence & 1, r.value)) {
            if (r.armed.valid) {
                out.awaited.valid = true;
                out.awaited.k = r.armed.k;
                out.awaited.hi_qpc = in.wake_qpc;
                out.awaited.foreign_epoch = o.foreign_epoch;
                out.awaited.boundary = r.armed.boundary;
                int64_t lo = in.wake_qpc - in.latency_ticks;
                // The marker was incomplete when the registration was made: never earlier than that.
                if (r.armed.registration_qpc > lo && r.armed.registration_qpc <= in.wake_qpc) lo = r.armed.registration_qpc;
                out.awaited.lo_qpc = lo;
            }
            r = Reg{}; // fired
        }
    }
    // A registration whose value is reached has fired (the kernel holds nothing for it): not outstanding any more. Its
    // armed marker found covered by another wake is a late pickup: dropped, never awaited.
    for (uint8_t f = 0; f < 2; ++f)
        if (reg_[f].outstanding && Covered(o, f, reg_[f].value)) reg_[f] = Reg{};

    // 5. Registration: one outstanding per fence. The OLDEST uncovered boundary marker (a frame's last marker) is armed
    // before the newest marker (the tail): it completes before any later tail on the same queue, so nothing is lost.
    // A game-fence target is selectable only while the game fence is bound and its registration has not been refused.
    const bool game_ok = in.game_fence_bound && !game_refused_;
    const auto selectable = [&](const Tail& t) { return (t.fence & 1) == kFenceOwn || game_ok; };
    const bool tail_ok = in.tail.valid && in.tail.session_generation == session_;
    const bool boundary_pending = in.boundary.valid && in.boundary.session_generation == session_ &&
        !Covered(o, in.boundary.fence & 1, in.boundary.value) && selectable(in.boundary);
    const bool tail_pending = tail_ok && !Covered(o, in.tail.fence & 1, in.tail.value) && selectable(in.tail);
    const Tail* target = boundary_pending ? &in.boundary : (tail_pending ? &in.tail : nullptr);
    if (target != nullptr) {
        const uint8_t f = target->fence & 1;
        Reg& r = reg_[f];
        if (!r.outstanding) {
            r.outstanding = true;
            r.value = target->value;
            r.armed.valid = true;
            r.armed.k = target->k;
            r.armed.boundary = boundary_pending;
            r.armed.registration_qpc = o.valid ? o.before_qpc : in.wake_qpc;
            out.registers[out.register_count++] = Registration{f, target->value, boundary_pending};
            ++registrations_;
            if (boundary_pending) ++boundary_registrations_;
        }
    } else if (o.valid) {
        // Drained: the next completion on each fence fires its event, whatever its value.
        for (uint8_t f = 0; f < 2; ++f) {
            if (reg_[f].outstanding) continue;
            if (f == kFenceGame && !game_ok) continue;
            const uint64_t completed = f == kFenceGame ? o.game_completed : o.own_completed;
            if (completed == UINT64_MAX) continue; // device removed: nothing can be armed
            reg_[f].outstanding = true;
            reg_[f].value = completed + 1;
            reg_[f].armed = Armed{};
            out.registers[out.register_count++] = Registration{f, completed + 1};
            ++registrations_;
        }
    }
    return out;
}

void WorkerStep::RegistrationFailed(uint8_t fence) noexcept
{
    if ((fence & 1) == kFenceGame) game_refused_ = true;
    reg_[fence & 1] = Reg{};
    if (registrations_ > 0) --registrations_;
}

Next WorkerStep::AfterRegistering(const Observation& obs) noexcept
{
    if (!obs.valid) return Next::Sleep;
    bool again = false;
    for (uint8_t f = 0; f < 2; ++f) {
        Reg& r = reg_[f];
        if (!r.outstanding) continue;
        if (Covered(obs, f, r.value)) {
            r = Reg{}; // completed while registering: fired at once, a late pickup (never awaited)
            again = true;
        } else if (r.armed.valid && obs.before_qpc > r.armed.registration_qpc) {
            r.armed.registration_qpc = obs.before_qpc;
        }
    }
    return again && rounds_ < kMaxImmediateRounds ? Next::GoAgain : Next::Sleep;
}

// ── The reference ─────────────────────────────────────────────────────────────────────────────
BindResult BoundFence::Bind(void* fence, const RefOps& ops) noexcept
{
    if (fence == nullptr || ops.retain == nullptr) return BindResult::Failed;
    uint32_t state = state_.load(std::memory_order_acquire);
    if (state == kReleased || state == kClosing) return BindResult::Closed;
    if (state == kBinding) return BindResult::Busy; // another thread is inside its retain: nobody waits for it
    if (state == kBound) return fence_.load(std::memory_order_acquire) == fence ? BindResult::Already : BindResult::Different;
    if (!state_.compare_exchange_strong(state, kBinding, std::memory_order_acq_rel)) {
        // Lost the transition to another thread or to Release: decide again once, without waiting.
        state = state_.load(std::memory_order_acquire);
        if (state == kReleased || state == kClosing) return BindResult::Closed;
        if (state == kBound) return fence_.load(std::memory_order_acquire) == fence ? BindResult::Already : BindResult::Different;
        return BindResult::Busy;
    }
    const bool retained = ops.retain(fence);
    uint32_t expected = kBinding;
    if (!retained) {
        if (!state_.compare_exchange_strong(expected, kNone, std::memory_order_acq_rel)) state_.store(kReleased, std::memory_order_release); // closed meanwhile
        return BindResult::Failed;
    }
    taken_.fetch_add(1, std::memory_order_relaxed);
    fence_.store(fence, std::memory_order_release);
    if (!state_.compare_exchange_strong(expected, kBound, std::memory_order_acq_rel)) {
        // Release found this binder inside its retain (state Closing): the reference is released here, by the binder.
        fence_.store(nullptr, std::memory_order_release);
        if (ops.release != nullptr) ops.release(fence);
        released_.fetch_add(1, std::memory_order_relaxed);
        state_.store(kReleased, std::memory_order_release);
        return BindResult::Closed;
    }
    return BindResult::Taken;
}

bool BoundFence::Release(const RefOps& ops) noexcept
{
    for (;;) {
        uint32_t state = state_.load(std::memory_order_acquire);
        if (state == kReleased || state == kClosing) return false;
        if (state == kNone) {
            if (state_.compare_exchange_strong(state, kReleased, std::memory_order_acq_rel)) return false; // closed for good
            continue;
        }
        if (state == kBinding) {
            // A binder is inside its retain: close behind it; it releases its own reference when it completes.
            if (state_.compare_exchange_strong(state, kClosing, std::memory_order_acq_rel)) return false;
            continue;
        }
        if (!state_.compare_exchange_strong(state, kReleased, std::memory_order_acq_rel)) continue; // Bound
        void* const fence = fence_.exchange(nullptr, std::memory_order_acq_rel);
        if (fence != nullptr && ops.release != nullptr) ops.release(fence);
        released_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
}

void BoundFence::Reset() noexcept
{
    state_.store(kNone, std::memory_order_relaxed);
    fence_.store(nullptr, std::memory_order_relaxed);
    taken_.store(0, std::memory_order_relaxed);
    released_.store(0, std::memory_order_relaxed);
}

} // namespace gpu_queue_borrow
