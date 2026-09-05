#include "nvapi_hooks.h"
#include "hooks.h"
#include "marker_log.h"
#include "predictor.h"
#include "scheduler.h"
#include "correlator.h"
#include "config.h"
#include "health.h"
#include "pcl_hooks.h"
#include "enforcement_dispatcher.h"
#include "streamline_hooks.h"
#include "frame_timeline.h"
#include "nvapi_marker_policy.h"
#include "feedback.h"

#include "presentation_gate.h"
#include "runtime_lifecycle.h"
#include "wake_guard.h"
#include "logger.h"
#include <intrin.h>
#include <array>
#include <atomic>
#include <algorithm>

// ── Trampolines ──
PFN_NvAPI_D3D_SetSleepMode     s_orig_sleep_mode      = nullptr;
PFN_NvAPI_D3D_Sleep            s_orig_sleep            = nullptr;
PFN_NvAPI_D3D_SetLatencyMarker s_orig_set_latency_marker = nullptr;

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
static NV_SET_SLEEP_MODE_PARAMS s_game_last_params = {};
static NvAPIFeedbackDevice s_game_params_identity = {};
static bool s_game_params_captured = false;
static std::atomic_flag s_game_params_lock = ATOMIC_FLAG_INIT;

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

static void ResetGameSleepParams() noexcept {
    GameParamsGuard lock;
    s_game_original_params = {};
    s_game_last_params = {};
    s_game_params_identity = {};
    s_game_params_captured = false;
    g_game_requested_interval.store(0, std::memory_order_relaxed);
}

bool NvAPI_IsSameDevice(IUnknown* left, IUnknown* right) noexcept {
    if (!left || !right) return false;
    if (left == right) return true;
    // Different COM interface pointers can represent the same D3D device.
    IUnknown* candidate_identity = nullptr;
    IUnknown* presenting_identity = nullptr;
    __try {
        left->QueryInterface(
            __uuidof(IUnknown),
            reinterpret_cast<void**>(&candidate_identity));
        right->QueryInterface(
            __uuidof(IUnknown),
            reinterpret_cast<void**>(&presenting_identity));
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        NvAPI_ReleaseFeedbackDevice(candidate_identity);
        NvAPI_ReleaseFeedbackDevice(presenting_identity);
        return false;
    }
    const bool matches = candidate_identity != nullptr &&
        candidate_identity == presenting_identity;
    NvAPI_ReleaseFeedbackDevice(candidate_identity);
    NvAPI_ReleaseFeedbackDevice(presenting_identity);
    return matches;
}

static bool IsPresentingFeedbackDevice(IUnknown* device) noexcept {
    if (!device) return false;
    IUnknown* presenting_device = SwapMgr_AcquirePresentingDevice();
    const ActiveAPI api = SwapMgr_GetActiveAPI();
    // Before presenter discovery, native NvAPI calls may bootstrap feedback.
    // An empty g_dev is NOT permission to bypass an already active presenter.
    // Failed/contended acquisition with an active API is conservatively rejected.
    if (!presenting_device) return api == ActiveAPI::None;
    const bool matches =
        (api == ActiveAPI::DX11 || api == ActiveAPI::DX12) &&
        NvAPI_IsSameDevice(device, presenting_device);
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

    const uint64_t write_sequence = BeginFeedbackDeviceTransition();
    IUnknown* previous = g_dev.load(std::memory_order_relaxed);
    if (previous == device) {
        EndFeedbackDeviceTransition(write_sequence);
        NvAPI_ReleaseFeedbackDevice(device);
        return false;
    }
    FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::NvApiFrame);
    ResetFeedbackProducerLifecycle();
    ResetGameSleepParams();
    g_dev.store(device, std::memory_order_relaxed);
    if (published_device) {
        *published_device = {
            device,
            FrameTimeline_CurrentProducerEpoch(
                FrameIdentityDomain::NvApiFrame),
            s_feedback_device_publication_sequence.load(
                std::memory_order_relaxed) + 2
        };
    }
    EndFeedbackDeviceTransition(write_sequence, true);
    // g_dev owns exactly one reference for the published lifetime.
    RetireFeedbackDevice(previous);
    return true;
}

