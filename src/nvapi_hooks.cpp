#include "fg_input_observer.h"
#include "nvapi_hooks.h"
#include "feedback_device_identity.h"
#include "hooks.h"
#include "marker_log.h"
#include "marker_stamps.h"
#include "marker_cycles.h"
#include "scheduler.h"
#include "pacing_sleep.h"
#include "correlator.h"
#include "config.h"
#include "health.h"
#include "pcl_hooks.h"
#include "streamline_hooks.h"
#include "streamline_gpu_handoff.h"
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
#include "display_state.h"
#include "wake_guard.h"
#include "logger.h"
#include "startup_breadcrumb.h"
#include "hang_progress.h"
#include "instrumentation_state.h"
#include "pacing_resolver.h"
#include "unified_pacer.h"
#include "frame_plan.h"
#include "gpu_readiness.h"
#include "module_presence.h"
#include <intrin.h>
#include <array>
#include <atomic>
#include <bitset>
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
// The feedback-device publication. `s_feedback_native`: the owned canonical NATIVE device (one reference, held for the
// published lifetime). `s_feedback_alias`: the incoming pointer it was captured with when that pointer is the ReShade
// proxy bound to it; BORROWED, never dereferenced, compared by value only. Both are written only inside the
// transition section (BeginFeedbackDeviceTransition .. End) and cleared together.
static std::atomic<IUnknown*> s_feedback_native{ nullptr };
static std::atomic<IUnknown*> s_feedback_alias{ nullptr };
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
// s_low_latency_forced_off: true while the driver has the Unified tuple's low latency off
// (set with the applied tuple, cleared whenever the game's parameters, or the limiter's
// interval with the game's low latency, reach the driver); only the transition log reads it
// (PreviousSleepMode). The forward lock serializes a transition (driver call and state
// change) with the forwarding of the game's own SetSleepMode calls, so a game call can
// neither land between the two nor read a state that does not match the driver; a
// reconcile that finds it taken skips and retries on its next Sleep.
static std::atomic<bool> s_low_latency_forced_off{ false };
static SRWLOCK s_sleep_mode_forward_lock = SRWLOCK_INIT;

// Unified pacer (unified owner): the applied Sleep-mode record
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
    g_pacing_pending_sleep.Invalidate();
    s_game_params_identity = {};
    s_game_params_captured = false;
    g_game_requested_interval.store(0, std::memory_order_relaxed);
    // The device the override was applied to is gone.
    s_low_latency_forced_off.store(false, std::memory_order_release);
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

// The feedback-device publication, read as one consistent view under the transition sequence (as
// NvAPI_IsFeedbackDeviceCurrent reads it). Values only: nothing here is dereferenced.
struct FeedbackView {
    IUnknown* native = nullptr;   // the owned canonical native (null: nothing published)
    IUnknown* alias = nullptr;    // the incoming pointer it was captured with, when bound to it (borrowed; never dereferenced)
    uint64_t publication = 0;
    uint32_t epoch = 0;
    uint64_t transition = 0;      // the transition sequence the view was read under (even)
};

// False while a transition is in progress or when one ran across the read.
static bool ReadFeedbackView(FeedbackView* view) noexcept {
    const uint64_t before = s_feedback_device_transition_sequence.load(std::memory_order_acquire);
    if ((before & 1u) != 0) return false;
    view->publication = s_feedback_device_publication_sequence.load(std::memory_order_relaxed);
    view->native = s_feedback_native.load(std::memory_order_relaxed);
    view->alias = s_feedback_alias.load(std::memory_order_relaxed);
    view->epoch = FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame);
    view->transition = before;
    std::atomic_thread_fence(std::memory_order_acquire);
    return before == s_feedback_device_transition_sequence.load(std::memory_order_relaxed);
}

// The incoming pointer is the publication's device by VALUE: its native, or the alias captured with it.
static bool IncomingIsPublished(IUnknown* incoming, const FeedbackView& view) noexcept {
    return view.native && (incoming == view.native || (view.alias && incoming == view.alias));
}

// The presenting device (SwapMgr) as a canonical native identity VALUE. False: none, or a publication in progress.
static bool PresentingIdentity(uint64_t* identity) noexcept {
    uint64_t sequence = 0;
    *identity = SwapMgr_PresentingDeviceIdentity(&sequence);
    return *identity != 0;
}

// What a hook learns from one look at an incoming device pointer (the game's or a plugin's: possibly a ReShade
// device proxy, which is never AddRef'd, Released or queried here).
//   presenting: the device is the presenting device (or, before any presenter is known, none contradicts it).
//   identity:   the publication token when the device is the published feedback device (a value, no reference).
//   capture:    an OWNED native device (released by the destructor unless taken) when the device is presenting
//               but not published yet.
struct IncomingDevice {
    bool presenting = false;
    NvAPIFeedbackDevice identity{};
    IUnknown* capture = nullptr;
    IncomingDevice() = default;
    IncomingDevice(const IncomingDevice&) = delete;
    IncomingDevice& operator=(const IncomingDevice&) = delete;
    ~IncomingDevice() noexcept { NvAPI_ReleaseFeedbackDevice(capture); }
};

// One log line the first time an incoming device is the published one by identity but not by value (a wrapper):
// that device takes one unwrap per call. The observable for the alias rule.
static void LogUnaliasedIncoming(IUnknown* incoming, IUnknown* native) noexcept {
    static std::atomic<bool> logged{ false };
    if (logged.exchange(true, std::memory_order_relaxed)) return;
    LOG_INFO("NvAPI: incoming device %p is the published device %p by identity only; it is resolved per call",
             static_cast<void*>(incoming), static_cast<void*>(native));
}

