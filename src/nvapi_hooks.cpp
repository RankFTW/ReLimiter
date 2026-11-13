#include "fg_input_observer.h"
#include "nvapi_hooks.h"
#include "feedback_device_identity.h"
#include "hooks.h"
#include "marker_log.h"
#include "marker_stamps.h"
#include "marker_cycles.h"
#include "scheduler.h"
#include "pacing_sleep.h"
#include "render_handoff.h"
#include "correlator.h"
#include "config.h"
#include "health.h"
#include "pcl_hooks.h"
#include "streamline_hooks.h"
#include "swapchain_manager.h"
#include "frame_timeline.h"
#include "nvapi_marker_policy.h"
#include "nvapi_marker_census.h"
#include "bounded_mpsc_queue.h"
#include "feedback.h"
#include "native_present_gate.h"
#include "presentation_gate.h"
#include "pacing_telemetry.h"
#include "reflex_clock_map.h"
#include "runtime_lifecycle.h"
#include "wake_guard.h"
#include "logger.h"
#include "instrumentation_state.h"
#include "pacing_resolver.h"
#include "unified_pacer.h"
#include "frame_plan.h"
#include "gpu_readiness.h"
#include "module_presence.h"
#include <intrin.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>

// MarkerStamps_Capture maps the marker value to its kind by this numbering (marker_stamps.h).
static_assert(SIMULATION_START == 0 && SIMULATION_END == 1 && RENDERSUBMIT_START == 2 &&
              RENDERSUBMIT_END == 3, "marker stamps assume the NV_LATENCY_MARKER_TYPE numbering");

// ── Trampolines ──
PFN_NvAPI_D3D_SetSleepMode     s_orig_sleep_mode      = nullptr;
PFN_NvAPI_D3D_Sleep            s_orig_sleep            = nullptr;
PFN_NvAPI_D3D_SetLatencyMarker s_orig_set_latency_marker = nullptr;
// NvAPI_D3D12_SetAsyncFrameMarker: pass-through timing only (Hook_SetAsyncFrameMarker).
static PFN_NvAPI_D3D12_SetAsyncFrameMarker s_orig_set_async_frame_marker = nullptr;
// NvAPI_D3D_GetSleepStatus: the trampoline serves the game's own calls
// (Hook_GetSleepStatus) and the developer poll of the driver's status
// (PollSleepStatus), which must not count as the game's call.
static PFN_NvAPI_D3D_GetSleepStatus s_orig_get_sleep_status = nullptr;

// ── Shared state ──
std::atomic<IUnknown*> g_dev{ nullptr };
static std::atomic<uint32_t> s_feedback_device_acquire_readers{ 0 };
// Writer exclusion/snapshot validation is independent of the identity handed
// to consumers. Rejected/no-op transitions advance only the writer sequence.
static std::atomic<uint64_t> s_feedback_device_transition_sequence{ 0 };
static std::atomic<uint64_t> s_feedback_device_publication_sequence{ 0 };
static std::array<IUnknown*, 16> s_retired_feedback_devices{};
static SRWLOCK s_retired_feedback_device_lock = SRWLOCK_INIT;
std::atomic<uint32_t> g_game_requested_interval{0};

// Game sleep-mode state is bound to one exact feedback-device publication.
// The identity is borrowed; restore first acquires and verifies the current
// device before dereferencing it.
static NV_SET_SLEEP_MODE_PARAMS s_game_original_params = {};
static NvAPIFeedbackDevice s_game_params_identity = {};
static bool s_game_params_captured = false;
static std::atomic_flag s_game_params_lock = ATOMIC_FLAG_INIT;
// bypass_low_latency_off: true while ReLimiter has turned the driver's low
// latency mode off through SetSleepMode (UpdateLowLatencyOverride). Cleared
// whenever the game's parameters reach the driver again. The forward lock
// serializes a transition (driver call and state change) with the forwarding
// of the game's own SetSleepMode calls, so a game call can neither land
// between the two nor undo the override; concurrent Sleep calls that find it
// taken skip the transition and retry on their next Sleep.
static std::atomic<bool> s_low_latency_forced_off{ false };
static SRWLOCK s_sleep_mode_forward_lock = SRWLOCK_INIT;

// Unified pacer (developer key unified_pacer): the applied Sleep-mode record
// {publication, owner, low latency off, interval, epoch}, written only under
// s_sleep_mode_forward_lock and only by a reconcile or a desired-tuple forward (key on, a
// key toggle or a restoration owed): with the key off and settled nothing is recorded. The
// record is valid only while it describes what a key-on reconcile applied; a key-off
// reconcile restores the key-off row and leaves the record invalid, so the next reconcile
// always forwards (an unrecorded record differs from every desired tuple).
// s_applied_key is the key the record was last reconciled under. s_sleep_mode_epoch is the
// epoch the scheduler bumps on key toggles and Dynamic MFG transitions: it is compared only
// inside the key-on tuple (a bump with the key on forces a forward; with the key off it
// changes nothing, a restoration is owed or it is not). The whole record is also mirrored in
// a seqlock so that the reconcile takes the forward lock only on a difference (one writer at
// a time: the lock holder).
static UnifiedSleepModeTuple s_applied_sleep_mode{};
static bool s_applied_sleep_valid = false;
static std::atomic<bool> s_applied_key{ false };
static std::atomic<uint32_t> s_sleep_mode_epoch{ 0 };
// The key generation: advanced by NvAPI_PrepareUnifiedKey on every actual key change, in both
// directions, before the key is exposed. The record carries the generation it was reconciled
// under and matches only that one, so a key state entered again (an off/on pair no scheduler
// bump observed, with a legacy key-off path in between that changed the driver and records
// nothing) never matches a record from the earlier one.
static std::atomic<uint32_t> s_key_generation{ 0 };
static uint32_t s_applied_generation = 0; // under the forward lock
struct AppliedSleepModeMirror {
    std::atomic<uint32_t> sequence{ 0 };
    std::atomic<uint64_t> publication{ 0 };
    std::atomic<uint64_t> packed{ 0 }; // owner | low latency off << 8 | valid << 9 | interval << 32
    std::atomic<uint32_t> epoch{ 0 };
    std::atomic<uint32_t> generation{ 0 };
};
static AppliedSleepModeMirror s_applied_sleep_mode_mirror;

static uint64_t PackSleepMode(const UnifiedSleepModeTuple& tuple, bool valid) noexcept {
    return static_cast<uint64_t>(tuple.owner) | (tuple.low_latency_off ? uint64_t{ 1 } << 8 : 0) |
        (valid ? uint64_t{ 1 } << 9 : 0) | (static_cast<uint64_t>(tuple.interval_us) << 32);
}
// Forward lock held.
static void PublishAppliedSleepModeMirror() noexcept {
    AppliedSleepModeMirror& mirror = s_applied_sleep_mode_mirror;
    const uint32_t sequence = mirror.sequence.load(std::memory_order_relaxed);
    mirror.sequence.store(sequence + 1, std::memory_order_relaxed); // odd: being written
    std::atomic_thread_fence(std::memory_order_release);
    mirror.publication.store(s_applied_sleep_mode.publication, std::memory_order_relaxed);
    mirror.packed.store(PackSleepMode(s_applied_sleep_mode, s_applied_sleep_valid), std::memory_order_relaxed);
    mirror.epoch.store(s_applied_sleep_mode.epoch, std::memory_order_relaxed);
    mirror.generation.store(s_applied_generation, std::memory_order_relaxed);
    mirror.sequence.store(sequence + 2, std::memory_order_release);
}
// Without the lock: true only when the mirror, read consistently, is a valid record equal to
// `desired` reconciled under `key`; a torn read, an invalid record or a difference answers
// false (the caller then takes the lock and decides).
static bool AppliedSleepModeMatches(const UnifiedSleepModeTuple& desired, bool key, uint32_t generation) noexcept {
    if (s_applied_key.load(std::memory_order_acquire) != key) return false;
    const AppliedSleepModeMirror& mirror = s_applied_sleep_mode_mirror;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const uint32_t before = mirror.sequence.load(std::memory_order_acquire);
        if (before & 1u) continue;
        const uint64_t publication = mirror.publication.load(std::memory_order_relaxed);
        const uint64_t packed = mirror.packed.load(std::memory_order_relaxed);
        const uint32_t epoch = mirror.epoch.load(std::memory_order_relaxed);
        const uint32_t applied_generation = mirror.generation.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (mirror.sequence.load(std::memory_order_relaxed) != before) continue;
        return publication == desired.publication && packed == PackSleepMode(desired, true) &&
            epoch == desired.epoch && applied_generation == generation;
    }
    return false;
}

// "A reconcile is pending" is ONE atomic word, read with one load by the Sleep, SetSleepMode
// and limiter paths and the scheduler's OnMarker: bit 0 is the key the configuration is
// publishing (noted BEFORE the pipeline exposes it), bit 1 a restoration owed (a Unified or
// Limiter tuple was applied to the driver and the game's parameters are not back yet).
// Pending is either bit. There is no separately computed flag: the bits are changed by
// atomic read-modify-write, so an older writer can never overwrite a newer bit, and each bit
// is cleared by the one operation that settles what it stands for (the key bit by the next
// publication, the owed bit by the reconcile that restored the driver or found nothing to
// restore, under the forward lock that serialises every driver forward). Key off and nothing
// owed: the word is 0.
static std::atomic<uint32_t> s_transition_state{ 0 };
constexpr uint32_t kTransitionKey = 1u, kTransitionOwed = 2u;

// A key-on reconcile or forward was applied (forward lock held): a tuple that is not the
// game's own leaves a restoration owed, the game's own tuple (Dynamic MFG) restores it.
static void StoreAppliedSleepMode(const UnifiedSleepModeTuple& tuple, uint32_t generation) noexcept {
    s_applied_sleep_mode = tuple;
    s_applied_sleep_valid = true;
    s_applied_generation = generation;
    s_applied_key.store(true, std::memory_order_release);
    PublishAppliedSleepModeMirror();
    if (tuple.owner == UnifiedSleepOwner::Game)
        s_transition_state.fetch_and(~kTransitionOwed, std::memory_order_acq_rel);
    else
        s_transition_state.fetch_or(kTransitionOwed, std::memory_order_acq_rel);
}
// A key-off reconcile or forward restored the key-off row (forward lock held): the record is
// not kept (the key-off paths record nothing), nothing is owed any more.
static void SettleKeyOffSleepMode() noexcept {
    s_applied_sleep_valid = false;
    s_applied_key.store(false, std::memory_order_release);
    PublishAppliedSleepModeMirror();
    s_transition_state.fetch_and(~kTransitionOwed, std::memory_order_acq_rel);
}
// A path that does not forward the tuple changed what the driver has (only reached while a
// reconcile is pending): the record no longer describes it.
static void InvalidateAppliedSleepMode() noexcept {
    s_applied_sleep_valid = false;
    PublishAppliedSleepModeMirror();
}

class GameParamsGuard {
public:
    GameParamsGuard() noexcept {
        while (s_game_params_lock.test_and_set(std::memory_order_acquire))
            _mm_pause();
    }
    ~GameParamsGuard() {
        s_game_params_lock.clear(std::memory_order_release);
    }
};
// Pacing-thread access to the captured parameters (the Sleep and marker paths): one
// attempt, never a spin. A holder that is descheduled leaves the reconcile pending.
class TryGameParamsGuard {
public:
    TryGameParamsGuard() noexcept : held_(!s_game_params_lock.test_and_set(std::memory_order_acquire)) {}
    ~TryGameParamsGuard() {
        if (held_) s_game_params_lock.clear(std::memory_order_release);
    }
    explicit operator bool() const noexcept { return held_; }
    TryGameParamsGuard(const TryGameParamsGuard&) = delete;
    TryGameParamsGuard& operator=(const TryGameParamsGuard&) = delete;

private:
    bool held_;
};

static void ResetGameSleepParams() noexcept {
    GameParamsGuard lock;
    s_game_original_params = {};
    // The status snapshots belong to the publication that is going away; the next
    // publication's token (EndFeedbackDeviceTransition advances the sequence by 2).
    g_sleep_status_token.store(
        static_cast<uint32_t>(((s_feedback_device_publication_sequence.load(std::memory_order_relaxed) + 2) >> 1) & kSleepStatusTokenMask),
        std::memory_order_release);
    g_sleep_status_newest.store(0, std::memory_order_relaxed);
    g_sleep_status_at_sleep_return.store(0, std::memory_order_relaxed);
    s_game_params_identity = {};
    s_game_params_captured = false;
    g_game_requested_interval.store(0, std::memory_order_relaxed);
    // The device the override was applied to is gone.
    s_low_latency_forced_off.store(false, std::memory_order_release);
}

bool NvAPI_IsSameDevice(IUnknown* left, IUnknown* right) noexcept {
    return FeedbackDeviceIdentity_IsSame(left, right);
}

enum class MarkerRejection : uint32_t {
    MissingIncoming,
    SelectedUnavailable,
    UnsupportedAPI,
    SelectedIdentity,
    FeedbackIdentity,
    GuardStaleToken,
    ForwardStaleToken,
    SchedulerOwner,
    OwnerStaleToken,
    EnforcementStaleToken,
    GateStaleToken
};

static void LogMarkerRejection(
    MarkerRejection reason, IUnknown* incoming, IUnknown* selected,
    const NV_LATENCY_MARKER_PARAMS* marker) noexcept {
    if (!marker) return;
    LARGE_INTEGER census_now{};QueryPerformanceCounter(&census_now);
    g_nvapi_marker_census.Record(static_cast<uint32_t>(marker->markerType),MarkerCensusStage::Rejected,
        {marker->frameID,reinterpret_cast<uint64_t>(incoming),0,census_now.QuadPart,
        GetCurrentThreadId(),0,static_cast<uint32_t>(reason),0});
    // One line per fixed reason for the process lifetime, not per device,
    // frame, epoch or caller. No dynamic reason table on the marker path.
    static std::atomic<uint32_t> s_logged_reasons{ 0 };
    const uint32_t bit = 1u << static_cast<uint32_t>(reason);
    if ((s_logged_reasons.load(std::memory_order_relaxed) & bit) != 0 ||
        (s_logged_reasons.fetch_or(bit, std::memory_order_relaxed) & bit) != 0)
        return;
    static constexpr const char* names[] = {
        "missing_incoming", "selected_unavailable", "unsupported_api",
        "selected_identity", "feedback_identity", "guard_stale_token",
        "post_forward_stale_token", "scheduler_owner_busy_or_reentrant",
        "post_owner_stale_token", "enforcement_stale_token", "gate_stale_token"
    };
    LOG_INFO("NvAPI: marker local rejection reason=%s incoming=%p selected=%p "
             "api=%u type=%u frameID=%llu (forwarding unchanged)",
             names[static_cast<uint32_t>(reason)], static_cast<void*>(incoming),
             static_cast<void*>(selected), static_cast<unsigned>(SwapMgr_GetActiveAPI()),
             static_cast<unsigned>(marker->markerType), marker->frameID);
}

// (g_dev, publication sequence) read as one consistent pair under the transition sequence, as
// NvAPI_IsFeedbackDeviceCurrent reads it. False while a transition is in progress.
static bool ReadFeedbackPublication(IUnknown** device, uint64_t* publication) noexcept {
    const uint64_t before = s_feedback_device_transition_sequence.load(std::memory_order_acquire);
    if ((before & 1u) != 0) return false;
    *publication = s_feedback_device_publication_sequence.load(std::memory_order_relaxed);
    *device = g_dev.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    return before == s_feedback_device_transition_sequence.load(std::memory_order_relaxed);
}

// One memoized POSITIVE answer of IsPresentingFeedbackDevice per thread (the game thread and the
// presenting thread each send markers): the incoming device is the published feedback device
// (g_dev, which owns a reference) of publication `feedback_publication`, and it is the same
// device as the presenting device (which SwapMgr owns a reference to) of publication
// `presenting_sequence`. Neither object can be destroyed, nor its address reused, while its
// publication sequence is unchanged (a capture, replacement or clear of the feedback device
// advances s_feedback_device_publication_sequence, a new, same or cleared presenting device
// advances the presenting sequence), and the identity relation of two live objects is fixed:
// so while both sequences are as memoized, an incoming pointer equal to g_dev is the same
// answer without a reference taken or a QueryInterface. An incoming pointer that only has
// the same identity as g_dev, a negative answer, a transition in progress and a non-DXGI API
// all take the full path below.
struct PresentingFeedbackMatch {
    IUnknown* device = nullptr;
    uint64_t feedback_publication = 0;
    uint64_t presenting_sequence = 0;
};
static thread_local PresentingFeedbackMatch t_presenting_match;

static bool IsPresentingFeedbackDevice(
    IUnknown* device, const NV_LATENCY_MARKER_PARAMS* marker = nullptr) noexcept {
    if (!device) {
        LogMarkerRejection(MarkerRejection::MissingIncoming, device, nullptr, marker);
        return false;
    }
    IUnknown* published = nullptr;
    uint64_t publication = 0;
    const bool memoizable = ReadFeedbackPublication(&published, &publication) && published == device;
    if (memoizable) {
        const PresentingFeedbackMatch& memo = t_presenting_match;
        if (memo.device == device && memo.feedback_publication == publication) {
            const uint64_t sequence = SwapMgr_PresentingDeviceSequence();
            const ActiveAPI memo_api = SwapMgr_GetActiveAPI();
            if ((sequence & 1u) == 0 && sequence == memo.presenting_sequence &&
                (memo_api == ActiveAPI::DX11 || memo_api == ActiveAPI::DX12))
                return true;
        }
    }
    const uint64_t presenting_sequence =
        memoizable ? SwapMgr_PresentingDeviceSequence() : 1u;
    IUnknown* presenting_device = SwapMgr_AcquirePresentingDevice();
    const ActiveAPI api = SwapMgr_GetActiveAPI();
    // Before presenter discovery, native NvAPI calls may bootstrap feedback.
    // An empty g_dev is NOT permission to bypass an already active presenter.
    // Failed/contended acquisition with an active API is conservatively rejected.
    if (!presenting_device) {
        if (api != ActiveAPI::None)
            LogMarkerRejection(MarkerRejection::SelectedUnavailable, device, nullptr, marker);
        return api == ActiveAPI::None;
    }
    const bool matches =
        (api == ActiveAPI::DX11 || api == ActiveAPI::DX12) &&
        NvAPI_IsSameDevice(device, presenting_device);
    if (!matches) {
        LogMarkerRejection(
            api == ActiveAPI::DX11 || api == ActiveAPI::DX12
                ? MarkerRejection::SelectedIdentity : MarkerRejection::UnsupportedAPI,
            device, presenting_device, marker);
    } else if (memoizable && (presenting_sequence & 1u) == 0) {
        // Memoize only when neither publication moved across the comparison: the compared
        // objects are then exactly the ones the two sequences name.
        IUnknown* published_after = nullptr;
        uint64_t publication_after = 0;
        if (ReadFeedbackPublication(&published_after, &publication_after) &&
            published_after == device && publication_after == publication &&
            SwapMgr_PresentingDeviceSequence() == presenting_sequence)
            t_presenting_match = { device, publication, presenting_sequence };
    }
    NvAPI_ReleaseFeedbackDevice(presenting_device);
    return matches;
}