NvAPIFeedbackDevice NvAPI_AcquireFeedbackDevice() noexcept {
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
            TryDrainRetiredFeedbackDevices();
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
        TryDrainRetiredFeedbackDevices();

        if (device && before == after && (after & 1u) == 0) {
            return { device, producer_epoch, publication_sequence };
        }
        NvAPI_ReleaseFeedbackDevice(device);
    }
    return {};
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
    explicit FeedbackDeviceGuard(IUnknown* device) noexcept
        : identity(NvAPI_AcquireFeedbackDevice()) {
        if (!NvAPI_IsSameDevice(identity.device, device) ||
            !IsPresentingFeedbackDevice(device) ||
            !NvAPI_IsFeedbackDeviceCurrent(identity)) {
            NvAPI_ReleaseFeedbackDevice(identity.device);
            identity = {};
        }
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
static std::atomic<HMODULE> g_rtss{ nullptr };

static bool CallerIsRTSS(void* return_address) {
    HMODULE rtss = g_rtss.load(std::memory_order_acquire);
    if (!rtss) {
        rtss = GetModuleHandleW(L"RTSSHooks64.dll");
        if (rtss) {
            g_rtss.store(rtss, std::memory_order_release);
        }
    }
    if (!rtss) return false;

    HMODULE caller = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(return_address), &caller);
    return caller == rtss;
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

// ── Hook: SetSleepMode — capture game params, forward unmodified ──
static NvAPI_Status __cdecl Hook_SetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* params) {
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
            LOG_WARN(
                "NvAPI: SetSleepMode first call — device captured, interval=%u us",
                params_valid ? params_snapshot.minimumIntervalUs : 0);
        }
    }

    // Bind original/last settings only to the exact current feedback device.
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
                s_game_last_params = {};
                s_game_params_identity = params_device.identity;
                s_game_params_captured = false;
            }
            if (!s_game_params_captured) {
                s_game_original_params = params_snapshot;
                s_game_params_captured = true;
            }
            s_game_last_params = params_snapshot;
            g_game_requested_interval.store(
                params_snapshot.minimumIntervalUs,
                std::memory_order_relaxed);
        }
    }

    // Forward game's params unmodified.
    return SafeInvokeSetSleepMode(pDev, params);
}