// Resolves an incoming device pointer. A pointer equal to the publication's native or alias is answered from values
// (no COM call, no lock, no interlocked operation). Any other pointer is unwrapped ONCE (IID_UnwrappedObject, which
// ReShade answers without its device lock) to its native device. The relation to the presenting device is a value
// comparison of canonical native identities. `published_first`: a device that is not the published one is rejected
// before the presenting device is consulted (the Sleep hook's order).
static void ResolveIncomingDevice(
    IUnknown* incoming, const NV_LATENCY_MARKER_PARAMS* marker, IncomingDevice* out,
    bool published_first = false) noexcept {
    if (!incoming) {
        LogMarkerRejection(MarkerRejection::MissingIncoming, incoming, nullptr, marker);
        return;
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        FeedbackView view;
        if (!ReadFeedbackView(&view)) continue;
        IUnknown* owned = nullptr;
        IUnknown* identity = view.native;
        bool published = IncomingIsPublished(incoming, view);
        if (!published) {
            if (published_first && !view.native) {
                LogMarkerRejection(MarkerRejection::FeedbackIdentity, incoming, nullptr, marker);
                return;
            }
            owned = DeviceIdentity_AcquireNative(incoming);
            if (!owned) {
                LogMarkerRejection(MarkerRejection::FeedbackIdentity, incoming, nullptr, marker);
                return;
            }
            identity = owned;
            if (view.native && owned == view.native) {
                LogUnaliasedIncoming(incoming, view.native);
                published = true;
                NvAPI_ReleaseFeedbackDevice(owned);
                owned = nullptr;
            } else if (published_first) {
                NvAPI_ReleaseFeedbackDevice(owned);
                LogMarkerRejection(MarkerRejection::FeedbackIdentity, incoming, nullptr, marker);
                return;
            }
        }
        const ActiveAPI api = SwapMgr_GetActiveAPI();
        uint64_t presenting_identity = 0;
        bool presenting = false;
        if (!PresentingIdentity(&presenting_identity)) {
            // Before presenter discovery, native NvAPI calls may bootstrap feedback. An empty publication is NOT
            // permission to bypass an already active presenter. A failed/contended read with an active API is
            // conservatively rejected.
            if (api != ActiveAPI::None)
                LogMarkerRejection(MarkerRejection::SelectedUnavailable, incoming, nullptr, marker);
            presenting = api == ActiveAPI::None;
        } else {
            const bool com_api = api == ActiveAPI::DX11 || api == ActiveAPI::DX12;
            presenting = com_api && reinterpret_cast<uint64_t>(identity) == presenting_identity;
            if (!presenting)
                LogMarkerRejection(
                    com_api ? MarkerRejection::SelectedIdentity : MarkerRejection::UnsupportedAPI,
                    incoming, reinterpret_cast<IUnknown*>(presenting_identity), marker);
        }
        if (!presenting) {
            NvAPI_ReleaseFeedbackDevice(owned);
            return;
        }
        // Nothing may have been published or cleared since the view: the token and the presenting relation then
        // describe the same publication.
        if (s_feedback_device_transition_sequence.load(std::memory_order_acquire) != view.transition) {
            NvAPI_ReleaseFeedbackDevice(owned);
            continue;
        }
        out->presenting = true;
        if (published) out->identity = { view.native, view.epoch, view.publication };
        else out->capture = owned;
        return;
    }
    LogMarkerRejection(MarkerRejection::GuardStaleToken, incoming, nullptr, marker);
}

// The incoming pointer is the published device: its token, with no presenting requirement (the sleep-status
// observer). False: not the published device, or the publication kept moving.
static bool ResolvePublishedToken(IUnknown* incoming, NvAPIFeedbackDevice* token) noexcept {
    if (!incoming) return false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        FeedbackView view;
        if (!ReadFeedbackView(&view)) continue;
        if (!view.native) return false;
        if (!IncomingIsPublished(incoming, view)) {
            IUnknown* owned = DeviceIdentity_AcquireNative(incoming);
            const bool same = owned == view.native;
            NvAPI_ReleaseFeedbackDevice(owned);
            if (!same) return false;
        }
        if (s_feedback_device_transition_sequence.load(std::memory_order_acquire) != view.transition) continue;
        *token = { view.native, view.epoch, view.publication };
        return true;
    }
    return false;
}

// Is the device of this publication token the presenting device? Values only: no incoming pointer is involved. The
// token's device is a canonical native identity, compared with the presenting device's.
static bool FeedbackIsPresenting(
    const NvAPIFeedbackDevice& token, const NV_LATENCY_MARKER_PARAMS* marker = nullptr) noexcept {
    const ActiveAPI api = SwapMgr_GetActiveAPI();
    uint64_t presenting_identity = 0;
    if (!PresentingIdentity(&presenting_identity)) {
        if (api != ActiveAPI::None)
            LogMarkerRejection(MarkerRejection::SelectedUnavailable, token.device, nullptr, marker);
        return api == ActiveAPI::None;
    }
    const bool com_api = api == ActiveAPI::DX11 || api == ActiveAPI::DX12;
    const bool presenting = com_api && reinterpret_cast<uint64_t>(token.device) == presenting_identity;
    if (!presenting)
        LogMarkerRejection(
            com_api ? MarkerRejection::SelectedIdentity : MarkerRejection::UnsupportedAPI,
            token.device, reinterpret_cast<IUnknown*>(presenting_identity), marker);
    return presenting;
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

bool NvAPI_HasFeedbackDevice() noexcept {
    return s_feedback_native.load(std::memory_order_acquire) != nullptr;
}

// The alias of a new publication: the incoming pointer it was captured with, when that pointer is provably bound to
// this native device, that is the ReShade device proxy the native names in its private data. Anything else (a
// Streamline wrapper, an unknown layer) is not an alias and is unwrapped at every call. The native itself needs no
// alias: it is compared as the native. Never dereferenced afterwards.
static IUnknown* ProxyAliasFor(IUnknown* incoming, IUnknown* native) noexcept {
    if (!incoming || incoming == native) return nullptr;
    return DeviceIdentity_ReShadeProxyOf(native) == incoming ? incoming : nullptr;
}

// Publishes `native` (an owned canonical native device, consumed: published or released) as the feedback device.
// `incoming` is the pointer it was resolved from (alias candidate). False: it already is the published device (the
// original publication, token and restore state are preserved), or the publication kept moving.
static bool CaptureFeedbackNative(
    IUnknown* incoming, IUnknown* native, NvAPIFeedbackDevice* published_device) noexcept {
    IUnknown* const alias = ProxyAliasFor(incoming, native);
    for (int attempt = 0; attempt < 3; ++attempt) {
        FeedbackView view;
        if (!ReadFeedbackView(&view)) continue;
        if (view.native == native) {
            // A no-op needs no writer section.
            NvAPI_ReleaseFeedbackDevice(native);
            return false; // Preserve the original publication AND token.
        }
        const uint64_t write_sequence = BeginFeedbackDeviceTransition();
        if (write_sequence != view.transition + 1) {
            // A writer ran since the view: the publication it compared may have moved.
            EndFeedbackDeviceTransition(write_sequence);
            continue;
        }
        IUnknown* previous = s_feedback_native.load(std::memory_order_relaxed);
        FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::NvApiFrame);
        ResetFeedbackProducerLifecycle();
        ResetGameSleepParams();
        s_feedback_native.store(native, std::memory_order_relaxed);
        s_feedback_alias.store(alias, std::memory_order_relaxed);
        if (published_device) {
            *published_device = {
                native,
                FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame),
                s_feedback_device_publication_sequence.load(
                    std::memory_order_relaxed) + 2
            };
        }
        EndFeedbackDeviceTransition(write_sequence, true);
        // s_feedback_native owns exactly one reference (on the native) for the published lifetime.
        RetireFeedbackDevice(previous);
        return true;
    }
    NvAPI_ReleaseFeedbackDevice(native);
    return false; // Bounded retry under publication churn; caller may retry.
}

