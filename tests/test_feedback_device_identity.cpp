// Production-linked Windows/MSVC C++17 tests, not a copied publication model.
// Includes nvapi_hooks.cpp and presentation_gate.cpp to exercise the actual
// private detour and the CPU output executors (H0/H1) with the
// production frame plans, cadence grid, output tracker and pipeline publication.
// The unrelated hook/scheduler services below are test
// doubles: these tests do NOT validate driver forwarding, pacing or live hooks.
#include "nvapi_hooks.h"
#ifdef _WIN64
void FgInput_RecordMarker(uint64_t, uint32_t, uint32_t, int64_t) noexcept {}
void StreamlineHandoff_RecordInbandFrame(uint32_t, uint64_t) noexcept {}
void Streamline_RecordReflexSyncStatement(uint32_t, bool) noexcept {}
#endif
#include "feedback_device_identity.h"
#include "config.h"
#include "feedback.h"
#include "frame_timeline.h"
#include "hooks.h"
#include "health.h"
#include "logger.h"
#include "startup_breadcrumb.h"
#include "marker_log.h"
#include "pcl_hooks.h"
#include "cadence_grid.h"
#include "frame_plan.h"
#include "pacing_resolver.h"
#include "presentation_gate.h"
#include "render_handoff.h"
#include "runtime_lifecycle.h"
#include "scheduler.h"
#include "streamline_hooks.h"
#include "swapchain_manager.h"
#include "wake_guard.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>

#include <d3d12.h>
#include "../src/nvapi_hooks.cpp"

// Gate code uses a controlled QPC/Spin boundary, not real waiting. Keep native
// hook timestamps real so the original-call and queue-order paths are intact.
static thread_local int64_t s_gate_qpc = 0;
static BOOL WINAPI GateQueryQpc(LARGE_INTEGER* value) {
    if (s_gate_qpc == 0) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        s_gate_qpc = now.QuadPart;
    }
    value->QuadPart = s_gate_qpc;
    return TRUE;
}
#define QueryPerformanceCounter GateQueryQpc
#include "../src/presentation_gate.cpp"
#undef QueryPerformanceCounter

std::atomic<int> g_user_target_fps{0};
namespace {
bool s_expect_check_failure = false; // the lock-fixture control: a failed CHECK is the expected outcome
void Check(bool passed, const char* expression, int line) {
    if (passed) return;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    if (s_expect_check_failure) std::exit(0);
    std::abort();
}
#define CHECK(expression) Check(!!(expression), #expression, __LINE__)

// Independently pinned to deps/reshade/source/com_utils.hpp and the AddRef
// return contract in deps/reshade/source/d3d12/d3d12_device.cpp.
constexpr GUID kUnwrappedObject = {
    0x7f2c9a11, 0x3b4e, 0x4d6a,
    { 0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42 }
};

// A sleep status word of the current feedback-device publication (SleepStatusWord's token).
static uint64_t CurrentStatusWord(uint32_t interval_us, bool use_game_sleep) {
    return SleepStatusWord(interval_us, use_game_sleep, g_sleep_status_token.load());
}

// The same consume-once operation the scheduler applies to each source row.
static PacingSourceRecord TakeSleepRow(int64_t release = INT64_MAX) {
    PacingSourceRecord row{};
    row.release_qpc = release;
    g_pacing_pending_sleep.Take(row);
    return row;
}

class FakeDevice final : public IUnknown {
public:
    // Graph edges are borrowed from fixtures whose lifetimes enclose each
    // call. QI output references, unlike these fixture edges, are always owned.
    IUnknown* canonical = this;
    IUnknown* unwrapped = nullptr;
    HRESULT unwrap_status = E_NOINTERFACE;
    HRESULT identity_status = S_OK;
    bool null_identity = false;
    bool fault_unwrap = false;
    void (*on_unwrap)() = nullptr;
    std::atomic<ULONG> references{1};
    std::atomic<unsigned> queries{0};
    std::atomic<unsigned> add_refs{0};
    std::atomic<unsigned> releases{0};