// ── Hook: Sleep — forward to driver for Reflex queue management. ──
// The driver's Sleep call handles internal timing synchronization even
// with minimumIntervalUs=0. Forwarding at the game's natural call point
// preserves optimal pipeline timing for non-overload throughput.
static NvAPI_Status __cdecl Hook_Sleep(IUnknown* pDev) {
    RuntimeCallbackGuard callback;
    if (callback.IsStopping()) return SafeInvokeSleep(pDev);
    const FeedbackDeviceGuard sleep_device(pDev);
    if (!sleep_device.identity.device) return SafeInvokeSleep(pDev);

    // Smooth Motion: bypass Reflex Sleep entirely — SM controls presentation
    // timing at the driver level, and Sleep calls conflict with it.
    if (IsNvSmoothMotionActive())
        return NVAPI_OK;

    if (s_orig_sleep) {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        const NvAPI_Status result = SafeInvokeSleep(pDev);
        QueryPerformanceCounter(&t1);
        if (!NvAPI_IsFeedbackDeviceCurrent(sleep_device.identity) ||
            !IsPresentingFeedbackDevice(pDev)) {
            return result;
        }
        double dur_us = qpc_to_us(t1.QuadPart - t0.QuadPart);
        if (dur_us > 0.0 && dur_us < 100000.0) {
            double prev = g_pacer_latency_us.load(std::memory_order_relaxed);
            double smoothed = (prev > 0.0) ? prev + 0.16 * (dur_us - prev) : dur_us;
            g_pacer_latency_us.store(smoothed, std::memory_order_relaxed);
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
    if (!IsPresentingFeedbackDevice(dev))
        return SafeInvokeLatencyMarker(dev, params);

    NV_LATENCY_MARKER_PARAMS marker_params{};
    if (!SafeCopyLatencyMarkerParams(params, &marker_params))
        return SafeInvokeLatencyMarker(dev, params);

    void* return_address = _ReturnAddress();
    const bool from_rtss = CallerIsRTSS(return_address);
    static std::atomic<bool> s_first_marker{ true };
    if (s_first_marker.exchange(false, std::memory_order_relaxed)) {
        LOG_WARN("NvAPI: First marker received — type=%u, frameID=%llu, source=%s",
                 marker_params.markerType, marker_params.frameID,
                 from_rtss ? "RTSS" : "direct");
    }

    // Capture device for GetLatency if not already set.
    // Some games (Streamline/DLSS-G) never call SetSleepMode through NvAPI,
    // so g_dev stays null. The marker hook always receives the device.
    if (NvAPI_CaptureFeedbackDevice(dev)) {
        LOG_WARN("NvAPI: presenting device captured/replaced from SetLatencyMarker");
    }
    const FeedbackDeviceGuard marker_device(dev);

    // Forward to driver FIRST — Reflex's JIT pacing depends on accurate
    // marker timing. Any work we do before forwarding shifts the driver's
    // perception of pipeline stage boundaries and degrades latency.
    NvAPI_Status result = SafeInvokeLatencyMarker(dev, params);
    // Never relabel a marker from A with B's epoch after the driver returns.
    // Failed admission still forwards exactly once with the original pointer.
    if (!NvAPI_IsFeedbackDeviceCurrent(marker_device.identity) ||
        !IsPresentingFeedbackDevice(dev)) {
        return result;
    }
    SchedulerOwnerGuard scheduler_owner;
    if (!scheduler_owner ||
        !NvAPI_IsFeedbackDeviceCurrent(marker_device.identity)) {
        return result;
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
        // Producer frame IDs may restart. Flush predictor monotonic/pending and
        // learned timing state before this lifetime's first marker is consumed.
        g_predictor.RequestFlush();
        s_marker_policy.Reset();
        s_enforcement_hits.store(0, std::memory_order_relaxed);
        s_render_submit_hits.store(0, std::memory_order_relaxed);
        s_fallback_applied.store(false, std::memory_order_relaxed);
        s_enforcement_marker.store(
            s_configured_enforcement_marker,
            std::memory_order_relaxed);
    }
    if (!s_marker_policy.ShouldProcess(
            from_rtss,
            static_cast<uint32_t>(marker_params.markerType),
            marker_params.frameID)) {
        return result;
    }
    if (from_rtss &&
        s_configured_enforcement_marker != SIMULATION_START) {
        return result;
    }
    if (from_rtss) {
        static std::atomic<bool> s_logged_rtss_simulation_start{ false };
        if (!s_logged_rtss_simulation_start.exchange(
                true,
                std::memory_order_relaxed)) {
            LOG_WARN(
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
        LOG_WARN("NvAPI: enforcement marker fallback — SIMULATION_START never received, "
                 "switching to RENDERSUBMIT_START (direct_markers=%llu, "
                 "rtss_sim_starts=%llu)",
                 s_marker_policy.DirectMarkerCount(),
                 s_marker_policy.RtssSimulationStartCount());
    }

    const ArrivalLeadMarker arrival_lead_marker =
        enforcement_marker == SIMULATION_START
            ? ArrivalLeadMarker::SimulationStart
            : ArrivalLeadMarker::RenderSubmitStart;
    const bool pcl_markers_flowing = PCL_MarkersFlowing();

    // Predictor and scheduler consume the same resolved marker snapshot,
    // including the exact callback that crosses the fallback threshold.
    // Skip NvAPI prediction when the PCL hook owns this marker stream.
    if (!pcl_markers_flowing) {
        g_predictor.OnMarker(
            static_cast<uint32_t>(marker_params.markerType),
            ts.QuadPart,
            marker_params.frameID,
            g_overload_active_flag.load(std::memory_order_relaxed),
            static_cast<uint32_t>(enforcement_marker));
    }

    // Take the exact-key deadline AFTER enforcement publishes it for this frame.
    // PRESENT_START needs THIS frame's deadline for the gate — not the previous
    // frame's. The old pre-enforcement snapshot captured frame N-1's deadline,
    // which is already in the past by the time PRESENT_START(N) fires, causing
    // the gate to never hold (session 48: 0.1% gating, -5.3% undershoot).
    if (marker_params.markerType == enforcement_marker && !pcl_markers_flowing) {
        if (!NvAPI_IsFeedbackDeviceCurrent(marker_device.identity)) return result;
        OnMarker(
            marker_params.frameID,
            ts.QuadPart,
            SchedulerTrigger::NvApiMarker,
            arrival_lead_marker);
        LARGE_INTEGER controller_end_qpc{};
        QueryPerformanceCounter(&controller_end_qpc);
        if (!NvAPI_IsFeedbackDeviceCurrent(marker_device.identity)) return result;
        PublishReflexControllerResidence(
            marker_params.frameID,
            marker_device.identity.producer_epoch,
            qpc_to_us(controller_end_qpc.QuadPart - ts.QuadPart));
    }

    // Feed correlator at PRESENT_START — only for present-based enforcement.
    // Marker-based paths (NvAPI/PCL) have unreliable scanout estimation with
    // FG because the gap between submission and PresentCount varies by 1-3
    // refresh periods frame-to-frame, producing ~6-22ms of noise that the
    // feedback loop can't track. The pacing quality from markers alone is
    // already excellent — the correlator just adds noise.
    // PRESENT_START gate: delegated to shared Presentation_Gate module.
    // Streamline forwards PCL markers through NvAPI after the PCL hook has
    // already gated them, so skip the complete local path while PCL flows.
    if (marker_params.markerType == PRESENT_START && !pcl_markers_flowing) {
        if (!NvAPI_IsFeedbackDeviceCurrent(marker_device.identity)) return result;
        Scheduler_PublishMarkerArrival(
            marker_params.frameID,
            ts.QuadPart,
            SchedulerTrigger::NvApiMarker);
        const SchedulerFrameDeadline deadline =
            Scheduler_TakeFrameDeadline(
                marker_params.frameID,
                SchedulerTrigger::NvApiMarker);
        if (!NvAPI_IsFeedbackDeviceCurrent(marker_device.identity) ||
            (deadline.producer_epoch != 0 &&
             deadline.producer_epoch != marker_device.identity.producer_epoch)) {
            return result;
        }
        PresentGate_Execute(
            ts.QuadPart,
            marker_params.frameID,
            deadline.deadline_qpc,
            marker_device.identity.producer_epoch,
            deadline.controller_generation);
    }

    return result;
}

// ── Public API ──
NvAPI_Status InvokeSetLatencyMarker(IUnknown* dev, NV_LATENCY_MARKER_PARAMS* params) {
    return s_orig_set_latency_marker(dev, params);
}

NvAPI_Status InvokeSetSleepMode(IUnknown* dev, NV_SET_SLEEP_MODE_PARAMS* params) {
    return s_orig_sleep_mode(dev, params);
}

void RestoreGameSleepMode() {
    if (!s_orig_sleep_mode || !s_game_params_captured) return;
    const NvAPIFeedbackDevice feedback_device =
        NvAPI_AcquireFeedbackDevice();
    if (!feedback_device.device) return;
    const bool matching_identity =
        NvAPI_IsSameFeedbackPublication(s_game_params_identity, feedback_device) &&
        NvAPI_IsFeedbackDeviceCurrent(feedback_device);
    if (!matching_identity) {
        NvAPI_ReleaseFeedbackDevice(feedback_device.device);
        return;
    }
    LOG_INFO("Restoring game's original sleep mode params");
    __try {
        s_orig_sleep_mode(
            feedback_device.device,
            &s_game_original_params);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    NvAPI_ReleaseFeedbackDevice(feedback_device.device);
}

// ── Installation ──
// NvAPI functions are accessed via NvAPI_QueryInterface with known IDs.
// The caller (dllmain) resolves the addresses and passes them here,
// or we resolve them ourselves from nvapi64.dll / nvapi.dll.

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

    if (pSetSleepMode)
        InstallHook(pSetSleepMode, (void*)&Hook_SetSleepMode, (void**)&s_orig_sleep_mode);
    if (pSleep)
        InstallHook(pSleep, (void*)&Hook_Sleep, (void**)&s_orig_sleep);
    if (pSetLatencyMarker) {
        const MH_STATUS status = InstallHook(
            pSetLatencyMarker,
            (void*)&Hook_SetLatencyMarker,
            (void**)&s_orig_set_latency_marker);
        if (status == MH_OK) {
            FrameTimeline_BeginProducerEpoch(FrameIdentityDomain::NvApiFrame);
        }
    }
#endif // _WIN64
}