bool NvAPI_CaptureFeedbackDevice(
    IUnknown* device,
    NvAPIFeedbackDevice* published_device) noexcept {
    if (published_device) *published_device = {};
    if (!device) return false;
    FeedbackView view;
    if (ReadFeedbackView(&view) && IncomingIsPublished(device, view)) return false;
    IUnknown* native = DeviceIdentity_AcquireNative(device);
    if (!native) return false;
    return CaptureFeedbackNative(device, native, published_device);
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
        IUnknown* device = s_feedback_native.load(std::memory_order_relaxed);
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
    IUnknown* device = s_feedback_native.load(std::memory_order_relaxed);
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
            s_feedback_native.load(std::memory_order_relaxed),
            FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame),
            s_feedback_device_publication_sequence.load(std::memory_order_relaxed)
        })) {
        EndFeedbackDeviceTransition(write_sequence);
        return;
    }
    IUnknown* cleared_device = s_feedback_native.load(std::memory_order_relaxed);
    s_feedback_native.store(nullptr, std::memory_order_relaxed);
    s_feedback_alias.store(nullptr, std::memory_order_relaxed);
    ResetGameSleepParams();
    FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::NvApiFrame);
    ResetFeedbackProducerLifecycle();
    EndFeedbackDeviceTransition(write_sequence, true);
    RetireFeedbackDevice(cleared_device);
}

void NvAPI_ClearFeedbackDevice(IUnknown* expected_device) noexcept {
    // `expected_device` names the publication by the pointer it was captured with: the native or the alias.
    const auto names_publication = [expected_device](IUnknown* native, IUnknown* alias) noexcept {
        return !expected_device || expected_device == native || (alias && expected_device == alias);
    };
    if (expected_device &&
        !names_publication(s_feedback_native.load(std::memory_order_acquire),
                           s_feedback_alias.load(std::memory_order_acquire))) {
        return;
    }
    const uint64_t write_sequence = BeginFeedbackDeviceTransition();
    IUnknown* cleared_device = s_feedback_native.load(std::memory_order_relaxed);
    if (!cleared_device ||
        !names_publication(cleared_device, s_feedback_alias.load(std::memory_order_relaxed))) {
        EndFeedbackDeviceTransition(write_sequence);
        return;
    }

    s_feedback_native.store(nullptr, std::memory_order_relaxed);
    s_feedback_alias.store(nullptr, std::memory_order_relaxed);
    ResetGameSleepParams();
    FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::NvApiFrame);
    ResetFeedbackProducerLifecycle();
    EndFeedbackDeviceTransition(write_sequence, true);
    RetireFeedbackDevice(cleared_device);
}

// destroy_device. Runs under ReShade's process-wide device lock, inside the Release of a device proxy: it makes no
// COM call at all. `identity` is the canonical identity of the destroyed device's native object. Another device (the
// common case: a second adapter's device, a launcher's, a probe's): a value comparison, no writer section. The
// published device: the publication is cleared exactly as NvAPI_ClearFeedbackDevice clears it, and its reference
// (on the native) is released, or retired to the next drain when a reader is inside its window. That release is on a
// native object, which takes no ReShade lock.
void NvAPI_OnDeviceDestroyed(uint64_t identity) noexcept {
    if (identity == 0) return;
    for (;;) {
        FeedbackView view;
        // A writer section is a few atomic operations and never calls COM: wait it out.
        if (!ReadFeedbackView(&view)) { SwitchToThread(); continue; }
        if (reinterpret_cast<uint64_t>(view.native) != identity) return;
        break;
    }
    const uint64_t write_sequence = BeginFeedbackDeviceTransition();
    IUnknown* const cleared_device = s_feedback_native.load(std::memory_order_relaxed);
    if (reinterpret_cast<uint64_t>(cleared_device) != identity) {
        EndFeedbackDeviceTransition(write_sequence);
        return;
    }
    s_feedback_native.store(nullptr, std::memory_order_relaxed);
    s_feedback_alias.store(nullptr, std::memory_order_relaxed);
    ResetGameSleepParams();
    FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::NvApiFrame);
    ResetFeedbackProducerLifecycle();
    EndFeedbackDeviceTransition(write_sequence, true);
    RetireFeedbackDevice(cleared_device);
}

// The capture step of Hook_SetSleepMode and Hook_SetLatencyMarker. `resolved` is the result of ResolveIncomingDevice
// for `incoming`: when it handed over an unpublished presenting device, that native device is published now. On
// return `resolved->identity` is the token of the publication that names `incoming` (empty when none does: another
// device is published and this one could not be, or churn). True when this call made a new publication.
static bool CaptureResolvedDevice(
    IUnknown* incoming, IncomingDevice* resolved, const NV_LATENCY_MARKER_PARAMS* marker) noexcept {
    bool captured = false;
    if (IUnknown* native = resolved->capture) {
        resolved->capture = nullptr;
        NvAPIFeedbackDevice published{};
        captured = CaptureFeedbackNative(incoming, native, &published);
        if (captured) resolved->identity = published;
    }
    if (!resolved->identity.device) {
        // Published meanwhile by another thread, or not at all: read the publication again.
        IncomingDevice again;
        ResolveIncomingDevice(incoming, marker, &again);
        resolved->identity = again.identity;
        if (!resolved->identity.device)
            LogMarkerRejection(MarkerRejection::FeedbackIdentity, incoming, nullptr, marker);
    }
    return captured;
}

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
// Hook_Sleep (the presenting device), through the trampoline: the game's hook
// never sees it. It carries the driver's answer about the game's frames
// (DisplayState_OnSleepStatus) for games that do not ask for it themselves: a
// game call within the last second skips it, unless the developer
// instrumentation (developer_settings) records and logs the status.
static constexpr uint32_t kPolledSleepStatusLogLimit = 64;
static SleepStatusTracker s_polled_sleep_status;
static std::atomic<ULONGLONG> s_sleep_status_poll_ms{0};
static std::atomic<ULONGLONG> s_game_sleep_status_ms{0};   // tick of the newest successful game call

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
// source record carries it. `publication_token` is
// the token of the publication the Sleep retained at entry
// (PublicationToken(sleep_device.identity)): the result is carried only when it
// belongs to that publication and that publication is still the current one.
static void PublishSleepStatusAtSleepReturn(uint32_t publication_token) noexcept {
    if (!PacingTelemetry_Active()) return;
    const auto pending = g_pacing_pending_sleep.Observe();
    const uint64_t newest = g_sleep_status_newest.load(std::memory_order_relaxed);
    const bool current = (newest >> 63) != 0 && SleepStatusToken(newest) == publication_token &&
        publication_token == g_sleep_status_token.load(std::memory_order_acquire);
    g_pacing_pending_sleep.Publish(pending, current, static_cast<uint32_t>(newest), ((newest >> 32) & 1) != 0);
}