    ~FakeDevice() {
        CHECK(references == 1);
        CHECK(add_refs == releases);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        ++queries;
        if (IsEqualIID(iid, __uuidof(IUnknown))) {
            if (!null_identity) {
                *result = canonical;
                canonical->AddRef();
            }
            return identity_status;
        }
        if (!IsEqualIID(iid, kUnwrappedObject)) return E_NOINTERFACE;
        if (on_unwrap) {
            auto callback = on_unwrap;
            on_unwrap = nullptr;
            callback(); // Reentrant writer: must never be under its lock.
        }
        if (fault_unwrap) {
            RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
            return E_FAIL;
        }
        if (unwrapped) {
            *result = unwrapped;
            unwrapped->AddRef();
        }
        return unwrap_status;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        ++add_refs;
        return ++references;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        CHECK(references > 1); // Stack fixture owns the initial reference.
        ++releases;
        return --references;
    }
    void Wrap(IUnknown* native) {
        unwrapped = native;
        unwrap_status = S_OK;
    }
};

unsigned s_lifecycle_resets = 0;
// SwapMgr link doubles' state: the presenting device (owned by the test), its publication
// sequence and the active API. Default: no presenting device, API None (native bootstrap).
IUnknown* s_test_presenting = nullptr;
uint64_t s_test_presenting_sequence = 0;
ActiveAPI s_test_api = ActiveAPI::None;
std::atomic<unsigned> s_forwarded_markers{0};
unsigned s_tail_markers = 0;
unsigned s_render_submit_starts = 0; // RENDERSUBMIT_START entries of the marker log (immediate or drained)
int64_t s_first_tail_timestamp = 0;
int64_t s_last_tail_timestamp = 0;
uint64_t s_simulation_start_id[8]{};
int64_t s_simulation_start_qpc[8]{};
int64_t s_last_render_submit_end_qpc = 0;
unsigned s_route_capability_publications = 0;
bool s_replay_enforcement = false;
std::vector<uint64_t> s_enforced_ids;
// Marker forward order: 'F' = driver forward, 'O' = OnMarker, with frame IDs.
bool s_record_call_order = false;
std::vector<std::pair<char, uint64_t>> s_call_order;
DWORD s_on_marker_sleep_ms = 0;
unsigned s_residence_publications = 0;
double s_last_residence_us = -1.0;
NvAPIFeedbackDevice s_reentrant_expected;
NvAPIFeedbackDevice s_reentrant_publication;
IUnknown* s_reentrant_device = nullptr;
std::atomic<bool> s_test_stopping{false};
HANDLE s_gate_entered = nullptr;
HANDLE s_gate_continue = nullptr;
std::atomic<bool> s_pause_gate{false};
unsigned s_gate_hold_publications = 0;
uint64_t s_clock_map_frame_id = 0; // last ReflexClockMap_RecordPresentStart
int64_t s_clock_map_qpc = 0;
bool s_native_context_enabled = false;
uint64_t s_native_swapchain_generation = 1;
bool s_smooth_motion = false;
unsigned s_sleep_calls = 0;
bool s_paces_source = true; // Scheduler_PacesSource
// When set, the Scheduler_PacesSource / UnifiedActive stubs answer from the published pipeline exactly as
// scheduler.cpp does, instead of from the flags of the other tests.
bool s_scheduler_from_pipeline = false;
unsigned s_record_sleep_calls = 0;   // Scheduler_RecordReflexSleep calls
int64_t s_recorded_sleep_qpc = 0;    // and its newest argument
unsigned s_driver_sleep_calls = 0;       // Scheduler_EndDriverSleep calls
int64_t s_driver_sleep_start_qpc = 0;    // its newest stamps
int64_t s_driver_sleep_end_qpc = 0;
unsigned s_pre_sleep_waits = 0;           // PreSleepWaitUntil calls (the unified start wait)
int64_t s_pre_sleep_wait_target = 0;      // and its newest arguments
int64_t s_pre_sleep_max_ahead = 0;        // the longest wait PreSleepWaitUntil was asked for (ticks)
// The unified owner: the Scheduler_Unified* stubs below, the Sleep hook's
// PreSleepWaitUntil (spins to its target when s_pre_sleep_wait_blocks) and Dynamic MFG.
bool s_unified_active = false;                 // Scheduler_UnifiedActive
SchedulerUnifiedReserve s_unified_reserve_result = SchedulerUnifiedReserve::Inactive;
SchedulerUnifiedReservation s_unified_reservation{}; // handed out on Reserved
unsigned s_unified_reserve_calls = 0;
// The next reservations find the owner taken, one per entry, each given that start estimate.
std::vector<int64_t> s_unified_busy_estimates;
int64_t s_unified_reserve_arrival = 0;
unsigned s_unified_publishes = 0;              // Scheduler_UnifiedPublishSleep
SchedulerUnifiedReservation s_unified_published{};
UnifiedSleepOutcome s_unified_outcome = UnifiedSleepOutcome::None;
int64_t s_unified_release_qpc = 0;
int64_t s_unified_arrival_qpc = 0;
UnifiedLimitResult s_unified_limit{};
bool s_pre_sleep_wait_blocks = false;
bool s_dmfg_active = false;                    // IsDmfgActive (64-bit)
int64_t RealQpc();
unsigned s_mode_calls = 0;
int s_sleep_action = 0;
NV_SET_SLEEP_MODE_PARAMS* s_expected_mode_params = nullptr;
std::vector<NV_SET_SLEEP_MODE_PARAMS> s_forwarded_modes; // recorded while s_record_modes
bool s_record_modes = false;
NvAPI_Status s_mode_result = -9;
void (*s_on_mode_forward)() = nullptr; // an interleaving point inside the driver's SetSleepMode
// DisplayState_OnSleepStatus (display_state.cpp is not linked): the driver's answer about the game's frames as the
// hooks hand it over.
int s_game_vrr_calls = 0;
bool s_game_vrr_fullscreen_vrr = false;
bool s_game_vrr_fullscreen_iflip = false;
bool s_game_vrr_own_read = false;

IUnknown* s_last_driver_device = nullptr; // the device the newest fake driver call received
NvAPI_Status __cdecl FakeSleepMode(IUnknown* device, NV_SET_SLEEP_MODE_PARAMS* params) {
    s_last_driver_device = device;
    if (s_expected_mode_params) CHECK(params == s_expected_mode_params); // unchanged original pointer
    ++s_mode_calls;
    if (s_record_modes) s_forwarded_modes.push_back(*params);
    if (s_on_mode_forward) s_on_mode_forward();
    return s_mode_result; // -9 by default: preserve a driver's failure; requested params are not accepted-state proof
}
// Actions: 2 clears the feedback device during Sleep, 4 faults natively, 8 lasts about a
// millisecond (a driver that paces).
NvAPI_Status __cdecl FakeDriverSleep(IUnknown* device) {
    s_last_driver_device = device;
    ++s_sleep_calls;
    if (s_sleep_action == 2) NvAPI_ClearFeedbackDevice(device);
    if (s_sleep_action == 4) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    if (s_sleep_action == 8) Sleep(1);
    return -7; // a driver failure is returned to the game unchanged
}

// The Reflex Sleep bypass is the real PacingPipeline_SleepBypass (the published test mode is Off, the default). The
// hooks' bypass decision then follows the published pipeline: Scheduler_PacesSource / Scheduler_UnifiedActive answer from it
// like scheduler.cpp (s_scheduler_from_pipeline) and g_user_target_fps is positive or not. The pipeline is what
// PacingPipeline_Resolve gives for the inputs the scheduler builds.
PacingCapabilities SleepCapabilities(PacingRoute route) {
    PacingCapabilities capabilities{};
    capabilities.x64 = true;
    capabilities.api = PacingApi::DX12;
    capabilities.route = route;
    return capabilities;
}
PacingRuntimeState SleepRuntime(bool pre_sleep_available, bool uncapped) {
    PacingRuntimeState runtime{};
    runtime.swapchain_valid = true;
    runtime.vrr = true;
    runtime.uncapped = uncapped;
    runtime.pre_sleep_available = pre_sleep_available;
    return runtime;
}
PacingPipelineSnapshot PublishResolvedPipeline(PacingRoute route, bool pre_sleep_available, bool uncapped) {
    const PacingPipelineSnapshot snapshot = PacingPipeline_Resolve(
        PacingPipeline_GetConfig(), SleepCapabilities(route), SleepRuntime(pre_sleep_available, uncapped));
    PacingPipeline_Publish(snapshot);
    return snapshot;
}
// ReLimiter paces the source (the present route's placement, on both architectures) or does not (the resolver's uncapped
// pipeline: mode Off, placement None; the driver limiter and Dynamic MFG pass-through are not paced by ReLimiter either).
void PublishSleepPipeline(bool paced) {
    s_scheduler_from_pipeline = true;
    if (paced) PublishResolvedPipeline(PacingRoute::PresentBased, false, false);
    else PublishResolvedPipeline(PacingRoute::NvApiMarkers, true, true);
    CHECK(Scheduler_PacesSource() == paced && !Scheduler_UnifiedActive());
}
void RestoreSleepPipeline(const PacingPipelineSnapshot& old_pipeline) {
    PacingPipeline_Publish(old_pipeline);
    s_scheduler_from_pipeline = false;
}

// Sleep/SetSleepMode detours forward the game's original pointers and return
// the driver's result unchanged; bypasses succeed without calling the driver.
void TestReflexCallForwarding() {
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    const auto old_sleep = s_orig_sleep;
    const auto old_mode = s_orig_sleep_mode;
    s_orig_sleep = FakeDriverSleep;
    s_orig_sleep_mode = FakeSleepMode;
    CHECK(Hook_Sleep(&device) == -7);
    NV_SET_SLEEP_MODE_PARAMS params{};
    params.version = NV_SLEEP_PARAMS_VER;
    params.minimumIntervalUs = 1234;
    params.bLowLatencyMode = params.bLowLatencyBoost = params.bUseMarkersToOptimize = params.bUseMinQueueTime = 1;
    s_expected_mode_params = &params;
    CHECK(Hook_SetSleepMode(&device, &params) == -9);
    CHECK(s_mode_calls == 1 && params.minimumIntervalUs == 1234);
    CHECK(g_game_requested_interval.load() == 1234);
    CHECK(PacingPipeline_SleepBypass()); // the published test mode is Off
    const PacingPipelineSnapshot old_pipeline = PacingPipeline_Read();
    PublishSleepPipeline(true);
    g_user_target_fps=157;
    g_pacer_latency_us=500.0;
    const auto bypass_calls=s_sleep_calls;
    const auto driver_sleep_calls=s_driver_sleep_calls;
    // Bypass: no driver call and no stale Sleep duration.
    CHECK(Hook_Sleep(&device)==NVAPI_OK && s_sleep_calls==bypass_calls);
    CHECK(g_pacer_latency_us.load()==0.0);
    // The scheduler is told of the driver's Sleep only when it ran.
    CHECK(s_driver_sleep_calls==driver_sleep_calls);
    // Passthrough (Dynamic MFG, uncapped) or the driver limiter: the driver's Sleep runs.
    PublishSleepPipeline(false);
    CHECK(Hook_Sleep(&device)==-7 && s_sleep_calls==bypass_calls+1);
    CHECK(s_driver_sleep_calls==driver_sleep_calls+1 && s_driver_sleep_start_qpc>0 &&
        s_driver_sleep_end_qpc>=s_driver_sleep_start_qpc);
    PublishSleepPipeline(true);
    g_user_target_fps=0;
    CHECK(Hook_Sleep(&device)==-7 && s_sleep_calls==bypass_calls+2);
    RestoreSleepPipeline(old_pipeline);

    // 0 forward, 2 device cleared during Sleep, 4 native fault,
    // 5 Smooth Motion bypass (x64 only), 6 no original trampoline.
    for (int scenario : {0, 2, 4, 5, 6}) {
        if (!NvAPI_HasFeedbackDevice()) CHECK(NvAPI_CaptureFeedbackDevice(&device));
        if (scenario == 6) s_orig_sleep = nullptr;
        s_sleep_action = scenario < 5 ? scenario : 0;
#ifdef _WIN64
        s_smooth_motion = scenario == 5;
#endif
        const unsigned before = s_sleep_calls;
        const unsigned driver_calls_before = s_driver_sleep_calls;
        const double latency_before = g_pacer_latency_us.load();
        const bool bypass = s_smooth_motion || scenario == 6;
        CHECK(Hook_Sleep(&device) == (bypass || scenario == 4 ? NVAPI_OK : -7));
        CHECK(s_sleep_calls == before + (bypass ? 0 : 1));
        // A device replaced during the driver call never updates Sleep latency.
        if (scenario == 2) CHECK(g_pacer_latency_us.load() == latency_before);
        // The driver's Sleep is published once it ran and the presenting
        // device is still current; not after the device was replaced during
        // it, with no trampoline, or under the Smooth Motion bypass.
        if (scenario == 0) CHECK(s_driver_sleep_calls == driver_calls_before + 1);
        if (scenario == 2 || scenario == 6) CHECK(s_driver_sleep_calls == driver_calls_before);
#ifdef _WIN64
        if (scenario == 5) CHECK(s_driver_sleep_calls == driver_calls_before);
#endif
    }
    s_smooth_motion = false;
    s_sleep_action = 0;
    s_orig_sleep = FakeDriverSleep;
    // Sleep must never wait behind settings publication.
    CHECK(!s_game_params_lock.test_and_set());
    CHECK(Hook_Sleep(&device) == -7);
    s_game_params_lock.clear();
    // Unsupported settings versions are still forwarded unchanged.
    params.version = 0;
    CHECK(Hook_SetSleepMode(&device, &params) == -9);
    s_orig_sleep = old_sleep;
    s_orig_sleep_mode = old_mode;
    NvAPI_ClearFeedbackDevice();
}

bool SameModeExceptLowLatency(const NV_SET_SLEEP_MODE_PARAMS& a, const NV_SET_SLEEP_MODE_PARAMS& b) {
    return a.version == b.version && a.minimumIntervalUs == b.minimumIntervalUs &&
        a.bUseMarkersToOptimize == b.bUseMarkersToOptimize && a.bUseMinQueueTime == b.bUseMinQueueTime &&
        std::memcmp(a.rsvd, b.rsvd, sizeof(a.rsvd)) == 0;
}
bool SameMode(const NV_SET_SLEEP_MODE_PARAMS& a, const NV_SET_SLEEP_MODE_PARAMS& b) {
    return SameModeExceptLowLatency(a, b) && a.bLowLatencyMode == b.bLowLatencyMode &&
        a.bLowLatencyBoost == b.bLowLatencyBoost;
}

// The Reflex driver limiter's SetSleepMode (key off, nothing pending): its interval goes out
// on a copy of the game's parameters with the game's own low latency, from the limiter's
// application and from every game call while it is applied; a held forward lock leaves the
// application pending; the Sleep, forwarded or bypassed, sends no SetSleepMode (nothing
// changes the driver's low latency mode with the key off); the shutdown restore sends the
// game's parameters exactly.
void TestDriverLimiterSleepMode() {
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    const auto old_sleep = s_orig_sleep;
    const auto old_mode = s_orig_sleep_mode;
    s_orig_sleep = FakeDriverSleep;
    s_orig_sleep_mode = FakeSleepMode;
    s_expected_mode_params = nullptr;
    s_mode_result = NVAPI_OK;
    s_record_modes = true;
    s_forwarded_modes.clear();
    CHECK(PacingPipeline_SleepBypass()); // the published test mode is Off
    const PacingPipelineSnapshot old_pipeline = PacingPipeline_Read();
    g_user_target_fps = 157;
    PublishSleepPipeline(false);

    NV_SET_SLEEP_MODE_PARAMS game{};
    game.version = NV_SLEEP_PARAMS_VER;
    game.bLowLatencyMode = game.bLowLatencyBoost = 1;
    game.minimumIntervalUs = 4321;
    game.bUseMarkersToOptimize = game.bUseMinQueueTime = 1;
    game.rsvd[3] = 9;
    CHECK(Hook_SetSleepMode(&device, &game) == NVAPI_OK);
    CHECK(s_forwarded_modes.size() == 1 && SameMode(s_forwarded_modes[0], game));
    s_forwarded_modes.clear();

    // The forwarded Sleep makes no SetSleepMode call and leaves the driver's low latency alone.
    const unsigned driver_sleeps = s_sleep_calls;
    CHECK(Hook_Sleep(&device) == -7 && s_sleep_calls == driver_sleeps + 1 && s_forwarded_modes.empty());
    CHECK(!s_low_latency_forced_off.load());
    // So does a bypassed one.
    PublishSleepPipeline(true);
    CHECK(Hook_Sleep(&device) == NVAPI_OK && s_sleep_calls == driver_sleeps + 1 && s_forwarded_modes.empty());
    CHECK(!s_low_latency_forced_off.load());
    PublishSleepPipeline(false);

    // The game's own call, with low latency on, is forwarded as it is.
    NV_SET_SLEEP_MODE_PARAMS again = game;
    again.minimumIntervalUs = 2222;
    CHECK(Hook_SetSleepMode(&device, &again) == NVAPI_OK);
    CHECK(s_forwarded_modes.size() == 1 && SameMode(s_forwarded_modes[0], again));
    CHECK(again.bLowLatencyMode == 1 && s_game_original_params.minimumIntervalUs == 2222);
    s_forwarded_modes.clear();

    // With the Reflex driver limiter applied its interval stays in every copy, low latency is the game's.
    NV_SET_SLEEP_MODE_PARAMS limited = again;
    limited.minimumIntervalUs = 8000;
    g_driver_limiter_interval_us = 8000;
    CHECK(Hook_SetSleepMode(&device, &again) == NVAPI_OK);
    CHECK(s_forwarded_modes.size() == 1 && SameMode(s_forwarded_modes[0], limited));
    s_forwarded_modes.clear();
    // The forward lock held (a SetSleepMode or a transition in flight): nothing is sent, the
    // flag is untouched, the operation stays pending for the next call.
    AcquireSRWLockExclusive(&s_sleep_mode_forward_lock);
    CHECK(NvAPI_ApplyDriverLimiter(7000) == DriverLimiterApply::Busy);
    ReleaseSRWLockExclusive(&s_sleep_mode_forward_lock);
    CHECK(s_forwarded_modes.empty() && !s_low_latency_forced_off.load());
    CHECK(NvAPI_ApplyDriverLimiter(6000) == DriverLimiterApply::Applied);
    limited.minimumIntervalUs = 6000;
    CHECK(s_forwarded_modes.size() == 1 && SameMode(s_forwarded_modes[0], limited));
    CHECK(!s_low_latency_forced_off.load());
    s_forwarded_modes.clear();
    // The Sleep with the limiter applied still sends nothing.
    g_driver_limiter_interval_us = 6000;
    CHECK(Hook_Sleep(&device) == -7 && s_forwarded_modes.empty());
    g_driver_limiter_interval_us = 0;

    // The shutdown restore sends the game's parameters exactly.
    RestoreGameSleepMode();
    CHECK(s_forwarded_modes.size() == 1 && SameMode(s_forwarded_modes[0], again));
    CHECK(!s_low_latency_forced_off.load());
    s_forwarded_modes.clear();

    g_driver_limiter_interval_us = 0;
    g_user_target_fps = 0;
    RestoreSleepPipeline(old_pipeline);
    s_record_modes = false;
    s_mode_result = -9;
    s_forwarded_modes.clear();
    s_orig_sleep = old_sleep;
    s_orig_sleep_mode = old_mode;
    NvAPI_ClearFeedbackDevice();
}

// Hook_Sleep: the newest sleep status is left for the next Source record when the Sleep
// returns, forwarded or bypassed.
void TestSleepStatusAtSleepReturn() {
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    const auto old_sleep = s_orig_sleep;
    s_orig_sleep = FakeDriverSleep;
    CHECK(PacingPipeline_SleepBypass()); // the published test mode is Off
    const PacingPipelineSnapshot old_pipeline = PacingPipeline_Read();
    g_user_target_fps = 157;
    PublishSleepPipeline(false);

    const auto saved_demand = g_pacing_telemetry_demand.load();
    PacingTelemetry_PublishDemand((uint64_t{100} << kPacingWindowShift) | kPacingReadout, true);
    g_sleep_status_newest = CurrentStatusWord(6500, true);
    const auto previous_window = g_pacing_pending_sleep.Observe();
    CHECK(Hook_Sleep(&device) == -7);
    const auto first = TakeSleepRow(101);
    CHECK(first.driver_sleep_status_valid && first.driver_sleep_interval_us == 6500 && first.driver_use_game_sleep);
    CHECK(!TakeSleepRow(102).driver_sleep_status_valid);
    // Rotation before the first source callback: a delayed prior-window writer
    // and an old source release may neither overwrite nor steal the new result.
    PacingTelemetry_PublishDemand((uint64_t{200} << kPacingWindowShift) | kPacingReadout, true);
    g_sleep_status_newest = CurrentStatusWord(7200, false);
    PublishSleepPipeline(true);
    CHECK(Hook_Sleep(&device) == NVAPI_OK);
    g_pacing_pending_sleep.Publish(previous_window, true, 1000, true);
    CHECK(!TakeSleepRow(199).driver_sleep_status_valid);
    const auto rotated = TakeSleepRow(202);
    CHECK(rotated.driver_sleep_status_valid && rotated.driver_sleep_interval_us == 7200 && !rotated.driver_use_game_sleep);
    PacingPendingWarning busy, duplicate;
    busy.Record(199);
    duplicate.Record(201);
    CHECK(!duplicate.Take(200, 199) && !busy.Take(200, 202));
    CHECK(duplicate.Take(200, 202) && !duplicate.Take(200, 203));

    g_sleep_status_newest = 0;
    g_pacing_telemetry_demand.store(saved_demand);
    g_user_target_fps = 0;
    RestoreSleepPipeline(old_pipeline);
    s_orig_sleep = old_sleep;
    NvAPI_ClearFeedbackDevice();
}

#ifdef _WIN64
// The unified owner (the key). The key is published as ResolvePipeline publishes it, WITHOUT
// the epoch (the scheduler bumps that later): the first hook after the publication must
// already see the transition through NvAPI_SleepModeReconcilePending. The tuple, the hard
// limit, the self-check and the Sleep hook are production code, the scheduler's reservation
// and publication are stubs.
// The owner is derived (PacingPipeline_ClampConfig): `unified` is true on 64-bit while the test mode is Off, so
// the key off is published as the test mode ReflexOnly (so the real PacingPipeline_SleepBypass is false while it is
// published).
PacingPipelineConfig WithKey(PacingPipelineConfig config, bool on) {
    config.test_mode = on ? PacingTestMode::Off : PacingTestMode::ReflexOnly;
    return config;
}
// The published configuration with its key carried through the derivation, to restore it afterwards.
PacingPipelineConfig CurrentConfig() { return WithKey(PacingPipeline_GetConfig(), PacingPipeline_Unified()); }
void SetUnifiedKey(bool on) {
    const PacingPipelineConfig config = WithKey(PacingPipeline_GetConfig(), on);
    // The order Config_ApplyPacingPipeline uses: the clamped key is noted FIRST, then published.
    NvAPI_PrepareUnifiedKey(PacingPipeline_ClampConfig(config).unified);
    PacingPipeline_SetConfig(config);
    CHECK(PacingPipeline_Unified() == on);
}
// The forward is the game's parameters with this interval, low latency and boost off when asked.
bool IsTuple(const NV_SET_SLEEP_MODE_PARAMS& forwarded, const NV_SET_SLEEP_MODE_PARAMS& game,
    uint32_t interval_us, bool low_latency_off) {
    NV_SET_SLEEP_MODE_PARAMS expected = game;
    expected.minimumIntervalUs = interval_us;
    if (low_latency_off) expected.bLowLatencyMode = expected.bLowLatencyBoost = 0;
    return SameMode(forwarded, expected);
}

// The Sleep-mode tuple: with the key off nothing is recorded; entering from the driver
// limiter clears the interval; leaving the key restores the game's interval and low latency
// at the FIRST hook after the key publication (no epoch bump); Dynamic MFG gives
// the game's parameters back; a game call while the boolean already matches still forwards
// the full tuple; a busy lock, captured parameters held by another thread and a failed
// call leave the reconcile pending (no backoff, no spin).
void TestUnifiedSleepModeTuple() {
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    const auto old_sleep = s_orig_sleep;
    const auto old_mode = s_orig_sleep_mode;
    const PacingPipelineConfig old_config = CurrentConfig();
    s_orig_sleep = FakeDriverSleep;
    s_orig_sleep_mode = FakeSleepMode;
    s_expected_mode_params = nullptr;
    s_mode_result = NVAPI_OK;
    s_record_modes = true;
    s_forwarded_modes.clear();
    g_user_target_fps = 0;
    s_paces_source = true;
    s_unified_active = false;
    CHECK(!NvAPI_SleepModeReconcilePending());

    // Key off: the game's call is forwarded as it is, and nothing is recorded.
    NV_SET_SLEEP_MODE_PARAMS game{};
    game.version = NV_SLEEP_PARAMS_VER;
    game.bLowLatencyMode = game.bLowLatencyBoost = 1;
    game.minimumIntervalUs = 4321;
    game.bUseMarkersToOptimize = game.bUseMinQueueTime = 1;
    game.rsvd[3] = 9;
    CHECK(Hook_SetSleepMode(&device, &game) == NVAPI_OK);
    CHECK(s_forwarded_modes.size() == 1 && SameMode(s_forwarded_modes[0], game));
    CHECK(!s_applied_sleep_valid && !s_applied_key.load() && !NvAPI_SleepModeReconcilePending());
    s_forwarded_modes.clear();

    // Entering from the driver limiter: the limiter's interval (not recorded with the key
    // off), then key on with the limiter released (its interval is already 0): the first
    // hook sees it pending, the interval is cleared, low latency is off.
    g_driver_limiter_interval_us = 8333;
    CHECK(NvAPI_ApplyDriverLimiter(8333) == DriverLimiterApply::Applied);
    CHECK(s_forwarded_modes.size() == 1 && IsTuple(s_forwarded_modes[0], game, 8333, false));
    CHECK(!s_applied_sleep_valid && !NvAPI_SleepModeReconcilePending());
    s_forwarded_modes.clear();
    SetUnifiedKey(true);
    CHECK(NvAPI_SleepModeReconcilePending());
    g_driver_limiter_interval_us = 0;
    NvAPI_ReleaseDriverLimiter();
    CHECK(s_forwarded_modes.size() == 1 && IsTuple(s_forwarded_modes[0], game, 0, true));
    CHECK(s_applied_sleep_valid && s_applied_key.load() && s_applied_sleep_mode.owner == UnifiedSleepOwner::Unified &&
          s_applied_sleep_mode.interval_us == 0 && s_low_latency_forced_off.load());
    s_forwarded_modes.clear();
    // Applied: the next Sleeps forward nothing, and the driver's Sleep still runs.
    const unsigned driver_sleeps = s_sleep_calls;
    CHECK(Hook_Sleep(&device) == -7 && Hook_Sleep(&device) == -7 && s_sleep_calls == driver_sleeps + 2);
    CHECK(s_forwarded_modes.empty());

    // A game call while the boolean already matches: the full tuple (interval 0 as well),
    // not the game's interval with low latency off.
    NV_SET_SLEEP_MODE_PARAMS again = game;
    again.minimumIntervalUs = 2222;
    CHECK(s_low_latency_forced_off.load());
    CHECK(Hook_SetSleepMode(&device, &again) == NVAPI_OK);
    CHECK(s_forwarded_modes.size() == 1 && IsTuple(s_forwarded_modes[0], again, 0, true));
    CHECK(s_game_original_params.minimumIntervalUs == 2222 && g_game_requested_interval.load() == 2222);
    CHECK(s_applied_sleep_mode.interval_us == 0);
    s_forwarded_modes.clear();

    // Dynamic MFG: the game's parameters exactly (a bump from OnMarker, key on only), then
    // its own calls unmodified (the game's pointer); leaving it: the tuple again.
    s_dmfg_active = true;
    NvAPI_BumpSleepModeEpoch();
    CHECK(Hook_Sleep(&device) == -7);
    CHECK(s_forwarded_modes.size() == 1 && SameMode(s_forwarded_modes[0], again));
    CHECK(s_applied_sleep_mode.owner == UnifiedSleepOwner::Game && !s_low_latency_forced_off.load());
    s_forwarded_modes.clear();
    NV_SET_SLEEP_MODE_PARAMS dmfg_call = again;
    dmfg_call.minimumIntervalUs = 3333;
    s_expected_mode_params = &dmfg_call;
    CHECK(Hook_SetSleepMode(&device, &dmfg_call) == NVAPI_OK);
    s_expected_mode_params = nullptr;
    CHECK(s_forwarded_modes.size() == 1 && SameMode(s_forwarded_modes[0], dmfg_call));
    CHECK(s_applied_sleep_mode.interval_us == 3333);
    s_forwarded_modes.clear();
    s_dmfg_active = false;
    NvAPI_BumpSleepModeEpoch();
    CHECK(Hook_Sleep(&device) == -7);
    CHECK(s_forwarded_modes.size() == 1 && IsTuple(s_forwarded_modes[0], dmfg_call, 0, true));
    CHECK(s_low_latency_forced_off.load());
    s_forwarded_modes.clear();

    // Leaving the key, through the real pending check: the key publication alone makes the
    // next Sleep reconcile (the epoch is bumped much later, or never in this test). The
    // driver still has low latency off, the interval is the game's again and so is low
    // latency; settled afterwards, nothing recorded.
    SetUnifiedKey(false);
    CHECK(NvAPI_SleepModeReconcilePending() && s_low_latency_forced_off.load());
    CHECK(Hook_Sleep(&device) == -7);
    CHECK(s_forwarded_modes.size() == 1 && IsTuple(s_forwarded_modes[0], dmfg_call, 3333, false));
    CHECK(!NvAPI_SleepModeReconcilePending() && !s_applied_sleep_valid && !s_applied_key.load());
    CHECK(!s_low_latency_forced_off.load());
    s_forwarded_modes.clear();
    CHECK(Hook_Sleep(&device) == -7 && s_forwarded_modes.empty()); // the key-off path, nothing to do
    // The scheduler's bump for the same transition arrives later (ResolvePipeline): it is
    // absorbed, the restoration is not forwarded a second time.
    NvAPI_BumpSleepModeEpoch();
    CHECK(!NvAPI_SleepModeReconcilePending());
    CHECK(Hook_Sleep(&device) == -7 && s_forwarded_modes.empty());

    // Leaving with the limiter: key on, then off with the limiter's interval published.
    SetUnifiedKey(true);
    CHECK(Hook_Sleep(&device) == -7);
    CHECK(s_forwarded_modes.size() == 1 && IsTuple(s_forwarded_modes[0], dmfg_call, 0, true));
    s_forwarded_modes.clear();
    g_driver_limiter_interval_us = 6000;
    SetUnifiedKey(false);
    CHECK(Hook_Sleep(&device) == -7);
    CHECK(s_forwarded_modes.size() == 1 && IsTuple(s_forwarded_modes[0], dmfg_call, 6000, false));
    CHECK(!NvAPI_SleepModeReconcilePending());
    s_forwarded_modes.clear();
    // Limiter released: the key-off path restores the game's parameters.
    g_driver_limiter_interval_us = 0;
    NvAPI_ReleaseDriverLimiter();
    CHECK(s_forwarded_modes.size() == 1 && SameMode(s_forwarded_modes[0], dmfg_call));
    CHECK(!s_low_latency_forced_off.load() && !NvAPI_SleepModeReconcilePending());
    s_forwarded_modes.clear();

    // A held forward lock, captured parameters held by another thread and a failed call leave
    // it pending, never blocking or spinning; the next call retries at once (no backoff).
    SetUnifiedKey(true);
    AcquireSRWLockExclusive(&s_sleep_mode_forward_lock);
    NvAPI_ReconcileSleepMode();
    ReleaseSRWLockExclusive(&s_sleep_mode_forward_lock);
    CHECK(s_forwarded_modes.empty() && NvAPI_SleepModeReconcilePending());
    CHECK(!s_game_params_lock.test_and_set()); // the captured parameters taken by "another thread"
    NvAPI_ReconcileSleepMode();
    CHECK(Hook_Sleep(&device) == -7);
    s_game_params_lock.clear();
    CHECK(s_forwarded_modes.empty() && NvAPI_SleepModeReconcilePending());
    s_mode_result = -9;
    NvAPI_ReconcileSleepMode();
    NvAPI_ReconcileSleepMode();
    CHECK(s_forwarded_modes.size() == 2 && !s_applied_sleep_valid);
    s_mode_result = NVAPI_OK;
    s_forwarded_modes.clear();
    NvAPI_ReconcileSleepMode();
    CHECK(s_forwarded_modes.size() == 1 && IsTuple(s_forwarded_modes[0], dmfg_call, 0, true));
    NvAPI_ReconcileSleepMode();
    CHECK(s_forwarded_modes.size() == 1);
    s_forwarded_modes.clear();

    // Key off again: the first reconcile restores the key-off row, then the key-off paths.
    SetUnifiedKey(false);
    CHECK(NvAPI_SleepModeReconcilePending());
    NvAPI_ReconcileSleepMode();
    CHECK(s_forwarded_modes.size() == 1 && SameMode(s_forwarded_modes[0], dmfg_call));
    CHECK(!NvAPI_SleepModeReconcilePending() && !s_applied_sleep_valid);

    PacingPipeline_SetConfig(old_config);
    g_driver_limiter_interval_us = 0;
    s_record_modes = false;
    s_mode_result = -9;
    s_forwarded_modes.clear();
    s_orig_sleep = old_sleep;
    s_orig_sleep_mode = old_mode;
    NvAPI_ClearFeedbackDevice();
}

// The key transition. Pending is published BEFORE the key, in both directions (Prepare first,
// as Config_ApplyPacingPipeline orders it): while the transition is in flight nothing is
// forwarded for either side. An older writer's clear (the restoration settling) never erases a
// newer pending (a key-on noted meanwhile). A key on and off in a game without captured
// parameters settles with no forward. A bump while a key-off restoration is forwarding
// satisfies the transition exactly once; a real bump with the key on is never swallowed. An
// unchanged-off call stores nothing.
void BumpDuringForward() {
    s_on_mode_forward = nullptr;
    NvAPI_BumpSleepModeEpoch();
}
void PrepareOnDuringForward() {
    s_on_mode_forward = nullptr;
    NvAPI_PrepareUnifiedKey(true);
}
void TestUnifiedKeyTransitions() {
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    const auto old_sleep = s_orig_sleep;
    const auto old_mode = s_orig_sleep_mode;
    const PacingPipelineConfig old_config = CurrentConfig();
    s_orig_sleep = FakeDriverSleep;
    s_orig_sleep_mode = FakeSleepMode;
    s_expected_mode_params = nullptr;
    s_mode_result = NVAPI_OK;
    s_record_modes = true;
    s_forwarded_modes.clear();
    g_user_target_fps = 0;
    g_driver_limiter_interval_us = 0;
    s_unified_active = false;
    CHECK(!NvAPI_SleepModeReconcilePending() && !s_game_params_captured);

    // An unchanged-off application stores nothing: the word and the epoch stay as they were.
    const uint32_t epoch_before = s_sleep_mode_epoch.load();
    NvAPI_PrepareUnifiedKey(false);
    NvAPI_PrepareUnifiedKey(false);
    CHECK(s_transition_state.load() == 0 && !NvAPI_SleepModeReconcilePending() &&
          s_sleep_mode_epoch.load() == epoch_before);

    // Key on, then off, in a game whose SetSleepMode parameters were never captured. Pending
    // is set before the key is visible, nothing is forwarded while the transition is in
    // flight, the reconcile has nothing to apply, and the key going off settles at once with
    // no forward (nothing was applied, nothing is owed).
    NvAPI_PrepareUnifiedKey(true);
    CHECK(NvAPI_SleepModeReconcilePending() && !PacingPipeline_Unified());
    NvAPI_ReconcileSleepMode();
    CHECK(Hook_Sleep(&device) == -7 && s_forwarded_modes.empty());
    SetUnifiedKey(true);
    CHECK(Hook_Sleep(&device) == -7 && s_forwarded_modes.empty() && NvAPI_SleepModeReconcilePending());
    CHECK((s_transition_state.load() & kTransitionOwed) == 0);
    SetUnifiedKey(false);
    CHECK(!NvAPI_SleepModeReconcilePending());
    CHECK(Hook_Sleep(&device) == -7 && s_forwarded_modes.empty());

    // With captured parameters a Unified tuple is applied and a restoration is owed.
    NV_SET_SLEEP_MODE_PARAMS game{};
    game.version = NV_SLEEP_PARAMS_VER;
    game.bLowLatencyMode = game.bLowLatencyBoost = 1;
    game.minimumIntervalUs = 4321;
    CHECK(Hook_SetSleepMode(&device, &game) == NVAPI_OK);
    CHECK(s_forwarded_modes.size() == 1 && SameMode(s_forwarded_modes[0], game) && !NvAPI_SleepModeReconcilePending());
    s_forwarded_modes.clear();
    SetUnifiedKey(true);
    CHECK(Hook_Sleep(&device) == -7);
    CHECK(s_forwarded_modes.size() == 1 && IsTuple(s_forwarded_modes[0], game, 0, true));
    CHECK((s_transition_state.load() & kTransitionOwed) != 0);
    s_forwarded_modes.clear();

    // Off direction: Prepare first keeps it pending (the restoration is owed) while the key is
    // still published on; in that window nothing is forwarded. Then the key is published and
    // the first hook restores the game's parameters, once.
    NvAPI_PrepareUnifiedKey(false);
    CHECK(NvAPI_SleepModeReconcilePending() && PacingPipeline_Unified());
    NvAPI_ReconcileSleepMode();
    CHECK(Hook_Sleep(&device) == -7 && s_forwarded_modes.empty());
    SetUnifiedKey(false);
    CHECK(Hook_Sleep(&device) == -7);
    CHECK(s_forwarded_modes.size() == 1 && SameMode(s_forwarded_modes[0], game));
    CHECK(!NvAPI_SleepModeReconcilePending());
    s_forwarded_modes.clear();

    // An older writer's clear must not erase a newer pending: a key-on is noted while the
    // key-off restoration is forwarding; when the restoration settles (clearing what it owed)
    // the pending the new transition set is still there, and the key-on reconcile follows.
    SetUnifiedKey(true);
    CHECK(Hook_Sleep(&device) == -7);
    s_forwarded_modes.clear();
    SetUnifiedKey(false);
    s_on_mode_forward = PrepareOnDuringForward;
    CHECK(Hook_Sleep(&device) == -7);
    CHECK(s_forwarded_modes.size() == 1 && SameMode(s_forwarded_modes[0], game));
    CHECK((s_transition_state.load() & kTransitionOwed) == 0 && (s_transition_state.load() & kTransitionKey) != 0);
    CHECK(NvAPI_SleepModeReconcilePending());
    s_forwarded_modes.clear();
    SetUnifiedKey(true); // the pipeline publishes what was noted
    CHECK(Hook_Sleep(&device) == -7);
    CHECK(s_forwarded_modes.size() == 1 && IsTuple(s_forwarded_modes[0], game, 0, true));
    s_forwarded_modes.clear();

    // A bump while the key-off restoration is forwarding satisfies the transition exactly once.
    SetUnifiedKey(false);
    s_on_mode_forward = BumpDuringForward;
    CHECK(Hook_Sleep(&device) == -7);
    CHECK(s_forwarded_modes.size() == 1 && SameMode(s_forwarded_modes[0], game));
    CHECK(!NvAPI_SleepModeReconcilePending());
    s_forwarded_modes.clear();
    CHECK(Hook_Sleep(&device) == -7 && s_forwarded_modes.empty());
    NvAPI_BumpSleepModeEpoch(); // and a later bump with the key off changes nothing
    CHECK(!NvAPI_SleepModeReconcilePending() && Hook_Sleep(&device) == -7 && s_forwarded_modes.empty());

    // A real bump with the key on is never swallowed: the tuple is forwarded again.
    SetUnifiedKey(true);
    CHECK(Hook_Sleep(&device) == -7);
    s_forwarded_modes.clear();
    NvAPI_BumpSleepModeEpoch();
    CHECK(Hook_Sleep(&device) == -7);
    CHECK(s_forwarded_modes.size() == 1 && IsTuple(s_forwarded_modes[0], game, 0, true));
    s_forwarded_modes.clear();
    SetUnifiedKey(false);
    CHECK(Hook_Sleep(&device) == -7 && s_forwarded_modes.size() == 1 && !NvAPI_SleepModeReconcilePending());

    // An off/on key pair the scheduler never observed (no bump), Dynamic MFG active: with the key
    // on the game's own tuple is recorded and nothing is owed; with the key off the driver limiter
    // (a key-off path) changes the driver's interval and records nothing; the key on again must
    // NOT accept the old record: the key generation moved, so the game's parameters are forwarded
    // again and the interval is restored. (Without the generation the epoch and the tuple are
    // unchanged and the old record matches: no forward.)
    s_forwarded_modes.clear();
    s_dmfg_active = true;
    SetUnifiedKey(true);
    CHECK(Hook_Sleep(&device) == -7);
    CHECK(s_forwarded_modes.size() == 1 && SameMode(s_forwarded_modes[0], game));
    CHECK(s_applied_sleep_valid && s_applied_sleep_mode.owner == UnifiedSleepOwner::Game &&
          (s_transition_state.load() & kTransitionOwed) == 0);
    s_forwarded_modes.clear();
    const uint32_t epoch_at_pair = s_sleep_mode_epoch.load();
    SetUnifiedKey(false);
    CHECK(!NvAPI_SleepModeReconcilePending()); // nothing owed: settled without a forward
    CHECK(NvAPI_ApplyDriverLimiter(8000) == DriverLimiterApply::Applied);
    CHECK(s_forwarded_modes.size() == 1 && IsTuple(s_forwarded_modes[0], game, 8000, false));
    CHECK(!NvAPI_SleepModeReconcilePending());
    s_forwarded_modes.clear();
    SetUnifiedKey(true);
    CHECK(s_sleep_mode_epoch.load() == epoch_at_pair); // the scheduler saw neither toggle
    CHECK(Hook_Sleep(&device) == -7);
    CHECK(s_forwarded_modes.size() == 1 && SameMode(s_forwarded_modes[0], game));
    CHECK(!s_low_latency_forced_off.load());
    s_forwarded_modes.clear();
    s_dmfg_active = false;
    SetUnifiedKey(false);
    CHECK(!NvAPI_SleepModeReconcilePending() && Hook_Sleep(&device) == -7 && s_forwarded_modes.empty());

    s_on_mode_forward = nullptr;
    PacingPipeline_SetConfig(old_config);
    NvAPI_PrepareUnifiedKey(PacingPipeline_Unified());
    CHECK(!NvAPI_SleepModeReconcilePending());
    s_record_modes = false;
    s_mode_result = -9;
    s_forwarded_modes.clear();
    s_orig_sleep = old_sleep;
    s_orig_sleep_mode = old_mode;
    NvAPI_ClearFeedbackDevice();
}

// A scripted environment for the Sleep hook's wait: a clock that only moves when the wait
// advances it, the ready index and its QPC, completions delivered at defined times (inside
// a start wait or a limit wait), the start-wait chunks and limit waits counted. No real time.
class ScriptedSleepEnv final : public UnifiedSleepEnv {
public:
    struct Completion {
        int64_t at;
        uint64_t index;
        int64_t qpc;
    };
    static constexpr int64_t kFrequency = 10'000'000;
    int64_t now = 1'000'000;
    uint64_t index = 0;
    int64_t ready_qpc = 0;
    std::vector<Completion> completions; // sorted by time
    size_t next_completion = 0;
    unsigned start_waits = 0;
    unsigned limit_waits = 0;
    int64_t longest_start_wait = 0;
    bool start_wait_fails = false;
    std::vector<int64_t> owner_releases; // the scheduler owner's releases, in order of time
    size_t next_owner_release = 0;
    unsigned owner_waits = 0;
    bool owner_wait_fails = false;