static uint64_t BeginFeedbackDeviceTransition() noexcept {
    uint64_t sequence =
        s_feedback_device_transition_sequence.load(
            std::memory_order_acquire);
    for (;;) {
        if ((sequence & 1u) != 0) {
            SwitchToThread();
            sequence = s_feedback_device_transition_sequence.load(
                std::memory_order_acquire);
            continue;
        }
        if (s_feedback_device_transition_sequence.compare_exchange_weak(
                sequence,
                sequence + 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return sequence + 1;
        }
    }
}

static void EndFeedbackDeviceTransition(
    uint64_t write_sequence, bool publication_changed = false) noexcept {
    if (publication_changed) {
        s_feedback_device_publication_sequence.fetch_add(
            2, std::memory_order_relaxed);
    }
    s_feedback_device_transition_sequence.store(
        write_sequence + 1,
        std::memory_order_release);
}

static bool AddRefFeedbackDevice(IUnknown* device) noexcept {
    if (!device) return false;
    __try {
        device->AddRef();
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void TryDrainRetiredFeedbackDevices() noexcept {
    if (s_feedback_device_acquire_readers.load(std::memory_order_acquire) != 0)
        return;
    std::array<IUnknown*, 16> retired{};
    AcquireSRWLockExclusive(&s_retired_feedback_device_lock);
    if (s_feedback_device_acquire_readers.load(std::memory_order_acquire) == 0) {
        retired = s_retired_feedback_devices;
        s_retired_feedback_devices.fill(nullptr);
    }
    ReleaseSRWLockExclusive(&s_retired_feedback_device_lock);
    for (IUnknown* device : retired) NvAPI_ReleaseFeedbackDevice(device);
}

static void RetireFeedbackDevice(IUnknown* device) noexcept {
    if (!device) return;
    bool retained = false;
    AcquireSRWLockExclusive(&s_retired_feedback_device_lock);
    for (IUnknown*& slot : s_retired_feedback_devices) {
        if (!slot) {
            slot = device;
            retained = true;
            break;
        }
    }
    ReleaseSRWLockExclusive(&s_retired_feedback_device_lock);
    if (!retained) {
        OutputDebugStringA("ReLimiter: feedback-device retired-ref queue overflow.\n");
        return;
    }
    TryDrainRetiredFeedbackDevices();
}

bool NvAPI_CaptureFeedbackDevice(
    IUnknown* device,
    NvAPIFeedbackDevice* published_device) noexcept {
    if (published_device) *published_device = {};
    if (!device) return false;
    if (g_dev.load(std::memory_order_acquire) == device) return false;
    if (!AddRefFeedbackDevice(device)) return false;

    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t before = s_feedback_device_transition_sequence.load(
            std::memory_order_acquire);
        if ((before & 1u) != 0) continue;
        const NvAPIFeedbackDevice snapshot = NvAPI_AcquireFeedbackDevice();
        // A null snapshot may be acquisition failure rather than an empty
        // publication. Never use it as permission to replace a live device.
        if (before != s_feedback_device_transition_sequence.load(
                std::memory_order_acquire) ||
            (!snapshot.device && g_dev.load(std::memory_order_acquire))) {
            NvAPI_ReleaseFeedbackDevice(snapshot.device);
            continue;
        }
        // Foreign COM may reenter ReLimiter. Compare only with a retained
        // snapshot and OUTSIDE writer exclusion (including all QI releases).
        const bool same_device = NvAPI_IsSameDevice(snapshot.device, device);
        if (same_device) {
            // A no-op needs no writer lock. Verify the exact compared lifetime
            // before preserving it, just as the raw-pointer fast path does.
            const bool unchanged = before == s_feedback_device_transition_sequence.load(
                std::memory_order_acquire) && NvAPI_IsFeedbackDeviceCurrent(snapshot);
            NvAPI_ReleaseFeedbackDevice(snapshot.device);
            if (!unchanged) continue;
            NvAPI_ReleaseFeedbackDevice(device);
            return false; // Preserve the original callable pointer AND token.
        }
        const uint64_t write_sequence = BeginFeedbackDeviceTransition();
        IUnknown* previous = g_dev.load(std::memory_order_relaxed);
        const bool unchanged = write_sequence == before + 1 &&
            (snapshot.device ? NvAPI_IsSameFeedbackPublication(snapshot, {
                previous,
                FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame),
                s_feedback_device_publication_sequence.load(std::memory_order_relaxed)
            }) : previous == nullptr);
        if (!unchanged) {
            EndFeedbackDeviceTransition(write_sequence);
            NvAPI_ReleaseFeedbackDevice(snapshot.device);
            continue;
        }
        FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::NvApiFrame);
        ResetFeedbackProducerLifecycle();
        ResetGameSleepParams();
        g_dev.store(device, std::memory_order_relaxed);
        if (published_device) {
            *published_device = {
                device,
                FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame),
                s_feedback_device_publication_sequence.load(
                    std::memory_order_relaxed) + 2
            };
        }
        EndFeedbackDeviceTransition(write_sequence, true);
        NvAPI_ReleaseFeedbackDevice(snapshot.device);
        // g_dev owns exactly one reference for the published lifetime.
        RetireFeedbackDevice(previous);
        return true;
    }
    NvAPI_ReleaseFeedbackDevice(device);
    return false; // Bounded retry under publication churn; caller may retry.
}

// `drain` false is the measurement observer's snapshot (Hook_GetSleepStatus): the
// same publication protocol, but it never takes the retirement lock, so it cannot
// wait on a drainer. It only counts as a reader while it loads and references the
// device; a retired reference it kept a drainer from releasing is reclaimed by the
// next draining acquisition or retirement (every marker and Sleep hook).
static NvAPIFeedbackDevice AcquireFeedbackDeviceImpl(bool drain) noexcept {
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t before =
            s_feedback_device_transition_sequence.load(
                std::memory_order_acquire);
        if ((before & 1u) != 0) continue;

        s_feedback_device_acquire_readers.fetch_add(
            1,
            std::memory_order_acq_rel);
        if (before != s_feedback_device_transition_sequence.load(
                std::memory_order_acquire)) {
            s_feedback_device_acquire_readers.fetch_sub(
                1,
                std::memory_order_release);
            if (drain) TryDrainRetiredFeedbackDevices();
            continue;
        }
        IUnknown* device = g_dev.load(std::memory_order_relaxed);
        const uint32_t producer_epoch =
            FrameTimeline_CurrentProducerEpoch(
                FrameIdentityDomain::NvApiFrame);
        const uint64_t publication_sequence =
            s_feedback_device_publication_sequence.load(std::memory_order_relaxed);
        if (device && !AddRefFeedbackDevice(device)) device = nullptr;
        const uint64_t after =
            s_feedback_device_transition_sequence.load(
                std::memory_order_acquire);
        s_feedback_device_acquire_readers.fetch_sub(
            1,
            std::memory_order_release);
        if (drain) TryDrainRetiredFeedbackDevices();

        if (device && before == after && (after & 1u) == 0) {
            return { device, producer_epoch, publication_sequence };
        }
        NvAPI_ReleaseFeedbackDevice(device);
    }
    return {};
}

NvAPIFeedbackDevice NvAPI_AcquireFeedbackDevice() noexcept {
    return AcquireFeedbackDeviceImpl(true);
}

bool NvAPI_IsFeedbackDeviceCurrent(
    const NvAPIFeedbackDevice& feedback_device) noexcept {
    if (!feedback_device.device ||
        (feedback_device.publication_sequence & 1u) != 0) {
        return false;
    }
    const uint64_t before =
        s_feedback_device_transition_sequence.load(
            std::memory_order_acquire);
    if ((before & 1u) != 0) return false;
    const uint64_t publication_sequence =
        s_feedback_device_publication_sequence.load(std::memory_order_relaxed);
    IUnknown* device = g_dev.load(std::memory_order_relaxed);
    const uint32_t producer_epoch =
        FrameTimeline_CurrentProducerEpoch(
            FrameIdentityDomain::NvApiFrame);
    const uint64_t after =
        s_feedback_device_transition_sequence.load(
            std::memory_order_acquire);
    return before == after && NvAPI_IsSameFeedbackPublication(
        feedback_device, { device, producer_epoch, publication_sequence });
}

void NvAPI_ReleaseFeedbackDevice(IUnknown* device) noexcept {
    if (!device) return;
    __try {
        device->Release();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
}

void NvAPI_ClearFeedbackDevice(
    const NvAPIFeedbackDevice& expected_device) noexcept {
    if (!expected_device.device ||
        (expected_device.publication_sequence & 1u) != 0) {
        return;
    }
    const uint64_t write_sequence = BeginFeedbackDeviceTransition();
    if (!NvAPI_IsSameFeedbackPublication(expected_device, {
            g_dev.load(std::memory_order_relaxed),
            FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame),
            s_feedback_device_publication_sequence.load(std::memory_order_relaxed)
        })) {
        EndFeedbackDeviceTransition(write_sequence);
        return;
    }
    IUnknown* cleared_device = g_dev.load(std::memory_order_relaxed);
    g_dev.store(nullptr, std::memory_order_relaxed);
    ResetGameSleepParams();
    FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::NvApiFrame);
    ResetFeedbackProducerLifecycle();
    EndFeedbackDeviceTransition(write_sequence, true);
    RetireFeedbackDevice(cleared_device);
}

void NvAPI_ClearFeedbackDevice(IUnknown* expected_device) noexcept {
    if (expected_device &&
        g_dev.load(std::memory_order_acquire) != expected_device) {
        return;
    }
    const uint64_t write_sequence = BeginFeedbackDeviceTransition();
    IUnknown* cleared_device = g_dev.load(std::memory_order_relaxed);
    if (!cleared_device ||
        (expected_device && cleared_device != expected_device)) {
        EndFeedbackDeviceTransition(write_sequence);
        return;
    }

    g_dev.store(nullptr, std::memory_order_relaxed);
    ResetGameSleepParams();
    FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::NvApiFrame);
    ResetFeedbackProducerLifecycle();
    EndFeedbackDeviceTransition(write_sequence, true);
    RetireFeedbackDevice(cleared_device);
}

// Retain the validated publication across native forwarding and every local
// early return. This owns a COM reference, not the feedback writer lock.
class FeedbackDeviceGuard final {
public:
    explicit FeedbackDeviceGuard(
        IUnknown* device, const NV_LATENCY_MARKER_PARAMS* marker = nullptr) noexcept
        : identity(NvAPI_AcquireFeedbackDevice()) {
        if (!NvAPI_IsSameDevice(identity.device, device)) {
            LogMarkerRejection(MarkerRejection::FeedbackIdentity, device, nullptr, marker);
        } else if (!IsPresentingFeedbackDevice(device, marker)) {
            // The selected-device check records its own precise rejection.
        } else if (!NvAPI_IsFeedbackDeviceCurrent(identity)) {
            LogMarkerRejection(MarkerRejection::GuardStaleToken, device, nullptr, marker);
        } else {
            return;
        }
        NvAPI_ReleaseFeedbackDevice(identity.device);
        identity = {};
    }
    ~FeedbackDeviceGuard() noexcept {
        NvAPI_ReleaseFeedbackDevice(identity.device);
    }
    FeedbackDeviceGuard(const FeedbackDeviceGuard&) = delete;
    FeedbackDeviceGuard& operator=(const FeedbackDeviceGuard&) = delete;

    NvAPIFeedbackDevice identity;
};

// PRESENT_START gate timing — now owned by presentation_gate.cpp
// (g_last_gate_sleep_us declared in presentation_gate.h)

// ── RTSS filter ──
// The caller is RTSS's when its return address lies in RTSSHooks64.dll's image: two compares of the
// image range the loader scan published (module_presence.h), no loader query per marker. The range
// is the module's own, so there is nothing per address to remember and nothing to invalidate when
// another module is loaded or unloaded: an address cached as "not RTSS" could go stale across an
// unload and reload into the same range, a range check cannot. RTSS itself loading or unloading
// is seen by the scan (the worker's next pass: after a LoadLibrary notification, else at most one
// 250 ms timeout late).
static bool CallerIsRTSS(void* return_address) {
    return ModulePresence_AddressInRtss(return_address);
}

static NvAPI_Status SafeInvokeSetSleepMode(
    IUnknown* device,
    NV_SET_SLEEP_MODE_PARAMS* params) noexcept
{
    if (!s_orig_sleep_mode) return NVAPI_OK;
    __try { return s_orig_sleep_mode(device, params); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return NVAPI_OK; }
}