static void PollSleepStatus(IUnknown* device, uint32_t publication_token) noexcept {
    if (!s_orig_get_sleep_status) return;
    const bool developer = g_config.developer_settings;
    const ULONGLONG now = GetTickCount64();
    if (!developer && now - s_game_sleep_status_ms.load(std::memory_order_relaxed) < 1000) return;
    ULONGLONG next = s_sleep_status_poll_ms.load(std::memory_order_relaxed);
    if (now < next ||
        !s_sleep_status_poll_ms.compare_exchange_strong(next, now + 1000, std::memory_order_relaxed))
        return;
    // The token is the retained Sleep device's (passed in), not whatever publication is
    // current now: a query that straddled a publication change describes the old one, so
    // neither RecordSleepStatus nor the driver's answer about the game's frames takes it.
    NV_GET_SLEEP_STATUS_PARAMS status{};
    status.version = static_cast<NvU32>(NV_GET_SLEEP_STATUS_PARAMS_VER);
    const NvAPI_Status result = SafeInvokeGetSleepStatus(device, &status);
    if (result == NVAPI_OK && publication_token == g_sleep_status_token.load(std::memory_order_acquire))
        DisplayState_OnSleepStatus(status.bFsVrr != 0, status.bFullscreenIFlip != 0, /*own_read=*/true);
    if (!developer) return;
    RecordSleepStatus(result, status, publication_token);
    if (s_polled_sleep_status.Changed(result, status) &&
        ReflexObserveBudget(s_polled_sleep_status.lines, kPolledSleepStatusLogLimit, "sleep status poll changed again"))
        LogSleepStatus("poll", result, status, nullptr);
}

// Thread map: the thread of the newest call of the Reflex Sleep, of each
// latency marker type and of each async marker type (with its queue), and
// the Sleeps that did not follow the frame pattern. A changed field publishes a
// generation; the Present is left out (no accessor of the game Present's thread exists).
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
    std::atomic<uint64_t> generation{0}, checked_generation{0};
    std::atomic<uint64_t> logged_signature{0};
    std::atomic<uint32_t> lines{0};
};
static ReflexThreadMap s_reflex_threads;

template<class T>
static void ObserveThreadMapField(std::atomic<T>& field, T value) noexcept {
    if (field.load(std::memory_order_relaxed) == value) return;
    field.store(value, std::memory_order_relaxed);
    s_reflex_threads.generation.fetch_add(1, std::memory_order_release);
}

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
    ObserveThreadMapField(s_reflex_threads.sleep_thread, static_cast<uint32_t>(GetCurrentThreadId()));
    if (s_reflex_threads.sleeps_since_simulation_start.fetch_add(1, std::memory_order_relaxed) != 0)
        s_reflex_threads.double_sleeps.fetch_add(1, std::memory_order_relaxed);
    const uint64_t calls = s_reflex_threads.sleep_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (calls < kThreadMapAfterSleeps) return;
    const uint64_t generation = s_reflex_threads.generation.load(std::memory_order_acquire);
    const bool first = calls == kThreadMapAfterSleeps;
    if (!first && generation == s_reflex_threads.checked_generation.load(std::memory_order_relaxed)) return;
    s_reflex_threads.checked_generation.store(generation, std::memory_order_relaxed);
    // Changes that reverted between Sleeps must not consume a log line.
    const uint64_t signature = ThreadMapSignature();
    const uint64_t previous = s_reflex_threads.logged_signature.exchange(signature, std::memory_order_relaxed);
    // A slower observer must not leave an obsolete signature behind a newer observer's
    // already-checked generation. Reconcile on the next Sleep if a writer raced our scan.
    if (generation != s_reflex_threads.generation.load(std::memory_order_acquire))
        s_reflex_threads.checked_generation.store(UINT64_MAX, std::memory_order_relaxed);
    if ((first || previous != signature) && ReflexObserveBudget(s_reflex_threads.lines, kThreadMapLogLimit, "thread map changed again"))
        LogThreadMap();
}

// A latency marker of the game on the presenting device (Hook_SetLatencyMarker).
static void ObserveLatencyMarkerThread(uint32_t type) noexcept {
    if (type >= kObserveMarkerTypes) return;
    ObserveThreadMapField(s_reflex_threads.marker_thread[type], static_cast<uint32_t>(GetCurrentThreadId()));
    // A SIMULATION_START with no Sleep since the previous one: a frame without a Sleep.
    if (type == SIMULATION_START &&
        s_reflex_threads.sleeps_since_simulation_start.exchange(0, std::memory_order_relaxed) == 0)
        s_reflex_threads.frames_without_sleep.fetch_add(1, std::memory_order_relaxed);
}

static void ObserveAsyncMarkerThread(uint32_t type, IUnknown* queue) noexcept {
    if (type >= kObserveMarkerTypes) return;
    ObserveThreadMapField(s_reflex_threads.async_thread[type], static_cast<uint32_t>(GetCurrentThreadId()));
    ObserveThreadMapField(s_reflex_threads.async_queue[type], reinterpret_cast<uint64_t>(queue));
}

// RenderSubmitEnd(N) and SimulationStart(N+1) can be emitted concurrently.
// Tail diagnostics must not take the single scheduler owner and make the
// actual enforcement marker fail admission. Only the two possible release
// markers need immediate scheduler ownership, and RenderSubmitStart only while
// it is or can become the enforcement marker (Hook_SetLatencyMarker). PresentStart runs its output
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