    int64_t Now() noexcept override { return now; }
    uint64_t ReadyIndex() noexcept override { return index; }
    int64_t ReadyQpc() noexcept override { return ready_qpc; }
    int64_t Frequency() noexcept override { return kFrequency; }
    void Advance(int64_t to) {
        while (next_completion < completions.size() && completions[next_completion].at <= to) {
            index = completions[next_completion].index;
            ready_qpc = completions[next_completion].qpc;
            ++next_completion;
        }
        if (to > now) now = to;
    }
    // Returns at the next completion before the deadline, else at the deadline.
    void WaitForChange(uint64_t, int64_t deadline_qpc) noexcept override {
        ++limit_waits;
        int64_t until = deadline_qpc;
        if (next_completion < completions.size() && completions[next_completion].at < until)
            until = completions[next_completion].at;
        Advance(until);
    }
    void StartWait(int64_t target_qpc) noexcept override {
        ++start_waits;
        if (target_qpc - now > longest_start_wait) longest_start_wait = target_qpc - now;
        Advance(start_wait_fails ? now + 100 : target_qpc); // a failure returns early
    }
    // True at the owner's next release before the target, else at the target as StartWait.
    bool OwnerWait(int64_t target_qpc) noexcept override {
        ++owner_waits;
        if (owner_wait_fails) {
            Advance(now + 100); // a failure returns early
            return false;
        }
        if (next_owner_release < owner_releases.size() && owner_releases[next_owner_release] < target_qpc) {
            Advance(owner_releases[next_owner_release++]);
            return true;
        }
        Advance(target_qpc);
        return false;
    }
};

// The coarse-plus-tail wait of the limit on a scripted clock.
class ScriptedWaitPrimitives final : public UnifiedWaitPrimitives {
public:
    int64_t now = 0;
    int64_t frequency = 10'000'000;
    uint64_t index = 5;
    int64_t coarse_overshoot = 0;    // ticks the OS timer returns late
    int64_t index_change_at = -1;    // the index changes at this time
    std::vector<uint32_t> coarse_requests;
    unsigned relaxes = 0;
    int64_t early_ticks = -1;        // the next early_waits coarse waits return after this many ticks
    int early_waits = 0;
    void Tick(int64_t to) {
        now = to;
        if (index_change_at >= 0 && now >= index_change_at) index = 6;
    }
    int64_t Now() noexcept override { return now; }
    int64_t Frequency() noexcept override { return frequency; }
    uint64_t Index() noexcept override { return index; }
    void CoarseWait(uint64_t, uint32_t milliseconds) noexcept override {
        coarse_requests.push_back(milliseconds);
        int64_t end = now + static_cast<int64_t>(milliseconds) * frequency / 1000 + coarse_overshoot;
        if (early_waits > 0) { // spurious early return: no change, less time than asked
            --early_waits;
            if (now + early_ticks < end) end = now + early_ticks;
        }
        if (index_change_at >= 0 && index_change_at < end) end = index_change_at; // woken by the change
        Tick(end);
    }
    void Relax() noexcept override {
        ++relaxes;
        Tick(now + 1);
    }
};

// The Sleep hook under the key. The hook-level cases assert only what a real clock cannot
// break (R is never before T for a consumed wait; a failed wait ends before T); every timing
// case runs on the scripted clock: the start wait to T FIRST (in chunks of at most 250 ms), the
// hard limit at E (never before T), R = the clock after it, the unfinished frames re-read at R.
void TestUnifiedSleepHook() {
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    const auto old_sleep = s_orig_sleep;
    const auto old_mode = s_orig_sleep_mode;
    const PacingPipelineConfig old_config = CurrentConfig();
    LARGE_INTEGER frequency_value{};
    QueryPerformanceFrequency(&frequency_value);
    const int64_t frequency = frequency_value.QuadPart;
    s_orig_sleep = FakeDriverSleep;
    s_orig_sleep_mode = FakeSleepMode;
    s_expected_mode_params = nullptr;
    s_mode_result = NVAPI_OK;
    s_record_modes = true;
    s_forwarded_modes.clear();
    SetUnifiedKey(true);
    NV_SET_SLEEP_MODE_PARAMS game{};
    game.version = NV_SLEEP_PARAMS_VER;
    game.bLowLatencyMode = game.bLowLatencyBoost = 1;
    game.minimumIntervalUs = 4321;
    CHECK(Hook_SetSleepMode(&device, &game) == NVAPI_OK);
    s_forwarded_modes.clear();
    const PacingPipelineSnapshot old_pipeline = PacingPipeline_Read();
    g_user_target_fps = 157;
    s_scheduler_from_pipeline = true;
    const PacingPipelineSnapshot unified = PublishResolvedPipeline(PacingRoute::NvApiMarkers, true, false);
    CHECK(PacingPipeline_SleepBypass() && unified.placement == PacingSourcePlacement::Unified);
    CHECK(Scheduler_PacesSource() && Scheduler_UnifiedActive());
    g_up_ready_index.store(6);
    g_up_ready_qpc.store(RealQpc());
    SchedulerUnifiedReservation reservation;
    reservation.token = 5;
    reservation.grid_index = 7;
    reservation.lifecycle_base = 1;
    reservation.interval_ticks = frequency / 100; // I = 10 ms
    s_unified_reserve_result = SchedulerUnifiedReserve::Reserved;

    // Completed: the start wait ran to T; the genuine Sleep ran (the bypass is not consulted,
    // so its NVAPI_OK would be the bypass's), the Sleep-mode forwarded nothing.
    s_pre_sleep_wait_blocks = true;
    reservation.target_qpc = RealQpc() + 3 * frequency / 1000;
    s_unified_reservation = reservation;
    unsigned waits = s_pre_sleep_waits;
    unsigned driver_sleeps = s_sleep_calls;
    unsigned reserves = s_unified_reserve_calls;
    unsigned publishes = s_unified_publishes;
    unsigned end_reports = s_driver_sleep_calls;
    CHECK(Hook_Sleep(&device) == -7);
    CHECK(s_unified_reserve_calls == reserves + 1 && s_unified_reserve_arrival > 0);
    // Real clock: the target is a few ms ahead, and the thread may be descheduled before the
    // hook reads the clock, so either valid outcome is accepted: the wait ran and Completed, or
    // T had passed at entry (no wait) and it is Late. Exact timing is asserted only on the
    // scripted clock below.
    CHECK(s_unified_publishes == publishes + 1 && s_unified_published.token == 5 &&
          s_unified_published.grid_index == 7);
    CHECK((s_pre_sleep_waits == waits + 1 && s_pre_sleep_wait_target == reservation.target_qpc &&
           s_unified_outcome == UnifiedSleepOutcome::Completed) ||
          (s_pre_sleep_waits == waits && s_unified_outcome == UnifiedSleepOutcome::Late));
    CHECK(s_sleep_calls == driver_sleeps + 1 && s_driver_sleep_calls == end_reports + 1);
    CHECK(s_unified_release_qpc >= reservation.target_qpc && s_unified_arrival_qpc == s_unified_reserve_arrival &&
          s_unified_arrival_qpc <= s_unified_release_qpc);
    CHECK(s_unified_limit.outstanding == 0 && s_unified_limit.flags == 0);
    CHECK(s_forwarded_modes.empty());

    // Late: no start wait was needed (T passed), R is the clock after the limit.
    reservation.target_qpc = RealQpc() - frequency / 1000;
    s_unified_reservation = reservation;
    waits = s_pre_sleep_waits;
    publishes = s_unified_publishes;
    CHECK(Hook_Sleep(&device) == -7);
    CHECK(s_pre_sleep_waits == waits && s_unified_publishes == publishes + 1);
    CHECK(s_unified_outcome == UnifiedSleepOutcome::Late && s_unified_release_qpc >= reservation.target_qpc);

    // Failed: the wait could not run to T (stopping, busy flag or timer failure): R < T, never
    // Completed or Late, and the limit did not run (inactive, nothing re-read). One call
    // only: the first chunk ended early.
    s_pre_sleep_wait_blocks = false;
    reservation.target_qpc = RealQpc() + 60 * frequency;
    s_unified_reservation = reservation;
    waits = s_pre_sleep_waits;
    publishes = s_unified_publishes;
    CHECK(Hook_Sleep(&device) == -7);
    CHECK(s_pre_sleep_waits == waits + 1 && s_unified_publishes == publishes + 1);
    CHECK(s_unified_outcome == UnifiedSleepOutcome::Failed && s_unified_release_qpc < reservation.target_qpc);
    CHECK(s_unified_limit.flags == 0 && s_unified_limit.outstanding == -1);

    // Duplicate, busy owner, inactive pipeline: no wait, nothing published, the Sleep forwarded.
    for (const auto status : {SchedulerUnifiedReserve::Duplicate, SchedulerUnifiedReserve::Busy,
             SchedulerUnifiedReserve::Inactive}) {
        s_unified_reserve_result = status;
        waits = s_pre_sleep_waits;
        publishes = s_unified_publishes;
        driver_sleeps = s_sleep_calls;
        reserves = s_unified_reserve_calls;
        CHECK(Hook_Sleep(&device) == -7);
        CHECK(s_unified_reserve_calls == reserves + 1 && s_pre_sleep_waits == waits &&
              s_unified_publishes == publishes && s_sleep_calls == driver_sleeps + 1);
    }
    s_unified_reserve_result = SchedulerUnifiedReserve::Reserved;
    g_up_ready_index.store(0);
    g_up_ready_qpc.store(0);

    // ── The wait on the scripted clock ──
    auto scripted_reservation = [&](const ScriptedSleepEnv& env, uint64_t grid, int64_t target, int64_t interval) {
        SchedulerUnifiedReservation scripted;
        scripted.token = 9;
        scripted.grid_index = grid;
        scripted.lifecycle_base = 1;
        scripted.target_qpc = target;
        scripted.interval_ticks = interval;
        (void)env;
        return scripted;
    };
    // An early arrival whose completions land BEFORE T: 4 frames outstanding at the arrival
    // (grid 10, ready 5), frames up to 8 complete 2 ms in, T is 8 ms in. The start wait runs
    // first and ends at T; at E one frame is outstanding: no wait, no flag, R is T exactly, the
    // row reports the count at R.
    {
        ScriptedSleepEnv env;
        const int64_t arrival = env.now;
        env.index = 5;
        env.ready_qpc = arrival;
        env.completions = {{arrival + 20'000, 8, arrival + 20'000}};
        const auto result = UnifiedSleepWait(env, scripted_reservation(env, 10, arrival + 80'000, 200'000), false);
        CHECK(result.outcome == UnifiedSleepOutcome::Completed && result.release_qpc == arrival + 80'000);
        CHECK(result.limit.flags == 0 && result.limit.outstanding == 1);
        CHECK(env.start_waits == 1 && env.limit_waits == 0 && result.limit.wait_ticks == 0);
    }
    // A late arrival, completions recent, none arriving: the limit waits to its one deadline
    // max(T, E) + I (flags waited and timeout), R is that deadline (never past it), and the
    // row reports 4 at R. No start wait.
    {
        ScriptedSleepEnv env;
        const int64_t arrival = env.now;
        env.index = 5;
        env.ready_qpc = arrival;
        const auto result = UnifiedSleepWait(env, scripted_reservation(env, 10, arrival - 10'000, 100'000), false);
        CHECK(result.outcome == UnifiedSleepOutcome::Late && env.start_waits == 0);
        CHECK((result.limit.flags & (kUpLimitWaited | kUpLimitTimeout)) == (kUpLimitWaited | kUpLimitTimeout));
        CHECK(result.release_qpc == arrival + 100'000 && result.limit.wait_ticks == 100'000);
        CHECK(result.limit.outstanding == 4);
    }
    // Three unfinished frames (grid 9, ready 5) are below the hard limit of four: with fresh
    // completions and a late arrival the limit does not wait, R is the arrival, the row reports 3.
    {
        ScriptedSleepEnv env;
        const int64_t arrival = env.now;
        env.index = 5;
        env.ready_qpc = arrival;
        const auto result = UnifiedSleepWait(env, scripted_reservation(env, 9, arrival - 10'000, 100'000), false);
        CHECK(result.outcome == UnifiedSleepOutcome::Late && env.start_waits == 0);
        CHECK(result.limit.flags == 0 && env.limit_waits == 0 && result.limit.wait_ticks == 0);
        CHECK(result.release_qpc == arrival && result.limit.outstanding == 3);
    }
    // A completion 2 ms in wakes the wait (flag waited, no time-out), and R is that moment.
    {
        ScriptedSleepEnv env;
        const int64_t arrival = env.now;
        env.index = 5;
        env.ready_qpc = arrival;
        env.completions = {{arrival + 20'000, 8, arrival + 20'000}};
        const auto result = UnifiedSleepWait(env, scripted_reservation(env, 10, arrival - 10'000, 5'000'000), false);
        CHECK((result.limit.flags & kUpLimitTimeout) == 0 && (result.limit.flags & kUpLimitWaited) != 0);
        CHECK(result.release_qpc == arrival + 20'000 && result.limit.wait_ticks == 20'000);
        CHECK(result.limit.outstanding == 1 && env.limit_waits == 1);
    }
    // No completion yet, or stale evidence (older than 2 I): inactive, no wait, -1 stays -1.
    for (const bool stale : {false, true}) {
        ScriptedSleepEnv env;
        const int64_t arrival = env.now;
        env.index = 5;
        env.ready_qpc = stale ? arrival - 3 * 100'000 : 0;
        const auto result = UnifiedSleepWait(env, scripted_reservation(env, 10, arrival - 10'000, 100'000), false);
        CHECK((result.limit.flags & kUpLimitInactive) != 0 && result.limit.outstanding == -1);
        CHECK(env.limit_waits == 0 && result.limit.wait_ticks == 0 && result.release_qpc == arrival);
    }
    // Chunked start wait for a long interval (a game limit of one second): 600 ms to T runs as
    // 250 + 250 + 100 ms, none longer than 250 ms, R is T, and the outcome is Completed.
    {
        ScriptedSleepEnv env;
        const int64_t arrival = env.now;
        env.index = 6;
        env.ready_qpc = arrival;
        const auto result = UnifiedSleepWait(env, scripted_reservation(env, 7, arrival + 6'000'000, 10'000'000), false);
        CHECK(env.start_waits == 3 && env.longest_start_wait == 2'500'000);
        CHECK(result.outcome == UnifiedSleepOutcome::Completed && result.release_qpc == arrival + 6'000'000);
    }
    // A start wait that fails (cancelled, stopping, timer failure): Failed with R < T after one
    // call, and the limit does not run.
    {
        ScriptedSleepEnv env;
        const int64_t arrival = env.now;
        env.index = 5;
        env.ready_qpc = arrival;
        env.start_wait_fails = true;
        const auto result = UnifiedSleepWait(env, scripted_reservation(env, 10, arrival + 6'000'000, 100'000), false);
        CHECK(env.start_waits == 1 && result.outcome == UnifiedSleepOutcome::Failed);
        CHECK(result.release_qpc < arrival + 6'000'000 && result.limit.flags == 0 &&
              result.limit.outstanding == -1 && env.limit_waits == 0);
    }
    // A busy owner at the reservation (another thread's marker holds it for a moment). The Sleep
    // waits for the owner's release, bounded by the law's start estimate, and reserves then with
    // its own arrival: the start wait stays in front of the game's marker and runs to the exact T.
    const auto busy_start = [&](ScriptedSleepEnv* env, std::vector<int64_t> estimates, int64_t target) {
        env->index = 9; // nothing unfinished in front of grid 10
        env->ready_qpc = env->now;
        s_unified_reservation = scripted_reservation(*env, 10, target, 200'000);
        s_unified_busy_estimates = std::move(estimates);
        reserves = s_unified_reserve_calls;
        publishes = s_unified_publishes;
        UnifiedSleepStart(*env, env->now);
        s_unified_busy_estimates.clear();
    };
    // Released at once: R is T whether the interval stayed, fell or rose since the estimate.
    for (const int64_t target : {int64_t{80'000}, int64_t{70'000}, int64_t{90'000}}) {
        ScriptedSleepEnv env;
        const int64_t arrival = env.now;
        env.owner_releases = {arrival + 50};
        busy_start(&env, {arrival + 80'000}, arrival + target);
        CHECK(s_unified_reserve_calls == reserves + 2 && s_unified_reserve_arrival == arrival);
        CHECK(env.owner_waits == 1 && env.start_waits == 1 && s_unified_publishes == publishes + 1);
        CHECK(s_unified_arrival_qpc == arrival && s_unified_outcome == UnifiedSleepOutcome::Completed &&
              s_unified_release_qpc == arrival + target);
    }
    // Released after T but before the estimate: released there, and the Sleep held the frame to it,
    // a completed start wait.
    {
        ScriptedSleepEnv env;
        const int64_t arrival = env.now;
        env.owner_releases = {arrival + 75'000};
        busy_start(&env, {arrival + 80'000}, arrival + 70'000);
        CHECK(env.owner_waits == 1 && env.start_waits == 0 && s_unified_publishes == publishes + 1);
        CHECK(s_unified_outcome == UnifiedSleepOutcome::Completed && s_unified_release_qpc == arrival + 75'000);
    }
    // The frame had arrived after its T (the interval fell that far): a late arrival, not a held start.
    {
        ScriptedSleepEnv env;
        const int64_t arrival = env.now;
        env.owner_releases = {arrival + 50};
        busy_start(&env, {arrival + 80'000}, arrival - 10);
        CHECK(env.owner_waits == 1 && env.start_waits == 0 && s_unified_publishes == publishes + 1);
        CHECK(s_unified_outcome == UnifiedSleepOutcome::Late && s_unified_release_qpc == arrival + 50);
    }
    // Taken again by the time the Sleep reserves: it waits for the next release.
    {
        ScriptedSleepEnv env;
        const int64_t arrival = env.now;
        env.owner_releases = {arrival + 50, arrival + 400};
        busy_start(&env, {arrival + 80'000, arrival + 80'000}, arrival + 80'000);
        CHECK(s_unified_reserve_calls == reserves + 3 && env.owner_waits == 2 && env.start_waits == 1);
        CHECK(s_unified_outcome == UnifiedSleepOutcome::Completed && s_unified_release_qpc == arrival + 80'000);
    }
    // A release more than one wait chunk (250 ms) away: the wait for it continues across chunks.
    {
        ScriptedSleepEnv env;
        const int64_t arrival = env.now;
        env.owner_releases = {arrival + 3'000'000};
        busy_start(&env, {arrival + 6'000'000}, arrival + 6'000'000);
        CHECK(s_unified_reserve_calls == reserves + 2 && env.owner_waits == 2 && env.start_waits == 2);
        CHECK(s_unified_outcome == UnifiedSleepOutcome::Completed && s_unified_release_qpc == arrival + 6'000'000);
    }
    // The owner kept through the estimate: the Sleep reserves there once more. T at or before the
    // estimate is released at the estimate, a completed start wait; a later T is waited for.
    for (const int64_t target : {int64_t{80'000}, int64_t{70'000}, int64_t{90'000}}) {
        ScriptedSleepEnv env;
        const int64_t arrival = env.now;
        busy_start(&env, {arrival + 80'000}, arrival + target);
        CHECK(s_unified_reserve_calls == reserves + 2 && env.owner_waits == 1);
        CHECK(env.start_waits == (target > 80'000 ? 1u : 0u) && s_unified_publishes == publishes + 1);
        CHECK(s_unified_outcome == UnifiedSleepOutcome::Completed &&
              s_unified_release_qpc == arrival + (target > 80'000 ? target : 80'000));
    }
    // Still taken at the estimate: nothing reserved or published, no further wait.
    {
        ScriptedSleepEnv env;
        const int64_t arrival = env.now;
        busy_start(&env, {arrival + 80'000, arrival + 80'000, arrival + 80'000}, arrival + 80'000);
        CHECK(s_unified_reserve_calls == reserves + 2 && env.owner_waits == 1 && env.now == arrival + 80'000);
        CHECK(env.start_waits == 0 && s_unified_publishes == publishes);
    }
    // Taken again after a release, and the law has no estimate any more (it was reset): forwarded.
    {
        ScriptedSleepEnv env;
        const int64_t arrival = env.now;
        env.owner_releases = {arrival + 50};
        busy_start(&env, {arrival + 80'000, 0}, arrival + 80'000);
        CHECK(s_unified_reserve_calls == reserves + 2 && env.owner_waits == 1 && env.now == arrival + 50);
        CHECK(s_unified_publishes == publishes);
    }
    // The wait for the owner ends on its own (cancelled, failed, stopping): one more reservation,
    // never a loop of them.
    {
        ScriptedSleepEnv env;
        const int64_t arrival = env.now;
        env.owner_wait_fails = true;
        busy_start(&env, {arrival + 80'000, arrival + 80'000, arrival + 80'000}, arrival + 80'000);
        CHECK(s_unified_reserve_calls == reserves + 2 && env.owner_waits == 1 && env.now == arrival + 100);
        CHECK(s_unified_publishes == publishes);
    }
    // No estimate ahead (the law has no origin, or the frame is already late): forwarded at once.
    for (const int64_t ahead : {int64_t{0}, int64_t{-1}}) {
        ScriptedSleepEnv env;
        const int64_t arrival = env.now;
        busy_start(&env, {ahead == 0 ? 0 : arrival + ahead}, arrival + 80'000);
        CHECK(s_unified_reserve_calls == reserves + 1 && env.owner_waits == 0 && env.start_waits == 0);
        CHECK(env.now == arrival && s_unified_publishes == publishes);
    }

    // The limit's own wait never asks for more than its budget (scripted clock, 10 MHz).
    {   // 8.33 ms left: the coarse wait is 8 ms (rounded down), the tail ends exactly at the deadline.
        ScriptedWaitPrimitives wait;
        UnifiedWaitForChange(wait, 5, 83'300);
        CHECK(wait.coarse_requests == std::vector<uint32_t>{8} && wait.now == 83'300 && wait.relaxes == 3'300);
    }
    {   // A change of the index during the coarse wait ends it at once, without the tail.
        ScriptedWaitPrimitives wait;
        wait.index_change_at = 20'000;
        UnifiedWaitForChange(wait, 5, 83'300);
        CHECK(wait.now == 20'000 && wait.relaxes == 0 && wait.index == 6);
    }
    {   // Under a millisecond left: no coarse wait, the tail alone, never past the deadline.
        ScriptedWaitPrimitives wait;
        UnifiedWaitForChange(wait, 5, 5'000);
        CHECK(wait.coarse_requests.empty() && wait.now == 5'000 && wait.relaxes == 5'000);
    }
    {   // A budget over one second waits one second and returns (the limit calls again).
        ScriptedWaitPrimitives wait;
        UnifiedWaitForChange(wait, 5, 20'000'000);
        CHECK(wait.coarse_requests == std::vector<uint32_t>{1000} && wait.now == 10'000'000 && wait.relaxes == 0);
    }
    {   // A deadline that has passed: nothing is waited for.
        ScriptedWaitPrimitives wait;
        wait.now = 100;
        UnifiedWaitForChange(wait, 5, 100);
        CHECK(wait.coarse_requests.empty() && wait.relaxes == 0 && wait.now == 100);
    }
    {   // An early coarse return without a change, 5 ms left: the coarse wait is repeated for
        // the 4 ms still remaining (floor), not a 5 ms spin; the deadline is met exactly.
        ScriptedWaitPrimitives wait;
        wait.early_waits = 1;
        wait.early_ticks = 10'000; // returns after 1 ms
        UnifiedWaitForChange(wait, 5, 50'000);
        CHECK((wait.coarse_requests == std::vector<uint32_t>{5, 4}) && wait.now == 50'000 && wait.relaxes == 0);
    }
    {   // A primitive that always returns early, one tick at a time: a bounded number of coarse
        // waits, then the tail, which ends exactly at the deadline (never past it).
        ScriptedWaitPrimitives wait;
        wait.early_waits = 1'000'000;
        wait.early_ticks = 1;
        UnifiedWaitForChange(wait, 5, 83'300);
        CHECK(wait.coarse_requests.size() == 8 && wait.now == 83'300 && wait.relaxes > 0);
    }

    // Self-check (the durations are scripted; the hook feeds it the genuine Sleep's duration): a
    // Sleep over 200 us in 16 of 64 sets "driver paces", fewer than 3 clear it, a bump (a key
    // toggle or Dynamic MFG) starts over.
    NvAPI_BumpSleepModeEpoch();
    const int64_t slow = us_to_qpc(1000.0), fast = us_to_qpc(50.0);
    CHECK(!g_unified_driver_pacing.load());
    for (int sleep = 0; sleep < 15; ++sleep) UnifiedSleepSelfCheck(slow);
    CHECK(!g_unified_driver_pacing.load());
    UnifiedSleepSelfCheck(slow);
    CHECK(g_unified_driver_pacing.load());
    for (int sleep = 0; sleep < 12; ++sleep) UnifiedSleepSelfCheck(fast);
    CHECK(g_unified_driver_pacing.load()); // the fast entries went to empty slots: 16 slow remain
    for (int sleep = 0; sleep < 64; ++sleep) UnifiedSleepSelfCheck(fast);
    CHECK(!g_unified_driver_pacing.load());
    for (int sleep = 0; sleep < 16; ++sleep) UnifiedSleepSelfCheck(slow);
    CHECK(g_unified_driver_pacing.load());
    NvAPI_BumpSleepModeEpoch();
    CHECK(!g_unified_driver_pacing.load());
    UnifiedSleepSelfCheck(slow);
    CHECK(!g_unified_driver_pacing.load()); // the ring started over
    {
        // Original 64-slot scan versus the bit history, including wrap, a reset, and
        // homogeneous stretches that need no atomic update once the window is full.
        bool slots[64]{};
        bool expected = false;
        NvAPI_BumpSleepModeEpoch();
        for (unsigned i = 0; i < 640; ++i) {
            if (i == 303) {
                NvAPI_BumpSleepModeEpoch();
                for (bool& slot : slots) slot = false;
                expected = false;
            }
            const bool is_slow = i % 231 < 70 || (i % 231 < 141 && i % 5 == 0);
            slots[(i >= 303 ? i - 303 : i) % 64] = is_slow;
            unsigned count = 0;
            for (bool slot : slots) count += slot;
            if (count >= 16) expected = true;
            if (count < 3) expected = false;
            UnifiedSleepSelfCheck(is_slow ? slow : fast);
            CHECK(g_unified_driver_pacing.load() == expected);
        }
    }

    // Not on the presenting device: the Sleep neither reserves nor publishes.
    {
        FakeDevice other;
        reserves = s_unified_reserve_calls;
        publishes = s_unified_publishes;
        s_unified_reserve_result = SchedulerUnifiedReserve::Reserved;
        (void)Hook_Sleep(&other);
        CHECK(s_unified_reserve_calls == reserves && s_unified_publishes == publishes);
    }

    RestoreSleepPipeline(old_pipeline);
    s_unified_reserve_result = SchedulerUnifiedReserve::Inactive;
    s_pre_sleep_wait_blocks = false;
    g_user_target_fps = 0;
    SetUnifiedKey(false);
    CHECK(NvAPI_SleepModeReconcilePending());
    NvAPI_ReconcileSleepMode();
    CHECK(!NvAPI_SleepModeReconcilePending());
    PacingPipeline_SetConfig(old_config);
    s_record_modes = false;
    s_mode_result = -9;
    s_forwarded_modes.clear();
    s_orig_sleep = old_sleep;
    s_orig_sleep_mode = old_mode;
    NvAPI_ClearFeedbackDevice();
}
// The Reflex Sleep bypass decision of Hook_Sleep, pinned to what the code does now: the owner is unified (published test
// mode Off) or a test mode; the published pipeline is what PacingPipeline_Resolve gives for the inputs the scheduler
// builds (route, runtime.pre_sleep_available = marker route && enforcement marker SimulationStart && a Reflex Sleep seen
// within the last second, runtime.uncapped); Scheduler_PacesSource / UnifiedActive answer from that pipeline like
// scheduler.cpp; the test mode in force is the real PacingPipeline_SleepBypass. A bypassed Sleep returns NVAPI_OK without
// the driver's Sleep, resets the Sleep latency to 0 and reports nothing to Scheduler_EndDriverSleep; the driver's Sleep
// returns the fake's -7 and is reported. Every Sleep on the presenting device is observed and recorded either way.
void SetSleepTestMode(PacingTestMode mode) {
    PacingPipelineConfig config = PacingPipeline_GetConfig();
    config.test_mode = mode;
    NvAPI_PrepareUnifiedKey(PacingPipeline_ClampConfig(config).unified);
    PacingPipeline_SetConfig(config);
    NvAPI_ReconcileSleepMode();
    // Key on: the reconcile stays pending (the key bit; no game parameters are captured here); key off: settled.
    CHECK(NvAPI_SleepModeReconcilePending() == (mode == PacingTestMode::Off));
    CHECK(PacingPipeline_TestMode() == mode && PacingPipeline_Unified() == (mode == PacingTestMode::Off));
    CHECK(PacingPipeline_SleepBypass() == (mode == PacingTestMode::Off));
}
struct SleepEffects {
    NvAPI_Status result;
    unsigned driver_sleeps, end_reports, recorded, observed, reserves;
    double latency_us;
    int64_t recorded_qpc;
};
SleepEffects CallSleep(FakeDevice* device) {
    g_pacer_latency_us = 500.0; // a stale duration: the bypass resets it, the driver's Sleep smooths it
    const unsigned driver_sleeps = s_sleep_calls, end_reports = s_driver_sleep_calls, recorded = s_record_sleep_calls;
    const uint64_t observed = s_reflex_threads.sleep_calls.load();
    const unsigned reserves = s_unified_reserve_calls;
    SleepEffects effects{};
    effects.result = Hook_Sleep(device);
    effects.driver_sleeps = s_sleep_calls - driver_sleeps;
    effects.end_reports = s_driver_sleep_calls - end_reports;
    effects.recorded = s_record_sleep_calls - recorded;
    effects.observed = static_cast<unsigned>(s_reflex_threads.sleep_calls.load() - observed);
    effects.reserves = s_unified_reserve_calls - reserves;
    effects.latency_us = g_pacer_latency_us.load();
    effects.recorded_qpc = s_recorded_sleep_qpc;
    return effects;
}
// Bypassed: the game's Sleep returns at once.
void CheckSleepBypassed(const SleepEffects& effects) {
    CHECK(effects.result == NVAPI_OK && effects.driver_sleeps == 0 && effects.end_reports == 0);
    CHECK(effects.latency_us == 0.0);
    CHECK(effects.recorded == 1 && effects.observed == 1 && effects.recorded_qpc > 0);
    CHECK(effects.reserves == 0);
}
// Not bypassed, not the unified start path: the driver's Sleep runs and is reported.
void CheckSleepForwarded(const SleepEffects& effects) {
    CHECK(effects.result == -7 && effects.driver_sleeps == 1 && effects.end_reports == 1);
    CHECK(effects.latency_us > 0.0);
    CHECK(effects.recorded == 1 && effects.observed == 1 && effects.recorded_qpc > 0);
    CHECK(effects.reserves == 0);
}
// The unified start path: reserved first (Inactive here: no start wait), then the driver's Sleep runs and is reported.
void CheckSleepUnified(const SleepEffects& effects) {
    CHECK(effects.result == -7 && effects.driver_sleeps == 1 && effects.end_reports == 1);
    CHECK(effects.latency_us > 0.0);
    CHECK(effects.recorded == 1 && effects.observed == 1 && effects.recorded_qpc > 0);
    CHECK(effects.reserves == 1);
}
void TestSleepBypassDecision() {
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    const auto old_sleep = s_orig_sleep;
    const auto old_mode = s_orig_sleep_mode;
    const PacingPipelineConfig old_config = CurrentConfig();
    const PacingPipelineSnapshot old_pipeline = PacingPipeline_Read();
    s_orig_sleep = FakeDriverSleep;
    s_orig_sleep_mode = FakeSleepMode;
    s_expected_mode_params = nullptr;
    s_mode_result = NVAPI_OK;
    s_record_modes = false;
    s_scheduler_from_pipeline = true;
    s_unified_reserve_result = SchedulerUnifiedReserve::Inactive;
    s_smooth_motion = false;
    s_dmfg_active = false;
    s_sleep_action = 0;

    // The owner is unified: the published test mode is Off.
    SetSleepTestMode(PacingTestMode::Off);
    CHECK(PacingPipeline_SleepBypass());
    g_user_target_fps = 157;

    // (a) The first Sleep after the start, and the first after more than a second without one: no Sleep is seen
    // (runtime.pre_sleep_available false), the placement is Marker, ReLimiter paces the source: bypassed.
    for (int round = 0; round < 2; ++round) {
        const PacingPipelineSnapshot marker = PublishResolvedPipeline(PacingRoute::NvApiMarkers, false, false);
        CHECK(marker.placement == PacingSourcePlacement::Marker && marker.mode == PacingTimingMode::Vrr);
        CHECK(Scheduler_PacesSource() && !Scheduler_UnifiedActive());
        CheckSleepBypassed(CallSleep(&device));
    }
    // (b) An enforcement marker other than SIMULATION_START: pre_sleep_available is false on every frame
    // (scheduler.cpp requires the marker to be SIMULATION_START), so the placement is Marker on every frame and every
    // Sleep is bypassed.
    for (int frame = 0; frame < 4; ++frame) {
        PublishResolvedPipeline(PacingRoute::NvApiMarkers, false, false);
        CheckSleepBypassed(CallSleep(&device));
        CheckSleepBypassed(CallSleep(&device)); // two Sleeps in one frame: both
    }
    // The same for the Streamline PCL marker route.
    const PacingPipelineSnapshot pcl = PublishResolvedPipeline(PacingRoute::PclMarkers, false, false);
    CHECK(pcl.placement == PacingSourcePlacement::Marker && Scheduler_PacesSource());
    CheckSleepBypassed(CallSleep(&device));
    // (c) The published placement is Present (the present route): bypassed.
    const PacingPipelineSnapshot present = PublishResolvedPipeline(PacingRoute::PresentBased, false, false);
    CHECK(present.placement == PacingSourcePlacement::Present && present.mode == PacingTimingMode::Vrr);
    CHECK(Scheduler_PacesSource() && !Scheduler_UnifiedActive());
    CheckSleepBypassed(CallSleep(&device));
    // (d) A Reflex Sleep was seen within the last second and the enforcement marker is SIMULATION_START
    // (pre_sleep_available): the placement is Unified, the unified start path runs and the driver's Sleep is called.
    const PacingPipelineSnapshot unified = PublishResolvedPipeline(PacingRoute::NvApiMarkers, true, false);
    CHECK(unified.placement == PacingSourcePlacement::Unified && unified.mode == PacingTimingMode::Vrr);
    CHECK(Scheduler_PacesSource() && Scheduler_UnifiedActive());
    CheckSleepUnified(CallSleep(&device));
    CheckSleepUnified(CallSleep(&device));
    // The idle cap (background, FG off) under the unified owner is Unified too while the game calls the Sleep.
    {
        PacingRuntimeState idle = SleepRuntime(true, false);
        idle.idle_cap = true;
        const PacingPipelineSnapshot idle_pipeline = PacingPipeline_Resolve(
            PacingPipeline_GetConfig(), SleepCapabilities(PacingRoute::NvApiMarkers), idle);
        PacingPipeline_Publish(idle_pipeline);
        CHECK(idle_pipeline.placement == PacingSourcePlacement::Unified && idle_pipeline.mode == PacingTimingMode::Idle);
        CHECK(Scheduler_UnifiedActive());
        CheckSleepUnified(CallSleep(&device));
    }
    // ... and back: no Sleep within the last second again, the first Sleep is bypassed.
    PublishResolvedPipeline(PacingRoute::NvApiMarkers, false, false);
    CheckSleepBypassed(CallSleep(&device));

    // (e) No cap (g_user_target_fps 0): never bypassed, whatever the placement of a, b and c.
    g_user_target_fps = 0;
    PublishResolvedPipeline(PacingRoute::NvApiMarkers, false, false); // a, b
    CheckSleepForwarded(CallSleep(&device));
    PublishResolvedPipeline(PacingRoute::PclMarkers, false, false);
    CheckSleepForwarded(CallSleep(&device));
    PublishResolvedPipeline(PacingRoute::PresentBased, false, false); // c
    CheckSleepForwarded(CallSleep(&device));
    // The resolver's own uncapped pipeline (mode Off, placement None) is not bypassed either, with a cap of 0 or not.
    const PacingPipelineSnapshot uncapped = PublishResolvedPipeline(PacingRoute::NvApiMarkers, true, true);
    CHECK(uncapped.mode == PacingTimingMode::Off && uncapped.placement == PacingSourcePlacement::None);
    CHECK(!Scheduler_PacesSource() && !Scheduler_UnifiedActive());
    CheckSleepForwarded(CallSleep(&device));
    g_user_target_fps = 157;
    CheckSleepForwarded(CallSleep(&device));

    // (f) The test modes: never bypassed, with or without a cap, whatever the published placement.
    for (const PacingTestMode mode : {PacingTestMode::ReflexOnly, PacingTestMode::DriverLimiter}) {
        SetSleepTestMode(mode);
        CHECK(!PacingPipeline_SleepBypass());
        for (const int cap : {157, 0}) {
            g_user_target_fps = cap;
            for (const PacingRoute route : {PacingRoute::NvApiMarkers, PacingRoute::PclMarkers, PacingRoute::PresentBased}) {
                // What the resolver gives in this mode.
                const PacingPipelineSnapshot resolved = PublishResolvedPipeline(route, true, false);
                if (mode == PacingTestMode::ReflexOnly)
                    CHECK(resolved.mode == PacingTimingMode::Off && resolved.placement == PacingSourcePlacement::None);
                CheckSleepForwarded(CallSleep(&device));
            }
            // A placement that would be paced by ReLimiter, published by hand: still not bypassed.
            for (const PacingSourcePlacement placement : {PacingSourcePlacement::Marker, PacingSourcePlacement::Present,
                     PacingSourcePlacement::PreSleep, PacingSourcePlacement::Unified}) {
                PacingPipelineSnapshot forced{};
                forced.mode = PacingTimingMode::Vrr;
                forced.route = placement == PacingSourcePlacement::Present ? PacingRoute::PresentBased : PacingRoute::NvApiMarkers;
                forced.placement = placement;
                forced.rung = PacingRung::SourceOnly;
                forced.reason = PacingFallbackReason::None;
                PacingPipeline_Publish(forced);
                CHECK(Scheduler_PacesSource() == (placement != PacingSourcePlacement::PreSleep));
                const SleepEffects effects = CallSleep(&device);
                // The driver's Sleep still runs, whatever the placement.
                CHECK(effects.result == -7 && effects.driver_sleeps == 1 && effects.end_reports == 1);
                CHECK(effects.recorded == 1 && effects.observed == 1 && effects.reserves == 0);
            }
        }
    }
    // The driver limiter's resolved placement: the driver paces the source, nothing is held, no entry is recorded.
    SetSleepTestMode(PacingTestMode::DriverLimiter);
    g_user_target_fps = 157;
    const PacingPipelineSnapshot limiter = PublishResolvedPipeline(PacingRoute::NvApiMarkers, true, false);
    CHECK(limiter.placement == PacingSourcePlacement::DriverLimiter && limiter.mode == PacingTimingMode::Vrr);
    CHECK(!Scheduler_PacesSource() && !Scheduler_UnifiedActive());
    CheckSleepForwarded(CallSleep(&device));

    SetSleepTestMode(PacingTestMode::Off);
    CHECK(PacingPipeline_SleepBypass());
    NvAPI_PrepareUnifiedKey(PacingPipeline_ClampConfig(old_config).unified);
    PacingPipeline_SetConfig(old_config);
    NvAPI_ReconcileSleepMode();
    CHECK(!NvAPI_SleepModeReconcilePending());
    PacingPipeline_Publish(old_pipeline);
    g_user_target_fps = 0;
    g_pacer_latency_us = 0.0;
    s_scheduler_from_pipeline = false;
    s_orig_sleep = old_sleep;
    s_orig_sleep_mode = old_mode;
    s_mode_result = -9;
    NvAPI_ClearFeedbackDevice();
}
#endif

void RepublishDuringQuery() {
    NvAPI_ClearFeedbackDevice(s_reentrant_expected);
    CHECK(NvAPI_CaptureFeedbackDevice(s_reentrant_device, &s_reentrant_publication));
}

#ifdef _WIN64
// Developer flip_metering_off: the nvapi_QueryInterface detour hides
// NvAPI_D3D12_SetFlipConfig on every request (nullptr, the driver is not asked)
// and forwards every other ID with the driver's result.
static int s_flip_config_log_writes = 0; // Log_Write lines of the SetFlipConfig observer
static int s_reflex_observe_log_writes = 0; // Log_Write lines that start with "Reflex observe: "
static int s_query_calls = 0;
static NvU32 s_query_last = 0;
static int s_query_target = 0;
static void* __cdecl FakeQueryInterface(NvU32 id) {
    ++s_query_calls;
    s_query_last = id;
    return &s_query_target;
}

void TestFlipMeteringBlock() {
    const auto old_query = s_orig_query_interface;
    s_orig_query_interface = FakeQueryInterface;
    s_query_calls = 0;
    CHECK(Hook_QueryInterface(0xF3148C42) == nullptr);
    CHECK(Hook_QueryInterface(0xF3148C42) == nullptr);
    CHECK(s_query_calls == 0);
    CHECK(Hook_QueryInterface(0x852CD1D2) == &s_query_target); // NvAPI_D3D_Sleep
    CHECK(s_query_calls == 1 && s_query_last == 0x852CD1D2);
    s_orig_query_interface = old_query;
}

// The SetFlipConfig observer passes the call through unchanged and logs one line
// per distinct configuration and driver answer (not per batch), at most
// kFlipConfigLogLimit lines and one line that says later changes are not logged.
static NvAPI_Status s_flip_driver_status = NVAPI_OK;
static NvU32 s_flip_driver_feedback = 0;
static int s_flip_driver_calls = 0;
static IUnknown* s_flip_driver_queue = nullptr;
static UINT s_flip_driver_source = 0;
static NV_FLIP_CONFIG* s_flip_driver_config = nullptr;
static NvAPI_Status __cdecl FakeSetFlipConfig(IUnknown* queue, UINT source, NV_FLIP_CONFIG* config) {
    ++s_flip_driver_calls;
    s_flip_driver_queue = queue; s_flip_driver_source = source; s_flip_driver_config = config;
    config->flipConfigFeedbackMask = s_flip_driver_feedback;
    return s_flip_driver_status;
}

void TestFlipConfigObserver() {
    const auto old = s_orig_set_flip_config;
    s_orig_set_flip_config = FakeSetFlipConfig;
    NV_FLIP_CONFIG config{};
    config.version = sizeof(NV_FLIP_CONFIG) | (2u << 16);
    config.nFramesPerBatch = 4; config.nPresentedFramesPerBatch = 4; config.timePerBatchMs = 12.5f;
    auto* queue = reinterpret_cast<IUnknown*>(&s_query_target);
    const int logs = s_flip_config_log_writes;
    // The driver's status and feedback reach the caller; the inputs stay as given.
    s_flip_driver_status = -104; s_flip_driver_feedback = 2;
    CHECK(Hook_SetFlipConfig(queue, 3, &config) == -104);
    CHECK(s_flip_driver_calls == 1 && s_flip_driver_queue == queue && s_flip_driver_source == 3 &&
          s_flip_driver_config == &config);
    CHECK(config.flipConfigFeedbackMask == 2 && config.nFramesPerBatch == 4 && config.timePerBatchMs == 12.5f);
    CHECK(s_flip_config_log_writes == logs + 1);
    // The newest call that enabled metering is kept for the pacing CSV: its QPC and batch time in µs.
    int64_t flip_qpc = 0;
    uint32_t flip_us = 0;
    CHECK(FlipConfig_Read(&flip_qpc, &flip_us) && flip_qpc > 0 && flip_us == 12500);
    // The same configuration with another batch time is not logged again.
    config.timePerBatchMs = 13.25f;
    Hook_SetFlipConfig(queue, 3, &config);
    CHECK(s_flip_config_log_writes == logs + 1);
    CHECK(FlipConfig_Read(&flip_qpc, &flip_us) && flip_us == 13250);
    // A change of the driver's answer is.
    s_flip_driver_status = NVAPI_OK; s_flip_driver_feedback = 0;
    Hook_SetFlipConfig(queue, 3, &config);
    CHECK(s_flip_config_log_writes == logs + 2);
    // A configuration that changes on every batch stops being logged at the limit.
    for (int i = 0; i < 64; ++i) {
        config.nFramesPerBatch = (i % 2) ? 4 : 0;
        Hook_SetFlipConfig(queue, 3, &config);
    }
    CHECK(s_flip_config_log_writes == logs + static_cast<int>(kFlipConfigLogLimit) + 1);
    CHECK(s_flip_driver_calls == 3 + 64);
    // The newest call that enabled metering: a call that turns it off does not
    // replace it, a V1 structure has no batch time, a later V2 call does.
    config.nFramesPerBatch = 4;
    config.timePerBatchMs = 13.25f;
    Hook_SetFlipConfig(queue, 3, &config);
    CHECK(FlipConfig_Read(&flip_qpc, &flip_us) && flip_us == 13250);
    const int64_t metering_qpc = flip_qpc;
    config.nFramesPerBatch = 0;
    Hook_SetFlipConfig(queue, 3, &config);
    CHECK(FlipConfig_Read(&flip_qpc, &flip_us) && flip_qpc == metering_qpc && flip_us == 13250);
    NV_FLIP_CONFIG v1 = config;
    v1.version = sizeof(NV_FLIP_CONFIG) | (1u << 16);
    v1.nFramesPerBatch = 2;
    Hook_SetFlipConfig(queue, 3, &v1);
    CHECK(FlipConfig_Read(&flip_qpc, &flip_us) && flip_qpc >= metering_qpc && flip_us == 0);
    config.nFramesPerBatch = 4;
    config.timePerBatchMs = 8.5f;
    Hook_SetFlipConfig(queue, 3, &config);
    CHECK(FlipConfig_Read(&flip_qpc, &flip_us) && flip_us == 8500);
    s_orig_set_flip_config = old;
}

// The Reflex observation hooks forward every argument and return the driver's
// result unchanged. They log the first SetReflexSync call and each change of its
// tuple (not each frame's timeInQueueUs), each out-of-band queue call up to a
// limit, and each change of the sleep status (the game's calls and the
// developer poll, once per second, through the trampoline).
static NvAPI_Status s_sync_status = NVAPI_OK;
static int s_sync_calls = 0;
static IUnknown* s_sync_device = nullptr;
static NV_SET_REFLEX_SYNC_PARAMS* s_sync_params = nullptr;
static NvAPI_Status __cdecl FakeSetReflexSync(IUnknown* device, NV_SET_REFLEX_SYNC_PARAMS* params) {
    ++s_sync_calls; s_sync_device = device; s_sync_params = params;
    return s_sync_status;
}
static int s_oob_calls = 0;
static IUnknown* s_oob_queue = nullptr;
static int s_oob_type = -1;
static NvAPI_Status __cdecl FakeNotifyOutOfBand(IUnknown* queue, int type) {
    ++s_oob_calls; s_oob_queue = queue; s_oob_type = type;
    return -5;
}
static int s_hint_calls = 0;
static IUnknown* s_hint_device = nullptr;
static NvAPI_Status __cdecl FakeLowLatencyHint(IUnknown* device) {
    ++s_hint_calls; s_hint_device = device;
    return -6;
}
static int s_status_calls = 0;
static NV_GET_SLEEP_STATUS_PARAMS s_status_input{};   // what the driver received
static NV_GET_SLEEP_STATUS_PARAMS s_status_answer{};  // what it answers
static NvAPI_Status s_status_result = NVAPI_OK;       // and its status
static NvAPI_Status __cdecl FakeGetSleepStatus(IUnknown*, NV_GET_SLEEP_STATUS_PARAMS* params) {
    ++s_status_calls;
    s_status_input = *params;
    *params = s_status_answer;
    return s_status_result;
}

void TestReflexObservation() {
    const auto old_sync = s_orig_set_reflex_sync;
    const auto old_oob = s_orig_notify_out_of_band;
    const auto old_hint = s_orig_low_latency_hint;
    const auto old_status = s_orig_get_sleep_status;
    s_orig_set_reflex_sync = FakeSetReflexSync;
    s_orig_notify_out_of_band = FakeNotifyOutOfBand;
    s_orig_low_latency_hint = FakeLowLatencyHint;
    s_orig_get_sleep_status = FakeGetSleepStatus;
    auto* device = reinterpret_cast<IUnknown*>(&s_query_target);

    // SetReflexSync: arguments and result pass through; the call is logged once.
    NV_SET_REFLEX_SYNC_PARAMS sync{};
    sync.version = static_cast<NvU32>(sizeof(sync)) | (1u << 16);
    sync.bEnable = 1;
    sync.vblankIntervalUs = 16667;
    sync.timeInQueueUs = -250;
    sync.timeInQueueUsTarget = 2000;
    sync.fgMultiplier = 2;
    int logs = s_reflex_observe_log_writes;
    s_sync_status = -104;
    CHECK(Hook_SetReflexSync(device, &sync) == -104);
    CHECK(s_sync_calls == 1 && s_sync_device == device && s_sync_params == &sync);
    CHECK(sync.bEnable == 1 && sync.bDisable == 0 && sync.vblankIntervalUs == 16667 && sync.timeInQueueUs == -250 &&
          sync.timeInQueueUsTarget == 2000 && sync.fgMultiplier == 2);
    CHECK(s_reflex_observe_log_writes == logs + 1);
    // timeInQueueUs changes every frame: no line.
    for (int i = 0; i < 10; ++i) {
        sync.timeInQueueUs = 100 * i;
        CHECK(Hook_SetReflexSync(device, &sync) == -104);
    }
    CHECK(s_sync_calls == 11 && s_reflex_observe_log_writes == logs + 1);
    // A change of the multiplier, of a reserved byte and of the driver's status each are.
    sync.fgMultiplier = 3;
    Hook_SetReflexSync(device, &sync);
    CHECK(s_reflex_observe_log_writes == logs + 2);
    sync.rsvd[5] = 0xAB;
    Hook_SetReflexSync(device, &sync);
    CHECK(s_reflex_observe_log_writes == logs + 3);
    s_sync_status = NVAPI_OK;
    Hook_SetReflexSync(device, &sync);
    CHECK(s_reflex_observe_log_writes == logs + 4);
    CHECK(Hook_SetReflexSync(device, nullptr) == NVAPI_OK && s_sync_params == nullptr); // forwarded as given

    // NotifyOutOfBandCommandQueue: each call, up to the limit and one line that says so.
    auto* queue = reinterpret_cast<IUnknown*>(&s_oob_type);
    logs = s_reflex_observe_log_writes;
    CHECK(Hook_NotifyOutOfBandCommandQueue(queue, 3) == -5);
    CHECK(s_oob_calls == 1 && s_oob_queue == queue && s_oob_type == 3);
    CHECK(s_reflex_observe_log_writes == logs + 1);
    for (int i = 0; i < 40; ++i) Hook_NotifyOutOfBandCommandQueue(queue, i % 4);
    CHECK(s_oob_calls == 41 && s_oob_type == 39 % 4);
    CHECK(s_reflex_observe_log_writes == logs + static_cast<int>(kOutOfBandLogLimit) + 1);

    // SetCreateCommandQueueLowLatencyHint.
    logs = s_reflex_observe_log_writes;
    CHECK(Hook_SetCreateCommandQueueLowLatencyHint(device) == -6);
    CHECK(s_hint_calls == 1 && s_hint_device == device);
    CHECK(s_reflex_observe_log_writes == logs + 1);

    // GetSleepStatus of the game: the driver's answer reaches the caller; a change
    // of the flags or of the interval by 5% or more is logged, a smaller one is not.
    s_status_answer = {};
    s_status_answer.bLowLatencyMode = 1;
    s_status_answer.sleepIntervalUs = 8000;
    s_status_answer.fgMultiplier = 2;
    NV_GET_SLEEP_STATUS_PARAMS out{};
    out.version = static_cast<NvU32>(NV_GET_SLEEP_STATUS_PARAMS_VER);
    logs = s_reflex_observe_log_writes;
    const int game_vrr_calls = s_game_vrr_calls;
    CHECK(Hook_GetSleepStatus(device, &out) == NVAPI_OK);
    CHECK(s_status_calls == 1 && s_status_input.version == static_cast<NvU32>(NV_GET_SLEEP_STATUS_PARAMS_VER) &&
          s_reflex_observe_log_writes == logs + 1);
    CHECK(out.bLowLatencyMode == 1 && out.sleepIntervalUs == 8000 && out.fgMultiplier == 2);
    // No device presents and none is published: its answer about the game's frames goes nowhere.
    CHECK(s_game_vrr_calls == game_vrr_calls);
    Hook_GetSleepStatus(device, &out);
    s_status_answer.sleepIntervalUs = 8200;   // 2.5%
    Hook_GetSleepStatus(device, &out);
    CHECK(s_reflex_observe_log_writes == logs + 1);
    s_status_answer.sleepIntervalUs = 9000;   // 12.5%
    Hook_GetSleepStatus(device, &out);
    CHECK(s_reflex_observe_log_writes == logs + 2);
    s_status_answer.fgMultiplier = 3;
    Hook_GetSleepStatus(device, &out);
    CHECK(s_reflex_observe_log_writes == logs + 3);

    // The newest successful result of the presenting device is kept for the pacing
    // CSV (driver_sleep_interval_us, driver_use_game_sleep): another device's
    // answer and a failed call are not, and a publication change clears it.
    g_sleep_status_newest = 0;
    FakeDevice presenting;
    CHECK(NvAPI_CaptureFeedbackDevice(&presenting));
    s_status_answer.sleepIntervalUs = 8333;
    s_status_answer.bUseGameSleep = 1;
    // Without the status instrumentation (developer_settings) the game's call is not recorded.
    const bool saved_developer = g_config.developer_settings;
    g_config.developer_settings = false;
    Hook_GetSleepStatus(&presenting, &out);
    CHECK(g_sleep_status_newest.load() == 0);
    // The driver's answer about the game's frames counts only from the published device while it presents: not
    // before it presents, not from another device (which also does not stop ReLimiter's own read), and not from a
    // failed call.
    CHECK(s_game_vrr_calls == game_vrr_calls);
    s_test_presenting = &presenting;
    s_test_presenting_sequence = 2;
    s_test_api = ActiveAPI::DX12;
    s_game_sleep_status_ms.store(0);
    Hook_GetSleepStatus(device, &out);
    CHECK(s_game_vrr_calls == game_vrr_calls && s_game_sleep_status_ms.load() == 0);
    Hook_GetSleepStatus(&presenting, &out);
    CHECK(s_game_vrr_calls == game_vrr_calls + 1 && !s_game_vrr_own_read && s_game_sleep_status_ms.load() != 0);
    g_sleep_status_newest = 0;
    g_config.developer_settings = true;
    Hook_GetSleepStatus(device, &out);
    CHECK(g_sleep_status_newest.load() == 0);
    Hook_GetSleepStatus(&presenting, &out);
    CHECK(g_sleep_status_newest.load() == CurrentStatusWord(8333, true));
    s_status_answer.sleepIntervalUs = 1111;
    s_status_result = -9;
    const int vrr_calls_before_failure = s_game_vrr_calls;
    Hook_GetSleepStatus(&presenting, &out);
    CHECK(g_sleep_status_newest.load() == CurrentStatusWord(8333, true));
    CHECK(s_game_vrr_calls == vrr_calls_before_failure);
    s_test_presenting = nullptr;
    s_test_presenting_sequence = 0;
    s_test_api = ActiveAPI::None;
    s_status_result = NVAPI_OK;
    const auto saved_demand = g_pacing_telemetry_demand.exchange(kPacingReadout);
    PublishSleepStatusAtSleepReturn(g_sleep_status_token.load());
    NvAPI_ClearFeedbackDevice();
    CHECK(g_sleep_status_newest.load() == 0 && !TakeSleepRow().driver_sleep_status_valid);
    // A result whose driver query straddled a publication change carries the old
    // publication's token: it is not recorded, and a stale word is not carried to the
    // next Source record.
    const uint32_t stale_token = g_sleep_status_token.load() + 1;
    NV_GET_SLEEP_STATUS_PARAMS stale_status{};
    stale_status.sleepIntervalUs = 4321;
    RecordSleepStatus(NVAPI_OK, stale_status, stale_token);
    CHECK(g_sleep_status_newest.load() == 0);
    RecordSleepStatus(NVAPI_OK, stale_status, g_sleep_status_token.load());
    CHECK(g_sleep_status_newest.load() == CurrentStatusWord(4321, false));
    g_sleep_status_newest = SleepStatusWord(4321, false, stale_token);
    PublishSleepStatusAtSleepReturn(g_sleep_status_token.load());
    CHECK(!TakeSleepRow().driver_sleep_status_valid);
    // A Sleep that retained the old publication does not carry the new publication's result.
    g_sleep_status_newest = CurrentStatusWord(4321, false);
    PublishSleepStatusAtSleepReturn(stale_token);
    CHECK(!TakeSleepRow().driver_sleep_status_valid);
    g_sleep_status_newest = 0;
    s_status_answer.bUseGameSleep = 0;
    s_status_answer.sleepIntervalUs = 9000;

    // The poll. Without developer_settings it reads only when no game call carried the answer within the last
    // second, and then only hands the answer to the display state (no record, no line). With it, one driver call
    // per second with a zeroed structure of the current version, through the trampoline.
    g_config.developer_settings = saved_developer;
    const bool old_developer = g_config.developer_settings;
    g_config.developer_settings = false;
    s_sleep_status_poll_ms.store(0);
    const int status_calls = s_status_calls;
    PollSleepStatus(device, g_sleep_status_token.load());   // the game asked just now
    CHECK(s_status_calls == status_calls);
    s_game_sleep_status_ms.store(0);                         // it has not asked within the last second
    s_status_answer.bFsVrr = 1;
    s_status_answer.bFullscreenIFlip = 1;
    const int vrr_calls = s_game_vrr_calls;
    logs = s_reflex_observe_log_writes;
    PollSleepStatus(device, g_sleep_status_token.load());
    CHECK(s_status_calls == status_calls + 1 && s_game_vrr_calls == vrr_calls + 1);
    CHECK(s_game_vrr_fullscreen_vrr && s_game_vrr_fullscreen_iflip && s_game_vrr_own_read);
    CHECK(g_sleep_status_newest.load() == 0 && s_reflex_observe_log_writes == logs);
    PollSleepStatus(device, g_sleep_status_token.load());
    CHECK(s_status_calls == status_calls + 1);   // once per second
    s_status_answer.bFsVrr = 0;
    s_status_answer.bFullscreenIFlip = 0;
    s_sleep_status_poll_ms.store(0);
    g_config.developer_settings = true;
    const int developer_calls = s_status_calls;
    logs = s_reflex_observe_log_writes;
    PollSleepStatus(device, g_sleep_status_token.load());
    CHECK(s_status_calls == developer_calls + 1);
    CHECK(s_status_input.version == static_cast<NvU32>(NV_GET_SLEEP_STATUS_PARAMS_VER) &&
          s_status_input.sleepIntervalUs == 0 && s_status_input.fgMultiplier == 0 &&
          s_status_input.bLowLatencyMode == 0);
    CHECK(s_reflex_observe_log_writes == logs + 1);
    PollSleepStatus(device, g_sleep_status_token.load());
    CHECK(s_status_calls == developer_calls + 1);
    // The poll's result is the newest one.
    CHECK(g_sleep_status_newest.load() == CurrentStatusWord(9000, false));
    // A poll whose Sleep retained a publication that is no longer current: the query runs,
    // the result is dropped (not tagged with the current publication's token, and not the
    // driver's answer about the game's frames).
    s_sleep_status_poll_ms.store(0);
    s_status_answer.sleepIntervalUs = 7777;
    g_sleep_status_newest = 0;
    const int vrr_calls_before_stale = s_game_vrr_calls;
    PollSleepStatus(device, g_sleep_status_token.load() + 1);
    CHECK(s_status_calls == developer_calls + 2 && g_sleep_status_newest.load() == 0);
    CHECK(s_game_vrr_calls == vrr_calls_before_stale);
    g_sleep_status_newest = CurrentStatusWord(7777, false);
    PollSleepStatus(device, g_sleep_status_token.load());
    CHECK(s_status_calls == developer_calls + 2); // once per second again
    // The snapshot a Sleep leaves for the next Source record.
    PublishSleepStatusAtSleepReturn(g_sleep_status_token.load());
    const auto row = TakeSleepRow();
    CHECK(row.driver_sleep_status_valid && row.driver_sleep_interval_us == 7777 && !row.driver_use_game_sleep);
    g_pacing_telemetry_demand.store(saved_demand);
    g_sleep_status_newest = 0;
    g_config.developer_settings = old_developer;

    // Thread map: one line after the 600th Sleep and one for each change of a
    // recorded thread or async queue; Sleeps in one frame and frames without a Sleep are counted.
    s_reflex_threads.sleep_calls.store(kThreadMapAfterSleeps - 1);
    ObserveLatencyMarkerThread(SIMULATION_START);
    const uint64_t doubles = s_reflex_threads.double_sleeps.load();
    const uint64_t without_sleep = s_reflex_threads.frames_without_sleep.load();
    logs = s_reflex_observe_log_writes;
    ObserveReflexSleep();
    CHECK(s_reflex_observe_log_writes == logs + 1);
    ObserveReflexSleep();
    CHECK(s_reflex_observe_log_writes == logs + 1 && s_reflex_threads.double_sleeps.load() == doubles + 1);
    ObserveAsyncMarkerThread(9, queue);
    ObserveReflexSleep();
    CHECK(s_reflex_observe_log_writes == logs + 2);
    const auto generation = s_reflex_threads.generation.load();
    const auto signature = ThreadMapSignature();
    ObserveAsyncMarkerThread(9, queue);
    CHECK(s_reflex_threads.generation.load() == generation);
    ObserveAsyncMarkerThread(9, nullptr);
    CHECK(ThreadMapSignature() != signature);
    ObserveAsyncMarkerThread(9, queue);
    CHECK(ThreadMapSignature() == signature && s_reflex_threads.generation.load() != generation);
    ObserveReflexSleep();
    CHECK(s_reflex_observe_log_writes == logs + 2); // a reverted tuple did not consume a line
    ObserveLatencyMarkerThread(SIMULATION_START); // a Sleep came before it
    CHECK(s_reflex_threads.frames_without_sleep.load() == without_sleep);
    ObserveLatencyMarkerThread(SIMULATION_START); // none did
    CHECK(s_reflex_threads.frames_without_sleep.load() == without_sleep + 1);

    s_orig_set_reflex_sync = old_sync;
    s_orig_notify_out_of_band = old_oob;
    s_orig_low_latency_hint = old_hint;
    s_orig_get_sleep_status = old_status;
}
#endif

// Fixtures of ReShade's device lock. Independently pinned to deps/reshade/source/d3d12/d3d12_device.cpp: a device
// proxy's AddRef/Release and its QueryInterface for its own interfaces or IUnknown take ONE process-wide,
// NON-recursive lock (g_d3d12_adapter_mutex); IID_UnwrappedObject and any other IID do not. The proxy registers
// itself in the native device's private data under its class GUID.
constexpr GUID kProxyClassD3D12 = {
    0x2523AFF4, 0x978B, 0x4939, { 0xBA, 0x16, 0x8E, 0xE8, 0x76, 0xA4, 0xCB, 0x2A }
};

class FakeReShadeLock {
public:
    std::atomic<DWORD> owner{0};
    std::atomic<unsigned> proxy_calls{0}; // proxy AddRef/Release/own-interface QueryInterface calls made
    std::atomic<bool> strict{false};      // strict: ANY such call fails the test
    void EnterProxyCall() {
        CHECK(!strict.load());
        ++proxy_calls;
        Enter();
    }
    // The thread that already holds the lock would wait for itself forever (the launch hang): fail at once.
    void Enter() {
        const DWORD self = GetCurrentThreadId();
        CHECK(owner.load() != self);
        DWORD expected = 0;
        while (!owner.compare_exchange_weak(expected, self)) { expected = 0; Sleep(0); }
    }
    void Leave() { owner.store(0); }
};

// ReShade running an event (init_device, destroy_device, unload_addons) under its lock.
class HeldLock {
public:
    explicit HeldLock(FakeReShadeLock& lock) : lock_(lock) { lock_.Enter(); }
    ~HeldLock() { lock_.Leave(); }
    HeldLock(const HeldLock&) = delete;
    HeldLock& operator=(const HeldLock&) = delete;
private:
    FakeReShadeLock& lock_;
};

// A native Direct3D 12 device object: no ReShade lock; names its ReShade proxy in its private data.
class FakeNative12 final : public ID3D12Object {
public:
    IUnknown* proxy = nullptr;
    std::atomic<ULONG> references{1}; // the stack fixture owns the initial reference
    std::atomic<unsigned> queries{0}, add_refs{0}, releases{0};
    // Armed for one thread: its next AddRef parks here (a reader between loading the publication and referencing it).
    HANDLE park_entered = nullptr, park_release = nullptr;
    std::atomic<DWORD> park_thread{0};

    ~FakeNative12() {
        CHECK(references == 1);
        CHECK(add_refs == releases);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        ++queries;
        if (!IsEqualIID(iid, __uuidof(IUnknown)) && !IsEqualIID(iid, __uuidof(ID3D12Object))) return E_NOINTERFACE;
        *result = static_cast<ID3D12Object*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        if (park_thread.load() == GetCurrentThreadId()) {
            park_thread.store(0);
            SetEvent(park_entered);
            CHECK(WaitForSingleObject(park_release, 10000) == WAIT_OBJECT_0);
        }
        ++add_refs;
        return ++references;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        CHECK(references > 1);
        ++releases;
        return --references;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
        if (!proxy || !IsEqualGUID(guid, kProxyClassD3D12) || !size || !data || *size < sizeof(void*)) return E_FAIL;
        *static_cast<void**>(data) = proxy;
        *size = sizeof(void*);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_NOTIMPL; }
};

// ReShade's device proxy over a native device. A stack fixture owns the initial reference.
class FakeProxy12 final : public IUnknown {
public:
    FakeProxy12(FakeNative12* native_device, FakeReShadeLock* device_lock) : native(native_device), lock(device_lock) {}
    FakeNative12* native;
    FakeReShadeLock* lock;
    ULONG refs = 1;
    std::atomic<unsigned> unwrap_queries{0};

    ~FakeProxy12() { CHECK(refs == 1); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (IsEqualIID(iid, kUnwrappedObject)) { // no lock
            ++unwrap_queries;
            native->AddRef();
            *result = static_cast<IUnknown*>(native);
            return S_OK;
        }
        if (IsEqualIID(iid, __uuidof(IUnknown))) { // AddRef under the lock
            lock->EnterProxyCall();
            ++refs;
            native->AddRef();
            lock->Leave();
            *result = this;
            return S_OK;
        }
        return native->QueryInterface(iid, result); // any other IID goes to the native object
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        lock->EnterProxyCall();
        const ULONG count = ++refs;
        native->AddRef();
        lock->Leave();
        return count;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        lock->EnterProxyCall();
        CHECK(refs > 1);
        const ULONG count = --refs;
        native->Release();
        lock->Leave();
        return count;
    }
};

// ctest feedback_device_lock_fixture: the fixture must fail AT ONCE on the old behaviour, a proxy AddRef from inside
// a ReShade event (expected output: "FAIL line N: owner.load() != self", not a hang).
void RunProxyCallUnderLockControl() {
    FakeReShadeLock lock;
    FakeNative12 native;
    FakeProxy12 proxy(&native, &lock);
    HeldLock held(lock);
    proxy.AddRef();
}

void TestIdentityNormalization() {
    FakeDevice native, unrelated, alias, wrapper, nested, sibling;
    alias.canonical = &native;
    wrapper.Wrap(&native);
    nested.Wrap(&wrapper);
    sibling.Wrap(&native);
    CHECK(!FeedbackDeviceIdentity_IsSame(nullptr, nullptr));
    CHECK(!FeedbackDeviceIdentity_IsSame(&native, nullptr));
    CHECK(!FeedbackDeviceIdentity_IsSame(nullptr, &native));
    CHECK(FeedbackDeviceIdentity_IsSame(&native, &native));
    CHECK(FeedbackDeviceIdentity_IsSame(&alias, &native));
    CHECK(FeedbackDeviceIdentity_IsSame(&native, &alias));
    CHECK(FeedbackDeviceIdentity_IsSame(&wrapper, &native));
    CHECK(FeedbackDeviceIdentity_IsSame(&native, &nested));
    CHECK(FeedbackDeviceIdentity_IsSame(&nested, &sibling));
    CHECK(!FeedbackDeviceIdentity_IsSame(&native, &unrelated));
    CHECK(!FeedbackDeviceIdentity_IsSame(&nested, &unrelated));
    CHECK(FeedbackDeviceIdentity_IsSame(&nested, &alias));
    // Only the terminal object is asked for its identity, once; a layer that answered the unwrap query is never
    // asked for IUnknown, AddRef'd or Released by the resolution (the incoming pointer is borrowed).
    IUnknown* const resolved = DeviceIdentity_AcquireNative(&nested);
    CHECK(resolved == &native);
    NvAPI_ReleaseFeedbackDevice(resolved);
    CHECK(nested.add_refs == 0 && nested.releases == 0);
}

void TestCyclesAndBounds() {
    FakeDevice native, self, self_alias, first, second;
    self.Wrap(&self);
    self_alias.Wrap(&self);
    CHECK(FeedbackDeviceIdentity_IsSame(&self, &self_alias));
    CHECK(!FeedbackDeviceIdentity_IsSame(&self, &native));
    first.Wrap(&second);
    second.Wrap(&first);
    CHECK(!FeedbackDeviceIdentity_IsSame(&first, &native));
    CHECK(!FeedbackDeviceIdentity_IsSame(&first, &second));
    CHECK(first.queries + second.queries < 32);

    std::array<FakeDevice, 12> deep;
    for (size_t i = 0; i + 1 < deep.size(); ++i) deep[i].Wrap(&deep[i + 1]);
    deep.back().Wrap(&native);
    CHECK(!FeedbackDeviceIdentity_IsSame(&deep.front(), &native));
    unsigned queries = 0;
    for (const auto& device : deep) queries += device.queries;
    CHECK(queries <= 16); // Fixed eight-layer production bound, not a loop hang.
}

void TestFailedQueries() {
    FakeDevice native, wrapper;
    wrapper.Wrap(&native);
    wrapper.unwrap_status = E_FAIL; // Nonconforming failure WITH an owned output.
    CHECK(!FeedbackDeviceIdentity_IsSame(&wrapper, &native));
    CHECK(native.references == 1);
    wrapper.unwrap_status = S_OK;
    wrapper.unwrapped = nullptr; // Nonconforming success WITHOUT an output.
    CHECK(!FeedbackDeviceIdentity_IsSame(&wrapper, &native));
    wrapper.fault_unwrap = true;
    CHECK(!FeedbackDeviceIdentity_IsSame(&wrapper, &native));
    wrapper.fault_unwrap = false;
    wrapper.Wrap(&native);
    // The layer that answered the unwrap query is never asked for its own identity: failures there are not seen.
    wrapper.identity_status = E_FAIL;
    wrapper.null_identity = true;
    CHECK(FeedbackDeviceIdentity_IsSame(&wrapper, &native));
    CHECK(wrapper.add_refs == 0 && wrapper.releases == 0);
    wrapper.identity_status = S_OK;
    wrapper.null_identity = false;
    // The terminal object's own identity failing fails closed.
    native.identity_status = E_FAIL;
    CHECK(!FeedbackDeviceIdentity_IsSame(&wrapper, &native));
    native.identity_status = S_OK;
    native.null_identity = true;
    CHECK(!FeedbackDeviceIdentity_IsSame(&wrapper, &native));
    native.null_identity = false;
    CHECK(native.references == 1);
}

void TestStableAliasCapture() {
    FakeDevice native, wrapper, alias, unrelated;
    wrapper.Wrap(&native);
    alias.canonical = &native;
    NvAPIFeedbackDevice original;
    CHECK(NvAPI_CaptureFeedbackDevice(&wrapper, &original));
    CHECK(original.device == &native); // The publication names the NATIVE device, not the pointer it came in by.
    CHECK(native.references == 2);     // Its one reference is on the native.
    CHECK(wrapper.add_refs == 0);      // The incoming pointer was never AddRef'd.
    const unsigned resets = s_lifecycle_resets;
    g_game_requested_interval.store(1234);
    for (IUnknown* candidate : { static_cast<IUnknown*>(&native),
             static_cast<IUnknown*>(&alias), static_cast<IUnknown*>(&wrapper) }) {
        NvAPIFeedbackDevice no_publication = original;
        CHECK(!NvAPI_CaptureFeedbackDevice(candidate, &no_publication));
        CHECK(!no_publication.device); // No ownership token granted for a no-op.
        const NvAPIFeedbackDevice current = NvAPI_AcquireFeedbackDevice();
        CHECK(NvAPI_IsSameFeedbackPublication(original, current));
        CHECK(current.device == &native); // Original publication and token retained.
        NvAPI_ReleaseFeedbackDevice(current.device);
        CHECK(NvAPI_IsFeedbackDeviceCurrent(original));
        CHECK(g_game_requested_interval.load() == 1234);
        CHECK(s_lifecycle_resets == resets);
    }
    CHECK(native.references == 2 && alias.references == 1);
    CHECK(wrapper.add_refs == 0 && wrapper.releases == 0);
    NvAPIFeedbackDevice stale_token = original;
    stale_token.publication_sequence += 2;
    NvAPI_ClearFeedbackDevice(stale_token); // A token of another publication is not authority.
    CHECK(NvAPI_IsFeedbackDeviceCurrent(original));

    NvAPIFeedbackDevice replacement;
    CHECK(NvAPI_CaptureFeedbackDevice(&unrelated, &replacement));
    CHECK(!NvAPI_IsFeedbackDeviceCurrent(original));
    CHECK(replacement.producer_epoch != original.producer_epoch);
    CHECK(replacement.publication_sequence == original.publication_sequence + 2);
    CHECK(g_game_requested_interval.load() == 0);
    CHECK(s_lifecycle_resets == resets + 1);
    CHECK(native.references == 1);
    NvAPI_ClearFeedbackDevice(original);
    CHECK(NvAPI_IsFeedbackDeviceCurrent(replacement));
    NvAPI_ClearFeedbackDevice(replacement);
    CHECK(!NvAPI_AcquireFeedbackDevice().device);

    // Also preserve a native-first publication when the wrapper arrives later.
    CHECK(NvAPI_CaptureFeedbackDevice(&native, &original));
    CHECK(!NvAPI_CaptureFeedbackDevice(&wrapper));
    CHECK(NvAPI_IsFeedbackDeviceCurrent(original));
    // The destroy event names the device by the canonical identity of its native object (as on_destroy_device).
    NvAPI_OnDeviceDestroyed(DeviceIdentity_OfNative(reinterpret_cast<uint64_t>(static_cast<IUnknown*>(&native))));
    CHECK(!NvAPI_IsFeedbackDeviceCurrent(original) && !NvAPI_HasFeedbackDevice());
    CHECK(native.references == 1);
}

// The identity comparison made while the incoming pointer's unwrap query runs is outside the writer section: a
// republication during it (the incoming pointer is a probe whose query clears and republishes) must be revalidated.
void TestReentrantRevalidation() {
    FakeDevice native, probe, unrelated, probe_unrelated;
    probe.Wrap(&native);
    probe_unrelated.Wrap(&unrelated);
    CHECK(NvAPI_CaptureFeedbackDevice(&native, &s_reentrant_expected));
    const auto original = s_reentrant_expected;
    // Clear and republish the SAME device during the unwrap. Exact transition and token revalidation must reject the
    // old snapshot, despite equivalence.
    s_reentrant_device = &native;
    probe.on_unwrap = RepublishDuringQuery;
    CHECK(!NvAPI_CaptureFeedbackDevice(&probe));
    CHECK(!NvAPI_IsFeedbackDeviceCurrent(original));
    CHECK(NvAPI_IsFeedbackDeviceCurrent(s_reentrant_publication));
    CHECK(s_reentrant_publication.publication_sequence == original.publication_sequence + 4);
    NvAPI_ClearFeedbackDevice(original);
    CHECK(NvAPI_IsFeedbackDeviceCurrent(s_reentrant_publication));

    // The same stale comparison must not suppress replacement of an unrelated publication that appeared while the
    // unwrap was running.
    s_reentrant_expected = s_reentrant_publication;
    s_reentrant_device = &unrelated;
    probe.on_unwrap = RepublishDuringQuery;
    NvAPIFeedbackDevice final_publication;
    CHECK(NvAPI_CaptureFeedbackDevice(&probe, &final_publication));
    CHECK(final_publication.device == &native);
    CHECK(!NvAPI_IsFeedbackDeviceCurrent(s_reentrant_publication));
    CHECK(NvAPI_IsFeedbackDeviceCurrent(final_publication));
    NvAPI_ClearFeedbackDevice(final_publication);

    // An UNEQUAL comparison also becomes stale: the callback has already published our candidate. Revalidate under
    // writer exclusion rather than overwrite it, reset again, or grant its token to this no-op capture.
    CHECK(NvAPI_CaptureFeedbackDevice(&native, &s_reentrant_expected));
    const auto before_unequal_query = s_reentrant_expected;
    s_reentrant_device = &unrelated;
    probe_unrelated.on_unwrap = RepublishDuringQuery;
    NvAPIFeedbackDevice no_publication;
    CHECK(!NvAPI_CaptureFeedbackDevice(&probe_unrelated, &no_publication));
    CHECK(!no_publication.device);
    CHECK(NvAPI_IsFeedbackDeviceCurrent(s_reentrant_publication));
    CHECK(s_reentrant_publication.publication_sequence ==
        before_unequal_query.publication_sequence + 4);
    NvAPI_ClearFeedbackDevice(s_reentrant_publication);
    s_reentrant_device = nullptr;
}

// While a device is published, the publication owns a reference on its native object, so the object cannot be freed
// and its address reused; after the clear the address can be reused, and the old token and an unrelated device's
// destroy event must not touch the new publication.
void TestRealAddressReuse() {
    alignas(FakeDevice) unsigned char storage[sizeof(FakeDevice)];
    FakeDevice* first = new (storage) FakeDevice;
    NvAPIFeedbackDevice stale;
    CHECK(NvAPI_CaptureFeedbackDevice(first, &stale));
    CHECK(first->references == 2); // Held by the publication: not freeable while published.
    NvAPI_ClearFeedbackDevice(stale);
    first->~FakeDevice();
    FakeDevice* reused = new (storage) FakeDevice;
    NvAPIFeedbackDevice current;
    CHECK(NvAPI_CaptureFeedbackDevice(reused, &current));
    CHECK(stale.device == current.device);
    CHECK(stale.producer_epoch != current.producer_epoch);
    CHECK(!NvAPI_IsSameFeedbackPublication(stale, current));
    NvAPI_ClearFeedbackDevice(stale);
    CHECK(NvAPI_IsFeedbackDeviceCurrent(current));
    FakeDevice other;
    NvAPI_OnDeviceDestroyed(DeviceIdentity_OfNative(reinterpret_cast<uint64_t>(static_cast<IUnknown*>(&other))));
    CHECK(NvAPI_IsFeedbackDeviceCurrent(current));
    NvAPI_ClearFeedbackDevice(current);
    reused->~FakeDevice();
}

NvAPI_Status __cdecl ForwardMarker(IUnknown* device, NV_LATENCY_MARKER_PARAMS* params) {
    s_last_driver_device = device;
    ++s_forwarded_markers;
    if (s_record_call_order) s_call_order.push_back({'F', params->frameID});
    return 77;
}

void SendMarker(IUnknown* device, uint64_t id, NV_LATENCY_MARKER_TYPE type) {
    NV_LATENCY_MARKER_PARAMS params{};
    params.frameID = id;
    params.markerType = type;
    CHECK(Hook_SetLatencyMarker(device, &params) == 77);
}

void SetPresenting(IUnknown* device, uint64_t sequence, ActiveAPI api) {
    s_test_presenting = device;
    s_test_presenting_sequence = sequence;
    s_test_api = api;
}
void ClearPresenting() { SetPresenting(nullptr, 0, ActiveAPI::None); }
IUnknown* Canonical(FakeNative12& native) { return static_cast<IUnknown*>(&native); }

// ResolveIncomingDevice, the one look every hook takes at an incoming device pointer: the game's pointer (the
// ReShade proxy, recorded as the publication's alias) and the native device are answered from values, with no call
// on any object; any other pointer is unwrapped once to its native device. The proxy fixture is in strict mode: any
// AddRef/Release/own-interface query on it fails the test.
void TestResolveIncomingDevice() {
    FakeReShadeLock lock;
    lock.strict.store(true);
    FakeNative12 native, other_native;
    FakeProxy12 proxy(&native, &lock), other_proxy(&other_native, &lock);
    native.proxy = &proxy;
    other_native.proxy = &other_proxy;
    IUnknown* const canonical = Canonical(native);
    FakeDevice wrapper;   // a foreign wrapper above the native device (it answers the unwrap query)
    wrapper.Wrap(&native);
    FakeDevice swallower; // a wrapper that swallows the unwrap query: terminal, its own identity
    FakeDevice aggregate; // swallows the unwrap query but answers IUnknown with the native's identity
    aggregate.canonical = canonical;
    const auto traffic = [&] { return native.queries + native.add_refs + proxy.unwrap_queries + wrapper.queries; };

    NvAPIFeedbackDevice published;
    CHECK(NvAPI_CaptureFeedbackDevice(&proxy, &published));
    CHECK(published.device == canonical);
    SetPresenting(canonical, 2, ActiveAPI::DX12);

    // The game's pointer and the native itself: values only, no COM call of any kind.
    for (IUnknown* incoming : { static_cast<IUnknown*>(&proxy), canonical }) {
        const unsigned before = traffic();
        for (int i = 0; i < 5; ++i) {
            IncomingDevice resolved;
            ResolveIncomingDevice(incoming, nullptr, &resolved);
            CHECK(resolved.presenting && !resolved.capture);
            CHECK(NvAPI_IsSameFeedbackPublication(published, resolved.identity));
            CHECK(FeedbackIsPresenting(resolved.identity));
        }
        CHECK(traffic() == before);
    }

    // A foreign wrapper is never the alias: unwrapped on every call, with no reference taken on the wrapper.
    for (int i = 0; i < 3; ++i) {
        const unsigned before = wrapper.queries;
        IncomingDevice resolved;
        ResolveIncomingDevice(&wrapper, nullptr, &resolved);
        CHECK(resolved.presenting && resolved.identity.device == canonical && !resolved.capture);
        CHECK(wrapper.queries > before);
        CHECK(wrapper.add_refs == 0 && wrapper.releases == 0);
    }
    // A wrapper that swallows the unwrap query is its own identity, never the presenting device's: not captured.
    {
        IncomingDevice resolved;
        ResolveIncomingDevice(&swallower, nullptr, &resolved);
        CHECK(!resolved.presenting && !resolved.capture && !resolved.identity.device);
    }
    // ... unless its canonical identity IS the presenting device's: it names the published native device.
    {
        IncomingDevice resolved;
        ResolveIncomingDevice(&aggregate, nullptr, &resolved);
        CHECK(resolved.presenting && resolved.identity.device == canonical && !resolved.capture);
    }

    // Another presenting device while this one is published: not presenting, nothing handed over.
    SetPresenting(Canonical(other_native), 2, ActiveAPI::DX12);
    {
        IncomingDevice resolved;
        ResolveIncomingDevice(&proxy, nullptr, &resolved);
        CHECK(!resolved.presenting && !resolved.identity.device && !resolved.capture);
        CHECK(!FeedbackIsPresenting(published));
    }
    // The presenting device, not published yet: handed over as an owned native, after one unwrap.
    {
        const unsigned before = other_proxy.unwrap_queries;
        IncomingDevice resolved;
        ResolveIncomingDevice(&other_proxy, nullptr, &resolved);
        CHECK(resolved.presenting && resolved.capture == Canonical(other_native) && !resolved.identity.device);
        CHECK(other_proxy.unwrap_queries == before + 1);
        CHECK(other_native.references == 2);
    }
    CHECK(other_native.references == 1); // the destructor released the unconsumed hand-over
    // Captured: published by its native identity, the proxy recorded as the alias, then answered from values.
    {
        IncomingDevice resolved;
        ResolveIncomingDevice(&other_proxy, nullptr, &resolved);
        CHECK(CaptureResolvedDevice(&other_proxy, &resolved, nullptr));
        CHECK(resolved.identity.device == Canonical(other_native));
        CHECK(!NvAPI_IsFeedbackDeviceCurrent(published));
        const unsigned before = other_proxy.unwrap_queries + other_native.queries + other_native.add_refs;
        IncomingDevice again;
        ResolveIncomingDevice(&other_proxy, nullptr, &again);
        CHECK(again.presenting && !again.capture && again.identity.device == resolved.identity.device);
        CHECK(other_proxy.unwrap_queries + other_native.queries + other_native.add_refs == before);
    }
    CHECK(native.references == 1);

    // A presenting publication in progress (odd sequence): unavailable; with an API active, rejected.
    SetPresenting(Canonical(other_native), 3, ActiveAPI::DX12);
    {
        IncomingDevice resolved;
        ResolveIncomingDevice(&other_proxy, nullptr, &resolved);
        CHECK(!resolved.presenting && !resolved.capture && !resolved.identity.device);
    }
    // Not a DXGI API: rejected.
    SetPresenting(Canonical(other_native), 4, ActiveAPI::Vulkan);
    {
        IncomingDevice resolved;
        ResolveIncomingDevice(&other_proxy, nullptr, &resolved);
        CHECK(!resolved.presenting && !resolved.capture && !resolved.identity.device);
    }
    // No presenter and no API yet: native calls bootstrap feedback (the published device and an unpublished one).
    ClearPresenting();
    {
        IncomingDevice resolved;
        ResolveIncomingDevice(&other_proxy, nullptr, &resolved);
        CHECK(resolved.presenting && !resolved.capture && resolved.identity.device == Canonical(other_native));
    }
    NvAPI_ClearFeedbackDevice();
    CHECK(!NvAPI_HasFeedbackDevice());
    {
        IncomingDevice resolved;
        ResolveIncomingDevice(&proxy, nullptr, &resolved);
        CHECK(resolved.presenting && resolved.capture == canonical && !resolved.identity.device);
    }
    // The sleep-status observer: the published device's token from values, no presenting requirement.
    CHECK(NvAPI_CaptureFeedbackDevice(&proxy, &published));
    {
        const unsigned before = traffic();
        NvAPIFeedbackDevice token;
        CHECK(ResolvePublishedToken(&proxy, &token) && NvAPI_IsSameFeedbackPublication(published, token));
        CHECK(traffic() == before);
        CHECK(!ResolvePublishedToken(&other_proxy, &token)); // another device: unwrapped once, not the published one
    }
    NvAPI_ClearFeedbackDevice();
    CHECK(native.references == 1 && other_native.references == 1);
    CHECK(lock.proxy_calls == 0 && proxy.refs == 1 && other_proxy.refs == 1);
    ClearPresenting();
}

// The launch hang. ReShade runs destroy_device (and add-on unload) while holding its process-wide, non-recursive
// device lock, and that lock is also taken by every AddRef/Release of a device proxy: a callback that touches the
// proxy of the device being destroyed waits for itself. Two devices A (published) and B (unrelated) with a fake
// process-wide lock held for the whole event, proxies in strict mode: the production destroy path (the canonical
// identity of the destroyed native, NvAPI_OnDeviceDestroyed) makes no proxy call, leaves an unrelated device's
// publication alone and clears the published one's. Not covered here: the glue in dllmain.cpp that calls these two
// functions (it is not linked), and the selection side (swapchain_selection_policy).
void TestReShadeLockCycle() {
    for (const bool proxy_first : { true, false }) {
        FakeReShadeLock lock;
        FakeNative12 native_a, native_b;
        FakeProxy12 proxy_a(&native_a, &lock), proxy_b(&native_b, &lock);
        native_a.proxy = &proxy_a;
        native_b.proxy = &proxy_b;
        lock.strict.store(true);
        NvAPIFeedbackDevice token;
        CHECK(NvAPI_CaptureFeedbackDevice(
            proxy_first ? static_cast<IUnknown*>(&proxy_a) : Canonical(native_a), &token));
        CHECK(token.device == Canonical(native_a));
        CHECK(native_a.references == 2 && proxy_a.refs == 1); // the publication's reference is on the native
        {
            HeldLock held(lock); // the whole event runs inside ReShade's lock
            // B: a second adapter's device, a launcher's, a probe's. Unrelated: nothing is cleared.
            NvAPI_OnDeviceDestroyed(DeviceIdentity_OfNative(reinterpret_cast<uint64_t>(Canonical(native_b))));
            CHECK(NvAPI_IsFeedbackDeviceCurrent(token) && NvAPI_HasFeedbackDevice());
            CHECK(native_a.references == 2);
            // A's device: the publication is cleared and its reference released, on the native.
            const uint64_t identity_a = DeviceIdentity_OfNative(reinterpret_cast<uint64_t>(Canonical(native_a)));
            CHECK(identity_a == reinterpret_cast<uint64_t>(token.device));
            NvAPI_OnDeviceDestroyed(identity_a);
            CHECK(!NvAPI_IsFeedbackDeviceCurrent(token) && !NvAPI_HasFeedbackDevice());
            CHECK(native_a.references == 1);
            NvAPI_OnDeviceDestroyed(identity_a);   // a second event, and an empty identity: nothing
            NvAPI_OnDeviceDestroyed(0);
        }
        CHECK(lock.proxy_calls == 0 && proxy_a.refs == 1 && proxy_b.refs == 1);
    }
}

// A reader parked between loading the publication and taking its reference (inside the native's AddRef) while the
// related device is destroyed under the fake lock: the callback returns (it never waits for the reader), the
// publication's reference is retired, and the reader's own drain releases it exactly once.
void TestReaderInFlight() {
    FakeReShadeLock lock;
    FakeNative12 native;
    FakeProxy12 proxy(&native, &lock);
    native.proxy = &proxy;
    NvAPIFeedbackDevice token;
    CHECK(NvAPI_CaptureFeedbackDevice(&proxy, &token));
    native.park_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    native.park_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK(native.park_entered && native.park_release);
    NvAPIFeedbackDevice reader_result;
    reader_result.device = &proxy; // overwritten by the reader
    std::thread reader([&] {
        native.park_thread.store(GetCurrentThreadId());
        reader_result = NvAPI_AcquireFeedbackDevice();
    });
    CHECK(WaitForSingleObject(native.park_entered, 10000) == WAIT_OBJECT_0);
    lock.strict.store(true);
    {
        HeldLock held(lock);
        NvAPI_OnDeviceDestroyed(DeviceIdentity_OfNative(reinterpret_cast<uint64_t>(Canonical(native))));
    }
    CHECK(!NvAPI_HasFeedbackDevice() && !NvAPI_IsFeedbackDeviceCurrent(token));
    CHECK(native.references == 2); // retired, not released: the reader is inside its window
    SetEvent(native.park_release);
    reader.join();
    CHECK(!reader_result.device);  // the reader never got the cleared publication
    CHECK(native.references == 1); // released exactly once, by the next drain
    CHECK(lock.proxy_calls == 0 && proxy.refs == 1);
    CloseHandle(native.park_entered);
    CloseHandle(native.park_release);
    native.park_entered = native.park_release = nullptr;
}

// Add-on unload can run at game exit under the same lock (destroy_device, then unload_addons). The unload path's
// reference release (NvAPI_ClearFeedbackDevice) and the restore of the game's sleep mode make no proxy call, and the
// restore's driver call is ReLimiter's own: it receives the native device.
void TestUnloadUnderLock() {
    FakeReShadeLock lock;
    FakeNative12 native;
    FakeProxy12 proxy(&native, &lock);
    native.proxy = &proxy;
    const auto old_mode = s_orig_sleep_mode;
    s_orig_sleep_mode = FakeSleepMode;
    s_expected_mode_params = nullptr;
    lock.strict.store(true);
    CHECK(NvAPI_CaptureFeedbackDevice(&proxy));
    SetPresenting(Canonical(native), 2, ActiveAPI::DX12);
    NV_SET_SLEEP_MODE_PARAMS params{};
    params.version = NV_SLEEP_PARAMS_VER;
    params.minimumIntervalUs = 2222;
    CHECK(Hook_SetSleepMode(&proxy, &params) == -9);
    CHECK(s_last_driver_device == &proxy); // the game's own call keeps the game's pointer
    {
        HeldLock held(lock);
        s_last_driver_device = nullptr;
        RestoreGameSleepMode();
        CHECK(s_last_driver_device == Canonical(native));
        NvAPI_ClearFeedbackDevice();
    }
    CHECK(!NvAPI_HasFeedbackDevice());
    CHECK(native.references == 1 && proxy.refs == 1 && lock.proxy_calls == 0);
    s_orig_sleep_mode = old_mode;
    ClearPresenting();
}

// The hooks' common case on the game's own pointer (the proxy), in strict mode: SetSleepMode, Sleep and a latency
// marker make no COM call on anything, the driver receives the game's pointer for the game's call, and the
// sleep-status observer's lookup is a value read too.
void TestHotPathTouchesNoProxy() {
    FakeReShadeLock lock;
    FakeNative12 native;
    FakeProxy12 proxy(&native, &lock);
    native.proxy = &proxy;
    const auto old_sleep = s_orig_sleep;
    const auto old_mode = s_orig_sleep_mode;
    const auto old_marker = s_orig_set_latency_marker;
    s_orig_sleep = FakeDriverSleep;
    s_orig_sleep_mode = FakeSleepMode;
    s_orig_set_latency_marker = ForwardMarker;
    s_expected_mode_params = nullptr;
    lock.strict.store(true);
    CHECK(NvAPI_CaptureFeedbackDevice(&proxy));
    SetPresenting(Canonical(native), 2, ActiveAPI::DX12);
    const auto traffic = [&] { return native.queries + native.add_refs + native.releases + proxy.unwrap_queries; };
    NV_SET_SLEEP_MODE_PARAMS params{};
    params.version = NV_SLEEP_PARAMS_VER;
    params.minimumIntervalUs = 4321;
    for (int round = 0; round < 3; ++round) {
        const unsigned before = traffic();
        s_last_driver_device = nullptr;
        CHECK(Hook_SetSleepMode(&proxy, &params) == -9);
        CHECK(s_last_driver_device == &proxy);
        s_last_driver_device = nullptr;
        CHECK(Hook_Sleep(&proxy) == -7);
        CHECK(s_last_driver_device == &proxy);
        s_last_driver_device = nullptr;
        SendMarker(&proxy, 700 + round, SIMULATION_START);
        CHECK(s_last_driver_device == &proxy);
        NvAPIFeedbackDevice token;
        CHECK(ResolvePublishedToken(&proxy, &token));
        CHECK(traffic() == before); // not one query, AddRef or Release on anything
    }
    CHECK(lock.proxy_calls == 0 && proxy.refs == 1);
    // ReLimiter's own driver call (restore) passes the native device.
    s_last_driver_device = nullptr;
    RestoreGameSleepMode();
    CHECK(s_last_driver_device == Canonical(native));
    NvAPI_ClearFeedbackDevice();
    CHECK(native.references == 1 && lock.proxy_calls == 0);
    s_orig_sleep = old_sleep;
    s_orig_sleep_mode = old_mode;
    s_orig_set_latency_marker = old_marker;
    ClearPresenting();
}

void TestDiagnosticAdmission() {
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    s_orig_set_latency_marker = ForwardMarker;
    s_tail_markers = 0;
    const unsigned forwarded_before = s_forwarded_markers;
    LARGE_INTEGER before{}, after{};
    QueryPerformanceCounter(&before);
    {
        SchedulerOwnerGuard owner;
        CHECK(owner);
        std::thread tail([&] { SendMarker(&device, 99, RENDERSUBMIT_END); });
        tail.join();
        CHECK(s_tail_markers == 0);
    }
    QueryPerformanceCounter(&after);
    // A free owner is also never claimed by a tail callback: marker-log
    // mutation is deferred even when there happened to be no contention.
    SendMarker(&device, 99, PRESENT_END);
    CHECK(s_tail_markers == 0);
    SendMarker(&device, 100, SIMULATION_START);
    CHECK(s_tail_markers == 2);
    CHECK(s_first_tail_timestamp >= before.QuadPart && s_first_tail_timestamp <= after.QuadPart);
    CHECK(s_last_tail_timestamp >= before.QuadPart);
    CHECK(s_forwarded_markers - forwarded_before == 3);

    // Controller/producer changes cannot apply diagnostic history to a new
    // lifetime. Native forwarding remains exactly once.
    SendMarker(&device, 100, RENDERSUBMIT_END);
    FrameTimeline_BeginControllerGeneration();
    SendMarker(&device, 101, SIMULATION_START);
    CHECK(s_tail_markers == 2);
    SendMarker(&device, 101, PRESENT_END);
    NvAPI_ClearFeedbackDevice();
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    SendMarker(&device, 102, SIMULATION_START);
    CHECK(s_tail_markers == 2);

    const uint64_t drops_before = s_deferred_markers.DroppedValues();
    for (size_t i = 0; i < kDeferredMarkerCapacity + 5; ++i)
        SendMarker(&device, 102, RENDERSUBMIT_END);
    CHECK(s_deferred_markers.DroppedValues() == drops_before + 5);
    SendMarker(&device, 103, SIMULATION_START);
    CHECK(s_tail_markers == 2 + kDeferredMarkerCapacity);
    NvAPI_ClearFeedbackDevice();
}

// g_cpu_latency_us: SIMULATION_START -> RENDERSUBMIT_END of the same frame,
// EMA alpha 0.16, computed on the owner when the deferred tail drains.
void TestCpuLatency() {
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    g_cpu_latency_us.store(0.0);
    SendMarker(&device, 600, SIMULATION_START);
    Sleep(2);
    SendMarker(&device, 600, RENDERSUBMIT_END);
    CHECK(g_cpu_latency_us.load() == 0.0); // deferred until the owner drains
    SendMarker(&device, 601, SIMULATION_START);
    const double first = g_cpu_latency_us.load();
    CHECK(first > 0.0 && first < 500000.0);
    // The next same-frame sample moves the EMA by alpha toward the sample.
    SendMarker(&device, 601, RENDERSUBMIT_END);
    SendMarker(&device, 602, SIMULATION_START);
    const double sample = qpc_to_us(s_last_render_submit_end_qpc - s_simulation_start_qpc[601 % 8]);
    const double smoothed = g_cpu_latency_us.load();
    CHECK(std::abs(smoothed - (first + 0.16 * (sample - first))) < 1e-6);
    // A RENDERSUBMIT_END without a same-frame SIMULATION_START is ignored.
    SendMarker(&device, 700, RENDERSUBMIT_END);
    SendMarker(&device, 603, SIMULATION_START);
    CHECK(g_cpu_latency_us.load() == smoothed);
    NvAPI_ClearFeedbackDevice();
}

// The enforcement marker is forwarded on entry and the wait is published as controller
// residence. Every marker, enforced or not, reaches the driver exactly once and returns the
// driver's status.
void TestMarkerForwardOrder() {
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    s_orig_set_latency_marker = ForwardMarker;
    s_record_call_order = true;
    s_on_marker_sleep_ms = 2;
    using Order = std::vector<std::pair<char, uint64_t>>;
    const uint64_t frame = 1101;
    const unsigned residences_before = s_residence_publications;

    s_call_order.clear();
    SendMarker(&device, frame, SIMULATION_START);
    CHECK(s_call_order == (Order{{'F', frame}, {'O', frame}}));
    CHECK(s_residence_publications == residences_before + 1);
    CHECK(s_last_residence_us > 0.0);

    // A repeated source is never paced twice but is still forwarded once.
    s_call_order.clear();
    SendMarker(&device, frame, SIMULATION_START);
    CHECK(s_call_order == (Order{{'F', frame}}));
    // Non-enforcement immediate and tail markers: one forward, no wait.
    s_call_order.clear();
    SendMarker(&device, frame, RENDERSUBMIT_START);
    SendMarker(&device, frame, RENDERSUBMIT_END);
    CHECK(s_call_order == (Order{{'F', frame}, {'F', frame}}));
    CHECK(s_residence_publications == residences_before + 1);
    s_on_marker_sleep_ms = 0;
    s_record_call_order = false;
    NvAPI_ClearFeedbackDevice();
}

#ifdef _WIN64
// Markers are forwarded first, with the key on or off (nothing waits in the hook), and the
// enforcement marker's wait is still published as controller residence.
void TestUnifiedMarkerForward() {
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    const PacingPipelineConfig old_config = CurrentConfig();
    s_orig_set_latency_marker = ForwardMarker;
    s_record_call_order = true;
    s_on_marker_sleep_ms = 2;
    using Order = std::vector<std::pair<char, uint64_t>>;

    s_call_order.clear();
    SendMarker(&device, 1301, SIMULATION_START);
    CHECK(s_call_order == (Order{{'F', 1301}, {'O', 1301}})); // key off
    CHECK(s_last_residence_us > 0.0);

    SetUnifiedKey(true);
    s_call_order.clear();
    SendMarker(&device, 1302, SIMULATION_START);
    CHECK(s_call_order == (Order{{'F', 1302}, {'O', 1302}})); // key on: forward first
    CHECK(s_last_residence_us > 0.0);
    s_call_order.clear();
    SendMarker(&device, 1302, RENDERSUBMIT_START);
    SendMarker(&device, 1302, RENDERSUBMIT_END);
    CHECK(s_call_order == (Order{{'F', 1302}, {'F', 1302}}));

    SetUnifiedKey(false);
    s_call_order.clear();
    SendMarker(&device, 1303, SIMULATION_START);
    CHECK(s_call_order == (Order{{'F', 1303}, {'O', 1303}})); // key off again

    PacingPipeline_SetConfig(old_config);
    // The key was published on and off without a reconcile (no game parameters): settled.
    CHECK(!NvAPI_SleepModeReconcilePending());
    s_on_marker_sleep_ms = 0;
    s_record_call_order = false;
    NvAPI_ClearFeedbackDevice();
}
#endif

void TestSourceAdmissionWithBusyTail() {
    // Immutable source-ID pattern from a captured trace:
    // 8768..13418 with 40 SimStart calls that collided with a busy owner.
    // Replays admission only, not frame pacing.
    constexpr uint64_t first = 8768, last = 13418;
    constexpr uint64_t missed[] = {8809,8969,9079,9194,9449,9605,9607,9634,
        9797,9929,10309,10436,10520,10584,10594,10618,10662,10686,10749,10770,
        10804,10806,10940,10977,11002,11032,11286,11292,11320,11686,11959,
        12247,12461,12579,12649,12830,12899,12932,13134,13392};
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    size_t next_missing = 0;
    s_enforced_ids.clear();
    s_replay_enforcement = true;
    for (uint64_t id = first; id <= last; ++id) {
        if (next_missing < std::size(missed) && id == missed[next_missing]) {
            ++next_missing;
            // Tail publication while the owner is busy must forward without
            // losing its observation or claiming that owner.
            SchedulerOwnerGuard owner;
            CHECK(owner);
            std::thread tail([&] { SendMarker(&device, id - 1, RENDERSUBMIT_END); });
            tail.join();
        }
        SendMarker(&device, id, SIMULATION_START);
    }
    s_replay_enforcement = false;
    CHECK(next_missing == 40);
    CHECK(s_enforced_ids.size() == last - first + 1);
    NvAPI_ClearFeedbackDevice();
}

// ── CPU output executor harness ──

PacingPlanKey Key(uint64_t id) {
    return {PacingIdentityDomain::NvApiFrame,
        FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame),
        FrameTimeline_CurrentControllerGeneration(), id};
}

int64_t RealQpc() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

int64_t RealFrequency() {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart;
}

void PublishPipeline(PacingTimingMode mode, PacingRung rung) {
    PacingPipelineSnapshot snapshot{};
    snapshot.mode = mode;
    snapshot.route = mode == PacingTimingMode::Off ? PacingRoute::None : PacingRoute::NvApiMarkers;
    snapshot.placement = mode == PacingTimingMode::Off ? PacingSourcePlacement::None : PacingSourcePlacement::Marker;
    snapshot.rung = rung;
    snapshot.reason = mode == PacingTimingMode::Off ? PacingFallbackReason::Passthrough : PacingFallbackReason::None;
    PacingPipeline_Publish(snapshot);
}

// Plans `key` on a fresh grid (index 1, so no spacing floor) with its output
// target lead_us after now and publishes grid and plan as the source stage
// does before its wait.
PacingFramePlan PublishPlan(const PacingPlanKey& key, PacingRung rung, double lead_us,
                            double interval_us = 10'000.0) {
    static CadenceGrid grid;
    grid = CadenceGrid{};
    CadenceGridPlanInput input{};
    input.now_qpc = RealQpc();
    input.qpc_frequency = RealFrequency();
    input.interval_us = interval_us;
    input.lead_us = lead_us;
    input.mode = PacingTimingMode::Vrr;
    input.controller_generation = key.controller_generation;
    const CadenceGridPlan grid_plan = grid.PlanNext(input);
    CHECK(grid_plan.valid && grid_plan.grid_index == 1);
    g_cadence_grid_publication.Publish(grid.Snapshot());
    PacingFramePlan plan{};
    plan.key = key;
    plan.grid_index = grid_plan.grid_index;
    plan.grid_epoch = grid_plan.epoch;
    plan.output_target_qpc = grid_plan.output_target_qpc;
    plan.source_wake_qpc = grid_plan.source_wake_qpc;
    plan.lead_us = lead_us;
    plan.interval_us = interval_us;
    plan.rung = rung;
    plan.mode = PacingTimingMode::Vrr;
    plan.marker = PacingSourceMarker::SimulationStart;
    CHECK(g_frame_plans.Publish(plan));
    return plan;
}

struct StageEvents {
    unsigned marker_arrivals = 0;
    unsigned output_releases = 0;
    unsigned other = 0;
    PacingStageEvent arrival{};
    PacingStageEvent release{};
};

StageEvents DrainStageEvents() {
    StageEvents events;
    PacingStageEvent event{};
    while (FramePlan_PopStageEvent(&event)) {
        if (event.kind == PacingStageEventKind::MarkerArrival) {
            ++events.marker_arrivals;
            events.arrival = event;
        } else if (event.kind == PacingStageEventKind::OutputRelease) {
            ++events.output_releases;
            events.release = event;
        } else {
            ++events.other;
        }
    }
    return events;
}

// PRESENT_START through the production detour on this thread. The gate's
// clock restarts at the real QPC and then only moves with HWSpin.
PacingPresentAttribution SendPresentStart(IUnknown* device, uint64_t id) {
    s_gate_qpc = 0;
    SendMarker(device, id, PRESENT_START);
    return t_pacing_present_attribution;
}

uint8_t Status(PacingPlanStatus status) { return static_cast<uint8_t>(status); }

int64_t TrackedOutput(const PacingFramePlan& plan) {
    int64_t output_qpc = 0;
    CHECK(g_cadence_output_tracker.Find(plan.key.controller_generation, plan.grid_index, &output_qpc));
    return output_qpc;
}

void TestPresentStartExecutors() {
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    DrainStageEvents();

    // No plan: Missing while the pipeline plans frames, Passthrough when Off.
    PublishPipeline(PacingTimingMode::Vrr, PacingRung::MarkerHold);
    auto attribution = SendPresentStart(&device, 800);
    CHECK(attribution.plan_status == Status(PacingPlanStatus::Missing));
    CHECK(attribution.marker_output == PacingOutputKind::None);
    CHECK(DrainStageEvents().output_releases == 0);
    PublishPipeline(PacingTimingMode::Off, PacingRung::SourceOnly);
    attribution = SendPresentStart(&device, 801);
    CHECK(attribution.plan_status == Status(PacingPlanStatus::Passthrough));
    PublishPipeline(PacingTimingMode::Vrr, PacingRung::MarkerHold);

    // H0: the SourceOnly plan is claimed and released at the arrival a.
    const auto source_plan = PublishPlan(Key(802), PacingRung::SourceOnly, 5'000.0);
    attribution = SendPresentStart(&device, 802);
    auto events = DrainStageEvents();
    CHECK(attribution.plan_status == Status(PacingPlanStatus::SourceOnly));
    CHECK(attribution.marker_output == PacingOutputKind::None);
    CHECK(events.marker_arrivals == 1 && events.output_releases == 1 && events.other == 0);
    CHECK(events.arrival.qpc == attribution.marker_qpc && events.release.qpc == attribution.marker_qpc);
    CHECK(events.release.status == PacingPlanStatus::SourceOnly &&
        events.release.rung == PacingRung::SourceOnly && events.release.key == source_plan.key);
    CHECK(TrackedOutput(source_plan) == attribution.marker_qpc);
    // A second PRESENT_START for the same key cannot claim it again.
    attribution = SendPresentStart(&device, 802);
    events = DrainStageEvents();
    CHECK(attribution.plan_status == Status(PacingPlanStatus::Duplicate));
    CHECK(events.output_releases == 0);

    // H1 Held: hold to T, tracker record and one OutputRelease before return.
    const unsigned holds_before = s_gate_hold_publications;
    const auto held_plan = PublishPlan(Key(803), PacingRung::MarkerHold, 5'000.0);
    attribution = SendPresentStart(&device, 803);
    events = DrainStageEvents();
    CHECK(attribution.plan_status == Status(PacingPlanStatus::Held));
    CHECK(attribution.marker_output == PacingOutputKind::MarkerCpuHold);
    CHECK(attribution.marker_hold_target_qpc == held_plan.output_target_qpc);
    CHECK(attribution.marker_hold_release_qpc >= held_plan.output_target_qpc);
    CHECK(events.marker_arrivals == 1 && events.output_releases == 1);
    CHECK(events.arrival.qpc == attribution.marker_qpc &&
        events.arrival.qpc < attribution.marker_hold_release_qpc); // a is before the hold
    CHECK(events.release.status == PacingPlanStatus::Held &&
        events.release.rung == PacingRung::MarkerHold &&
        events.release.qpc == attribution.marker_hold_release_qpc);
    // Recorded before the hook returned, i.e. before the game can forward Present.
    CHECK(TrackedOutput(held_plan) == attribution.marker_hold_release_qpc);
    CHECK(s_gate_hold_publications == holds_before + 1);
    CHECK(g_last_gate_sleep_us.load() > 0.0);

    // Late: arrival at or after T; no hold, decision time recorded.
    const auto late_plan = PublishPlan(Key(804), PacingRung::MarkerHold, 0.0);
    Sleep(1);
    attribution = SendPresentStart(&device, 804);
    events = DrainStageEvents();
    CHECK(attribution.plan_status == Status(PacingPlanStatus::Late));
    CHECK(attribution.marker_output == PacingOutputKind::None);
    CHECK(events.output_releases == 1 && events.release.status == PacingPlanStatus::Late);
    CHECK(TrackedOutput(late_plan) == events.release.qpc &&
        events.release.qpc >= late_plan.output_target_qpc);

    // Undeliverable: T - now > 1.5 I is never held.
    const auto far_plan = PublishPlan(Key(805), PacingRung::MarkerHold, 40'000.0, 10'000.0);
    attribution = SendPresentStart(&device, 805);
    events = DrainStageEvents();
    CHECK(attribution.plan_status == Status(PacingPlanStatus::Undeliverable));
    CHECK(attribution.marker_output == PacingOutputKind::None);
    CHECK(events.output_releases == 1 && events.release.status == PacingPlanStatus::Undeliverable);
    CHECK(TrackedOutput(far_plan) == events.release.qpc &&
        events.release.qpc < far_plan.output_target_qpc);

    // Duplicate: the plan's output was claimed elsewhere; no hold, no output.
    const auto claimed_plan = PublishPlan(Key(806), PacingRung::MarkerHold, 5'000.0);
    PacingFramePlan claimed{};
    CHECK(g_frame_plans.ClaimOutput(claimed_plan.key, PacingRung::MarkerHold, &claimed) ==
        FramePlanClaim::Claimed);
    attribution = SendPresentStart(&device, 806);
    events = DrainStageEvents();
    CHECK(attribution.plan_status == Status(PacingPlanStatus::Duplicate));
    CHECK(attribution.marker_output == PacingOutputKind::None && events.output_releases == 0);

    // H3 plan: PRESENT_START reports the arrival only; OtherRung calls do
    // nothing and never consume the plan.
    {
        const auto later_plan = PublishPlan(Key(808), PacingRung::GpuQueueWait, 5'000.0);
        attribution = SendPresentStart(&device, 808);
        events = DrainStageEvents();
        CHECK(attribution.plan_status == Status(PacingPlanStatus::Unknown));
        CHECK(attribution.marker_output == PacingOutputKind::None);
        CHECK(events.marker_arrivals == 1 && events.output_releases == 0);
        PresentationGateResult other{};
        s_gate_qpc = 0;
        PresentGate_Execute(later_plan.key, PacingRung::MarkerHold, RealQpc(), &other);
        CHECK(!other.claimed && !other.held && other.status == PacingPlanStatus::Unknown);
        CHECK(DrainStageEvents().output_releases == 0);
    }

    // Decline of the executor (PresentGate_DeclineOutput): the GpuQueueWait plan is
    // claimed and reported Undeliverable at the decision time, never held.
    const auto declined_plan = PublishPlan(Key(809), PacingRung::GpuQueueWait, 5'000.0);
    PresentationGateResult declined{};
    PresentGate_DeclineOutput(declined_plan.key, PacingRung::MarkerHold, RealQpc(), &declined);
    CHECK(!declined.claimed && declined.status == PacingPlanStatus::Unknown); // OtherRung
    const int64_t decision_qpc = RealQpc();
    PresentGate_DeclineOutput(declined_plan.key, PacingRung::GpuQueueWait, decision_qpc, &declined);
    events = DrainStageEvents();
    CHECK(declined.claimed && !declined.held && declined.status == PacingPlanStatus::Undeliverable);
    CHECK(events.output_releases == 1 && events.release.qpc == decision_qpc &&
        events.release.status == PacingPlanStatus::Undeliverable);
    CHECK(TrackedOutput(declined_plan) == decision_qpc);
    PresentGate_DeclineOutput(declined_plan.key, PacingRung::GpuQueueWait, decision_qpc, &declined);
    CHECK(!declined.claimed && declined.status == PacingPlanStatus::Duplicate);
    CHECK(DrainStageEvents().output_releases == 0);
    NvAPI_ClearFeedbackDevice();
}

void TestEnforcementDuringGateAndCancellation() {
    // Real native hook + exact-key plan + actual H1 executor. Freeze the render
    // gate and invoke the next SimulationStart on this thread: the hold never
    // needs the owner, so source enforcement proceeds during it.
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    PublishPipeline(PacingTimingMode::Vrr, PacingRung::MarkerHold);
    s_gate_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s_gate_continue = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK(s_gate_entered && s_gate_continue);
    for (int mode = 0; mode < 3; ++mode) {
        ResetEvent(s_gate_entered);
        ResetEvent(s_gate_continue);
        s_test_stopping = false;
        DrainStageEvents();
        const uint64_t frame = 300 + static_cast<uint64_t>(mode) * 10;
        const auto plan = PublishPlan(Key(frame), PacingRung::MarkerHold, 10'000.0);
        s_pause_gate = true;
        const unsigned forwarded_before = s_forwarded_markers;
        const unsigned holds_before = s_gate_hold_publications;
        s_enforced_ids.clear();
        s_replay_enforcement = true;
        PacingPresentAttribution attribution{};
        std::thread render([&] {
            attribution = SendPresentStart(&device, frame); // the render thread's hold outcome
        });
        CHECK(WaitForSingleObject(s_gate_entered, 5000) == WAIT_OBJECT_0);
        SendMarker(&device, frame + 1, SIMULATION_START);
        CHECK(s_enforced_ids == std::vector<uint64_t>{frame + 1});
        CHECK(s_forwarded_markers == forwarded_before + 2);
        if (mode == 1) FrameTimeline_BeginControllerGeneration();
        if (mode == 2) s_test_stopping = true;
        SetEvent(s_gate_continue);
        render.join();
        s_replay_enforcement = false;
        const auto events = DrainStageEvents();
        CHECK(s_gate_hold_publications == holds_before + 1);
        // Held to T when current; a lifecycle change cancels it (Stale) before T.
        CHECK(attribution.key.domain == PacingIdentityDomain::NvApiFrame &&
            attribution.key.producer_epoch == plan.key.producer_epoch &&
            attribution.key.frame_id == frame);
        CHECK(attribution.marker_output == PacingOutputKind::MarkerCpuHold);
        CHECK(attribution.marker_hold_target_qpc == plan.output_target_qpc);
        CHECK((attribution.marker_hold_release_qpc >= plan.output_target_qpc) == (mode == 0));
        CHECK(attribution.plan_status ==
            Status(mode == 0 ? PacingPlanStatus::Held : PacingPlanStatus::Stale));
        CHECK(events.output_releases == 1 &&
            events.release.qpc == attribution.marker_hold_release_qpc);
    }
    s_test_stopping = false;
    CloseHandle(s_gate_entered);
    CloseHandle(s_gate_continue);
    s_gate_entered = s_gate_continue = nullptr;
    NvAPI_ClearFeedbackDevice();
}

void TestNativeRouteFromEveryMarker() {
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    NativeGate_SetMode(NativeGateMode::Marker);
    PublishPipeline(PacingTimingMode::Vrr, PacingRung::MarkerHold);
    s_native_swapchain_generation = FrameTimeline_RegisterPresentSwapchain(0x1000, PresentTimelineApi::D3D12);
    s_native_context_enabled = true;
    const unsigned publications_before = s_route_capability_publications;
    for (uint64_t frame = 2000; frame < 2034; ++frame) {
        // Source only, marker hold and GPU queue wait (the FG input wait is not enumerated) and
        // a frame with no plan at all feed the route.
        const uint64_t variant = frame % 4;
        if (variant != 3) {
            const PacingRung rung = variant == 0 ? PacingRung::SourceOnly :
                variant == 1 ? PacingRung::MarkerHold : PacingRung::GpuQueueWait;
            PublishPlan(Key(frame), rung, 2'000.0);
        }
        s_clock_map_frame_id = 0;
        s_clock_map_qpc = 0;
        const auto attribution = SendPresentStart(&device, frame);
        DrainStageEvents();
        // Same-thread attribution for the following native Present and one
        // clock-map sample read before forwarding.
        CHECK(attribution.key.domain == PacingIdentityDomain::NvApiFrame &&
            attribution.key.frame_id == frame && attribution.marker_qpc > 0);
        CHECK((attribution.marker_output == PacingOutputKind::MarkerCpuHold) == (variant == 1));
        CHECK(s_clock_map_frame_id == frame && s_clock_map_qpc > 0 &&
            s_clock_map_qpc <= attribution.marker_qpc);
        NativeGate_BeginScope(0x1000, s_native_swapchain_generation);
        {
            NativeGateCall call(NativeGate_GetContext(RealQpc()), 0);
            CHECK(call.trace.source_frame_id == frame);
            CHECK((call.trace.status == NativeGateStatus::Native) == (frame >= 2032));
            call.Complete(true, false);
        }
        NativeGate_EndScope(0x1000, s_native_swapchain_generation);
    }
    CHECK(NativeGate_IsRouteQualified());
    CHECK(s_route_capability_publications - publications_before == 34);
    s_native_context_enabled = false;
    NvAPI_ClearFeedbackDevice();
}

// RENDERSUBMIT_START, SIMULATION_END and PRESENT_END of a planned GPU wait
// frame are one stage event each at the plan's grid index (the carried GPU hold and the
// simulation → render queue; the lead decides per rung whether to use them).
// CPU rungs and unplanned frames push nothing; every marker is forwarded once.
void TestFrameMarkerStageEvents() {
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    s_orig_set_latency_marker = ForwardMarker;
    DrainStageEvents();
    // Every queued stage event, of any kind; *last receives the newest.
    const auto drain = [](PacingStageEvent* last) {
        unsigned count = 0;
        PacingStageEvent event{};
        while (FramePlan_PopStageEvent(&event)) {
            ++count;
            *last = event;
        }
        return count;
    };
    const unsigned forwarded_before = s_forwarded_markers;
    PacingStageEvent event{};
    const PacingFramePlan h3 = PublishPlan(Key(2100), PacingRung::GpuQueueWait, 2'000.0);
    SendMarker(&device, 2100, RENDERSUBMIT_START);
    CHECK(drain(&event) == 1 && event.kind == PacingStageEventKind::RenderSubmitStart);
    CHECK(event.key.frame_id == 2100 && event.grid_index == h3.grid_index &&
        event.rung == PacingRung::GpuQueueWait && event.qpc > 0);
    SendMarker(&device, 2100, SIMULATION_END);
    CHECK(drain(&event) == 1 && event.kind == PacingStageEventKind::SimulationEnd &&
        event.key.frame_id == 2100 && event.grid_index == h3.grid_index &&
        event.rung == PacingRung::GpuQueueWait && event.qpc > 0);
    SendMarker(&device, 2100, PRESENT_END);
    CHECK(drain(&event) == 1 && event.kind == PacingStageEventKind::PresentEnd &&
        event.key.frame_id == 2100 && event.grid_index == h3.grid_index && event.qpc > 0);
    const PacingFramePlan fg = PublishPlan(Key(2101), PacingRung::FgInputWait, 2'000.0);
    SendMarker(&device, 2101, RENDERSUBMIT_START);
    CHECK(drain(&event) == 1 && event.kind == PacingStageEventKind::RenderSubmitStart &&
        event.key.frame_id == 2101 && event.rung == PacingRung::FgInputWait);
    SendMarker(&device, 2101, SIMULATION_END);
    CHECK(drain(&event) == 1 && event.kind == PacingStageEventKind::SimulationEnd &&
        event.key.frame_id == 2101 && event.grid_index == fg.grid_index && event.qpc > 0);
    SendMarker(&device, 2101, PRESENT_END);
    CHECK(drain(&event) == 1 && event.kind == PacingStageEventKind::PresentEnd &&
        event.key.frame_id == 2101 && event.grid_index == fg.grid_index && event.qpc > 0);
    // A marker hold, then a frame without a plan: no event.
    PublishPlan(Key(2102), PacingRung::MarkerHold, 2'000.0);
    SendMarker(&device, 2102, RENDERSUBMIT_START);
    SendMarker(&device, 2102, SIMULATION_END);
    SendMarker(&device, 2102, PRESENT_END);
    CHECK(drain(&event) == 3 && event.kind == PacingStageEventKind::PresentEnd &&
        event.key.frame_id == 2102 && event.rung == PacingRung::MarkerHold);
    SendMarker(&device, 2103, RENDERSUBMIT_START);
    SendMarker(&device, 2103, SIMULATION_END);
    SendMarker(&device, 2103, PRESENT_END);
    CHECK(drain(&event) == 0);
    CHECK(s_forwarded_markers - forwarded_before == 12);
    NvAPI_ClearFeedbackDevice();
}

// RENDERSUBMIT_START of frame N comes from the render thread while the game thread starts frame
// N + 1 (its Sleep and SIMULATION_START need the scheduler owner). Once this producer lifetime has
// admitted a SIMULATION_START it never claims the owner: its marker-log entry is queued and arrives
// with the next owner, a busy owner loses nothing, and its stage event still reaches the lead. A
// game that sends no SIMULATION_START keeps the fallback: there it claims the owner and is enforced.
void TestRenderSubmitStartAdmission() {
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    s_orig_set_latency_marker = ForwardMarker;
    s_enforced_ids.clear();
    s_replay_enforcement = true;
    s_render_submit_starts = 0;

    // No SIMULATION_START in this lifetime: logged at once, and enforced after the warm-up.
    for (uint64_t id = 1; id <= 250; ++id) SendMarker(&device, 3000 + id, RENDERSUBMIT_START);
    CHECK(s_render_submit_starts == 250);
    CHECK(!s_enforced_ids.empty() && s_enforced_ids.back() == 3250);

    // A new lifetime with SIMULATION_START: that marker is enforced, RENDERSUBMIT_START is not.
    NvAPI_ClearFeedbackDevice();
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    s_enforced_ids.clear();
    s_render_submit_starts = 0;
    SendMarker(&device, 3301, SIMULATION_START);
    {
        SchedulerOwnerGuard owner;
        CHECK(owner);
        std::thread render([&] { SendMarker(&device, 3301, RENDERSUBMIT_START); });
        render.join();
    }
    CHECK(s_render_submit_starts == 0);
    SendMarker(&device, 3302, SIMULATION_START);
    CHECK(s_render_submit_starts == 1); // not lost to the busy owner
    // A free owner is not claimed either; the stage event is pushed at the marker itself.
    DrainStageEvents();
    PublishPlan(Key(3302), PacingRung::GpuQueueWait, 2'000.0);
    SendMarker(&device, 3302, RENDERSUBMIT_START);
    PacingStageEvent event{};
    CHECK(FramePlan_PopStageEvent(&event) && event.kind == PacingStageEventKind::RenderSubmitStart &&
          event.key.frame_id == 3302);
    CHECK(s_render_submit_starts == 1);
    SendMarker(&device, 3303, SIMULATION_START);
    CHECK(s_render_submit_starts == 2);
    CHECK((s_enforced_ids == std::vector<uint64_t>{3301, 3302, 3303}));
    s_replay_enforcement = false;
    DrainStageEvents();
    NvAPI_ClearFeedbackDevice();
}

void ReplayDawnwalker(const char* path) {
    std::ifstream file(path);
    CHECK(file.good());
    const auto split = [](const std::string& line) {
        std::vector<std::string> fields;
        std::istringstream stream(line);
        std::string field;
        while (std::getline(stream, field, ',')) fields.push_back(field);
        return fields;
    };
    std::string line;
    std::getline(file, line);
    const auto header = split(line);
    const auto column = [&](const std::string& name) {
        const auto found = std::find(header.begin(), header.end(), name);
        CHECK(found != header.end());
        return static_cast<size_t>(found - header.begin());
    };
    const auto type_column = column("event_type"), id_column = column("frame_id");
    std::vector<uint64_t> captured;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        const auto fields = split(line);
        if (fields.at(type_column) == "release_request") captured.push_back(std::stoull(fields.at(id_column)));
    }
    CHECK(captured.size() == 1738 && captured.front() == 15063 && captured.back() == 17259);
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    const uint64_t first = captured.front(), last = captured.back();
    s_enforced_ids.clear();
    s_replay_enforcement = true;
    for (uint64_t id = first; id <= last; ++id) SendMarker(&device, id, SIMULATION_START);
    s_replay_enforcement = false;
    CHECK(s_enforced_ids.size() == 2197);
    std::printf("Dawnwalker source-ID replay: complete handoff 2197 admitted. "
                "Not a runtime FPS prediction.\n");
    NvAPI_ClearFeedbackDevice();
}
} // namespace

// Unrelated-subsystem link doubles. Publication, COM identity, epoch generation,
// game-params reset, exact token checks, frame plans, the cadence grid and the
// CPU executors are the actual production code.
Config g_config;
MarkerLog g_marker_log;
void ResetFeedbackProducerLifecycle() noexcept { ++s_lifecycle_resets; }
IUnknown* SwapMgr_AcquirePresentingDevice() noexcept {
    if (s_test_presenting) s_test_presenting->AddRef();
    return s_test_presenting;
}
uint64_t SwapMgr_PresentingDeviceSequence() noexcept { return s_test_presenting_sequence; }
uint64_t SwapMgr_PresentingDeviceIdentity(uint64_t* sequence) noexcept {
    if (sequence) *sequence = s_test_presenting_sequence;
    return (s_test_presenting_sequence & 1u) == 0 ? reinterpret_cast<uint64_t>(s_test_presenting) : 0;
}
ActiveAPI SwapMgr_GetActiveAPI() { return s_test_api; } // default None: native calls bootstrap feedback
// The trail (startup_breadcrumb.h): the Reflex Sleep and latency marker hooks mark their first call; nothing records it here.
void Breadcrumb_Trail(TrailKind, uint8_t, uint32_t, uint16_t) noexcept {}
void Log_Write(LogLevel, const char* fmt, ...) {
#ifdef _WIN64
    if (fmt && std::strncmp(fmt, "NvAPI: SetFlipConfig", 20) == 0) ++s_flip_config_log_writes;
    if (fmt && std::strncmp(fmt, "Reflex observe: ", 16) == 0) ++s_reflex_observe_log_writes;
#else
    (void)fmt;
#endif
}
// Frame-ID-keyed SIMULATION_START samples for the CPU-latency readout; tail
// markers (drained from the deferred queue) are counted.
void MarkerLog::Record(uint32_t type, int64_t ts, uint64_t id) noexcept {
    if (type == SIMULATION_START) {
        s_simulation_start_id[id % 8] = id;
        s_simulation_start_qpc[id % 8] = ts;
    }
    if (type == RENDERSUBMIT_END) s_last_render_submit_end_qpc = ts;
    if (type == RENDERSUBMIT_START) ++s_render_submit_starts;
    if (!IsImmediateTimingMarker(static_cast<NV_LATENCY_MARKER_TYPE>(type))) {
        if (s_tail_markers == 0) s_first_tail_timestamp = ts;
        ++s_tail_markers;
        s_last_tail_timestamp = ts;
    }
}
int64_t MarkerLog::Get(uint64_t id, uint32_t type) noexcept {
    if (type != SIMULATION_START || s_simulation_start_id[id % 8] != id) return 0;
    return s_simulation_start_qpc[id % 8];
}
void RecordNvAPIMarker() {}
void OnMarker(uint64_t frame_id, int64_t, SchedulerTrigger, PacingSourceMarker) {
    if (s_replay_enforcement) s_enforced_ids.push_back(frame_id);
    if (s_record_call_order) s_call_order.push_back({'O', frame_id});
    if (s_on_marker_sleep_ms) Sleep(s_on_marker_sleep_ms); // stands in for the source wait
}
// Hook_Sleep's scheduler calls (scheduler.cpp and sleep.cpp are not linked).
void Scheduler_RecordReflexSleep(int64_t qpc) noexcept {
    ++s_record_sleep_calls;
    s_recorded_sleep_qpc = qpc;
}
// display_state.cpp is not linked: the driver's answer about the game's frames as the hooks hand it over.
void DisplayState_OnSleepStatus(bool fullscreen_vrr, bool fullscreen_iflip, bool own_read) noexcept {
    ++s_game_vrr_calls;
    s_game_vrr_fullscreen_vrr = fullscreen_vrr;
    s_game_vrr_fullscreen_iflip = fullscreen_iflip;
    s_game_vrr_own_read = own_read;
}
// With s_scheduler_from_pipeline the next answer as scheduler.cpp does, from the published pipeline.
bool Scheduler_PacesSource() noexcept {
    if (!s_scheduler_from_pipeline) return s_paces_source;
    const PacingSourcePlacement placement = PacingPipeline_Read().placement;
    return placement == PacingSourcePlacement::Marker || placement == PacingSourcePlacement::Unified ||
        placement == PacingSourcePlacement::Present;
}
void Scheduler_EndDriverSleep(int64_t start_qpc, int64_t end_qpc) noexcept {
    ++s_driver_sleep_calls;
    s_driver_sleep_start_qpc = start_qpc;
    s_driver_sleep_end_qpc = end_qpc;
}
// The unified owner: the scheduler's reservation and publication, controlled by
// the s_unified_* stubs; the hard limit, the tuple and the Sleep hook are production code.
bool Scheduler_UnifiedActive() noexcept {
    if (!s_scheduler_from_pipeline) return s_unified_active;
    const PacingPipelineSnapshot pipeline = PacingPipeline_Read();
    return pipeline.placement == PacingSourcePlacement::Unified &&
        (pipeline.mode == PacingTimingMode::Vrr || pipeline.mode == PacingTimingMode::Fixed ||
         pipeline.mode == PacingTimingMode::Idle);
}
SchedulerUnifiedReserve Scheduler_UnifiedReserve(int64_t arrival_qpc, SchedulerUnifiedReservation* out) noexcept {
    ++s_unified_reserve_calls;
    s_unified_reserve_arrival = arrival_qpc;
    if (!s_unified_busy_estimates.empty()) {
        if (out) {
            *out = SchedulerUnifiedReservation{};
            out->target_qpc = s_unified_busy_estimates.front();
        }
        s_unified_busy_estimates.erase(s_unified_busy_estimates.begin());
        return SchedulerUnifiedReserve::Busy;
    }
    if (s_unified_reserve_result == SchedulerUnifiedReserve::Reserved && out) *out = s_unified_reservation;
    return s_unified_reserve_result;
}
void Scheduler_UnifiedPublishSleep(const SchedulerUnifiedReservation& reservation, UnifiedSleepOutcome outcome,
    int64_t release_qpc, int64_t arrival_qpc, const UnifiedLimitResult& limit) noexcept {
    ++s_unified_publishes;
    s_unified_published = reservation;
    s_unified_outcome = outcome;
    s_unified_release_qpc = release_qpc;
    s_unified_arrival_qpc = arrival_qpc;
    s_unified_limit = limit;
}
bool PreSleepWaitUntil(int64_t target_qpc) {
    ++s_pre_sleep_waits;
    s_pre_sleep_wait_target = target_qpc;
    if (RealQpc() < target_qpc && target_qpc - RealQpc() > s_pre_sleep_max_ahead)
        s_pre_sleep_max_ahead = target_qpc - RealQpc();
    if (s_pre_sleep_wait_blocks) {
        // The wait that reaches its target (a timer for the long part, a spin for the rest).
        while (RealQpc() < target_qpc) {
            if (target_qpc - RealQpc() > us_to_qpc(5000.0)) Sleep(1);
            else _mm_pause();
        }
    }
    return true;
}
// The production clock's wait for the owner; the scripted environment has its own.
bool Scheduler_AwaitOwner(int64_t) noexcept { return false; }
void PacingTelemetry_PushWait(const PacingWaitRecord&) noexcept {} // async marker hook timing; not asserted here
void PublishReflexControllerResidence(uint64_t, uint32_t, double residence_us) noexcept {
    ++s_residence_publications;
    s_last_residence_us = residence_us;
}
void PublishReflexGateHold(uint64_t, uint32_t, double) noexcept { ++s_gate_hold_publications; }
bool RuntimeLifecycle_IsStopping() noexcept { return s_test_stopping.load(); }
uint64_t SwapMgr_GetNativeHandle() { return 0x1000; }
NativeGateContext NativeGate_GetContext(int64_t now) noexcept {
    if (!s_native_context_enabled) return {};
    return {0x1000, s_native_swapchain_generation, FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame),
        FrameTimeline_CurrentControllerGeneration(), now, 10'000'000, true, true};
}
// native_present_gate_runtime.cpp is not linked; count the per-marker calls.
void NativeGate_PublishRouteCapability(uint32_t) noexcept { ++s_route_capability_publications; }
void HWSpin(int64_t target) {
    if (s_pause_gate.exchange(false)) {
        SetEvent(s_gate_entered);
        CHECK(WaitForSingleObject(s_gate_continue, 5000) == WAIT_OBJECT_0);
    }
    s_gate_qpc = target;
}
int64_t GetQPCFrequency() { return 10'000'000; }
void ReflexClockMap_RecordPresentStart(uint32_t, uint64_t frame_id, int64_t present_start_qpc) noexcept {
    s_clock_map_frame_id = frame_id;
    s_clock_map_qpc = present_start_qpc;
}
int64_t us_to_qpc(double us) { return static_cast<int64_t>(us * 10.0); }
double qpc_to_us(int64_t delta) { return static_cast<double>(delta) / 10.0; }
RuntimeCallbackGuard::RuntimeCallbackGuard() noexcept : stopping_(false) {}
RuntimeCallbackGuard::~RuntimeCallbackGuard() = default;
MH_STATUS InstallHook(void*, void*, void**) { return MH_ERROR_UNSUPPORTED_FUNCTION; }
bool Hooks_InBatch() noexcept { return false; }
bool Hooks_TargetEnabled(void*) noexcept { return false; }
#ifdef _WIN64
bool PCL_MarkersFlowing() { return false; }
bool IsNvSmoothMotionActive() { return s_smooth_motion; }
bool IsDmfgActive() { return s_dmfg_active; }
#endif

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--proxy-call-under-lock") == 0) {
        s_expect_check_failure = true; // the control passes when the fixture's CHECK fires
        RunProxyCallUnderLockControl();
        std::puts("the fixture did not detect a proxy call under the lock");
        return 1;
    }
    TestIdentityNormalization();
    TestCyclesAndBounds();
    TestFailedQueries();
    TestStableAliasCapture();
    TestReentrantRevalidation();
    TestRealAddressReuse();
    TestDiagnosticAdmission();
    TestRenderSubmitStartAdmission();
    TestCpuLatency();
    TestMarkerForwardOrder();
    TestSourceAdmissionWithBusyTail();
    TestPresentStartExecutors();
    TestEnforcementDuringGateAndCancellation();
    TestNativeRouteFromEveryMarker();
    TestFrameMarkerStageEvents();
    TestReflexCallForwarding();
    TestDriverLimiterSleepMode();
    TestSleepStatusAtSleepReturn();
#ifdef _WIN64
    TestUnifiedSleepModeTuple();
    TestUnifiedKeyTransitions();
    TestUnifiedSleepHook();
    TestSleepBypassDecision();
    TestUnifiedMarkerForward();
#endif
#ifdef _WIN64
    TestFlipMeteringBlock();
    TestFlipConfigObserver();
    TestReflexObservation();
#endif
    // No-FG token: native mode transitions clear it; the scheduler-side
    // observation entry point republishes it (not a fixture-only publication).
    NativeGate_SetMode(NativeGateMode::Marker);
    const uint32_t controller=FrameTimeline_CurrentControllerGeneration();
    NativeGate_ObserveFrameGeneration(1,controller);
    CHECK(NativeGate_HasNoFrameGeneration(controller));
    for(int transition=0;transition<3;++transition) {
        NativeGate_SetMode(NativeGateMode::Native);
        NativeGate_ObserveFrameGeneration(1,controller);
        NativeGate_SetMode(NativeGateMode::Marker);
        CHECK(!NativeGate_HasNoFrameGeneration(controller));
        NativeGate_ObserveFrameGeneration(1,controller);
        CHECK(NativeGate_HasNoFrameGeneration(controller));
        NativeGate_ObserveFrameGeneration(2,controller);
        CHECK(!NativeGate_HasNoFrameGeneration(controller));
        NativeGate_ObserveFrameGeneration(1,controller);
    }
    NativeGate_PublishNoFrameGeneration(false,controller);
    CHECK(!NativeGate_HasNoFrameGeneration(controller));
    NativeGate_PublishNoFrameGeneration(true,controller);
    CHECK(NativeGate_HasNoFrameGeneration(controller));
    TestResolveIncomingDevice();
    TestReShadeLockCycle();
    TestReaderInFlight();
    TestUnloadUnderLock();
    TestHotPathTouchesNoProxy();
    if (argc == 2) ReplayDawnwalker(argv[1]);
    CHECK(!NvAPI_HasFeedbackDevice());
    std::puts("feedback device identity/publication tests passed (production-linked; no live driver)");
    return 0;
}