static bool SafeCopySleepModeParams(
    const NV_SET_SLEEP_MODE_PARAMS* source,
    NV_SET_SLEEP_MODE_PARAMS* destination) noexcept
{
    if (!source || !destination) return false;
    __try {
        *destination = *source;
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static NvAPI_Status SafeInvokeSleep(IUnknown* device) noexcept
{
    if (!s_orig_sleep) return NVAPI_OK;
    __try { return s_orig_sleep(device); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return NVAPI_OK; }
}

static NvAPI_Status SafeInvokeLatencyMarker(
    IUnknown* device,
    NV_LATENCY_MARKER_PARAMS* params) noexcept
{
    if (!s_orig_set_latency_marker) return NVAPI_OK;
    __try { return s_orig_set_latency_marker(device, params); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return NVAPI_OK; }
}

static bool SafeCopyLatencyMarkerParams(
    const NV_LATENCY_MARKER_PARAMS* source,
    NV_LATENCY_MARKER_PARAMS* destination) noexcept
{
    if (!source || !destination) return false;
    __try {
        *destination = *source;
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static NvAPI_Status SafeInvokeAsyncFrameMarker(
    IUnknown* queue,
    NV_ASYNC_FRAME_MARKER_PARAMS* params) noexcept
{
    if (!s_orig_set_async_frame_marker) return NVAPI_OK;
    __try { return s_orig_set_async_frame_marker(queue, params); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return NVAPI_OK; }
}

static bool SafeReadAsyncFrameMarker(
    const NV_ASYNC_FRAME_MARKER_PARAMS* params,
    uint64_t* frame_id,
    uint32_t* marker_type,
    uint64_t* present_frame_id,
    uint8_t* vendor_internal) noexcept
{
    __try {
        *frame_id = params->frameID;
        *marker_type = params->markerType;
        *present_frame_id = params->presentFrameID;
        *vendor_internal = params->vendorInternal;
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The newest NvAPI_D3D_SetLatencyMarker frame ID of the game itself (not
// RTSS); the first SetAsyncFrameMarker call logs it to show that both use
// one frame-ID domain.
static std::atomic<uint64_t> s_newest_app_marker_frame{0};

// ── Reflex observation (diagnostics only) ──
// What the game and Streamline ask of Reflex and what the driver answers: the
// observation hooks (below, with Hook_SetFlipConfig), the thread map and the
// sleep status poll read arguments and results and log them, with line limits.
// Nothing here changes a parameter, a result, the order or the timing of a
// forwarded call. Every line starts with "Reflex observe: ".

// File name of the module that contains the return address ("unknown" when
// none does). Called only when a line is logged; the hook passes its own
// _ReturnAddress().
static void CallerModuleName(void* return_address, char* out, size_t size) noexcept {
    char path[MAX_PATH] = {};
    HMODULE caller = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(return_address), &caller) && caller != nullptr &&
        GetModuleFileNameA(caller, path, MAX_PATH) != 0) {
        const char* name = std::strrchr(path, '\\');
        std::snprintf(out, size, "%s", name ? name + 1 : path);
    } else {
        std::snprintf(out, size, "unknown");
    }
}

// Lower-case hex of the bytes when any of them is non-zero (returns true),
// else an empty string (returns false). `size` includes the terminator.
static bool ReservedBytesHex(const uint8_t* bytes, size_t count, char* out, size_t size) noexcept {
    static constexpr char digits[] = "0123456789abcdef";
    out[0] = '\0';
    bool any = false;
    for (size_t i = 0; i < count; ++i) any = any || bytes[i] != 0;
    if (!any) return false;
    size_t pos = 0;
    for (size_t i = 0; i < count && pos + 2 < size; ++i) {
        out[pos++] = digits[bytes[i] >> 4];
        out[pos++] = digits[bytes[i] & 15];
    }
    out[pos] = '\0';
    return true;
}

// FNV-1a over fixed-width fields: the change signature of a logged tuple.
static constexpr uint64_t kObserveHashSeed = 14695981039346656037ull;
static uint64_t ObserveHash(uint64_t hash, const void* data, size_t size) noexcept {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

// One line counter per log: true while the line is within the limit; the first
// refusal logs one line that later lines are dropped.
static bool ReflexObserveBudget(std::atomic<uint32_t>& lines, uint32_t limit, const char* what) noexcept {
    const uint32_t line = lines.fetch_add(1, std::memory_order_relaxed);
    if (line < limit) return true;
    if (line == limit)
        LOG_INFO("Reflex observe: %s; later lines are not logged (limit %u lines)", what, limit);
    return false;
}

static NvAPI_Status SafeInvokeGetSleepStatus(
    IUnknown* device,
    NV_GET_SLEEP_STATUS_PARAMS* params) noexcept
{
    if (!s_orig_get_sleep_status) return NVAPI_ERROR;
    __try { return s_orig_get_sleep_status(device, params); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return NVAPI_ERROR; }
}

// Change detection of the driver's sleep status, for the game's own calls and
// for ReLimiter's poll (one tracker each). sleepIntervalUs varies: a change of
// less than 5% of the last logged value is no change.
struct SleepStatusTracker {
    std::atomic<uint64_t> signature{UINT64_MAX};
    std::atomic<uint32_t> interval_us{0};
    std::atomic<uint32_t> lines{0};

    // True for the first result and whenever the status, a flag, the version,
    // the multiplier or a reserved byte changes, or the interval by 5% or more.
    bool Changed(NvAPI_Status status, const NV_GET_SLEEP_STATUS_PARAMS& s) noexcept {
        uint64_t hash = kObserveHashSeed;
        hash = ObserveHash(hash, &status, sizeof(status));
        hash = ObserveHash(hash, &s.version, sizeof(s.version));
        hash = ObserveHash(hash, &s.bLowLatencyMode, sizeof(s.bLowLatencyMode));
        hash = ObserveHash(hash, &s.bFsVrr, sizeof(s.bFsVrr));
        hash = ObserveHash(hash, &s.bCplVsyncOn, sizeof(s.bCplVsyncOn));
        hash = ObserveHash(hash, &s.bUseGameSleep, sizeof(s.bUseGameSleep));
        hash = ObserveHash(hash, &s.bFullscreenIFlip, sizeof(s.bFullscreenIFlip));
        hash = ObserveHash(hash, &s.fgMultiplier, sizeof(s.fgMultiplier));
        hash = ObserveHash(hash, s.rsvd, sizeof(s.rsvd));
        const bool fields_changed = signature.exchange(hash, std::memory_order_relaxed) != hash;
        const uint32_t last = interval_us.load(std::memory_order_relaxed);
        const uint64_t diff = s.sleepIntervalUs > last ? s.sleepIntervalUs - last : last - s.sleepIntervalUs;
        const bool interval_changed = diff != 0 && diff * 20u >= last;
        if (fields_changed || interval_changed) interval_us.store(s.sleepIntervalUs, std::memory_order_relaxed);
        return fields_changed || interval_changed;
    }
};

// `source` is "game call" or "poll"; `module` is the game call's caller.
static void LogSleepStatus(const char* source, NvAPI_Status status, const NV_GET_SLEEP_STATUS_PARAMS& s,
    const char* module) noexcept {
    char hex[2 * sizeof(s.rsvd) + 1];
    char rsvd_text[sizeof(hex) + 8] = "";
    if (ReservedBytesHex(s.rsvd, sizeof(s.rsvd), hex, sizeof(hex)))
        std::snprintf(rsvd_text, sizeof(rsvd_text), " rsvd=%s", hex);
    char module_text[MAX_PATH + 16] = "";
    if (module) std::snprintf(module_text, sizeof(module_text), " module=%s", module);
    LOG_INFO("Reflex observe: GetSleepStatus %s status=%d version=0x%08x low_latency=%u fs_vrr=%u cpl_vsync=%u "
             "interval=%u us use_game_sleep=%u fullscreen_iflip=%u fg_multiplier=%u%s%s thread=%lu",
             source, status, s.version, s.bLowLatencyMode ? 1u : 0u, s.bFsVrr ? 1u : 0u,
             s.bCplVsyncOn ? 1u : 0u, s.sleepIntervalUs, s.bUseGameSleep ? 1u : 0u,
             s.bFullscreenIFlip ? 1u : 0u, static_cast<unsigned>(s.fgMultiplier), rsvd_text, module_text,
             static_cast<unsigned long>(GetCurrentThreadId()));
}

// The driver's own sleep status, read by ReLimiter at most once per second from
// Hook_Sleep (the presenting device, under developer_settings), through the
// trampoline: the game's hook never sees it. The only driver call this
// observation adds. The developer key sleep_status_every_frame reads it at
// every Sleep instead, so the pacing CSV's driver_sleep_interval_us follows the
// driver's interval frame by frame.
static constexpr uint32_t kPolledSleepStatusLogLimit = 64;
static SleepStatusTracker s_polled_sleep_status;
static std::atomic<ULONGLONG> s_sleep_status_poll_ms{0};

// The newest successful result of NvAPI_D3D_GetSleepStatus, from the game's
// call or from the poll (g_sleep_status_newest).
static void RecordSleepStatus(NvAPI_Status result, const NV_GET_SLEEP_STATUS_PARAMS& status,
    uint32_t publication_token) noexcept {
    if (result != NVAPI_OK) return;
    // A query that straddled a publication change belongs to the old publication.
    if (publication_token != g_sleep_status_token.load(std::memory_order_acquire)) return;
    g_sleep_status_newest.store(SleepStatusWord(status.sleepIntervalUs, status.bUseGameSleep != 0, publication_token),
        std::memory_order_relaxed);
}

// The token of a feedback-device publication (g_sleep_status_token's value while it is
// the current one): the publication a retained or acquired identity belongs to.
static uint32_t PublicationToken(const NvAPIFeedbackDevice& identity) noexcept {
    return static_cast<uint32_t>((identity.publication_sequence >> 1) & kSleepStatusTokenMask);
}

// The newest result when the presenting device's Sleep returns: the next
// source record carries it (g_sleep_status_at_sleep_return). `publication_token` is
// the token of the publication the Sleep retained at entry
// (PublicationToken(sleep_device.identity)): the result is carried only when it
// belongs to that publication and that publication is still the current one.
static void PublishSleepStatusAtSleepReturn(uint32_t publication_token) noexcept {
    const uint64_t newest = g_sleep_status_newest.load(std::memory_order_relaxed);
    const bool current = (newest >> 63) != 0 && SleepStatusToken(newest) == publication_token &&
        publication_token == g_sleep_status_token.load(std::memory_order_acquire);
    g_sleep_status_at_sleep_return.store(current ? newest : 0, std::memory_order_relaxed);
}

static void PollSleepStatus(IUnknown* device, uint32_t publication_token) noexcept {
    if (!g_config.developer_settings || !s_orig_get_sleep_status) return;
    if (!g_sleep_status_every_frame.load(std::memory_order_relaxed)) {
        const ULONGLONG now = GetTickCount64();
        ULONGLONG next = s_sleep_status_poll_ms.load(std::memory_order_relaxed);
        if (now < next ||
            !s_sleep_status_poll_ms.compare_exchange_strong(next, now + 1000, std::memory_order_relaxed))
            return;
    }
    // The token is the retained Sleep device's (passed in), not whatever publication is
    // current now: RecordSleepStatus drops the result when that lifetime has ended.
    NV_GET_SLEEP_STATUS_PARAMS status{};
    status.version = static_cast<NvU32>(NV_GET_SLEEP_STATUS_PARAMS_VER);
    const NvAPI_Status result = SafeInvokeGetSleepStatus(device, &status);
    RecordSleepStatus(result, status, publication_token);
    if (s_polled_sleep_status.Changed(result, status) &&
        ReflexObserveBudget(s_polled_sleep_status.lines, kPolledSleepStatusLogLimit, "sleep status poll changed again"))
        LogSleepStatus("poll", result, status, nullptr);
}

// Thread map: the thread of the newest call of the Reflex Sleep, of each
// latency marker type and of each async marker type (with its queue), and
// the Sleeps that did not follow the frame pattern. Relaxed atomics only; the
// Present is left out (no accessor of the game Present's thread exists).
static constexpr uint32_t kObserveMarkerTypes = 13;   // NV_LATENCY_MARKER_TYPE 0..12
static constexpr uint64_t kThreadMapAfterSleeps = 600;
static constexpr uint32_t kThreadMapLogLimit = 16;
struct ReflexThreadMap {
    std::atomic<uint32_t> sleep_thread{0};
    std::atomic<uint32_t> marker_thread[kObserveMarkerTypes]{};
    std::atomic<uint32_t> async_thread[kObserveMarkerTypes]{};
    std::atomic<uint64_t> async_queue[kObserveMarkerTypes]{};
    std::atomic<uint64_t> sleep_calls{0};
    // Sleeps since the newest SIMULATION_START: a second one is two Sleeps in one frame.
    std::atomic<uint64_t> sleeps_since_simulation_start{0};
    std::atomic<uint64_t> double_sleeps{0};
    std::atomic<uint64_t> frames_without_sleep{0};
    std::atomic<uint64_t> logged_signature{0};
    std::atomic<uint32_t> lines{0};
};
static ReflexThreadMap s_reflex_threads;

static uint64_t ThreadMapSignature() noexcept {
    uint64_t hash = kObserveHashSeed;
    const uint32_t sleep_thread = s_reflex_threads.sleep_thread.load(std::memory_order_relaxed);
    hash = ObserveHash(hash, &sleep_thread, sizeof(sleep_thread));
    for (uint32_t type = 0; type < kObserveMarkerTypes; ++type) {
        const uint32_t marker = s_reflex_threads.marker_thread[type].load(std::memory_order_relaxed);
        const uint32_t async = s_reflex_threads.async_thread[type].load(std::memory_order_relaxed);
        const uint64_t queue = s_reflex_threads.async_queue[type].load(std::memory_order_relaxed);
        hash = ObserveHash(hash, &marker, sizeof(marker));
        hash = ObserveHash(hash, &async, sizeof(async));
        hash = ObserveHash(hash, &queue, sizeof(queue));
    }
    return hash;
}

static void LogThreadMap() noexcept {
    char markers[224] = "";
    char async[544] = "";
    size_t markers_length = 0, async_length = 0;
    for (uint32_t type = 0; type < kObserveMarkerTypes; ++type) {
        const uint32_t marker = s_reflex_threads.marker_thread[type].load(std::memory_order_relaxed);
        if (marker != 0)
            markers_length += std::snprintf(markers + markers_length, sizeof(markers) - markers_length,
                "%s%u:%u", markers_length ? "," : "", type, marker);
        const uint32_t async_thread = s_reflex_threads.async_thread[type].load(std::memory_order_relaxed);
        if (async_thread != 0)
            async_length += std::snprintf(async + async_length, sizeof(async) - async_length,
                "%s%u:%u@0x%llx", async_length ? "," : "", type, async_thread,
                static_cast<unsigned long long>(s_reflex_threads.async_queue[type].load(std::memory_order_relaxed)));
    }
    LOG_INFO("Reflex observe: threads sleep=%u markers={%s} async={%s} double_sleeps=%llu "
             "frames_without_sleep=%llu sleep_calls=%llu",
             s_reflex_threads.sleep_thread.load(std::memory_order_relaxed), markers, async,
             static_cast<unsigned long long>(s_reflex_threads.double_sleeps.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(s_reflex_threads.frames_without_sleep.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(s_reflex_threads.sleep_calls.load(std::memory_order_relaxed)));
}

// A Sleep on the presenting device (Hook_Sleep). One line after the first 600
// Sleeps and again whenever a recorded thread or async queue changes.
static void ObserveReflexSleep() noexcept {
    s_reflex_threads.sleep_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (s_reflex_threads.sleeps_since_simulation_start.fetch_add(1, std::memory_order_relaxed) != 0)
        s_reflex_threads.double_sleeps.fetch_add(1, std::memory_order_relaxed);
    const uint64_t calls = s_reflex_threads.sleep_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (calls < kThreadMapAfterSleeps) return;
    const uint64_t signature = ThreadMapSignature();
    const bool first = calls == kThreadMapAfterSleeps;
    if (!first && s_reflex_threads.logged_signature.load(std::memory_order_relaxed) == signature) return;
    if (s_reflex_threads.logged_signature.exchange(signature, std::memory_order_relaxed) == signature && !first)
        return; // another thread logged it first
    if (ReflexObserveBudget(s_reflex_threads.lines, kThreadMapLogLimit, "thread map changed again"))
        LogThreadMap();
}

// A latency marker of the game on the presenting device (Hook_SetLatencyMarker).
static void ObserveLatencyMarkerThread(uint32_t type) noexcept {
    if (type >= kObserveMarkerTypes) return;
    s_reflex_threads.marker_thread[type].store(GetCurrentThreadId(), std::memory_order_relaxed);
    // A SIMULATION_START with no Sleep since the previous one: a frame without a Sleep.
    if (type == SIMULATION_START &&
        s_reflex_threads.sleeps_since_simulation_start.exchange(0, std::memory_order_relaxed) == 0)
        s_reflex_threads.frames_without_sleep.fetch_add(1, std::memory_order_relaxed);
}

static void ObserveAsyncMarkerThread(uint32_t type, IUnknown* queue) noexcept {
    if (type >= kObserveMarkerTypes) return;
    s_reflex_threads.async_thread[type].store(GetCurrentThreadId(), std::memory_order_relaxed);
    s_reflex_threads.async_queue[type].store(reinterpret_cast<uint64_t>(queue), std::memory_order_relaxed);
}

// RenderSubmitEnd(N) and SimulationStart(N+1) can be emitted concurrently.
// Tail diagnostics must not take the single scheduler owner and make the
// actual enforcement marker fail admission. Only the two possible release
// markers need immediate scheduler ownership. PresentStart runs its output
// executor lock-free on the exact plan key and queues its marker-log entry.
static bool IsImmediateTimingMarker(NV_LATENCY_MARKER_TYPE type) noexcept {
    return type == SIMULATION_START || type == RENDERSUBMIT_START;
}

struct DeferredNvApiMarker {
    uint64_t frame_id = 0;
    int64_t timestamp_qpc = 0;
    uint32_t producer_epoch = 0;
    uint32_t controller_generation = 0;
    NV_LATENCY_MARKER_TYPE type = SIMULATION_END;
};

static constexpr size_t kDeferredMarkerCapacity = 256;
static relimiter::BoundedMpscQueue<DeferredNvApiMarker, kDeferredMarkerCapacity>
    s_deferred_markers;

void NvAPI_RecordCpuLatency(uint64_t frame_id, int64_t render_submit_end_qpc) noexcept {
    // SIM_START -> RENDERSUBMIT_END of the same frame, EMA alpha 0.16. The
    // marker log is the frame-ID-keyed ring holding the SIM_START timestamp.
    const int64_t simulation_start_qpc = g_marker_log.Get(frame_id, SIMULATION_START);
    if (simulation_start_qpc == 0) return;
    const double latency_us = qpc_to_us(render_submit_end_qpc - simulation_start_qpc);
    if (!(latency_us > 0.0 && latency_us < 500000.0)) return;
    const double previous = g_cpu_latency_us.load(std::memory_order_relaxed);
    g_cpu_latency_us.store(previous > 0.0 ? previous + 0.16 * (latency_us - previous) : latency_us,
        std::memory_order_relaxed);
}

static void DrainDeferredMarkers(
    NvApiMarkerPolicy* policy,
    uint32_t producer_epoch) noexcept {
    const uint32_t generation = FrameTimeline_CurrentControllerGeneration();
    DeferredNvApiMarker observation{};
    // The owner alone mutates the marker log and policy state. A busy producer
    // or full queue may lose diagnostics, never wait for or displace enforcement.
    for (size_t count = 0; count < kDeferredMarkerCapacity; ++count) {
        if (!s_deferred_markers.Pop(&observation)) break;
        if (observation.producer_epoch != producer_epoch ||
            observation.controller_generation != generation ||
            producer_epoch != FrameTimeline_CurrentProducerEpoch(
                FrameIdentityDomain::NvApiFrame) ||
            generation != FrameTimeline_CurrentControllerGeneration()) continue;
        if (!policy->ShouldProcess(false, observation.type, observation.frame_id))
            continue;
        // Use the source capture timestamp, not the later dequeue time. Do not
        // refresh marker health here: queued history is not live source activity.
        g_marker_log.Record(observation.type, observation.timestamp_qpc,
                            observation.frame_id);
        // The PCL hook owns the CPU-latency readout while PCL markers flow.
        if (observation.type == RENDERSUBMIT_END && !PCL_MarkersFlowing())
            NvAPI_RecordCpuLatency(observation.frame_id, observation.timestamp_qpc);
    }
}

// SetSleepMode on the presenting device with the game's captured parameters:
// minimumIntervalUs replaced when `interval_us` is given (an explicit 0 is forwarded, the
// unified owner's), and bLowLatencyMode and bLowLatencyBoost cleared when `low_latency_off`.
// Only this copy changes; the captured parameters stay the game's, and
// `game_params` (optional) receives them as captured, `publication` (optional) the
// publication they belong to. The presenting device must be the one they belong to.
static DriverLimiterApply ApplyCapturedSleepMode(
    const uint32_t* interval_us, bool low_latency_off,
    NV_SET_SLEEP_MODE_PARAMS* game_params = nullptr,
    uint64_t* publication = nullptr, bool nonblocking = false) noexcept {
    if (!s_orig_sleep_mode) return DriverLimiterApply::Failed;
    NV_SET_SLEEP_MODE_PARAMS params{};
    NvAPIFeedbackDevice params_identity{};
    if (nonblocking) {
        // The reconcile on the Sleep and marker paths: one attempt at the captured
        // parameters; contention is Busy (pending), never a spin.
        TryGameParamsGuard lock;
        if (!lock) return DriverLimiterApply::Busy;
        if (!s_game_params_captured) return DriverLimiterApply::NoGameParams;
        params = s_game_original_params;
        params_identity = s_game_params_identity;
    } else {
        GameParamsGuard lock;
        if (!s_game_params_captured) return DriverLimiterApply::NoGameParams;
        params = s_game_original_params;
        params_identity = s_game_params_identity;
    }
    if (game_params) *game_params = params;
    if (publication) *publication = params_identity.publication_sequence;
    if (interval_us) params.minimumIntervalUs = *interval_us;
    if (low_latency_off) {
        params.bLowLatencyMode = 0;
        params.bLowLatencyBoost = 0;
    }
    const NvAPIFeedbackDevice feedback_device = NvAPI_AcquireFeedbackDevice();
    if (!feedback_device.device) return DriverLimiterApply::NoDevice;
    DriverLimiterApply result = DriverLimiterApply::NoDevice;
    if (NvAPI_IsSameFeedbackPublication(params_identity, feedback_device) &&
        NvAPI_IsFeedbackDeviceCurrent(feedback_device)) {
        result = SafeInvokeSetSleepMode(feedback_device.device, &params) == NVAPI_OK
            ? DriverLimiterApply::Applied
            : DriverLimiterApply::Failed;
    }
    NvAPI_ReleaseFeedbackDevice(feedback_device.device);
    return result;
}

// Moves the driver's low latency mode with the Reflex Sleep: off while it is
// bypassed with bypass_low_latency_off, off while it is forwarded with
// driver_low_latency_off, the game's own otherwise. Called from Hook_Sleep on
// the presenting device. The state changes only after a successful driver
// call; a thread that loses the try-lock, a missing game parameter set and a
// failed call all retry on a later Sleep (so a key switched at run time
// re-applies the mode at the next Sleep). A state that already matches costs a
// few atomic loads. The call carries the Reflex driver limiter's interval while
// it is applied: the limiter must survive the change.
static void UpdateLowLatencyOverride(bool bypass) noexcept {
    const bool want = bypass ? g_bypass_low_latency_off.load(std::memory_order_relaxed)
                             : g_driver_low_latency_off.load(std::memory_order_relaxed);
    if (want == s_low_latency_forced_off.load(std::memory_order_acquire)) return;
    if (!TryAcquireSRWLockExclusive(&s_sleep_mode_forward_lock)) return;
    if (want != s_low_latency_forced_off.load(std::memory_order_acquire)) {
        NV_SET_SLEEP_MODE_PARAMS game{};
        uint32_t limiter_us = g_driver_limiter_interval_us.load(std::memory_order_relaxed);
        if (ApplyCapturedSleepMode(limiter_us != 0 ? &limiter_us : nullptr, want, &game) ==
            DriverLimiterApply::Applied) {
            s_low_latency_forced_off.store(want, std::memory_order_release);
            if (want)
                LOG_INFO("NvAPI: driver low latency mode off while the Reflex Sleep is %s "
                         "(game requested low_latency=%u boost=%u)", bypass ? "bypassed" : "forwarded",
                    game.bLowLatencyMode ? 1u : 0u, game.bLowLatencyBoost ? 1u : 0u);
            else
                LOG_INFO("NvAPI: driver low latency mode back to the game's (low_latency=%u boost=%u)",
                    game.bLowLatencyMode ? 1u : 0u, game.bLowLatencyBoost ? 1u : 0u);
        }
    }
    ReleaseSRWLockExclusive(&s_sleep_mode_forward_lock);
}

// ── Unified pacer: the Sleep-mode tuple ──
static const char* SleepOwnerText(UnifiedSleepOwner owner) noexcept {
    switch (owner) {
    case UnifiedSleepOwner::Unified: return "unified";
    case UnifiedSleepOwner::Limiter: return "limiter";
    case UnifiedSleepOwner::LowLatencyOff: return "low_latency_off";
    default: return "game";
    }
}

// Whether ReLimiter's own wait paces the game's source, for the bypass decision. In the
// window between the key publication and ResolvePipeline the published placement is still
// Unified, which Scheduler_PacesSource does not list: while a reconcile is pending the
// placement is read too (settled and key off the flag is false and nothing more is read).
static bool SleepSourcePaced() noexcept {
    if (Scheduler_PacesSource()) return true;
    return NvAPI_SleepModeReconcilePending() &&
        PacingPipeline_Read().placement == PacingSourcePlacement::Unified;
}

// The Sleep bypass state, for the callers without a Sleep in hand (OnMarker). The key-on
// rows of the tuple ignore it, so it is not read then.
static bool CurrentSleepBypass() noexcept {
    if (PacingPipeline_UnifiedPacerKey()) return false;
    return NvAPI_ShouldBypassSleep(g_reflex_sleep_bypass.load(std::memory_order_relaxed),
        g_user_target_fps.load(std::memory_order_relaxed), SleepSourcePaced());
}

// The desired tuple (UnifiedPacer_SleepModeDesired) for these game parameters, under this key.
static UnifiedSleepModeTuple DesiredSleepMode(
    bool key, bool bypass, uint64_t publication, uint32_t game_interval_us) noexcept {
    UnifiedSleepModeInputs inputs;
    inputs.key = key;
    inputs.dmfg = key && IsDmfgActive(); // only the key-on rows depend on it
    inputs.bypass = bypass;
    inputs.bypass_low_latency_off = g_bypass_low_latency_off.load(std::memory_order_relaxed);
    inputs.driver_low_latency_off = g_driver_low_latency_off.load(std::memory_order_relaxed);
    inputs.limiter_interval_us = g_driver_limiter_interval_us.load(std::memory_order_relaxed);
    inputs.game_interval_us = game_interval_us;
    inputs.publication = publication;
    inputs.epoch = s_sleep_mode_epoch.load(std::memory_order_acquire);
    return UnifiedPacer_SleepModeDesired(inputs);
}

// What the driver had before a forward, for the transition log (forward lock held).
static UnifiedSleepModeTuple PreviousSleepMode() noexcept {
    UnifiedSleepModeTuple before = s_applied_sleep_valid ? s_applied_sleep_mode : UnifiedSleepModeTuple{};
    before.low_latency_off = s_low_latency_forced_off.load(std::memory_order_acquire);
    return before;
}

// One line per change of owner or of the driver's low latency mode.
static void LogSleepModeTransition(const UnifiedSleepModeTuple& before,
    const UnifiedSleepModeTuple& after, const NV_SET_SLEEP_MODE_PARAMS& game) noexcept {
    if (before.owner == after.owner && before.low_latency_off == after.low_latency_off) return;
    LOG_INFO("NvAPI: Sleep mode %s -> %s (driver low latency %s, interval %u us; game requested "
             "low_latency=%u boost=%u interval=%u us)",
        SleepOwnerText(before.owner), SleepOwnerText(after.owner), after.low_latency_off ? "off" : "the game's",
        after.interval_us, game.bLowLatencyMode ? 1u : 0u, game.bLowLatencyBoost ? 1u : 0u,
        game.minimumIntervalUs);
}

// The desired tuple for the game parameters captured now (nothing captured: publication 0),
// under the key published now. One attempt at the captured parameters: false on contention
// (the reconcile stays pending, the caller never spins).
static bool DesiredForCapturedParams(bool bypass, UnifiedSleepModeTuple* desired, bool* key,
    uint32_t* generation) noexcept {
    // The generation before the key: the publication advances it before it exposes the key.
    *generation = s_key_generation.load(std::memory_order_acquire);
    uint64_t publication = 0;
    uint32_t game_interval_us = 0;
    {
        TryGameParamsGuard lock;
        if (!lock) return false;
        if (s_game_params_captured) {
            publication = s_game_params_identity.publication_sequence;
            game_interval_us = s_game_original_params.minimumIntervalUs;
        }
    }
    *key = PacingPipeline_UnifiedPacerKey();
    // The key the configuration is publishing is noted first (NvAPI_PrepareUnifiedKey): while
    // it differs from the published one a transition is in flight and nothing is forwarded
    // for either side of it. The reconcile stays pending.
    if (((s_transition_state.load(std::memory_order_acquire) & kTransitionKey) != 0) != *key) return false;
    *desired = DesiredSleepMode(*key, bypass, publication, game_interval_us);
    return true;
}

// NvAPI_ReconcileSleepMode under the forward lock: the desired tuple against the applied
// record, field by field (a matching low latency boolean alone is not a match; a record
// that is not valid is a difference). On a difference the captured game parameters go to
// the driver with low latency and boost cleared when off and the interval pointer &0
// (Unified), &limiter (Limiter) or nullptr (otherwise); only Applied settles it (key on:
// the record; key off: the key-off row is restored and nothing is recorded), anything else
// stays pending.
static void ReconcileSleepModeLocked(const UnifiedSleepModeTuple& desired, bool key, uint32_t generation) noexcept {
    if (s_applied_sleep_valid && s_applied_key.load(std::memory_order_relaxed) == key &&
        s_applied_generation == generation && desired == s_applied_sleep_mode) return;
    const uint32_t* interval = desired.owner == UnifiedSleepOwner::Unified ||
        desired.owner == UnifiedSleepOwner::Limiter ? &desired.interval_us : nullptr;
    NV_SET_SLEEP_MODE_PARAMS game{};
    uint64_t applied_publication = 0;
    // A tuple that is not the game's own is owed a restoration from the moment it may reach
    // the driver (the key may go off while the call is in flight).
    const bool was_owed = (s_transition_state.load(std::memory_order_acquire) & kTransitionOwed) != 0;
    if (key && desired.owner != UnifiedSleepOwner::Game)
        s_transition_state.fetch_or(kTransitionOwed, std::memory_order_acq_rel);
    const DriverLimiterApply result =
        ApplyCapturedSleepMode(interval, desired.low_latency_off, &game, &applied_publication, true);
    if (result != DriverLimiterApply::Applied) {
        // Nothing reached the driver: without captured parameters there is nothing to
        // restore either (a key on and off in such a game settles without a forward); a
        // contended or device-less attempt leaves what was owed before it, no more. A failed
        // driver call stays owed (it may have been taken).
        if (result == DriverLimiterApply::NoGameParams ||
            ((result == DriverLimiterApply::Busy || result == DriverLimiterApply::NoDevice) && !was_owed))
            s_transition_state.fetch_and(~kTransitionOwed, std::memory_order_acq_rel);
        return;
    }
    UnifiedSleepModeTuple applied = desired;
    applied.publication = applied_publication; // what the call actually carried
    if (interval == nullptr) applied.interval_us = game.minimumIntervalUs;
    const UnifiedSleepModeTuple before = PreviousSleepMode();
    if (key) StoreAppliedSleepMode(applied, generation);
    else SettleKeyOffSleepMode();
    s_low_latency_forced_off.store(desired.low_latency_off, std::memory_order_release);
    LogSleepModeTransition(before, applied, game);
}

// Never blocks: a held forward lock or captured parameters held by another thread leave it
// pending for the next call.
static void ReconcileSleepMode(bool bypass) noexcept {
    UnifiedSleepModeTuple desired;
    bool key = false;
    uint32_t generation = 0;
    if (!DesiredForCapturedParams(bypass, &desired, &key, &generation)) return;
    // Nothing to do (the usual case under the key): no lock.
    if (AppliedSleepModeMatches(desired, key, generation)) return;
    if (!TryAcquireSRWLockExclusive(&s_sleep_mode_forward_lock)) return;
    ReconcileSleepModeLocked(desired, key, generation);
    ReleaseSRWLockExclusive(&s_sleep_mode_forward_lock);
}

// The scheduler's bump (key toggles and Dynamic MFG transitions). The epoch is compared only
// inside the key-on tuple, so a bump with the key on forces a forward and a bump with the key
// off changes nothing: a restoration in flight satisfies its transition exactly once, whether
// the bump lands before, during or after it, and no real change is swallowed.
void NvAPI_BumpSleepModeEpoch() noexcept {
    s_sleep_mode_epoch.fetch_add(1, std::memory_order_acq_rel);
    // The self-check starts over (Hook_Sleep clears its ring at the next unified Sleep).
    g_unified_driver_pacing.store(false, std::memory_order_relaxed);
}

// Config_ApplyPacingPipeline, BEFORE PacingPipeline_SetConfig, with the key the clamp will
// publish: the key is noted (so every reader that can observe the new key already sees the
// reconcile pending) before the pipeline exposes it. An unchanged key is a load and a
// not-taken branch.
void NvAPI_PrepareUnifiedKey(bool clamped_key) noexcept {
    if (((s_transition_state.load(std::memory_order_relaxed) & kTransitionKey) != 0) == clamped_key) return;
    // An actual key change, either direction: a new generation first (a record from the earlier
    // state of this key can never match again), then the key bit.
    s_key_generation.fetch_add(1, std::memory_order_acq_rel);
    if (clamped_key) s_transition_state.fetch_or(kTransitionKey, std::memory_order_acq_rel);
    else s_transition_state.fetch_and(~kTransitionKey, std::memory_order_acq_rel);
}

bool NvAPI_SleepModeReconcilePending() noexcept {
    return s_transition_state.load(std::memory_order_acquire) != 0;
}

void NvAPI_ReconcileSleepMode() noexcept {
    ReconcileSleepMode(CurrentSleepBypass());
}

// Hook_SetSleepMode under the forward lock, key on or an epoch pending: the desired tuple
// built from the game's NEW parameters goes to the driver and, once accepted, is the
// applied record. The owner Game forwards the game's own pointer unchanged.
static NvAPI_Status ForwardDesiredSleepMode(IUnknown* device, NV_SET_SLEEP_MODE_PARAMS* original,
    const NV_SET_SLEEP_MODE_PARAMS& snapshot, const NvAPIFeedbackDevice& identity) noexcept {
    const uint32_t generation = s_key_generation.load(std::memory_order_acquire);
    const bool key = PacingPipeline_UnifiedPacerKey();
    const UnifiedSleepModeTuple desired = DesiredSleepMode(
        key, CurrentSleepBypass(), identity.publication_sequence, snapshot.minimumIntervalUs);
    NvAPI_Status result = NVAPI_OK;
    // As in the reconcile: owed from the moment the tuple may reach the driver.
    if (key && desired.owner != UnifiedSleepOwner::Game)
        s_transition_state.fetch_or(kTransitionOwed, std::memory_order_acq_rel);
    if (desired.owner == UnifiedSleepOwner::Game) {
        result = SafeInvokeSetSleepMode(device, original);
    } else {
        NV_SET_SLEEP_MODE_PARAMS copy = snapshot;
        if (desired.owner == UnifiedSleepOwner::Unified || desired.owner == UnifiedSleepOwner::Limiter)
            copy.minimumIntervalUs = desired.interval_us;
        if (desired.low_latency_off) {
            copy.bLowLatencyMode = 0;
            copy.bLowLatencyBoost = 0;
        }
        result = SafeInvokeSetSleepMode(device, &copy);
    }
    if (result == NVAPI_OK) {
        const UnifiedSleepModeTuple before = PreviousSleepMode();
        if (key) StoreAppliedSleepMode(desired, generation);
        else SettleKeyOffSleepMode();
        s_low_latency_forced_off.store(desired.low_latency_off, std::memory_order_release);
        LogSleepModeTransition(before, desired, snapshot);
    }
    return result;
}

// ── Unified pacer: the Sleep hook's start wait and self-check ──
// The hard limit's wait on a change of the ready index, written against a small set of
// primitives so a scripted clock can drive it: never a wait beyond the absolute deadline.
// The coarse wait (WaitOnAddress) asks for whole milliseconds rounded DOWN, so it sits inside
// the remaining budget; a change of the index returns at once; what is left (less than a
// millisecond) is a bounded precise tail (yield loop, stopped by a change or the deadline).
// A budget over one second waits one second and returns (the limit loop calls again).
class UnifiedWaitPrimitives {
public:
    virtual int64_t Now() noexcept = 0;
    virtual int64_t Frequency() noexcept = 0;
    virtual uint64_t Index() noexcept = 0;
    virtual void CoarseWait(uint64_t observed, uint32_t milliseconds) noexcept = 0;
    virtual void Relax() noexcept = 0;

protected:
    ~UnifiedWaitPrimitives() = default;
};

constexpr int64_t kUnifiedCoarseWaitMaxMs = 1000;
constexpr int kUnifiedCoarseWaitAttempts = 8; // early returns tolerated before the tail takes over

// WaitOnAddress may return early without a change: the remaining time is recomputed and the
// coarse wait repeated while at least a millisecond remains (same absolute deadline); only
// the final sub-millisecond is spun. After eight early returns the tail takes over and spins
// the wait's remaining time to the same absolute deadline, never past it. That remainder is
// bounded by the interval: the hard limit's deadline is max(T, E) + I(n), so one call spins at
// most I(n) (and the limit never waits again after its deadline). Reaching eight early returns
// needs a primitive that misbehaves repeatedly; the bound only caps what it can cost.
static void UnifiedWaitForChange(UnifiedWaitPrimitives& primitives, uint64_t observed, int64_t deadline_qpc) noexcept {
    const int64_t frequency = primitives.Frequency();
    for (int attempt = 0; attempt < kUnifiedCoarseWaitAttempts; ++attempt) {
        const int64_t remaining = deadline_qpc - primitives.Now();
        if (remaining <= 0) return;
        const int64_t milliseconds = frequency > 0 ? remaining * 1000 / frequency : 0; // rounded down
        if (milliseconds < 1) break;
        const int64_t coarse = milliseconds > kUnifiedCoarseWaitMaxMs ? kUnifiedCoarseWaitMaxMs : milliseconds;
        primitives.CoarseWait(observed, static_cast<uint32_t>(coarse));
        if (milliseconds > kUnifiedCoarseWaitMaxMs || primitives.Index() != observed) return;
    }
    // The precise tail: the part of a millisecond the coarse waits left.
    while (primitives.Now() < deadline_qpc) {
        if (primitives.Index() != observed) return;
        primitives.Relax();
    }
}

// What the Sleep hook's wait needs beyond the hard limit's environment: the tick rate and one
// bounded wait toward a target (PreSleepWaitUntil in production). A scripted environment
// drives the whole flow in the tests.
class UnifiedSleepEnv : public UnifiedLimitEnv {
public:
    virtual int64_t Frequency() noexcept = 0;
    // Returns when the wait ended: at the target, or earlier (cancelled, failed, stopping).
    virtual void StartWait(int64_t target_qpc) noexcept = 0;

protected:
    ~UnifiedSleepEnv() = default;
};

// The production environment: QPC, g_up_ready_index (and the QPC of its newest completion)
// and WaitOnAddress on it, PreSleepWaitUntil for the start wait.
class UnifiedLimitClock final : public UnifiedSleepEnv, private UnifiedWaitPrimitives {
public:
    int64_t Now() noexcept override {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }
    // The largest completed grid index and the newest completion QPC of any frame: two
    // independent monotone maxima, read separately (no coherence needed).
    uint64_t ReadyIndex() noexcept override { return g_up_ready_index.load(std::memory_order_acquire); }
    int64_t ReadyQpc() noexcept override { return g_up_ready_qpc.load(std::memory_order_acquire); }
    void WaitForChange(uint64_t observed, int64_t deadline_qpc) noexcept override {
        UnifiedWaitForChange(*this, observed, deadline_qpc);
    }
    int64_t Frequency() noexcept override { return GetQPCFrequency(); }
    void StartWait(int64_t target_qpc) noexcept override { (void)PreSleepWaitUntil(target_qpc, true); }

private:
    uint64_t Index() noexcept override { return g_up_ready_index.load(std::memory_order_acquire); }
    void CoarseWait(uint64_t observed, uint32_t milliseconds) noexcept override {
        WaitOnAddress(&g_up_ready_index, &observed, sizeof(observed), milliseconds);
    }
    void Relax() noexcept override { YieldProcessor(); }
};

// Longest single start wait: PreSleepWaitUntil rejects more than 500 ms, and the law accepts
// any interval (a game limit of one second), so the wait to T runs in chunks.
constexpr int64_t kUnifiedStartWaitChunkDivisor = 4; // 250 ms

struct UnifiedSleepWaitResult {
    UnifiedSleepOutcome outcome = UnifiedSleepOutcome::None;
    int64_t release_qpc = 0;
    UnifiedLimitResult limit;
};

// The start wait to T first, in bounded chunks, ended early only by a cancellation, a failure
// or the shutdown (R < T: Failed); the hard limit at E, the moment after the wait reached T
// (the arrival when the frame is already late), never before T; R = the clock after it; the
// unfinished frames re-read at R (inactive stays -1).
static UnifiedSleepWaitResult UnifiedSleepWait(
    UnifiedSleepEnv& env, const SchedulerUnifiedReservation& reservation) noexcept {
    UnifiedSleepWaitResult result;
    const int64_t target = reservation.target_qpc;
    const int64_t frequency = env.Frequency();
    const int64_t chunk_ticks = frequency > 0 ? frequency / kUnifiedStartWaitChunkDivisor : 0;
    const bool wait_needed = target > env.Now();
    for (int64_t now = env.Now(); now < target; now = env.Now()) {
        const int64_t chunk_target = chunk_ticks > 0 && target - now > chunk_ticks ? now + chunk_ticks : target;
        env.StartWait(chunk_target);
        if (env.Now() < chunk_target) break; // cancelled, failed or stopping: R < T
    }
    result.limit.outstanding = -1;
    result.limit.ready_age_ticks = -1;
    if (env.Now() >= target) {
        const UnifiedLimitInput input{ reservation.grid_index, reservation.lifecycle_base, target,
            reservation.interval_ticks };
        result.limit = UnifiedHardLimit(input, &env);
    }
    result.release_qpc = env.Now();
    if (result.limit.outstanding >= 0)
        result.limit.outstanding = static_cast<int32_t>(UnifiedPacer_Outstanding(
            reservation.grid_index, env.ReadyIndex(), reservation.lifecycle_base));
    result.outcome = result.release_qpc < target ? UnifiedSleepOutcome::Failed
        : wait_needed ? UnifiedSleepOutcome::Completed : UnifiedSleepOutcome::Late;
    return result;
}

// Reserve (owner, nonblocking), the wait above, then the mailbox publication. A duplicate
// Sleep, a busy owner and an inactive pipeline wait for nothing and publish nothing: the
// Sleep is simply forwarded.
static void UnifiedSleepStart(int64_t arrival_qpc) noexcept {
    SchedulerUnifiedReservation reservation;
    if (Scheduler_UnifiedReserve(arrival_qpc, &reservation) != SchedulerUnifiedReserve::Reserved) return;
    UnifiedLimitClock clock;
    const UnifiedSleepWaitResult waited = UnifiedSleepWait(clock, reservation);
    Scheduler_UnifiedPublishSleep(reservation, waited.outcome, waited.release_qpc, arrival_qpc, waited.limit);
}

// Self-check on the genuine Sleep call: a driver that paces on its own (the control
// panel's low latency mode, a game limit) makes it last. A 64-entry ring of relaxed
// atomics (game thread) of the Sleeps over 200 us: "driver paces" at 16 or more, cleared
// below 3; one log line per transition, g_unified_driver_pacing for the flag and status.
static std::array<std::atomic<uint8_t>, 64> s_unified_sleep_long{};
static std::atomic<uint32_t> s_unified_sleep_head{ 0 };
static std::atomic<uint32_t> s_unified_check_epoch{ 0 };
constexpr double kUnifiedDriverPacesSleepUs = 200.0;
constexpr uint32_t kUnifiedDriverPacesSet = 16;
constexpr uint32_t kUnifiedDriverPacesClear = 3;

static void UnifiedSleepSelfCheck(int64_t duration_ticks) noexcept {
    const uint32_t epoch = s_sleep_mode_epoch.load(std::memory_order_relaxed);
    if (epoch != s_unified_check_epoch.load(std::memory_order_relaxed)) {
        // A key toggle or a Dynamic MFG transition: start over.
        for (auto& entry : s_unified_sleep_long) entry.store(0, std::memory_order_relaxed);
        s_unified_sleep_head.store(0, std::memory_order_relaxed);
        s_unified_check_epoch.store(epoch, std::memory_order_relaxed);
        g_unified_driver_pacing.store(false, std::memory_order_relaxed);
    }
    const uint32_t head = s_unified_sleep_head.fetch_add(1, std::memory_order_relaxed);
    s_unified_sleep_long[head % s_unified_sleep_long.size()].store(
        qpc_to_us(duration_ticks) > kUnifiedDriverPacesSleepUs ? 1 : 0, std::memory_order_relaxed);
    uint32_t count = 0;
    for (const auto& entry : s_unified_sleep_long) count += entry.load(std::memory_order_relaxed);
    const bool paces = g_unified_driver_pacing.load(std::memory_order_relaxed);
    if (!paces && count >= kUnifiedDriverPacesSet) {
        g_unified_driver_pacing.store(true, std::memory_order_relaxed);
        LOG_INFO("NvAPI: unified pacer: the driver's Sleep paces the game (%u of %zu Sleeps over %.0f us)",
            count, s_unified_sleep_long.size(), kUnifiedDriverPacesSleepUs);
    } else if (paces && count < kUnifiedDriverPacesClear) {
        g_unified_driver_pacing.store(false, std::memory_order_relaxed);
        LOG_INFO("NvAPI: unified pacer: the driver's Sleep no longer paces the game (%u of %zu Sleeps over %.0f us)",
            count, s_unified_sleep_long.size(), kUnifiedDriverPacesSleepUs);
    }
}

// ── Hook: SetSleepMode — capture game params, forward them (limiter interval and low latency override on a copy) ──
static NvAPI_Status __cdecl Hook_SetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* params) {
    void* const caller_address = _ReturnAddress();
    RuntimeCallbackGuard callback;
    if (callback.IsStopping())
        return SafeInvokeSetSleepMode(pDev, params);
    if (!IsPresentingFeedbackDevice(pDev))
        return SafeInvokeSetSleepMode(pDev, params);

    NV_SET_SLEEP_MODE_PARAMS params_snapshot{};
    const bool params_valid =
        SafeCopySleepModeParams(params, &params_snapshot);

    if (NvAPI_CaptureFeedbackDevice(pDev)) {
        static std::atomic<bool> s_logged_device_capture{ false };
        if (!s_logged_device_capture.exchange(true, std::memory_order_relaxed)) {
            LOG_INFO(
                "NvAPI: SetSleepMode first call — device captured, interval=%u us",
                params_valid ? params_snapshot.minimumIntervalUs : 0);
        }
    }

    // The game's Reflex mode decides whether its Sleep absorbs a GPU
    // backlog. Logged on change only (some games call this every frame).
    if (params_valid) {
        static std::atomic<uint64_t> s_logged_sleep_mode{ UINT64_MAX };
        static std::atomic<uint64_t> s_logged_sleep_mode_extra{ UINT64_MAX };
        const uint64_t word = (static_cast<uint64_t>(params_snapshot.minimumIntervalUs) << 8) |
            (params_snapshot.bLowLatencyMode ? 1u : 0u) |
            (params_snapshot.bLowLatencyBoost ? 2u : 0u) |
            (params_snapshot.bUseMarkersToOptimize ? 4u : 0u) |
            (params_snapshot.bUseMinQueueTime ? 8u : 0u);
        uint64_t extra = ObserveHash(kObserveHashSeed, &params_snapshot.version, sizeof(params_snapshot.version));
        extra = ObserveHash(extra, params_snapshot.rsvd, sizeof(params_snapshot.rsvd));
        const bool word_changed = s_logged_sleep_mode.exchange(word, std::memory_order_relaxed) != word;
        const bool extra_changed = s_logged_sleep_mode_extra.exchange(extra, std::memory_order_relaxed) != extra;
        if (word_changed || extra_changed) {
            char module[MAX_PATH];
            CallerModuleName(caller_address, module, sizeof(module));
            char hex[2 * sizeof(params_snapshot.rsvd) + 1];
            char rsvd_text[sizeof(hex) + 8] = "";
            if (ReservedBytesHex(params_snapshot.rsvd, sizeof(params_snapshot.rsvd), hex, sizeof(hex)))
                std::snprintf(rsvd_text, sizeof(rsvd_text), " rsvd=%s", hex);
            LOG_INFO("NvAPI: game SetSleepMode low_latency=%u boost=%u markers_optimize=%u interval=%u us "
                     "min_queue_time=%u version=0x%08x%s module=%s thread=%lu",
                params_snapshot.bLowLatencyMode ? 1u : 0u, params_snapshot.bLowLatencyBoost ? 1u : 0u,
                params_snapshot.bUseMarkersToOptimize ? 1u : 0u, params_snapshot.minimumIntervalUs,
                params_snapshot.bUseMinQueueTime ? 1u : 0u, params_snapshot.version, rsvd_text, module,
                static_cast<unsigned long>(GetCurrentThreadId()));
        }
    }

    // Bind the original settings only to the exact current feedback device.
    // Auxiliary device calls are still forwarded but cannot overwrite restore
    // state for the presenting game device.
    const FeedbackDeviceGuard params_device(pDev);
    if (params_valid && params_device.identity.device) {
        GameParamsGuard params_lock;
        // A may have waited here while B replaced/reset the restoration state.
        // This snapshot check is nonblocking: never acquire the device writer
        // lock under GameParamsGuard (writers take these locks in reverse order).
        if (NvAPI_IsFeedbackDeviceCurrent(params_device.identity)) {
            if (!NvAPI_IsSameFeedbackPublication(
                    s_game_params_identity, params_device.identity)) {
                s_game_original_params = {};
                s_game_params_identity = params_device.identity;
                s_game_params_captured = false;
                // Another device: the override was never applied to it.
                s_low_latency_forced_off.store(false, std::memory_order_release);
            }
            // The newest game parameters: the driver limiter applies
            // its interval on top of them and restores them.
            s_game_original_params = params_snapshot;
            s_game_params_captured = true;
            g_game_requested_interval.store(
                params_snapshot.minimumIntervalUs,
                std::memory_order_relaxed);
        }
    }

    // Forwarding is serialized with override transitions (above).
    AcquireSRWLockExclusive(&s_sleep_mode_forward_lock);
    NvAPI_Status result = NVAPI_OK;
    // While the Reflex driver limiter is applied, the game's own call
    // would clear it: forward a copy with ReLimiter's interval instead. The
    // game's parameters above stay captured for the restore.
    const uint32_t limiter_us = g_driver_limiter_interval_us.load(std::memory_order_relaxed);
    const bool low_latency_off = s_low_latency_forced_off.load(std::memory_order_acquire);
    if (params_valid && params_device.identity.device && NvAPI_SleepModeReconcilePending()) {
        // Key unified_pacer or a pending epoch: the desired tuple, built from the
        // game's new parameters, replaces the three branches below.
        result = ForwardDesiredSleepMode(pDev, params, params_snapshot, params_device.identity);
    } else if (limiter_us != 0 && params_valid && params_device.identity.device) {
        NV_SET_SLEEP_MODE_PARAMS limited = params_snapshot;
        limited.minimumIntervalUs = limiter_us;
        // driver_low_latency_off: the limiter's copy carries the override too.
        if (low_latency_off) {
            limited.bLowLatencyMode = 0;
            limited.bLowLatencyBoost = 0;
        }
        result = SafeInvokeSetSleepMode(pDev, &limited);
    } else if (params_valid && params_device.identity.device && low_latency_off) {
        // The low latency override (bypass_low_latency_off or
        // driver_low_latency_off) is applied to the driver: the game's own
        // call, which may carry low latency on every frame, must not undo it.
        // The captured game parameters stay the game's.
        NV_SET_SLEEP_MODE_PARAMS forced = params_snapshot;
        forced.bLowLatencyMode = 0;
        forced.bLowLatencyBoost = 0;
        result = SafeInvokeSetSleepMode(pDev, &forced);
    } else {
        // Forward game's params unmodified.
        result = SafeInvokeSetSleepMode(pDev, params);
    }
    ReleaseSRWLockExclusive(&s_sleep_mode_forward_lock);
    return result;
}

// ── Hook: Sleep — forward to driver for Reflex queue management. ──
// The driver's Sleep call handles internal timing synchronization even
// with minimumIntervalUs=0. Forwarding at the game's natural call point
// preserves optimal pipeline timing for non-overload throughput.
static NvAPI_Status __cdecl Hook_Sleep(IUnknown* pDev) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) return SafeInvokeSleep(pDev);
    const FeedbackDeviceGuard sleep_device(pDev);
    const uint32_t sleep_token = PublicationToken(sleep_device.identity);
    if (!sleep_device.identity.device) return SafeInvokeSleep(pDev);

    // Smooth Motion: bypass Reflex Sleep entirely — SM controls presentation
    // timing at the driver level, and Sleep calls conflict with it.
    if (IsNvSmoothMotionActive()) return NVAPI_OK;

    if (s_orig_sleep) {
        // The bypass is decided once per call, before the pre-wait: the
        // pre-Sleep margin is learned separately with and without the
        // driver's Sleep after the hook.
        // Key unified_pacer: this Sleep is the start of the frame. On the
        // presenting device it replaces the pre-Sleep branch below, and the bypass is
        // not consulted: the driver's Sleep always runs. With the key off, one load and
        // a not-taken branch: the bypass first, then the presenting device, as before.
        bool bypass = false;
        bool presenting_at_entry = false;
        bool unified = false;
        if (PacingPipeline_UnifiedPacerKey()) {
            presenting_at_entry = IsPresentingFeedbackDevice(pDev);
            unified = presenting_at_entry && Scheduler_UnifiedActive();
            bypass = !unified && NvAPI_ShouldBypassSleep(
                g_reflex_sleep_bypass.load(std::memory_order_relaxed),
                g_user_target_fps.load(std::memory_order_relaxed), SleepSourcePaced());
        } else {
            bypass = NvAPI_ShouldBypassSleep(g_reflex_sleep_bypass.load(std::memory_order_relaxed),
                g_user_target_fps.load(std::memory_order_relaxed), SleepSourcePaced());
            presenting_at_entry = IsPresentingFeedbackDevice(pDev);
        }
        // Pre-Sleep placement: record the Sleep on the presenting
        // device, then wait toward the next source target before the driver
        // Sleep. The owner is held only to plan and to record the wait end,
        // not during the waits. A busy owner skips the wait; the
        // SIMULATION_START trim still paces the frame.
        if (presenting_at_entry) {
            ObserveReflexSleep();
            // The Sleep-mode tuple (key on, or an epoch pending), else
            // bypass_low_latency_off: the driver's low latency mode follows
            // the bypass, before the pre-Sleep wait.
            if (NvAPI_SleepModeReconcilePending()) ReconcileSleepMode(bypass);
            else UpdateLowLatencyOverride(bypass);
            LARGE_INTEGER sleep_entry{};
            QueryPerformanceCounter(&sleep_entry);
            Scheduler_RecordReflexSleep(sleep_entry.QuadPart);
            if (unified) {
                UnifiedSleepStart(sleep_entry.QuadPart);
            } else if (Scheduler_PreSleepPlacementActive()) {
                // Render handoff limit: with
                // the Sleep bypassed, a frame that is not held waits here,
                // before the game reads input, until the render thread has
                // taken the newest released frame.
                int64_t handoff_qpc = 0;
                int64_t plan_qpc = sleep_entry.QuadPart;
                uint8_t handoff_domain = 0;
                uint32_t handoff_epoch = 0;
                uint64_t handoff_frame = 0;
                if (bypass && Scheduler_RenderHandoffFrame(&handoff_domain, &handoff_epoch, &handoff_frame)) {
                    const double interval_us = g_effective_interval_us.load(std::memory_order_relaxed);
                    const RenderHandoffWait waited = RenderHandoff_WaitFor(handoff_domain, handoff_epoch,
                        handoff_frame, us_to_qpc(RENDER_HANDOFF_STALL_INTERVALS * interval_us), &handoff_qpc);
                    if (waited == RenderHandoffWait::Deadline) Scheduler_RenderHandoffStalled();
                    if (waited != RenderHandoffWait::Ready) handoff_qpc = 0;
                    LARGE_INTEGER handoff_end{};
                    QueryPerformanceCounter(&handoff_end);
                    plan_qpc = handoff_end.QuadPart;
                }
                int64_t pre_sleep_target = 0;
                uint64_t pre_sleep_index = 0;
                bool pre_sleep_precise = false;
                bool planned = false;
                {
                    SchedulerOwnerGuard scheduler_owner;
                    planned = scheduler_owner && Scheduler_PlanReflexSleep(sleep_entry.QuadPart, plan_qpc,
                        !bypass, handoff_qpc, &pre_sleep_target, &pre_sleep_index, &pre_sleep_precise);
                }
                if (planned) {
                    // pre_sleep_precise: the wait ends at the target with the
                    // hardware spin, inside this call, before the driver's Sleep.
                    PreSleepWaitUntil(pre_sleep_target, pre_sleep_precise);
                    LARGE_INTEGER wait_end{};
                    QueryPerformanceCounter(&wait_end);
                    SchedulerOwnerGuard scheduler_owner;
                    if (scheduler_owner) Scheduler_EndReflexSleep(pre_sleep_index, wait_end.QuadPart);
                }
            } else if (Scheduler_DriverLimiterHoldActive()) {
                // driver_limiter_hold: no wait of ReLimiter's own, the driver's
                // Sleep releases the frame. Only the entry is recorded, the
                // moment the game reached ReLimiter for the next grid index.
                SchedulerOwnerGuard scheduler_owner;
                if (scheduler_owner) Scheduler_RecordReflexSleepEntry(sleep_entry.QuadPart);
            }
        }
        if (bypass) {
            g_pacer_latency_us.store(0.0,std::memory_order_relaxed);
            if (presenting_at_entry) {
                PollSleepStatus(pDev, sleep_token);
                PublishSleepStatusAtSleepReturn(sleep_token);
            }
            return NVAPI_OK;
        }
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        const NvAPI_Status result = SafeInvokeSleep(pDev);
        QueryPerformanceCounter(&t1);
        if (unified) UnifiedSleepSelfCheck(t1.QuadPart - t0.QuadPart);
        if (!NvAPI_IsFeedbackDeviceCurrent(sleep_device.identity) ||
            !IsPresentingFeedbackDevice(pDev)) {
            return result;
        }
        // The driver's Sleep for the next grid index: its start and return go
        // to the scheduler, which records them for the frame and, under the
        // developer A/B gpu_paced_cadence, judges whether the driver released
        // it (cadence_grid.h). A busy owner skips it, like the pre-wait's end.
        // The call itself is unchanged.
        if (presenting_at_entry) {
            SchedulerOwnerGuard scheduler_owner;
            if (scheduler_owner) Scheduler_EndDriverSleep(t0.QuadPart, t1.QuadPart);
        }
        double dur_us = qpc_to_us(t1.QuadPart - t0.QuadPart);
        if (dur_us > 0.0 && dur_us < 100000.0) {
            double prev = g_pacer_latency_us.load(std::memory_order_relaxed);
            double smoothed = (prev > 0.0) ? prev + 0.16 * (dur_us - prev) : dur_us;
            g_pacer_latency_us.store(smoothed, std::memory_order_relaxed);
        }
        if (presenting_at_entry) {
            PollSleepStatus(pDev, sleep_token);
            PublishSleepStatusAtSleepReturn(sleep_token);
        }
        return result;
    }
    return NVAPI_OK;
}

// ── Hook: SetLatencyMarker — RTSS filter, record, forward ──
static NvAPI_Status __cdecl Hook_SetLatencyMarker(IUnknown* dev, NV_LATENCY_MARKER_PARAMS* params) {
    RuntimeCallbackGuard callback;
    if (!s_orig_set_latency_marker) return NVAPI_OK;
    if (callback.IsStopping() || !params)
        return SafeInvokeLatencyMarker(dev, params);

    NV_LATENCY_MARKER_PARAMS marker_params{};
    if (!SafeCopyLatencyMarkerParams(params, &marker_params))
        return SafeInvokeLatencyMarker(dev, params);
    LARGE_INTEGER census_entry{};QueryPerformanceCounter(&census_entry);
    // Developer key unified_queue_upstream: freeze the lifetime (reset generation, producer epoch)
    // and the sample before any admission step below that could be descheduled across a change.
    const MarkerStampCapture stamp_capture = MarkerStamps_Capture(
        static_cast<uint32_t>(marker_params.markerType),
        [] { return FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame); },
        [&census_entry] {
            return MarkerClockSample{ census_entry.QuadPart, MarkerCycles_ThreadCycles(), GetCurrentThreadId() };
        });
    const auto census_caller=reinterpret_cast<uint64_t>(_ReturnAddress());
    g_nvapi_marker_census.Record(static_cast<uint32_t>(marker_params.markerType),MarkerCensusStage::Entry,
        {marker_params.frameID,reinterpret_cast<uint64_t>(dev),census_caller,census_entry.QuadPart,
        GetCurrentThreadId(),0,0,0});
    if (!IsPresentingFeedbackDevice(dev, &marker_params))
        return SafeInvokeLatencyMarker(dev, params);

    void* return_address = _ReturnAddress();
    const bool from_rtss = CallerIsRTSS(return_address);
    if (!from_rtss) s_newest_app_marker_frame.store(marker_params.frameID, std::memory_order_relaxed);
    // Developer key unified_queue_upstream: the game's own SIMULATION_START, SIMULATION_END,
    // RENDERSUBMIT_START and RENDERSUBMIT_END, captured at hook entry above (generation, producer
    // epoch, arrival QPC and the thread's CPU cycles and id: marker_cycles.h), on the thread that
    // sends the marker (the game thread for the simulation markers, the render thread for the
    // render submit markers; any thread), and published here once the marker is the game's own on
    // the presenting device: a call that was suspended across another lifetime (an epoch change, a
    // reset) stamps nothing into it. With the key off: one load and a branch not taken at entry,
    // nothing here. ReLimiter's own injected markers (reflex_inject.cpp) reach the driver through
    // the trampoline, never this hook; RTSS markers are not the game's.
    if (!from_rtss)
        MarkerStamps_Publish(stamp_capture, kMarkerDomainNvApi, marker_params.frameID,
                             [] { return FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame); });
    static std::atomic<bool> s_first_marker{ true };
    if (s_first_marker.exchange(false, std::memory_order_relaxed)) {
        LOG_INFO("NvAPI: First marker received — type=%u, frameID=%llu, source=%s",
                 marker_params.markerType, marker_params.frameID,
                 from_rtss ? "RTSS" : "direct");
    }

    // Capture device for GetLatency if not already set.
    // Some games (Streamline/DLSS-G) never call SetSleepMode through NvAPI,
    // so g_dev stays null. The marker hook always receives the device.
    if (NvAPI_CaptureFeedbackDevice(dev)) {
        LOG_INFO("NvAPI: presenting device captured/replaced from SetLatencyMarker");
    }
    const FeedbackDeviceGuard marker_device(dev, &marker_params);

    // Clock map: the local QPC closest to the driver's presentStartTime
    // is read immediately before forwarding; it is recorded only after the
    // marker passes validation below.
    LARGE_INTEGER present_start_forward_qpc{};
    if (marker_params.markerType == PRESENT_START && !from_rtss)
        QueryPerformanceCounter(&present_start_forward_qpc);
    // Forward to driver FIRST — Reflex's JIT pacing depends on accurate
    // marker timing. Any work we do before forwarding shifts the driver's
    // perception of pipeline stage boundaries and degrades latency.
    // marker_forward_order=after_wait instead forwards direct
    // immediate markers on exit, after any source wait, as the PCL path does:
    // the driver (and frame generation) then sees the paced release.
    const bool forward_after_wait = !from_rtss &&
        IsImmediateTimingMarker(marker_params.markerType) &&
        g_marker_forward_after_wait.load(std::memory_order_relaxed) &&
        // Key unified_pacer: the marker is forwarded first whatever the order option
        // says; nothing waits in this hook for the unified path.
        !PacingPipeline_UnifiedPacerKey();
    NvAPI_Status result = NVAPI_OK;
    bool forwarded = false;
    // Every path returns through forward(): exactly one driver call per marker.
    const auto forward = [&]() {
        if (!forwarded) {
            result = SafeInvokeLatencyMarker(dev, params);
            forwarded = true;
        }
        return result;
    };
    if (!forward_after_wait) forward();
    if (!from_rtss) ObserveLatencyMarkerThread(static_cast<uint32_t>(marker_params.markerType));
    // Never relabel a marker from A with B's epoch after the driver returns.
    // Failed admission still forwards exactly once with the original pointer.
    if (!marker_device.identity.device) return forward(); // Guard logged the cause.
    if (!NvAPI_IsFeedbackDeviceCurrent(marker_device.identity)) {
        LogMarkerRejection(MarkerRejection::ForwardStaleToken, dev, nullptr, &marker_params);
        return forward();
    }
    if (!IsPresentingFeedbackDevice(dev, &marker_params)) return forward();
    if (!IsImmediateTimingMarker(marker_params.markerType)) {
        // RTSS tail markers were never eligible. Keep that filter before the
        // queue; only direct, validated current-device observations enter it.
        if (from_rtss) return forward();
        LARGE_INTEGER captured_qpc{};
        QueryPerformanceCounter(&captured_qpc);
        const PacingPlanKey frame_key{PacingIdentityDomain::NvApiFrame,
            marker_device.identity.producer_epoch,
            FrameTimeline_CurrentControllerGeneration(), marker_params.frameID};
        RecordNvAPIMarker();
        if (!s_deferred_markers.Push({marker_params.frameID, captured_qpc.QuadPart,
            frame_key.producer_epoch, frame_key.controller_generation, marker_params.markerType})) {
            static std::atomic<bool> logged_overflow{false};
            if (!logged_overflow.exchange(true, std::memory_order_relaxed)) {
                LOG_INFO("NvAPI: deferred diagnostic marker queue full; diagnostics dropped, enforcement unaffected");
            }
        }
        // SIMULATION_END and PRESENT_END of a planned frame reach the lead
        // (the game's simulation → render queue and the render handoff
        // offset, lead_controller.h); the push never blocks.
        if ((marker_params.markerType == SIMULATION_END || marker_params.markerType == PRESENT_END) &&
            !PCL_MarkersFlowing()) {
            PresentGate_OnFrameMarker(frame_key, marker_params.markerType == SIMULATION_END
                ? PacingStageEventKind::SimulationEnd : PacingStageEventKind::PresentEnd,
                captured_qpc.QuadPart);
        }
        if (marker_params.markerType == PRESENT_START && !PCL_MarkersFlowing()) {
            // Gate N may overlap SimulationStart(N+1) on a different thread.
            // PRESENT_START never claims the scheduler owner: the executor
            // reads only the exact-key plan, the grid and the output tracker.
            const PacingFrameKey present_key = PacingPlanKey_Frame(frame_key);
            if (!NvAPI_IsFeedbackDeviceCurrent(marker_device.identity) ||
                frame_key.controller_generation != FrameTimeline_CurrentControllerGeneration()) {
                PacingTelemetry_SetPresentAttribution(
                    PresentGate_MakeAttribution(present_key, captured_qpc.QuadPart, {}));
                NativeGate_WithdrawMarker(); // no Present on another thread takes an older frame
                LogMarkerRejection(MarkerRejection::GateStaleToken, dev, nullptr, &marker_params);
                return forward();
            }
            // Every NvAPI frame feeds the native route, whatever the plan's
            // rung, so the route qualifies and carries this key to the native
            // Present for H2 and H3.
            NativeGate_OnMarker(NativeGate_GetContext(captured_qpc.QuadPart), frame_key);
            NativeGate_PublishRouteCapability(frame_key.controller_generation);
            // H0 releases here; H1 holds here; H2/H3 plans output later.
            PresentationGateResult gate_result{};
            PresentGate_OnPresentStart(frame_key, captured_qpc.QuadPart, &gate_result);
            LARGE_INTEGER release{};
            QueryPerformanceCounter(&release);
            NativeGate_OnMarkerReleased(release.QuadPart);
            if (NvAPI_IsFeedbackDeviceCurrent(marker_device.identity))
                FgInput_RecordMarker(frame_key.frame_id, frame_key.producer_epoch,
                    frame_key.controller_generation, release.QuadPart);
            PacingTelemetry_SetPresentAttribution(
                PresentGate_MakeAttribution(present_key, captured_qpc.QuadPart, gate_result));
        }
        // After any hold, so the keyed write never delays the gate. The QPC
        // itself was read before forwarding.
        if (marker_params.markerType == PRESENT_START)
            ReflexClockMap_RecordPresentStart(frame_key.producer_epoch,
                frame_key.frame_id, present_start_forward_qpc.QuadPart);
        return forward();
    }
    // RENDERSUBMIT_START of a planned frame is a stage event for the lead
    // (the simulation → render queue and the render handoff offset)
    // and is published as the render handoff (render_handoff.h), before
    // the scheduler-owner guard: it runs on the render thread, often while
    // the next SIMULATION_START owns the scheduler (or waits for this
    // handoff), and neither step blocks.
    if (marker_params.markerType == RENDERSUBMIT_START && !from_rtss && !PCL_MarkersFlowing()) {
        LARGE_INTEGER submit_qpc{};
        QueryPerformanceCounter(&submit_qpc);
        PresentGate_OnFrameMarker(PacingPlanKey{PacingIdentityDomain::NvApiFrame,
            marker_device.identity.producer_epoch,
            FrameTimeline_CurrentControllerGeneration(), marker_params.frameID},
            PacingStageEventKind::RenderSubmitStart, submit_qpc.QuadPart);
        // gpu_start_probe: the frame whose first submission the probe times.
        // A load of the developer flag and a not-taken branch with the key off.
        if (g_gpu_start_probe.load(std::memory_order_relaxed))
            GpuReadiness_ArmProbeForFrame(marker_device.identity.producer_epoch, marker_params.frameID);
    }
    SchedulerOwnerGuard scheduler_owner;
    if (!scheduler_owner) {
        LogMarkerRejection(MarkerRejection::SchedulerOwner, dev, nullptr, &marker_params);
        return forward();
    }
    if (!NvAPI_IsFeedbackDeviceCurrent(marker_device.identity)) {
        LogMarkerRejection(MarkerRejection::OwnerStaleToken, dev, nullptr, &marker_params);
        return forward();
    }

    // Resolve the configured trigger before source filtering. RTSS markers are
    // normally ignored, but some games expose a partial direct NvAPI stream
    // while RTSS supplies the driver-visible SIMULATION_START. Admit only that
    // missing marker after direct-marker warm-up; RTSS alone must never select
    // the marker path or drive enforcement.
    static const NV_LATENCY_MARKER_TYPE s_configured_enforcement_marker =
        g_config.enforcement_marker == "RenderSubmitStart"
            ? RENDERSUBMIT_START
            : SIMULATION_START;
    static std::atomic<NV_LATENCY_MARKER_TYPE> s_enforcement_marker{
        s_configured_enforcement_marker };
    static NvApiMarkerPolicy s_marker_policy;
    static NvApiMarkerPromotion s_marker_promotion;
    static std::atomic<int> s_enforcement_hits{ 0 };
    static std::atomic<int> s_render_submit_hits{ 0 };
    static std::atomic<bool> s_fallback_applied{ false };
    static std::atomic<uint32_t> s_marker_policy_epoch{ 0 };
    const uint32_t marker_policy_epoch =
        marker_device.identity.producer_epoch;
    const uint32_t previous_marker_policy_epoch =
        s_marker_policy_epoch.exchange(
            marker_policy_epoch,
            std::memory_order_acq_rel);
    if (previous_marker_policy_epoch != marker_policy_epoch) {
        // Producer frame IDs may restart. Reset marker admission state before
        // this lifetime's first marker is consumed.
        s_marker_policy.Reset();
        s_marker_promotion.Reset();
        s_enforcement_hits.store(0, std::memory_order_relaxed);
        s_render_submit_hits.store(0, std::memory_order_relaxed);
        s_fallback_applied.store(false, std::memory_order_relaxed);
        s_enforcement_marker.store(
            s_configured_enforcement_marker,
            std::memory_order_relaxed);
    }
    DrainDeferredMarkers(&s_marker_policy, marker_policy_epoch);
    if (!NvAPI_IsFeedbackDeviceCurrent(marker_device.identity)) return forward();
    if (!s_marker_policy.ShouldProcess(
            from_rtss,
            static_cast<uint32_t>(marker_params.markerType),
            marker_params.frameID)) {
        g_nvapi_marker_census.Record(static_cast<uint32_t>(marker_params.markerType),MarkerCensusStage::Rejected,
            {marker_params.frameID,reinterpret_cast<uint64_t>(dev),census_caller,census_entry.QuadPart,
            GetCurrentThreadId(),marker_policy_epoch,100,
            static_cast<uint32_t>(s_enforcement_marker.load(std::memory_order_relaxed))});
        return forward();
    }
    if (from_rtss &&
        s_configured_enforcement_marker != SIMULATION_START) {
        return forward();
    }
    if (from_rtss) {
        static std::atomic<bool> s_logged_rtss_simulation_start{ false };
        if (!s_logged_rtss_simulation_start.exchange(
                true,
                std::memory_order_relaxed)) {
            LOG_INFO(
                "NvAPI: accepting RTSS SIMULATION_START to complete direct game marker stream "
                "(direct_markers=%llu, rtss_sim_starts=%llu)",
                s_marker_policy.DirectMarkerCount(),
                s_marker_policy.RtssSimulationStartCount());
        }
    }

    // Record that the game is sending NvAPI markers (distinct from our own
    // enforcement timestamps). Used to gate present-based fallback off.
    RecordNvAPIMarker();

    LARGE_INTEGER ts;
    QueryPerformanceCounter(&ts);

    g_marker_log.Record(static_cast<uint32_t>(marker_params.markerType), ts.QuadPart, marker_params.frameID);
    g_nvapi_marker_census.Record(static_cast<uint32_t>(marker_params.markerType),MarkerCensusStage::Admitted,
        {marker_params.frameID,reinterpret_cast<uint64_t>(dev),census_caller,ts.QuadPart,
        GetCurrentThreadId(),marker_policy_epoch,from_rtss?1u:0u,
        static_cast<uint32_t>(s_enforcement_marker.load(std::memory_order_relaxed))});

    // Enforcement trigger: configurable marker point.
    // Skip if PCL markers are flowing — Vulkan+Streamline games send markers
    // through both slPCLSetMarker and NvAPI (Streamline forwards internally).
    // The PCL hook handles enforcement for those games; doing it here too
    // would cause double sleep per frame.
    // Auto-fallback: if we've received 200+ markers but never seen our
    // enforcement marker type, switch to the most common marker type we DO see.
    // This handles games like Greedfall 2 that send RENDERSUBMIT_START but
    // never SIMULATION_START — without this, the scheduler never runs.
    NV_LATENCY_MARKER_TYPE enforcement_marker =
        s_enforcement_marker.load(std::memory_order_relaxed);
    if (marker_params.markerType == enforcement_marker) {
        s_enforcement_hits.fetch_add(1, std::memory_order_relaxed);
    }
    if (!from_rtss && marker_params.markerType == RENDERSUBMIT_START) {
        s_render_submit_hits.fetch_add(1, std::memory_order_relaxed);
    }
    if (s_configured_enforcement_marker == SIMULATION_START &&
        s_marker_policy.DirectMarkerCount() >= 200 &&
        s_enforcement_hits.load(std::memory_order_relaxed) == 0 &&
        s_render_submit_hits.load(std::memory_order_relaxed) > 0 &&
        !s_fallback_applied.exchange(true, std::memory_order_relaxed)) {
        // Never seen our enforcement marker — fall back to RENDERSUBMIT_START
        // which is the most common alternative marker games send.
        s_enforcement_marker.store(RENDERSUBMIT_START, std::memory_order_relaxed);
        enforcement_marker = RENDERSUBMIT_START;
        LOG_INFO("NvAPI: enforcement marker fallback — no admitted SIMULATION_START before threshold, "
                 "switching to RENDERSUBMIT_START (direct_markers=%llu, "
                 "rtss_sim_starts=%llu)",
                 s_marker_policy.DirectMarkerCount(),
                 s_marker_policy.RtssSimulationStartCount());
    }

    if(s_configured_enforcement_marker==SIMULATION_START &&
        enforcement_marker==RENDERSUBMIT_START && !from_rtss &&
        marker_params.markerType==SIMULATION_START && !PCL_MarkersFlowing() &&
        s_marker_promotion.Observe(marker_params.frameID,ts.QuadPart,GetQPCFrequency())) {
        s_enforcement_marker.store(SIMULATION_START,std::memory_order_relaxed);
        enforcement_marker=SIMULATION_START;
        LOG_INFO("NvAPI: restoring configured SIMULATION_START after 8 consecutive direct markers "
            "frameID=%llu producer=%u thread=%u (RenderSubmit fallback retired)",
            marker_params.frameID,marker_policy_epoch,GetCurrentThreadId());
    }

    // The scheduler consumes the resolved marker snapshot, including the exact
    // callback that crosses the fallback threshold.
    const PacingSourceMarker source_marker =
        enforcement_marker == SIMULATION_START
            ? PacingSourceMarker::SimulationStart
            : PacingSourceMarker::RenderSubmitStart;
    const bool pcl_markers_flowing = PCL_MarkersFlowing();

    // The source stage publishes this frame's plan before its wait, so the
    // PRESENT_START executor of the same frame finds it by exact key.
    if (marker_params.markerType == enforcement_marker && !pcl_markers_flowing) {
        // Overlapping simulation/render threads may deliver an older source
        // after promotion. Never pace an already dispatched source twice.
        if(!s_marker_promotion.CanEnforce(marker_params.frameID))return forward();
        if (!NvAPI_IsFeedbackDeviceCurrent(marker_device.identity)) {
            LogMarkerRejection(MarkerRejection::EnforcementStaleToken, dev, nullptr, &marker_params);
            return forward();
        }
        s_marker_promotion.Enforced(marker_params.frameID);
        g_nvapi_marker_census.Record(static_cast<uint32_t>(marker_params.markerType),MarkerCensusStage::Enforced,
            {marker_params.frameID,reinterpret_cast<uint64_t>(dev),census_caller,ts.QuadPart,
            GetCurrentThreadId(),marker_policy_epoch,0,static_cast<uint32_t>(enforcement_marker)});
        OnMarker(
            marker_params.frameID,
            ts.QuadPart,
            SchedulerTrigger::NvApiMarker,
            source_marker);
        // After-wait order: the driver's stage starts at the paced release.
        const bool stage_contains_wait = forwarded;
        forward();
        LARGE_INTEGER controller_end_qpc{};
        QueryPerformanceCounter(&controller_end_qpc);
        if (!NvAPI_IsFeedbackDeviceCurrent(marker_device.identity)) {
            LogMarkerRejection(MarkerRejection::EnforcementStaleToken, dev, nullptr, &marker_params);
            return forward();
        }
        // The residence is the ReLimiter time inside the driver's source
        // stage; the stage-demand computation subtracts it from that stage. With
        // after-wait forwarding the wait precedes the stage, so it is zero.
        PublishReflexControllerResidence(
            marker_params.frameID,
            marker_device.identity.producer_epoch,
            stage_contains_wait
                ? qpc_to_us(controller_end_qpc.QuadPart - ts.QuadPart)
                : 0.0);
        static std::atomic<bool> s_logged_native_enforcement{ false };
        if (!s_logged_native_enforcement.exchange(true, std::memory_order_relaxed)) {
            // OnMarker has no result: this proves dispatch/completion, not that
            // a sleep occurred or a frame plan was published on this frame.
            LOG_INFO("NvAPI: first native marker enforcement scheduler call completed "
                     "source=%s type=%u frameID=%llu incoming=%p published=%p "
                     "epoch=%u publication=%llu",
                     from_rtss ? "RTSS" : "direct",
                     static_cast<unsigned>(marker_params.markerType), marker_params.frameID,
                     static_cast<void*>(dev), static_cast<void*>(marker_device.identity.device),
                     marker_device.identity.producer_epoch,
                     marker_device.identity.publication_sequence);
        }
    }

    return forward();
}

// ── Hook: SetAsyncFrameMarker — pass-through timing ──
// With the Reflex Sleep bypassed and DLSS frame generation, the NVIDIA driver
// still paces: it sleeps the thread that calls the game's Present inside this
// call (Streamline's sl.dlss_g, through sl.common). The hook only times the
// call for telemetry; it never modifies the parameters or the result, and
// never suppresses or delays the marker.
static NvAPI_Status __cdecl Hook_SetAsyncFrameMarker(IUnknown* queue, NV_ASYNC_FRAME_MARKER_PARAMS* params) {
    RuntimeCallbackGuard callback;
    if (!s_orig_set_async_frame_marker) return NVAPI_OK;
    if (callback.IsStopping() || !params) return SafeInvokeAsyncFrameMarker(queue, params);

    void* const caller_address = _ReturnAddress();
    uint64_t frame_id = 0;
    uint64_t present_frame_id = 0;
    uint32_t marker_type = 0;
    uint8_t vendor_internal = 0;
    const bool known = SafeReadAsyncFrameMarker(params, &frame_id, &marker_type, &present_frame_id,
        &vendor_internal);
    LARGE_INTEGER call_entry{};
    QueryPerformanceCounter(&call_entry);
    const NvAPI_Status result = SafeInvokeAsyncFrameMarker(queue, params);
    LARGE_INTEGER call_exit{};
    QueryPerformanceCounter(&call_exit);
    if (!known) return result;

    ObserveAsyncMarkerThread(marker_type, queue);
    // The first call of each marker type (types above 31 are not logged).
    static std::atomic<uint32_t> s_first_call_types{ 0 };
    const uint32_t type_bit = marker_type < 32 ? 1u << marker_type : 0u;
    if (type_bit != 0 && (s_first_call_types.load(std::memory_order_relaxed) & type_bit) == 0 &&
        (s_first_call_types.fetch_or(type_bit, std::memory_order_relaxed) & type_bit) == 0) {
        char module[MAX_PATH];
        CallerModuleName(caller_address, module, sizeof(module));
        LOG_INFO("NvAPI: first SetAsyncFrameMarker call - type=%u, frameID=%llu, presentFrameID=%llu, "
                 "newest app NvAPI marker frameID=%llu, thread=%u, %.1f us, vendorInternal=%u, queue=%p, "
                 "module=%s",
                 marker_type, frame_id, present_frame_id,
                 s_newest_app_marker_frame.load(std::memory_order_relaxed), GetCurrentThreadId(),
                 qpc_to_us(call_exit.QuadPart - call_entry.QuadPart), vendor_internal ? 1u : 0u,
                 static_cast<void*>(queue), module);
    }
    PacingWaitRecord record{};
    record.key = PacingFrameKey{PacingIdentityDomain::NvApiFrame,
        FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame), frame_id};
    record.kind = PacingWaitKind::AsyncFrameMarker;
    record.thread_id = GetCurrentThreadId();
    record.marker_type = marker_type;
    record.entry_qpc = call_entry.QuadPart;
    record.exit_qpc = call_exit.QuadPart;
    PacingTelemetry_PushWait(record);
    return result;
}

#ifdef _WIN64
// ── Hook: SetFlipConfig — pass-through observation ──
// DLSS Frame Generation calls NvAPI_D3D12_SetFlipConfig before the first Present
// of each batch so the display engine spaces that batch's flips (hardware flip
// metering). The hook never alters the call or its result. It logs the first
// configuration and each change of the batch size, the presented frames, the
// driver's status and its feedback (not the batch time, which follows the frame
// rate), at most kFlipConfigLogLimit times, so a log shows whether metering was
// asked for and what the driver answered.
static constexpr NvU32 kNvApiIdSetFlipConfig = 0xF3148C42;
static constexpr uint32_t kFlipConfigLogLimit = 16;
static PFN_NvAPI_D3D12_SetFlipConfig s_orig_set_flip_config = nullptr;
// The flip metering block's trampoline (below): the original nvapi_QueryInterface
// once the block's hook exists, enabled or not.
static PFN_NvAPI_QueryInterface s_orig_query_interface = nullptr;
static std::atomic<uint64_t> s_flip_config_signature{UINT64_MAX};
static std::atomic<uint64_t> s_flip_config_calls{0};
static std::atomic<uint32_t> s_flip_config_logs{0};

static NvAPI_Status SafeInvokeSetFlipConfig(IUnknown* queue, UINT source, NV_FLIP_CONFIG* config) noexcept {
    if (!s_orig_set_flip_config) return NVAPI_ERROR;
    __try { return s_orig_set_flip_config(queue, source, config); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return NVAPI_ERROR; }
}

static bool SafeReadFlipConfig(const NV_FLIP_CONFIG* config, NV_FLIP_CONFIG* out) noexcept {
    __try { *out = *config; return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static const char* FlipConfigFeedbackText(NvU32 mask) noexcept {
    switch (mask) {
    case 0: return "ok";
    case 1: return "no_iflip";
    case 2: return "mirroring_detected";
    case 3: return "no_iflip+mirroring_detected";
    default: return "unknown_bits";
    }
}

static NvAPI_Status __cdecl Hook_SetFlipConfig(IUnknown* queue, UINT source, NV_FLIP_CONFIG* config) {
    RuntimeCallbackGuard callback;
    // The call's time for the pacing CSV (flip_config_qpc), read before the
    // driver sees it; nothing of the call changes.
    LARGE_INTEGER call_qpc{};
    QueryPerformanceCounter(&call_qpc);
    const NvAPI_Status result = SafeInvokeSetFlipConfig(queue, source, config);
    const uint64_t call = s_flip_config_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    NV_FLIP_CONFIG seen{};
    if (callback.IsStopping() || !config || !SafeReadFlipConfig(config, &seen)) return result;
    const NvU32 struct_version = seen.version >> 16;
    const bool v2 = struct_version >= 2;
    // The newest call that enabled metering, with its batch time in µs (0 when
    // the structure has none or it is not positive).
    if (seen.nFramesPerBatch > 0) {
        const double batch_us = v2 && seen.timePerBatchMs > 0.0f
            ? static_cast<double>(seen.timePerBatchMs) * 1000.0 : 0.0;
        FlipConfig_Record(call_qpc.QuadPart, batch_us >= 4294967295.0 ? UINT32_MAX
            : static_cast<uint32_t>(batch_us + 0.5));
    }
    const NvU32 presented = v2 ? seen.nPresentedFramesPerBatch : 0;
    const uint64_t signature = (static_cast<uint64_t>(static_cast<uint32_t>(result)) << 32) |
        (static_cast<uint64_t>(seen.nFramesPerBatch & 0xFFu) << 24) |
        (static_cast<uint64_t>(presented & 0xFFu) << 16) |
        (static_cast<uint64_t>(seen.flipConfigFeedbackMask & 0xFFu) << 8) | (struct_version & 0xFFu);
    if (s_flip_config_signature.exchange(signature, std::memory_order_relaxed) == signature) return result;
    const uint32_t logged = s_flip_config_logs.fetch_add(1, std::memory_order_relaxed);
    if (logged < kFlipConfigLogLimit) {
        LOG_INFO("NvAPI: SetFlipConfig call %llu: status=%d frames_per_batch=%u%s presented_per_batch=%u "
                 "time_per_batch_ms=%.2f feedback=0x%x (%s) vidpn_source=%u version=%u thread=%lu",
                 static_cast<unsigned long long>(call), result, seen.nFramesPerBatch,
                 seen.nFramesPerBatch == 0 ? " (metering off)" : "", presented,
                 v2 ? static_cast<double>(seen.timePerBatchMs) : 0.0, seen.flipConfigFeedbackMask,
                 FlipConfigFeedbackText(seen.flipConfigFeedbackMask), source, struct_version,
                 static_cast<unsigned long>(GetCurrentThreadId()));
    } else if (logged == kFlipConfigLogLimit) {
        LOG_INFO("NvAPI: SetFlipConfig configuration changed again; later changes are not logged (limit %u lines)",
                 kFlipConfigLogLimit);
    }
    return result;
}

// ── Reflex observation hooks (pass-through; see "Reflex observation" above) ──
// NvAPI_D3D_SetReflexSync, NvAPI_D3D12_NotifyOutOfBandCommandQueue,
// NvAPI_D3D_GetSleepStatus (the game's own calls) and
// NvAPI_D3D12_SetCreateCommandQueueLowLatencyHint. Each forwards the original
// arguments to the driver and returns its result unchanged; arguments are
// copied (SEH-guarded) only to be logged.
static constexpr NvU32 kNvApiIdSetReflexSync = 0xB9F6FAFF;
static constexpr NvU32 kNvApiIdNotifyOutOfBandCommandQueue = 0x03D6E8CB;
static constexpr NvU32 kNvApiIdGetSleepStatus = 0xAEF96CA1;
static constexpr NvU32 kNvApiIdSetCreateCommandQueueLowLatencyHint = 0x548C224F;
static constexpr uint32_t kReflexSyncLogLimit = 32;
static constexpr uint32_t kOutOfBandLogLimit = 32;
static constexpr uint32_t kGameSleepStatusLogLimit = 16;
static constexpr uint32_t kLowLatencyHintLogLimit = 8;
static constexpr ULONGLONG kReflexSyncSummaryMs = 5000;

static PFN_NvAPI_D3D_SetReflexSync s_orig_set_reflex_sync = nullptr;
static PFN_NvAPI_D3D12_NotifyOutOfBandCommandQueue s_orig_notify_out_of_band = nullptr;
static PFN_NvAPI_D3D12_SetCreateCommandQueueLowLatencyHint s_orig_low_latency_hint = nullptr;

static NvAPI_Status SafeInvokeSetReflexSync(IUnknown* device, NV_SET_REFLEX_SYNC_PARAMS* params) noexcept {
    if (!s_orig_set_reflex_sync) return NVAPI_ERROR;
    __try { return s_orig_set_reflex_sync(device, params); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return NVAPI_ERROR; }
}

static bool SafeCopySleepStatus(
    const NV_GET_SLEEP_STATUS_PARAMS* source,
    NV_GET_SLEEP_STATUS_PARAMS* destination) noexcept
{
    if (!source || !destination) return false;
    __try {
        *destination = *source;
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeCopyReflexSyncParams(const NV_SET_REFLEX_SYNC_PARAMS* source,
    NV_SET_REFLEX_SYNC_PARAMS* destination) noexcept {
    __try { *destination = *source; return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static NvAPI_Status SafeInvokeNotifyOutOfBand(IUnknown* queue, int type) noexcept {
    if (!s_orig_notify_out_of_band) return NVAPI_ERROR;
    __try { return s_orig_notify_out_of_band(queue, type); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return NVAPI_ERROR; }
}

static NvAPI_Status SafeInvokeLowLatencyHint(IUnknown* device) noexcept {
    if (!s_orig_low_latency_hint) return NVAPI_ERROR;
    __try { return s_orig_low_latency_hint(device); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return NVAPI_ERROR; }
}

// SetReflexSync: a line for the first call and each change of the tuple
// (timeInQueueUs is left out of it: it changes every frame); timeInQueueUs is
// summarized instead, at most every kReflexSyncSummaryMs while calls arrive.
static std::atomic<uint64_t> s_reflex_sync_signature{UINT64_MAX};
static std::atomic<uint64_t> s_reflex_sync_calls{0};
static std::atomic<uint32_t> s_reflex_sync_lines{0};
struct ReflexSyncWindow {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> enables{0};
    std::atomic<uint64_t> disables{0};
    std::atomic<uint64_t> queue_samples{0};     // calls with bEnable
    std::atomic<int64_t> queue_sum{0};
    std::atomic<int32_t> queue_min{INT32_MAX};
    std::atomic<int32_t> queue_max{INT32_MIN};
    std::atomic<NvU32> target_us{0};            // newest values
    std::atomic<NvU32> vblank_us{0};
    std::atomic<NvU32> fg_multiplier{0};
    std::atomic<ULONGLONG> next_summary_ms{0};
};
static ReflexSyncWindow s_reflex_sync_window;

static void RecordReflexSyncCall(const NV_SET_REFLEX_SYNC_PARAMS& seen) noexcept {
    ReflexSyncWindow& window = s_reflex_sync_window;
    window.calls.fetch_add(1, std::memory_order_relaxed);
    if (seen.bDisable) window.disables.fetch_add(1, std::memory_order_relaxed);
    if (seen.bEnable) {
        window.enables.fetch_add(1, std::memory_order_relaxed);
        const int32_t queue_us = seen.timeInQueueUs;
        window.queue_samples.fetch_add(1, std::memory_order_relaxed);
        window.queue_sum.fetch_add(queue_us, std::memory_order_relaxed);
        int32_t low = window.queue_min.load(std::memory_order_relaxed);
        while (queue_us < low && !window.queue_min.compare_exchange_weak(low, queue_us, std::memory_order_relaxed)) {}
        int32_t high = window.queue_max.load(std::memory_order_relaxed);
        while (queue_us > high && !window.queue_max.compare_exchange_weak(high, queue_us, std::memory_order_relaxed)) {}
    }
    window.target_us.store(seen.timeInQueueUsTarget, std::memory_order_relaxed);
    window.vblank_us.store(seen.vblankIntervalUs, std::memory_order_relaxed);
    window.fg_multiplier.store(seen.fgMultiplier, std::memory_order_relaxed);
    // The first call starts the first window.
    const ULONGLONG now = GetTickCount64();
    ULONGLONG next = window.next_summary_ms.load(std::memory_order_relaxed);
    if (next == 0) {
        window.next_summary_ms.compare_exchange_strong(next, now + kReflexSyncSummaryMs, std::memory_order_relaxed);
        return;
    }
    if (now < next ||
        !window.next_summary_ms.compare_exchange_strong(next, now + kReflexSyncSummaryMs, std::memory_order_relaxed))
        return;
    const uint64_t calls = window.calls.exchange(0, std::memory_order_relaxed);
    const uint64_t enables = window.enables.exchange(0, std::memory_order_relaxed);
    const uint64_t disables = window.disables.exchange(0, std::memory_order_relaxed);
    const uint64_t samples = window.queue_samples.exchange(0, std::memory_order_relaxed);
    const int64_t sum = window.queue_sum.exchange(0, std::memory_order_relaxed);
    const int32_t low_us = window.queue_min.exchange(INT32_MAX, std::memory_order_relaxed);
    const int32_t high_us = window.queue_max.exchange(INT32_MIN, std::memory_order_relaxed);
    if (calls == 0) return;
    if (samples != 0) {
        LOG_INFO("Reflex observe: SetReflexSync summary calls=%llu enable=%llu disable=%llu "
                 "time_in_queue_us min=%d mean=%.1f max=%d (n=%llu) target=%u us vblank_interval=%u us "
                 "fg_multiplier=%u",
                 static_cast<unsigned long long>(calls), static_cast<unsigned long long>(enables),
                 static_cast<unsigned long long>(disables), low_us,
                 static_cast<double>(sum) / static_cast<double>(samples), high_us,
                 static_cast<unsigned long long>(samples), window.target_us.load(std::memory_order_relaxed),
                 window.vblank_us.load(std::memory_order_relaxed),
                 static_cast<unsigned>(window.fg_multiplier.load(std::memory_order_relaxed)));
    } else {
        LOG_INFO("Reflex observe: SetReflexSync summary calls=%llu enable=%llu disable=%llu "
                 "time_in_queue_us none (no enable call) target=%u us vblank_interval=%u us fg_multiplier=%u",
                 static_cast<unsigned long long>(calls), static_cast<unsigned long long>(enables),
                 static_cast<unsigned long long>(disables), window.target_us.load(std::memory_order_relaxed),
                 window.vblank_us.load(std::memory_order_relaxed),
                 static_cast<unsigned>(window.fg_multiplier.load(std::memory_order_relaxed)));
    }
}

static NvAPI_Status __cdecl Hook_SetReflexSync(IUnknown* device, NV_SET_REFLEX_SYNC_PARAMS* params) {
    void* const caller_address = _ReturnAddress();
    RuntimeCallbackGuard callback;
    NV_SET_REFLEX_SYNC_PARAMS seen{};
    // The inputs as the game gave them, copied before the driver sees them.
    const bool read = !callback.IsStopping() && params && SafeCopyReflexSyncParams(params, &seen);
    const NvAPI_Status result = SafeInvokeSetReflexSync(device, params);
    if (!read) return result;
    const uint64_t call = s_reflex_sync_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    RecordReflexSyncCall(seen);
    const uint32_t flags = (seen.bEnable ? 1u : 0u) | (seen.bDisable ? 2u : 0u) | (seen.flagsRsvd << 2);
    uint64_t signature = kObserveHashSeed;
    signature = ObserveHash(signature, &result, sizeof(result));
    signature = ObserveHash(signature, &seen.version, sizeof(seen.version));
    signature = ObserveHash(signature, &flags, sizeof(flags));
    signature = ObserveHash(signature, &seen.vblankIntervalUs, sizeof(seen.vblankIntervalUs));
    signature = ObserveHash(signature, &seen.timeInQueueUsTarget, sizeof(seen.timeInQueueUsTarget));
    signature = ObserveHash(signature, &seen.fgMultiplier, sizeof(seen.fgMultiplier));
    signature = ObserveHash(signature, seen.rsvd, sizeof(seen.rsvd));
    if (s_reflex_sync_signature.exchange(signature, std::memory_order_relaxed) == signature) return result;
    if (!ReflexObserveBudget(s_reflex_sync_lines, kReflexSyncLogLimit, "SetReflexSync parameters changed again"))
        return result;
    char module[MAX_PATH];
    CallerModuleName(caller_address, module, sizeof(module));
    char hex[2 * sizeof(seen.rsvd) + 1];
    char rsvd_text[sizeof(hex) + 8] = "";
    if (ReservedBytesHex(seen.rsvd, sizeof(seen.rsvd), hex, sizeof(hex)))
        std::snprintf(rsvd_text, sizeof(rsvd_text), " rsvd=%s", hex);
    LOG_INFO("Reflex observe: SetReflexSync call %llu: status=%d version=0x%08x enable=%u disable=%u "
             "flags_rsvd=0x%x vblank_interval=%u us time_in_queue=%d us time_in_queue_target=%u us "
             "fg_multiplier=%u%s module=%s thread=%lu",
             static_cast<unsigned long long>(call), result, seen.version, seen.bEnable ? 1u : 0u,
             seen.bDisable ? 1u : 0u, static_cast<unsigned>(seen.flagsRsvd), seen.vblankIntervalUs,
             seen.timeInQueueUs, seen.timeInQueueUsTarget, static_cast<unsigned>(seen.fgMultiplier), rsvd_text,
             module, static_cast<unsigned long>(GetCurrentThreadId()));
    return result;
}

static const char* OutOfBandTypeName(int type) noexcept {
    switch (type) {
    case 0: return "render";
    case 1: return "present";
    case 2: return "ignore";
    case 3: return "render_present";
    default: return "unknown";
    }
}

static std::atomic<uint32_t> s_out_of_band_lines{0};
static NvAPI_Status __cdecl Hook_NotifyOutOfBandCommandQueue(IUnknown* queue, int type) {
    void* const caller_address = _ReturnAddress();
    RuntimeCallbackGuard callback;
    const NvAPI_Status result = SafeInvokeNotifyOutOfBand(queue, type);
    if (callback.IsStopping() ||
        !ReflexObserveBudget(s_out_of_band_lines, kOutOfBandLogLimit, "NotifyOutOfBandCommandQueue calls continue"))
        return result;
    char module[MAX_PATH];
    CallerModuleName(caller_address, module, sizeof(module));
    LOG_INFO("Reflex observe: NotifyOutOfBandCommandQueue queue=%p type=%d (%s) status=%d module=%s thread=%lu",
             static_cast<void*>(queue), type, OutOfBandTypeName(type), result, module,
             static_cast<unsigned long>(GetCurrentThreadId()));
    return result;
}

static SleepStatusTracker s_game_sleep_status;
static NvAPI_Status __cdecl Hook_GetSleepStatus(IUnknown* device, NV_GET_SLEEP_STATUS_PARAMS* params) {
    void* const caller_address = _ReturnAddress();
    RuntimeCallbackGuard callback;
    // Without the status instrumentation (developer_settings or sleep_status_every_frame)
    // the game's call is only forwarded and logged: no feedback-device acquisition. With
    // it, the published feedback device and its publication token are captured before the
    // driver query: a result is the frame's sleep status only when it came from that
    // device and the publication has not changed since (RecordSleepStatus). The
    // acquisition does not wait and never drains retired references (a contended or
    // empty one drops the observation).
    const bool observe = g_config.developer_settings || g_sleep_status_every_frame.load(std::memory_order_relaxed);
    const NvAPIFeedbackDevice published = observe ? AcquireFeedbackDeviceImpl(false) : NvAPIFeedbackDevice{};
    const NvAPI_Status result = SafeInvokeGetSleepStatus(device, params);
    NV_GET_SLEEP_STATUS_PARAMS seen{};
    // The driver's answer, read after the call.
    if (callback.IsStopping() || !params || !SafeCopySleepStatus(params, &seen)) {
        NvAPI_ReleaseFeedbackDevice(published.device);
        return result;
    }
    if (published.device) {
        if (NvAPI_IsSameDevice(device, published.device))
            RecordSleepStatus(result, seen, static_cast<uint32_t>((published.publication_sequence >> 1) & kSleepStatusTokenMask));
        NvAPI_ReleaseFeedbackDevice(published.device);
    }
    if (!s_game_sleep_status.Changed(result, seen) ||
        !ReflexObserveBudget(s_game_sleep_status.lines, kGameSleepStatusLogLimit, "GetSleepStatus changed again"))
        return result;
    char module[MAX_PATH];
    CallerModuleName(caller_address, module, sizeof(module));
    LogSleepStatus("game call", result, seen, module);
    return result;
}

static std::atomic<uint32_t> s_low_latency_hint_lines{0};
static NvAPI_Status __cdecl Hook_SetCreateCommandQueueLowLatencyHint(IUnknown* device) {
    void* const caller_address = _ReturnAddress();
    RuntimeCallbackGuard callback;
    const NvAPI_Status result = SafeInvokeLowLatencyHint(device);
    if (callback.IsStopping() ||
        !ReflexObserveBudget(s_low_latency_hint_lines, kLowLatencyHintLogLimit,
            "SetCreateCommandQueueLowLatencyHint calls continue"))
        return result;
    char module[MAX_PATH];
    CallerModuleName(caller_address, module, sizeof(module));
    LOG_INFO("Reflex observe: SetCreateCommandQueueLowLatencyHint device=%p status=%d module=%s thread=%lu",
             static_cast<void*>(device), result, module, static_cast<unsigned long>(GetCurrentThreadId()));
    return result;
}
#endif

// ── Public API ──
NvAPI_Status InvokeSetLatencyMarker(IUnknown* dev, NV_LATENCY_MARKER_PARAMS* params) {
    return s_orig_set_latency_marker(dev, params);
}

NvAPI_Status InvokeSetSleepMode(IUnknown* dev, NV_SET_SLEEP_MODE_PARAMS* params) {
    return s_orig_sleep_mode(dev, params);
}

void RestoreGameSleepMode() {
    if (!s_orig_sleep_mode) return;
    // Copy under the lock: Hook_SetSleepMode updates the parameters at runtime
    // and the driver limiter restores them outside shutdown.
    NV_SET_SLEEP_MODE_PARAMS params{};
    NvAPIFeedbackDevice params_identity{};
    {
        GameParamsGuard lock;
        if (!s_game_params_captured) return;
        params = s_game_original_params;
        params_identity = s_game_params_identity;
    }
    const NvAPIFeedbackDevice feedback_device =
        NvAPI_AcquireFeedbackDevice();
    if (!feedback_device.device) return;
    const bool matching_identity =
        NvAPI_IsSameFeedbackPublication(params_identity, feedback_device) &&
        NvAPI_IsFeedbackDeviceCurrent(feedback_device);
    if (!matching_identity) {
        NvAPI_ReleaseFeedbackDevice(feedback_device.device);
        return;
    }
    LOG_INFO("Restoring game's sleep mode params");
    SafeInvokeSetSleepMode(feedback_device.device, &params);
    // The game's parameters, low latency mode included, are the driver's again.
    s_low_latency_forced_off.store(false, std::memory_order_release);
    NvAPI_ReleaseFeedbackDevice(feedback_device.device);
}

DriverLimiterApply NvAPI_ApplyDriverLimiter(uint32_t minimum_interval_us) noexcept {
    // The limiter applies while the Sleep is forwarded: driver_low_latency_off
    // decides whether the call carries the game's low latency settings or the
    // override (bypass_low_latency_off needs the bypass, which excludes the limiter).
    // The driver call and the flag that records it are one operation under the
    // forward lock, like UpdateLowLatencyOverride: a concurrent SetSleepMode
    // can neither land between the two nor read a flag that does not match
    // the driver. Nonblocking admission: a held lock leaves the operation
    // pending (Busy), repaired by the next OnMarker's call.
    if (!TryAcquireSRWLockExclusive(&s_sleep_mode_forward_lock)) return DriverLimiterApply::Busy;
    const bool low_latency_off = g_driver_low_latency_off.load(std::memory_order_relaxed);
    const DriverLimiterApply result = ApplyCapturedSleepMode(&minimum_interval_us, low_latency_off);
    if (result == DriverLimiterApply::Applied) {
        s_low_latency_forced_off.store(low_latency_off, std::memory_order_release);
        // Only while a reconcile is pending (the key on, or one to settle): the record
        // no longer describes the driver. Settled and key off: nothing is recorded.
        if (NvAPI_SleepModeReconcilePending()) InvalidateAppliedSleepMode();
    }
    ReleaseSRWLockExclusive(&s_sleep_mode_forward_lock);
    return result;
}

void NvAPI_ReleaseDriverLimiter() noexcept {
    // Key unified_pacer or a pending epoch: the tuple's own reconcile applies the row
    // that holds now (the limiter interval is already 0 here). Otherwise the game's
    // parameters, as before.
    if (NvAPI_SleepModeReconcilePending()) {
        NvAPI_ReconcileSleepMode();
        return;
    }
    RestoreGameSleepMode();
}

// ── Installation ──
// NvAPI functions are accessed via NvAPI_QueryInterface with known IDs.
// The caller (dllmain) resolves the addresses and passes them here,
// or we resolve them ourselves from nvapi64.dll / nvapi.dll.

#ifdef _WIN64
// The targets InstallNvAPIHooks queued, for NvAPIHooks_ReportInstalled (written once, by the installing thread).
struct NvApiInstallTargets {
    bool pending = false;
    void* set_latency_marker = nullptr;
    void* async_frame_marker = nullptr;
    void* set_flip_config = nullptr;
    void* reflex_sync = nullptr;
    void* out_of_band = nullptr;
    void* sleep_status = nullptr;
    void* low_latency_hint = nullptr;
};
static NvApiInstallTargets s_nvapi_install_targets;
#endif

// What the install reports and acts on is read after the enable: inside a batch InstallHook only queues.
void NvAPIHooks_ReportInstalled() {
#ifdef _WIN64
    NvApiInstallTargets& targets = s_nvapi_install_targets;
    if (!targets.pending) return;
    targets.pending = false;
    if (targets.set_latency_marker && Hooks_TargetEnabled(targets.set_latency_marker)) {
        FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::NvApiFrame);
    }
    const auto installed = [](void* target) { return Hooks_TargetEnabled(target) ? "installed" : "not installed"; };
    if (targets.async_frame_marker)
        LOG_INFO("NvAPI: SetAsyncFrameMarker timing hook %s", installed(targets.async_frame_marker));
    if (targets.set_flip_config)
        LOG_INFO("NvAPI: SetFlipConfig observation hook %s", installed(targets.set_flip_config));
    const auto observer = [&](void* target) { return target ? installed(target) : "unavailable"; };
    LOG_INFO("Reflex observe: hooks SetReflexSync %s, NotifyOutOfBandCommandQueue %s, GetSleepStatus %s, "
             "SetCreateCommandQueueLowLatencyHint %s",
             observer(targets.reflex_sync), observer(targets.out_of_band),
             observer(targets.sleep_status), observer(targets.low_latency_hint));
#endif
}

void InstallNvAPIHooks() {
#ifndef _WIN64
    // NvAPI Reflex hooks are 64-bit only — Reflex/DLSS/FG don't exist on 32-bit.
    // The 32-bit nvapi.dll may resolve the same function IDs to unrelated internal
    // functions, producing garbage marker data that breaks enforcement path selection.
    return;
#else
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (!nvapi) return;

    // NvAPI_QueryInterface is the single export used to get all NvAPI functions
    auto NvAPI_QI = reinterpret_cast<void*(__cdecl*)(NvU32)>(
        GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!NvAPI_QI) return;

    // NvAPI function IDs (well-known, from NvAPI SDK)
    constexpr NvU32 ID_SetSleepMode     = 0xAC1CA9E0;
    constexpr NvU32 ID_Sleep            = 0x852CD1D2;
    constexpr NvU32 ID_SetLatencyMarker = 0xD9984C05;

    void* pSetSleepMode     = NvAPI_QI(ID_SetSleepMode);
    void* pSleep            = NvAPI_QI(ID_Sleep);
    void* pSetLatencyMarker = NvAPI_QI(ID_SetLatencyMarker);
    constexpr NvU32 ID_SetAsyncFrameMarker = 0x13C98F73;
    void* pSetAsyncFrameMarker = NvAPI_QI(ID_SetAsyncFrameMarker);

    NvApiInstallTargets& targets = s_nvapi_install_targets;
    targets = {};
    targets.pending = true;
    if (pSetSleepMode)
        InstallHook(pSetSleepMode, (void*)&Hook_SetSleepMode, (void**)&s_orig_sleep_mode);
    if (pSleep)
        InstallHook(pSleep, (void*)&Hook_Sleep, (void**)&s_orig_sleep);
    if (pSetLatencyMarker) {
        targets.set_latency_marker = pSetLatencyMarker;
        InstallHook(
            pSetLatencyMarker,
            (void*)&Hook_SetLatencyMarker,
            (void**)&s_orig_set_latency_marker);
    }
    if (pSetAsyncFrameMarker) {
        targets.async_frame_marker = pSetAsyncFrameMarker;
        InstallHook(
            pSetAsyncFrameMarker,
            (void*)&Hook_SetAsyncFrameMarker,
            (void**)&s_orig_set_async_frame_marker);
    }
    // Through the block's trampoline when the block's hook exists (enabled now or
    // later by a reload): this request must never reach the block, which would log
    // it as its first hidden request. Under the block frame generation never gets
    // the function, so nothing is observed.
    if (void* pSetFlipConfig = s_orig_query_interface ? s_orig_query_interface(kNvApiIdSetFlipConfig)
                                                      : NvAPI_QI(kNvApiIdSetFlipConfig)) {
        targets.set_flip_config = pSetFlipConfig;
        InstallHook(pSetFlipConfig, (void*)&Hook_SetFlipConfig, (void**)&s_orig_set_flip_config);
    }
    // Reflex observation hooks: resolved through NvAPI_QI like the hooks above.
    const auto install_observer = [&](NvU32 id, void* detour, void** original) -> void* {
        void* target = NvAPI_QI(id);
        if (target) InstallHook(target, detour, original);
        return target;
    };
    targets.reflex_sync = install_observer(kNvApiIdSetReflexSync,
        reinterpret_cast<void*>(&Hook_SetReflexSync), reinterpret_cast<void**>(&s_orig_set_reflex_sync));
    targets.out_of_band = install_observer(kNvApiIdNotifyOutOfBandCommandQueue,
        reinterpret_cast<void*>(&Hook_NotifyOutOfBandCommandQueue),
        reinterpret_cast<void**>(&s_orig_notify_out_of_band));
    targets.sleep_status = install_observer(kNvApiIdGetSleepStatus,
        reinterpret_cast<void*>(&Hook_GetSleepStatus), reinterpret_cast<void**>(&s_orig_get_sleep_status));
    targets.low_latency_hint = install_observer(kNvApiIdSetCreateCommandQueueLowLatencyHint,
        reinterpret_cast<void*>(&Hook_SetCreateCommandQueueLowLatencyHint),
        reinterpret_cast<void**>(&s_orig_low_latency_hint));
    // Inside a batch the hooks are only queued: the caller reports after the batch applied.
    if (!Hooks_InBatch()) NvAPIHooks_ReportInstalled();
#endif // _WIN64
}

// ── Flip metering block (developer flip_metering_off) ──
// NvAPI_D3D12_SetFlipConfig (QueryInterface ID 0xF3148C42) configures hardware
// flip metering: the display engine spaces the flips of a batch of frames,
// which DLSS Frame Generation relies on where the GPU has it. The key hides the
// function: nvapi_QueryInterface returns nullptr for its ID (as Special K,
// OptiScaler and RenoDX Commander do), so frame generation never configures
// flip metering and runs as on a GPU without it. Installed during init, not
// with the deferred hooks: frame generation resolves the function itself, and
// a pointer it resolved before the hook stays usable.
#ifdef _WIN64
static std::atomic<bool> s_flip_config_hidden_logged{false};

static void* __cdecl Hook_QueryInterface(NvU32 id) {
    if (id != kNvApiIdSetFlipConfig) return s_orig_query_interface ? s_orig_query_interface(id) : nullptr;
    if (!s_flip_config_hidden_logged.exchange(true, std::memory_order_relaxed)) {
        char path[MAX_PATH] = "unknown";
        HMODULE caller = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCSTR>(_ReturnAddress()), &caller) && caller != nullptr) {
            GetModuleFileNameA(caller, path, MAX_PATH);
        }
        const char* name = std::strrchr(path, '\\');
        LOG_WARN("Flip metering off: NvAPI_D3D12_SetFlipConfig hidden from %s", name ? name + 1 : path);
    }
    return nullptr;
}
#endif

// The block's target while its install awaits the report (written by the installing thread).
static void* s_flip_block_target = nullptr;

void InstallFlipMeteringBlock() {
#ifdef _WIN64
    // Pinned: the hook lives in nvapi64.dll for the rest of the process.
    HMODULE nvapi = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"nvapi64.dll", &nvapi) &&
        (LoadLibraryW(L"nvapi64.dll") == nullptr ||
         !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"nvapi64.dll", &nvapi))) {
        LOG_WARN("Flip metering off: nvapi64.dll unavailable, nothing hidden");
        return;
    }
    void* query = reinterpret_cast<void*>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (query == nullptr) {
        LOG_WARN("Flip metering off: nvapi_QueryInterface not found, nothing hidden");
        return;
    }
    // Inside a batch the hook is only queued: the caller reports after the batch applied.
    s_flip_block_target = query;
    const MH_STATUS status = InstallHook(query, reinterpret_cast<void*>(&Hook_QueryInterface),
        reinterpret_cast<void**>(&s_orig_query_interface));
    if (status != MH_OK) {
        LOG_WARN("Flip metering off: nvapi_QueryInterface hook not installed (%d), flip metering unchanged", status);
        s_flip_block_target = nullptr;
        return;
    }
    if (!Hooks_InBatch()) ReportFlipMeteringBlock();
#endif
}

void ReportFlipMeteringBlock() {
#ifdef _WIN64
    void* const query = s_flip_block_target;
    if (!query) return;
    s_flip_block_target = nullptr;
    if (Hooks_TargetEnabled(query)) {
        LOG_INFO("Flip metering off: nvapi_QueryInterface hook installed, NvAPI_D3D12_SetFlipConfig hidden from later requests");
    } else {
        LOG_WARN("Flip metering off: nvapi_QueryInterface hook not installed, flip metering unchanged");
    }
#endif
}