// ── Unified pacer: the Sleep-mode tuple ──
static const char* SleepOwnerText(UnifiedSleepOwner owner) noexcept {
    switch (owner) {
    case UnifiedSleepOwner::Unified: return "unified";
    case UnifiedSleepOwner::Limiter: return "limiter";
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

// The desired tuple (UnifiedPacer_SleepModeDesired) for these game parameters, under this key.
static UnifiedSleepModeTuple DesiredSleepMode(
    bool key, uint64_t publication, uint32_t game_interval_us) noexcept {
    UnifiedSleepModeInputs inputs;
    inputs.key = key;
    inputs.dmfg = key && IsDmfgActive(); // only the key-on rows depend on it
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
static bool DesiredForCapturedParams(UnifiedSleepModeTuple* desired, bool* key,
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
    *key = PacingPipeline_Unified();
    // The key the configuration is publishing is noted first (NvAPI_PrepareUnifiedKey): while
    // it differs from the published one a transition is in flight and nothing is forwarded
    // for either side of it. The reconcile stays pending.
    if (((s_transition_state.load(std::memory_order_acquire) & kTransitionKey) != 0) != *key) return false;
    *desired = DesiredSleepMode(*key, publication, game_interval_us);
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
static void ReconcileSleepMode() noexcept {
    UnifiedSleepModeTuple desired;
    bool key = false;
    uint32_t generation = 0;
    if (!DesiredForCapturedParams(&desired, &key, &generation)) return;
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
    ReconcileSleepMode();
}

// Hook_SetSleepMode under the forward lock, key on or an epoch pending: the desired tuple
// built from the game's NEW parameters goes to the driver and, once accepted, is the
// applied record. The owner Game forwards the game's own pointer unchanged.
static NvAPI_Status ForwardDesiredSleepMode(IUnknown* device, NV_SET_SLEEP_MODE_PARAMS* original,
    const NV_SET_SLEEP_MODE_PARAMS& snapshot, const NvAPIFeedbackDevice& identity) noexcept {
    const uint32_t generation = s_key_generation.load(std::memory_order_acquire);
    const bool key = PacingPipeline_Unified();
    const UnifiedSleepModeTuple desired = DesiredSleepMode(
        key, identity.publication_sequence, snapshot.minimumIntervalUs);
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

// What the Sleep hook's wait needs beyond the hard limit's environment: the tick rate, one
// bounded wait toward a target (PreSleepWaitUntil in production) and that wait with the
// scheduler owner's release as a second way out (Scheduler_AwaitOwner). A scripted
// environment drives the whole flow in the tests.
class UnifiedSleepEnv : public UnifiedLimitEnv {
public:
    virtual int64_t Frequency() noexcept = 0;
    // Returns when the wait ended: at the target, or earlier (cancelled, failed, stopping).
    virtual void StartWait(int64_t target_qpc) noexcept = 0;
    // The same wait, ended also by the release of the scheduler owner: true then (or when the
    // owner is free already). False: it ended as StartWait does.
    virtual bool OwnerWait(int64_t target_qpc) noexcept = 0;

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
    void StartWait(int64_t target_qpc) noexcept override { (void)PreSleepWaitUntil(target_qpc); }
    bool OwnerWait(int64_t target_qpc) noexcept override { return Scheduler_AwaitOwner(target_qpc); }

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

// The end of one chunk of a wait from `now` toward `target`.
static int64_t UnifiedWaitChunkTarget(UnifiedSleepEnv& env, int64_t now, int64_t target) noexcept {
    const int64_t frequency = env.Frequency();
    const int64_t chunk_ticks = frequency > 0 ? frequency / kUnifiedStartWaitChunkDivisor : 0;
    return chunk_ticks > 0 && target - now > chunk_ticks ? now + chunk_ticks : target;
}

// One wait toward a start target, in bounded chunks. It returns before the target only when a chunk
// ended early (cancelled, failed or stopping).
static void UnifiedWaitToTarget(UnifiedSleepEnv& env, int64_t target) noexcept {
    for (int64_t now = env.Now(); now < target; now = env.Now()) {
        const int64_t chunk_target = UnifiedWaitChunkTarget(env, now, target);
        env.StartWait(chunk_target);
        if (env.Now() < chunk_target) break; // cancelled, failed or stopping: R < T
    }
}

// The scheduler owner's release, awaited until a deadline in the same chunks. True: released before
// the deadline. False: the deadline was reached, or a chunk ended on its own.
static bool UnifiedAwaitOwner(UnifiedSleepEnv& env, int64_t deadline) noexcept {
    for (int64_t now = env.Now(); now < deadline; now = env.Now()) {
        const int64_t chunk_target = UnifiedWaitChunkTarget(env, now, deadline);
        if (env.OwnerWait(chunk_target)) return true;
        if (env.Now() < chunk_target) break;
    }
    return false;
}

// The start wait to T first, in bounded chunks, ended early only by a cancellation, a failure
// or the shutdown (R < T: Failed); the hard limit at E, the moment after the wait reached T
// (the arrival when the frame is already late), never before T; R = the clock after it; the
// unfinished frames re-read at R (inactive stays -1). `held`: this Sleep arrived before T and
// waited for the scheduler owner before it could reserve, so a release at or after T is a
// completed start wait whatever is left of it.
static UnifiedSleepWaitResult UnifiedSleepWait(
    UnifiedSleepEnv& env, const SchedulerUnifiedReservation& reservation, bool held) noexcept {
    UnifiedSleepWaitResult result;
    const int64_t target = reservation.target_qpc;
    const bool wait_needed = held || target > env.Now();
    UnifiedWaitToTarget(env, target);
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
// Sleep and an inactive pipeline wait for nothing and publish nothing: the Sleep is simply
// forwarded. A busy owner (another thread's marker or Present holds it for a moment) must not
// move the start wait behind the game's SIMULATION_START: Streamline stamps that marker before
// it reaches the marker hook and DLSS-G sets its flip metering batch time from the spacing of
// those stamps, so a wait at the marker shows two frames at the wrong time. The Sleep waits for
// the owner's release instead and reserves then, with its own arrival: the reservation plans
// the exact T and waits to it like any other. The law's start estimate bounds that wait, so a
// holder that keeps the owner cannot hold the frame past its expected start; at the estimate
// the Sleep reserves once more. No estimate ahead (the frame is late, or the law has no
// origin), or the owner still taken at the estimate: forwarded, the marker plans.
static void UnifiedSleepStart(UnifiedSleepEnv& env, int64_t arrival_qpc) noexcept {
    SchedulerUnifiedReservation reservation;
    SchedulerUnifiedReserve status = Scheduler_UnifiedReserve(arrival_qpc, &reservation);
    bool waited = false;
    while (status == SchedulerUnifiedReserve::Busy && reservation.target_qpc > env.Now()) {
        const bool released = UnifiedAwaitOwner(env, reservation.target_qpc);
        waited = true;
        status = Scheduler_UnifiedReserve(arrival_qpc, &reservation);
        if (!released) break; // the estimate, or the wait ended on its own: that was the last reservation
    }
    if (status != SchedulerUnifiedReserve::Reserved) return;
    const bool held = waited && reservation.target_qpc > arrival_qpc;
    const UnifiedSleepWaitResult waited_start = UnifiedSleepWait(env, reservation, held);
    Scheduler_UnifiedPublishSleep(reservation, waited_start.outcome, waited_start.release_qpc, arrival_qpc,
        waited_start.limit);
}

// Self-check on the genuine Sleep call: a driver that paces on its own (the control
// panel's low latency mode, a game limit) makes it last. The low bit is the newest
// Sleep, 1 if over 200 us: "driver paces" at 16 of the last 64 or more, cleared
// below 3; one log line per transition, g_unified_driver_pacing for the flag and status.
static std::atomic<uint64_t> s_unified_sleep_history{0};
constexpr size_t kUnifiedSleepHistorySize = 64;
static std::atomic<uint32_t> s_unified_check_epoch{ 0 };
constexpr double kUnifiedDriverPacesSleepUs = 200.0;
constexpr uint32_t kUnifiedDriverPacesSet = 16;
constexpr uint32_t kUnifiedDriverPacesClear = 3;

static void UnifiedSleepSelfCheck(int64_t duration_ticks) noexcept {
    const uint32_t epoch = s_sleep_mode_epoch.load(std::memory_order_relaxed);
    if (epoch != s_unified_check_epoch.load(std::memory_order_relaxed)) {
        // A key toggle or a Dynamic MFG transition: start over.
        s_unified_sleep_history.store(0, std::memory_order_relaxed);
        s_unified_check_epoch.store(epoch, std::memory_order_relaxed);
        g_unified_driver_pacing.store(false, std::memory_order_relaxed);
    }
    const uint64_t bit = qpc_to_us(duration_ticks) > kUnifiedDriverPacesSleepUs ? 1 : 0;
    uint64_t previous = s_unified_sleep_history.load(std::memory_order_relaxed);
    uint64_t history;
    do {
        history = (previous << 1) | bit;
        if (previous == history) break; // homogeneous window: the load is this push's linearization point
    } while (!s_unified_sleep_history.compare_exchange_weak(previous, history, std::memory_order_relaxed));
    const uint32_t count = static_cast<uint32_t>(std::bitset<kUnifiedSleepHistorySize>(history).count());
    const bool paces = g_unified_driver_pacing.load(std::memory_order_relaxed);
    if (!paces && count >= kUnifiedDriverPacesSet) {
        g_unified_driver_pacing.store(true, std::memory_order_relaxed);
        LOG_INFO("NvAPI: unified pacer: the driver's Sleep paces the game (%u of %zu Sleeps over %.0f us)",
            count, kUnifiedSleepHistorySize, kUnifiedDriverPacesSleepUs);
    } else if (paces && count < kUnifiedDriverPacesClear) {
        g_unified_driver_pacing.store(false, std::memory_order_relaxed);
        LOG_INFO("NvAPI: unified pacer: the driver's Sleep no longer paces the game (%u of %zu Sleeps over %.0f us)",
            count, kUnifiedSleepHistorySize, kUnifiedDriverPacesSleepUs);
    }
}

// ── Hook: SetSleepMode — capture game params, forward them (the limiter's interval on a copy) ──
static NvAPI_Status __cdecl Hook_SetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* params) {
    void* const caller_address = _ReturnAddress();
    RuntimeCallbackGuard callback;
    if (callback.IsStopping())
        return SafeInvokeSetSleepMode(pDev, params);
    // One resolve answers "is this the presenting device" and "is it the published feedback device" from values
    // (or one unwrap); the token it yields is the one the restore state below binds to.
    IncomingDevice params_device;
    ResolveIncomingDevice(pDev, nullptr, &params_device);
    if (!params_device.presenting)
        return SafeInvokeSetSleepMode(pDev, params);

    NV_SET_SLEEP_MODE_PARAMS params_snapshot{};
    const bool params_valid =
        SafeCopySleepModeParams(params, &params_snapshot);

    if (CaptureResolvedDevice(pDev, &params_device, nullptr)) {
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

    // Forwarding is serialized with the Sleep-mode transitions (above).
    AcquireSRWLockExclusive(&s_sleep_mode_forward_lock);
    NvAPI_Status result = NVAPI_OK;
    // While the Reflex driver limiter is applied, the game's own call
    // would clear it: forward a copy with ReLimiter's interval instead. The
    // game's parameters above stay captured for the restore.
    const uint32_t limiter_us = g_driver_limiter_interval_us.load(std::memory_order_relaxed);
    if (params_valid && params_device.identity.device && NvAPI_SleepModeReconcilePending()) {
        // Unified owner or a pending epoch: the desired tuple, built from the
        // game's new parameters, replaces the two branches below.
        result = ForwardDesiredSleepMode(pDev, params, params_snapshot, params_device.identity);
    } else if (limiter_us != 0 && params_valid && params_device.identity.device) {
        NV_SET_SLEEP_MODE_PARAMS limited = params_snapshot;
        limited.minimumIntervalUs = limiter_us;
        result = SafeInvokeSetSleepMode(pDev, &limited);
    } else {
        // Forward game's params unmodified.
        result = SafeInvokeSetSleepMode(pDev, params);
    }
    ReleaseSRWLockExclusive(&s_sleep_mode_forward_lock);
    return result;
}

// The Reflex Sleep hook's progress for the stall report (hang_progress.h): entered, left (every way out of the hook),
// and the time and thread of the last entry. A thread that entered and never left is inside the hook.
struct SleepProgressScope {
    SleepProgressScope() noexcept {
        if (!HangReport_Active()) return;
        HangProgress& progress = g_hang_progress;
        LARGE_INTEGER entry;
        QueryPerformanceCounter(&entry);
        progress.sleep_entry_qpc.store(static_cast<uint64_t>(entry.QuadPart), std::memory_order_relaxed);
        progress.sleep_entry_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        HangProgress_Bump(progress.sleep_entered);
        static std::atomic<bool> s_trail_first_sleep{false};
        Breadcrumb_TrailFirst(s_trail_first_sleep, TrailKind::FirstReflexSleep);
    }
    ~SleepProgressScope() { HangProgress_Bump(g_hang_progress.sleep_left); }
    SleepProgressScope(const SleepProgressScope&) = delete;
    SleepProgressScope& operator=(const SleepProgressScope&) = delete;
};

// ── Hook: Sleep — forward to driver for Reflex queue management. ──
// The driver's Sleep call handles internal timing synchronization even
// with minimumIntervalUs=0. Forwarding at the game's natural call point
// preserves optimal pipeline timing for non-overload throughput.
static NvAPI_Status __cdecl Hook_Sleep(IUnknown* pDev) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) return SafeInvokeSleep(pDev);
    const SleepProgressScope hang_progress_scope;
    IncomingDevice sleep_device;
    ResolveIncomingDevice(pDev, nullptr, &sleep_device, /*published_first=*/true);
    const uint32_t sleep_token = PublicationToken(sleep_device.identity);
    if (!sleep_device.identity.device) return SafeInvokeSleep(pDev);

    // Smooth Motion: bypass Reflex Sleep entirely — SM controls presentation
    // timing at the driver level, and Sleep calls conflict with it.
    if (IsNvSmoothMotionActive()) return NVAPI_OK;

    if (s_orig_sleep) {
        // The bypass is decided once per call, before the start wait.
        // Unified owner: this Sleep is the start of the frame. On the
        // presenting device it runs the unified start wait, and the bypass is
        // not consulted: the driver's Sleep always runs. With the key off, one load and
        // a not-taken branch: the bypass first, then the presenting device, as before.
        bool bypass = false;
        bool presenting_at_entry = false;
        bool unified = false;
        if (PacingPipeline_Unified()) {
            presenting_at_entry = FeedbackIsPresenting(sleep_device.identity);
            unified = presenting_at_entry && Scheduler_UnifiedActive();
            bypass = !unified && NvAPI_ShouldBypassSleep(
                PacingPipeline_SleepBypass(),
                g_user_target_fps.load(std::memory_order_relaxed), SleepSourcePaced());
        } else {
            bypass = NvAPI_ShouldBypassSleep(PacingPipeline_SleepBypass(),
                g_user_target_fps.load(std::memory_order_relaxed), SleepSourcePaced());
            presenting_at_entry = FeedbackIsPresenting(sleep_device.identity);
        }
        // Record the Sleep on the presenting device; with the key the unified start wait
        // runs here, before the driver's Sleep.
        if (presenting_at_entry) {
            ObserveReflexSleep();
            // The Sleep-mode tuple (key on, or an epoch pending).
            if (NvAPI_SleepModeReconcilePending()) ReconcileSleepMode();
            LARGE_INTEGER sleep_entry{};
            QueryPerformanceCounter(&sleep_entry);
            Scheduler_RecordReflexSleep(sleep_entry.QuadPart);
            if (unified) {
                UnifiedLimitClock clock;
                UnifiedSleepStart(clock, sleep_entry.QuadPart);
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
            !FeedbackIsPresenting(sleep_device.identity)) {
            return result;
        }
        // The driver's Sleep for the next grid index: its start and return go
        // to the scheduler, which records them for the frame. A busy owner
        // skips it. The call itself is unchanged.
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
    // Progress for the stall report (hang_progress.h), the entry time already read above: two stores, none while the
    // report is off.
    if (HangReport_Active()) {
        g_hang_progress.marker_qpc.store(static_cast<uint64_t>(census_entry.QuadPart), std::memory_order_relaxed);
        HangProgress_Bump(g_hang_progress.marker_count);
        static std::atomic<bool> s_trail_first_marker{false};
        Breadcrumb_TrailFirst(s_trail_first_marker, TrailKind::FirstLatencyMarker);
    }
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
    IncomingDevice marker_device;
    ResolveIncomingDevice(dev, &marker_params, &marker_device);
    if (!marker_device.presenting)
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

    // Capture device for GetLatency if not already set. Some games call
    // SetLatencyMarker on a different device than SetSleepMode, or only the marker
    // hook sees the presenting device first. The marker hook always receives the device.
    if (CaptureResolvedDevice(dev, &marker_device, &marker_params))
        LOG_INFO("NvAPI: presenting device captured/replaced from SetLatencyMarker");

    // Clock map: the local QPC closest to the driver's presentStartTime
    // is read immediately before forwarding; it is recorded only after the
    // marker passes validation below.
    LARGE_INTEGER present_start_forward_qpc{};
    if (marker_params.markerType == PRESENT_START && !from_rtss)
        QueryPerformanceCounter(&present_start_forward_qpc);
    // Forward to driver FIRST — Reflex's JIT pacing depends on accurate
    // marker timing. Any work we do before forwarding shifts the driver's
    // perception of pipeline stage boundaries and degrades latency.
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
    forward();
    if (!from_rtss) ObserveLatencyMarkerThread(static_cast<uint32_t>(marker_params.markerType));
    // Never relabel a marker from A with B's epoch after the driver returns.
    // Failed admission still forwards exactly once with the original pointer.
    if (!marker_device.identity.device) return forward(); // Guard logged the cause.
    if (!NvAPI_IsFeedbackDeviceCurrent(marker_device.identity)) {
        LogMarkerRejection(MarkerRejection::ForwardStaleToken, dev, nullptr, &marker_params);
        return forward();
    }
    if (!FeedbackIsPresenting(marker_device.identity, &marker_params)) return forward();

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
    // RENDERSUBMIT_START of a planned frame is a stage event for the lead
    // (the simulation → render queue), before the scheduler-owner guard: it
    // runs on the render thread, often while the next SIMULATION_START owns
    // the scheduler, and the push never blocks.
    if (marker_params.markerType == RENDERSUBMIT_START && !from_rtss && !PCL_MarkersFlowing()) {
        LARGE_INTEGER submit_qpc{};
        QueryPerformanceCounter(&submit_qpc);
        PresentGate_OnFrameMarker(PacingPlanKey{PacingIdentityDomain::NvApiFrame,
            marker_device.identity.producer_epoch,
            FrameTimeline_CurrentControllerGeneration(), marker_params.frameID},
            PacingStageEventKind::RenderSubmitStart, submit_qpc.QuadPart);
    }
    // RENDERSUBMIT_START(N) arrives from the render thread at the moment the game thread calls the
    // Sleep and SIMULATION_START of frame N + 1, which both need the scheduler owner. It claims the
    // owner only while it is, or can still become, the enforcement marker: configured as it, chosen
    // by the fallback, or before this producer lifetime admitted its first SIMULATION_START (the
    // fallback fires only with none). Otherwise it is an observation like the tail markers: queued
    // here, logged when the next owner drains. The words are written under the owner; a read that
    // races a producer change queues an observation the drain drops by its epoch.
    const bool render_submit_deferred = marker_params.markerType == RENDERSUBMIT_START &&
        s_configured_enforcement_marker == SIMULATION_START &&
        s_enforcement_marker.load(std::memory_order_relaxed) == SIMULATION_START &&
        s_enforcement_hits.load(std::memory_order_relaxed) > 0 &&
        s_marker_policy_epoch.load(std::memory_order_acquire) == marker_device.identity.producer_epoch;
    if (!IsImmediateTimingMarker(marker_params.markerType) || render_submit_deferred) {
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
        // (the game's simulation → render queue and the start term's two
        // sign tests, lead_controller.h); the push never blocks. Stamped at
        // the hook's entry, as the PCL hook stamps them: the time after the
        // driver call can lie behind the game's next arrival when this
        // thread is descheduled inside it, and the start term would read
        // that as a game asking for a frame before its Present ended.
        if ((marker_params.markerType == SIMULATION_END || marker_params.markerType == PRESENT_END) &&
            !PCL_MarkersFlowing()) {
            PresentGate_OnFrameMarker(frame_key, marker_params.markerType == SIMULATION_END
                ? PacingStageEventKind::SimulationEnd : PacingStageEventKind::PresentEnd,
                census_entry.QuadPart);
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
            // Present for H3.
            NativeGate_OnMarker(NativeGate_GetContext(captured_qpc.QuadPart), frame_key);
            NativeGate_PublishRouteCapability(frame_key.controller_generation);
            // H0 releases here; H1 holds here; H3 plans output later.
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
    SchedulerOwnerGuard scheduler_owner;
    if (!scheduler_owner) {
        LogMarkerRejection(MarkerRejection::SchedulerOwner, dev, nullptr, &marker_params);
        return forward();
    }
    if (!NvAPI_IsFeedbackDeviceCurrent(marker_device.identity)) {
        LogMarkerRejection(MarkerRejection::OwnerStaleToken, dev, nullptr, &marker_params);
        return forward();
    }
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
        // A marker forwarded before the wait leaves the wait inside the driver's stage.
        const bool stage_contains_wait = forwarded;
        forward();
        LARGE_INTEGER controller_end_qpc{};
        QueryPerformanceCounter(&controller_end_qpc);
        if (!NvAPI_IsFeedbackDeviceCurrent(marker_device.identity)) {
            LogMarkerRejection(MarkerRejection::EnforcementStaleToken, dev, nullptr, &marker_params);
            return forward();
        }
        // The residence is the ReLimiter time inside the driver's source
        // stage; the stage-demand computation subtracts it from that stage. A
        // marker not forwarded before the wait leaves the wait outside it: zero.
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
    static std::atomic<uint32_t> s_first_call_types{ 0 };
    const uint32_t type_bit = known && marker_type < 32 ? 1u << marker_type : 0u;
    const bool readout = PacingTelemetry_Active();
    const bool time_call = readout || (type_bit && !(s_first_call_types.load(std::memory_order_relaxed) & type_bit));
    LARGE_INTEGER call_entry{};
    if (time_call) QueryPerformanceCounter(&call_entry);
    const NvAPI_Status result = SafeInvokeAsyncFrameMarker(queue, params);
    LARGE_INTEGER call_exit{};
    if (time_call) QueryPerformanceCounter(&call_exit);
    if (!known) return result;

    // The frame id this call states for the frame being presented (PRESENT_START names the frame the Present of this
    // thread is running for): the frame generation route pairs that Present's marker by it, in a game that presents on
    // another thread than its markers (streamline_gpu_handoff.h). After the call has returned; it touches neither the
    // marker nor its result.
    StreamlineHandoff_RecordInbandFrame(marker_type, frame_id);

    ObserveAsyncMarkerThread(marker_type, queue);
    // The first call of each marker type (types above 31 are not logged).
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
    if (!readout) return result;
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
        // Hardware flip metering is at work only where the batch has frames to space and the driver names no
        // obstacle. A game without frame generation configures a batch of ONE in every frame, status 0 (Rogue
        // Core and Phantom Blade Zero, logs of 2026-11-17: frames_per_batch=1; 3 and 4 with frame generation at
        // 3x and 4x), and the driver's feedback says where it cannot meter (no independent flip, mirroring).
        if (result == NVAPI_OK && seen.nFramesPerBatch >= 2 && seen.flipConfigFeedbackMask == 0)
            g_flip_metering_accepted_qpc.store(call_qpc.QuadPart, std::memory_order_relaxed);
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
    // Streamline's in-band statement of frame generation's multiplier (fg_state_policy.h): one lock-free fold.
    Streamline_RecordReflexSyncStatement(seen.fgMultiplier, seen.bDisable != 0);
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
    // The published feedback device's token, read before the driver query (the incoming pointer is the
    // publication's alias, or unwrapped once; no reference is taken and no ReShade lock is entered; the lookup does
    // not wait and never drains retired references). A result counts only when it came from that device while it
    // presents, and its publication has not changed since: as the frame's sleep status (RecordSleepStatus, under
    // the status instrumentation, developer_settings) and as the driver's answer about the game's frames
    // (display_state.h). Another device's answer never decides the pacing mode and never stops ReLimiter's own read.
    const bool observe = g_config.developer_settings;
    NvAPIFeedbackDevice published{};
    const bool presenting = ResolvePublishedToken(device, &published) &&
        SwapMgr_GetActiveAPI() != ActiveAPI::None && FeedbackIsPresenting(published);
    const NvAPI_Status result = SafeInvokeGetSleepStatus(device, params);
    NV_GET_SLEEP_STATUS_PARAMS seen{};
    // The driver's answer, read after the call.
    if (callback.IsStopping() || !params || !SafeCopySleepStatus(params, &seen)) return result;
    // While the game asks (Streamline does), ReLimiter's own read (PollSleepStatus) stays off.
    if (presenting && result == NVAPI_OK &&
        PublicationToken(published) == g_sleep_status_token.load(std::memory_order_acquire)) {
        s_game_sleep_status_ms.store(GetTickCount64(), std::memory_order_relaxed);
        DisplayState_OnSleepStatus(seen.bFsVrr != 0, seen.bFullscreenIFlip != 0, /*own_read=*/false);
    }
    if (observe && published.device)
        RecordSleepStatus(result, seen, PublicationToken(published));
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
    // The limiter applies while the Sleep is forwarded, with the game's own low latency
    // settings. The driver call and the flag that records it are one operation under the
    // forward lock: a concurrent SetSleepMode can neither land between the two nor read a
    // flag that does not match the driver. Nonblocking admission: a held lock leaves the
    // operation pending (Busy), repaired by the next OnMarker's call.
    if (!TryAcquireSRWLockExclusive(&s_sleep_mode_forward_lock)) return DriverLimiterApply::Busy;
    const DriverLimiterApply result = ApplyCapturedSleepMode(&minimum_interval_us, false);
    if (result == DriverLimiterApply::Applied) {
        s_low_latency_forced_off.store(false, std::memory_order_release);
        // Only while a reconcile is pending (the key on, or one to settle): the record
        // no longer describes the driver. Settled and key off: nothing is recorded.
        if (NvAPI_SleepModeReconcilePending()) InvalidateAppliedSleepMode();
    }
    ReleaseSRWLockExclusive(&s_sleep_mode_forward_lock);
    return result;
}

void NvAPI_ReleaseDriverLimiter() noexcept {
    // Unified owner or a pending epoch: the tuple's own reconcile applies the row
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
