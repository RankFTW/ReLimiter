// Production-linked Windows/MSVC C++17 tests, not a copied publication model.
// Includes nvapi_hooks.cpp and presentation_gate.cpp to exercise the actual
// private detour and the CPU output executors (H0/H1, and the H2 core) with the
// production frame plans, cadence grid, output tracker and pipeline publication.
// The unrelated hook/scheduler services below are test
// doubles: these tests do NOT validate driver forwarding, pacing or live hooks.
#include "nvapi_hooks.h"
#ifdef _WIN64
void FgInput_RecordMarker(uint64_t, uint32_t, uint32_t, int64_t) noexcept {}
#endif
#include "feedback_device_identity.h"
#include "config.h"
#include "feedback.h"
#include "frame_timeline.h"
#include "hooks.h"
#include "health.h"
#include "logger.h"
#include "marker_log.h"
#include "pcl_hooks.h"
#include "cadence_grid.h"
#include "frame_plan.h"
#include "pacing_resolver.h"
#include "presentation_gate.h"
#include "runtime_lifecycle.h"
#include "scheduler.h"
#include "streamline_hooks.h"
#include "swapchain_manager.h"
#include "wake_guard.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>

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
void Check(bool passed, const char* expression, int line) {
    if (passed) return;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    std::abort();
}
#define CHECK(expression) Check(!!(expression), #expression, __LINE__)

// Independently pinned to deps/reshade/source/com_utils.hpp and the AddRef
// return contract in deps/reshade/source/d3d12/d3d12_device.cpp.
constexpr GUID kUnwrappedObject = {
    0x7f2c9a11, 0x3b4e, 0x4d6a,
    { 0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42 }
};

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
std::atomic<unsigned> s_forwarded_markers{0};
unsigned s_tail_markers = 0;
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
// Render handoff limit in the Sleep hook (Scheduler_* stubs below).
bool s_pre_sleep_active = false;
uint64_t s_handoff_frame = 0;      // Scheduler_RenderHandoffFrame; 0 = not limited
unsigned s_handoff_stalls = 0;     // Scheduler_RenderHandoffStalled calls
int64_t s_planned_handoff_qpc = -1; // handoff_qpc of the newest Scheduler_PlanReflexSleep
int64_t RealQpc();
// Render-thread start hold (stubs below: sleep.cpp and the telemetry worker are not linked).
unsigned s_render_start_waits = 0;
int64_t s_render_start_wait_target = 0;
std::vector<PacingWaitRecord> s_wait_records;
unsigned s_mode_calls = 0;
int s_sleep_action = 0;
NV_SET_SLEEP_MODE_PARAMS* s_expected_mode_params = nullptr;

NvAPI_Status __cdecl FakeSleepMode(IUnknown*, NV_SET_SLEEP_MODE_PARAMS* params) {
    CHECK(params == s_expected_mode_params); // unchanged original pointer
    ++s_mode_calls;
    return -9; // preserve a driver's failure; requested params are not accepted-state proof
}
// Actions: 2 clears the feedback device during Sleep, 4 faults natively.
NvAPI_Status __cdecl FakeDriverSleep(IUnknown* device) {
    ++s_sleep_calls;
    if (s_sleep_action == 2) NvAPI_ClearFeedbackDevice(device);
    if (s_sleep_action == 4) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    return -7; // a driver failure is returned to the game unchanged
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
    g_reflex_sleep_bypass=true;g_user_target_fps=157;
    g_pacer_latency_us=500.0;
    const auto bypass_calls=s_sleep_calls;
    // Bypass: no driver call and no stale Sleep duration.
    CHECK(Hook_Sleep(&device)==NVAPI_OK && s_sleep_calls==bypass_calls);
    CHECK(g_pacer_latency_us.load()==0.0);
    // Passthrough (Dynamic MFG, uncapped) or the driver limiter: the driver's Sleep runs.
    s_paces_source=false;
    CHECK(Hook_Sleep(&device)==-7 && s_sleep_calls==bypass_calls+1);
    s_paces_source=true;
    g_user_target_fps=0;
    CHECK(Hook_Sleep(&device)==-7 && s_sleep_calls==bypass_calls+2);
    g_reflex_sleep_bypass=false;

    // Render handoff limit: a bypassed Sleep
    // waits until the render thread has taken the newest released frame, then
    // plans the pre-wait with that handoff, before the game reads input.
    g_reflex_sleep_bypass=true;g_user_target_fps=157;
    s_pre_sleep_active=true;
    // Stall span 3 I = 60 ms: each Sleep below can overrun by a whole 15.6 ms
    // timer tick.
    g_effective_interval_us=20000.0;
    RenderHandoff_Publish(1, 7, 41, RealQpc());
    s_handoff_frame=42;
    std::atomic<int64_t> published_qpc{0};
    const int64_t wait_start=RealQpc();
    std::thread render_thread([&published_qpc] {
        Sleep(3);
        const int64_t qpc = RealQpc();
        published_qpc = qpc;
        RenderHandoff_Publish(1, 7, 42, qpc);
    });
    CHECK(Hook_Sleep(&device)==NVAPI_OK);
    render_thread.join();
    CHECK(s_planned_handoff_qpc==published_qpc.load() && s_planned_handoff_qpc>wait_start);
    CHECK(s_handoff_stalls==0);
    // Frames 43 and 44 are still queued ahead of frame 45: each handoff is
    // progress, so the wait outlives the 60 ms span and ends at frame 45.
    s_handoff_frame=45;
    const int64_t drain_start=RealQpc();
    std::thread draining_thread([&published_qpc] {
        for (uint64_t frame = 43; frame <= 45; ++frame) {
            Sleep(30);
            const int64_t qpc = RealQpc();
            published_qpc = qpc;
            RenderHandoff_Publish(1, 7, frame, qpc);
        }
    });
    CHECK(Hook_Sleep(&device)==NVAPI_OK);
    draining_thread.join();
    CHECK(s_planned_handoff_qpc==published_qpc.load() && s_handoff_stalls==0);
    CHECK(s_planned_handoff_qpc-drain_start>=us_to_qpc(80000.0));
    // The render thread takes no frame: the wait ends after 3 I, is
    // reported, and the pre-wait is planned without a handoff.
    s_handoff_frame=46;
    const int64_t stall_start=RealQpc();
    CHECK(Hook_Sleep(&device)==NVAPI_OK);
    CHECK(s_handoff_stalls==1 && s_planned_handoff_qpc==0);
    CHECK(RealQpc()-stall_start>=us_to_qpc(55000.0));
    // Another producer's handoff ends the wait at once without a stall.
    RenderHandoff_Publish(1, 8, 1, RealQpc());
    CHECK(Hook_Sleep(&device)==NVAPI_OK && s_handoff_stalls==1 && s_planned_handoff_qpc==0);
    // The stall span follows the render thread's own frame time: 17
    // consecutive handoffs 30 ms apart set it to 30 ms, so a wait with a 1 ms
    // floor ends only after three of those frame times. A frame gap is not an
    // interval, and another stream has no frame time.
    {
        const int64_t now = RealQpc();
        for (uint64_t frame = 1; frame <= 17; ++frame) {
            RenderHandoff_Publish(1, 9, frame, now - us_to_qpc(30000.0 * static_cast<double>(17 - frame)));
        }
        const int64_t cycle = RenderHandoff_CycleTicks(1, 9);
        CHECK(cycle >= us_to_qpc(30000.0) - 1 && cycle <= us_to_qpc(30000.0) + 1);
        CHECK(RenderHandoff_CycleTicks(1, 7) == 0);
        RenderHandoff_Publish(1, 9, 19, now + us_to_qpc(1000.0));
        CHECK(RenderHandoff_CycleTicks(1, 9) == cycle);
        int64_t handoff_qpc = 0;
        const int64_t start = RealQpc();
        CHECK(RenderHandoff_WaitFor(1, 9, 20, us_to_qpc(1000.0), &handoff_qpc) == RenderHandoffWait::Deadline);
        CHECK(RealQpc() - start >= us_to_qpc(85000.0));
        // A render thread that takes each frame only after its wait stalled
        // (200 ms apart here) leaves the frame time unchanged, so its stall
        // span does not grow with its own stalls.
        for (uint64_t frame = 20; frame <= 40; ++frame) {
            if (frame > 20) {
                CHECK(RenderHandoff_WaitFor(1, 9, frame, 0, &handoff_qpc) == RenderHandoffWait::Deadline);
            }
            RenderHandoff_Publish(1, 9, frame, now + us_to_qpc(1000.0 + 200000.0 * static_cast<double>(frame - 19)));
        }
        CHECK(RenderHandoff_CycleTicks(1, 9) == cycle);
        // Frames no wait stalled for count again: 20 intervals of 200 ms are
        // most of the window.
        for (uint64_t frame = 41; frame <= 60; ++frame) {
            RenderHandoff_Publish(1, 9, frame, now + us_to_qpc(1000.0 + 200000.0 * static_cast<double>(frame - 19)));
        }
        const int64_t slow_cycle = RenderHandoff_CycleTicks(1, 9);
        CHECK(slow_cycle >= us_to_qpc(200000.0) - 1 && slow_cycle <= us_to_qpc(200000.0) + 1);
    }
    // The driver's Sleep runs (bypass off): no handoff wait.
    g_reflex_sleep_bypass=false;
    s_planned_handoff_qpc=-1;
    CHECK(Hook_Sleep(&device)==-7 && s_planned_handoff_qpc==0);
    s_handoff_frame=0;
    s_pre_sleep_active=false;
    g_user_target_fps=0;

    // 0 forward, 2 device cleared during Sleep, 4 native fault,
    // 5 Smooth Motion bypass (x64 only), 6 no original trampoline.
    for (int scenario : {0, 2, 4, 5, 6}) {
        if (!g_dev.load()) CHECK(NvAPI_CaptureFeedbackDevice(&device));
        if (scenario == 6) s_orig_sleep = nullptr;
        s_sleep_action = scenario < 5 ? scenario : 0;
#ifdef _WIN64
        s_smooth_motion = scenario == 5;
#endif
        const unsigned before = s_sleep_calls;
        const double latency_before = g_pacer_latency_us.load();
        const bool bypass = s_smooth_motion || scenario == 6;
        CHECK(Hook_Sleep(&device) == (bypass || scenario == 4 ? NVAPI_OK : -7));
        CHECK(s_sleep_calls == before + (bypass ? 0 : 1));
        // A device replaced during the driver call never updates Sleep latency.
        if (scenario == 2) CHECK(g_pacer_latency_us.load() == latency_before);
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

NV_SET_SLEEP_MODE_PARAMS s_recorded_mode{};
const NV_SET_SLEEP_MODE_PARAMS* s_recorded_pointer = nullptr;
unsigned s_recorded_calls = 0;
NvAPI_Status __cdecl RecordingSleepMode(IUnknown*, NV_SET_SLEEP_MODE_PARAMS* params) {
    s_recorded_mode = *params;
    s_recorded_pointer = params;
    ++s_recorded_calls;
    return NVAPI_OK;
}

// reflex_markers_optimize: with game the detour forwards the game's own
// pointer; with off every SetSleepMode ReLimiter forwards or applies carries
// bUseMarkersToOptimize = 0 (one call, combined with the driver limiter's
// interval) while the captured game parameters keep 1. Restore forwards the
// game's original parameters exactly.
void TestMarkersOptimizeOverride() {
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    const auto old_mode = s_orig_sleep_mode;
    s_orig_sleep_mode = RecordingSleepMode;
    NV_SET_SLEEP_MODE_PARAMS params{};
    params.version = NV_SLEEP_PARAMS_VER;
    params.minimumIntervalUs = 1234;
    params.bLowLatencyMode = params.bLowLatencyBoost = params.bUseMarkersToOptimize = params.bUseMinQueueTime = 1;
    g_driver_limiter_interval_us.store(0);

    g_reflex_markers_optimize_off.store(false);
    CHECK(Hook_SetSleepMode(&device, &params) == NVAPI_OK);
    CHECK(s_recorded_pointer == &params && s_recorded_mode.bUseMarkersToOptimize == 1);

    // off: a forwarded copy; the game's structure and the captured copy keep 1.
    g_reflex_markers_optimize_off.store(true);
    unsigned calls = s_recorded_calls;
    CHECK(Hook_SetSleepMode(&device, &params) == NVAPI_OK);
    CHECK(s_recorded_calls == calls + 1);
    CHECK(s_recorded_pointer != &params);
    CHECK(s_recorded_mode.bUseMarkersToOptimize == 0 && s_recorded_mode.minimumIntervalUs == 1234);
    CHECK(s_recorded_mode.bLowLatencyMode && s_recorded_mode.bLowLatencyBoost && s_recorded_mode.bUseMinQueueTime);
    CHECK(params.bUseMarkersToOptimize == 1);
    {
        GameParamsGuard lock;
        CHECK(s_game_params_captured && s_game_original_params.bUseMarkersToOptimize == 1);
    }

    // off with the driver limiter: still one call carrying both changes.
    g_driver_limiter_interval_us.store(16667);
    calls = s_recorded_calls;
    CHECK(Hook_SetSleepMode(&device, &params) == NVAPI_OK);
    CHECK(s_recorded_calls == calls + 1);
    CHECK(s_recorded_mode.bUseMarkersToOptimize == 0 && s_recorded_mode.minimumIntervalUs == 16667);
    CHECK(params.minimumIntervalUs == 1234 && g_game_requested_interval.load() == 1234);
    g_driver_limiter_interval_us.store(0);

    // The limiter's own application and the immediate reapply carry the override.
    CHECK(NvAPI_ApplyDriverLimiter(8333) == DriverLimiterApply::Applied);
    CHECK(s_recorded_mode.bUseMarkersToOptimize == 0 && s_recorded_mode.minimumIntervalUs == 8333);
    CHECK(NvAPI_ReapplyGameSleepMode() == DriverLimiterApply::Applied);
    CHECK(s_recorded_mode.bUseMarkersToOptimize == 0 && s_recorded_mode.minimumIntervalUs == 1234);
    g_driver_limiter_interval_us.store(9000);
    CHECK(NvAPI_ReapplyGameSleepMode() == DriverLimiterApply::Applied);
    CHECK(s_recorded_mode.bUseMarkersToOptimize == 0 && s_recorded_mode.minimumIntervalUs == 9000);
    g_driver_limiter_interval_us.store(0);

    // The shutdown restore is the game's original, without the override.
    calls = s_recorded_calls;
    RestoreGameSleepMode();
    CHECK(s_recorded_calls == calls + 1);
    CHECK(s_recorded_mode.bUseMarkersToOptimize == 1 && s_recorded_mode.minimumIntervalUs == 1234);
    CHECK(s_recorded_mode.bLowLatencyMode && s_recorded_mode.bLowLatencyBoost && s_recorded_mode.bUseMinQueueTime);

    // Back to game: the reapply returns the game's value at once.
    g_reflex_markers_optimize_off.store(false);
    CHECK(NvAPI_ReapplyGameSleepMode() == DriverLimiterApply::Applied);
    CHECK(s_recorded_mode.bUseMarkersToOptimize == 1 && s_recorded_mode.minimumIntervalUs == 1234);
    CHECK(Hook_SetSleepMode(&device, &params) == NVAPI_OK && s_recorded_pointer == &params);

    // Without a captured game call there is nothing to reapply.
    NvAPI_ClearFeedbackDevice();
    CHECK(NvAPI_ReapplyGameSleepMode() == DriverLimiterApply::NoGameParams);
    s_orig_sleep_mode = old_mode;
}

void RepublishDuringQuery() {
    NvAPI_ClearFeedbackDevice(s_reentrant_expected);
    CHECK(NvAPI_CaptureFeedbackDevice(s_reentrant_device, &s_reentrant_publication));
}

void TestIdentityNormalization() {
    FakeDevice native, unrelated, alias, wrapper, nested, sibling;
    alias.canonical = &native;
    wrapper.Wrap(&native);
    nested.Wrap(&wrapper);
    sibling.Wrap(&native);
    CHECK(!NvAPI_IsSameDevice(nullptr, nullptr));
    CHECK(!NvAPI_IsSameDevice(&native, nullptr));
    CHECK(!NvAPI_IsSameDevice(nullptr, &native));
    CHECK(NvAPI_IsSameDevice(&native, &native));
    CHECK(NvAPI_IsSameDevice(&alias, &native));
    CHECK(NvAPI_IsSameDevice(&native, &alias));
    CHECK(NvAPI_IsSameDevice(&wrapper, &native));
    CHECK(NvAPI_IsSameDevice(&native, &nested));
    CHECK(NvAPI_IsSameDevice(&nested, &sibling));
    CHECK(!NvAPI_IsSameDevice(&native, &unrelated));
    CHECK(!NvAPI_IsSameDevice(&nested, &unrelated));
    CHECK(FeedbackDeviceIdentity_IsSame(&nested, &alias));
}

void TestCyclesAndBounds() {
    FakeDevice native, self, self_alias, first, second;
    self.Wrap(&self);
    self_alias.Wrap(&self);
    CHECK(NvAPI_IsSameDevice(&self, &self_alias));
    CHECK(!NvAPI_IsSameDevice(&self, &native));
    first.Wrap(&second);
    second.Wrap(&first);
    CHECK(!NvAPI_IsSameDevice(&first, &native));
    CHECK(!NvAPI_IsSameDevice(&first, &second));
    CHECK(first.queries + second.queries < 32);

    std::array<FakeDevice, 12> deep;
    for (size_t i = 0; i + 1 < deep.size(); ++i) deep[i].Wrap(&deep[i + 1]);
    deep.back().Wrap(&native);
    CHECK(!NvAPI_IsSameDevice(&deep.front(), &native));
    unsigned queries = 0;
    for (const auto& device : deep) queries += device.queries;
    CHECK(queries <= 16); // Fixed eight-layer production bound, not a loop hang.
}

void TestFailedQueries() {
    FakeDevice native, wrapper;
    wrapper.Wrap(&native);
    wrapper.unwrap_status = E_FAIL; // Nonconforming failure WITH an owned output.
    CHECK(!NvAPI_IsSameDevice(&wrapper, &native));
    CHECK(native.references == 1);
    wrapper.unwrap_status = S_OK;
    wrapper.unwrapped = nullptr; // Nonconforming success WITHOUT an output.
    CHECK(!NvAPI_IsSameDevice(&wrapper, &native));
    wrapper.fault_unwrap = true;
    CHECK(!NvAPI_IsSameDevice(&wrapper, &native));
    wrapper.fault_unwrap = false;
    wrapper.identity_status = E_FAIL;
    CHECK(!NvAPI_IsSameDevice(&wrapper, &native));
    wrapper.identity_status = S_OK;
    wrapper.null_identity = true;
    CHECK(!NvAPI_IsSameDevice(&wrapper, &native));
}

void TestStableAliasCapture() {
    FakeDevice native, wrapper, alias, unrelated;
    wrapper.Wrap(&native);
    alias.canonical = &native;
    NvAPIFeedbackDevice original;
    CHECK(NvAPI_CaptureFeedbackDevice(&wrapper, &original));
    CHECK(original.device == &wrapper);
    CHECK(wrapper.references == 2);
    const unsigned resets = s_lifecycle_resets;
    g_game_requested_interval.store(1234);
    for (IUnknown* candidate : { static_cast<IUnknown*>(&native),
             static_cast<IUnknown*>(&alias), static_cast<IUnknown*>(&wrapper) }) {
        NvAPIFeedbackDevice no_publication = original;
        CHECK(!NvAPI_CaptureFeedbackDevice(candidate, &no_publication));
        CHECK(!no_publication.device); // No ownership token granted for a no-op.
        const NvAPIFeedbackDevice current = NvAPI_AcquireFeedbackDevice();
        CHECK(NvAPI_IsSameFeedbackPublication(original, current));
        CHECK(current.device == &wrapper); // Original callable pointer retained.
        NvAPI_ReleaseFeedbackDevice(current.device);
        CHECK(NvAPI_IsFeedbackDeviceCurrent(original));
        CHECK(g_game_requested_interval.load() == 1234);
        CHECK(s_lifecycle_resets == resets);
    }
    CHECK(native.references == 1 && alias.references == 1);
    NvAPIFeedbackDevice alias_token = original;
    alias_token.device = &native;
    CHECK(!NvAPI_IsSameFeedbackPublication(alias_token, original));
    NvAPI_ClearFeedbackDevice(alias_token); // Identity is NOT publication authority.
    CHECK(NvAPI_IsFeedbackDeviceCurrent(original));

    NvAPIFeedbackDevice replacement;
    CHECK(NvAPI_CaptureFeedbackDevice(&unrelated, &replacement));
    CHECK(!NvAPI_IsFeedbackDeviceCurrent(original));
    CHECK(replacement.producer_epoch != original.producer_epoch);
    CHECK(replacement.publication_sequence == original.publication_sequence + 2);
    CHECK(g_game_requested_interval.load() == 0);
    CHECK(s_lifecycle_resets == resets + 1);
    CHECK(wrapper.references == 1);
    NvAPI_ClearFeedbackDevice(original);
    CHECK(NvAPI_IsFeedbackDeviceCurrent(replacement));
    NvAPI_ClearFeedbackDevice(replacement);
    CHECK(!NvAPI_AcquireFeedbackDevice().device);

    // Also preserve a native-first publication when the wrapper arrives later.
    CHECK(NvAPI_CaptureFeedbackDevice(&native, &original));
    CHECK(!NvAPI_CaptureFeedbackDevice(&wrapper));
    CHECK(NvAPI_IsFeedbackDeviceCurrent(original));
    // Same public identity + exact-token clear pattern as on_destroy_device.
    const auto destruction_snapshot = NvAPI_AcquireFeedbackDevice();
    CHECK(NvAPI_IsSameDevice(destruction_snapshot.device, &wrapper));
    NvAPI_ClearFeedbackDevice(destruction_snapshot);
    NvAPI_ReleaseFeedbackDevice(destruction_snapshot.device);
}

void TestReentrantRevalidation() {
    FakeDevice native, wrapper, unrelated;
    wrapper.Wrap(&native);
    CHECK(NvAPI_CaptureFeedbackDevice(&wrapper, &s_reentrant_expected));
    const auto original = s_reentrant_expected;
    // Clear and republish the SAME pointer during slow QI. Exact transition
    // and token revalidation must reject the old snapshot, despite equivalence.
    s_reentrant_device = &wrapper;
    wrapper.on_unwrap = RepublishDuringQuery;
    CHECK(!NvAPI_CaptureFeedbackDevice(&native));
    CHECK(!NvAPI_IsFeedbackDeviceCurrent(original));
    CHECK(NvAPI_IsFeedbackDeviceCurrent(s_reentrant_publication));
    CHECK(s_reentrant_publication.publication_sequence == original.publication_sequence + 4);
    NvAPI_ClearFeedbackDevice(original);
    CHECK(NvAPI_IsFeedbackDeviceCurrent(s_reentrant_publication));

    // The same stale comparison must not suppress replacement of an unrelated
    // publication that appeared while QI was running.
    s_reentrant_expected = s_reentrant_publication;
    s_reentrant_device = &unrelated;
    wrapper.on_unwrap = RepublishDuringQuery;
    NvAPIFeedbackDevice final_publication;
    CHECK(NvAPI_CaptureFeedbackDevice(&native, &final_publication));
    CHECK(final_publication.device == &native);
    CHECK(!NvAPI_IsFeedbackDeviceCurrent(s_reentrant_publication));
    CHECK(NvAPI_IsFeedbackDeviceCurrent(final_publication));
    NvAPI_ClearFeedbackDevice(final_publication);

    // An UNEQUAL comparison also becomes stale: the callback has already
    // published our candidate. Revalidate under writer exclusion rather than
    // overwrite it, reset again, or grant its token to this no-op capture.
    CHECK(NvAPI_CaptureFeedbackDevice(&wrapper, &s_reentrant_expected));
    const auto before_unequal_query = s_reentrant_expected;
    s_reentrant_device = &unrelated;
    wrapper.on_unwrap = RepublishDuringQuery;
    NvAPIFeedbackDevice no_publication;
    CHECK(!NvAPI_CaptureFeedbackDevice(&unrelated, &no_publication));
    CHECK(!no_publication.device);
    CHECK(NvAPI_IsFeedbackDeviceCurrent(s_reentrant_publication));
    CHECK(s_reentrant_publication.publication_sequence ==
        before_unequal_query.publication_sequence + 4);
    NvAPI_ClearFeedbackDevice(s_reentrant_publication);
    s_reentrant_device = nullptr;
}

void TestRealAddressReuse() {
    alignas(FakeDevice) unsigned char storage[sizeof(FakeDevice)];
    FakeDevice* first = new (storage) FakeDevice;
    NvAPIFeedbackDevice stale;
    CHECK(NvAPI_CaptureFeedbackDevice(first, &stale));
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
    NvAPI_ClearFeedbackDevice(current);
    reused->~FakeDevice();
}

NvAPI_Status __cdecl ForwardMarker(IUnknown*, NV_LATENCY_MARKER_PARAMS* params) {
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

// marker_forward_order. before_wait forwards the enforcement marker on
// entry and publishes the wait as controller residence; after_wait forwards it
// after OnMarker (the source wait) with zero residence. Every marker, enforced
// or not, reaches the driver exactly once and returns the driver's status.
void TestMarkerForwardOrder() {
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    s_orig_set_latency_marker = ForwardMarker;
    s_record_call_order = true;
    s_on_marker_sleep_ms = 2;
    using Order = std::vector<std::pair<char, uint64_t>>;
    for (const bool after_wait : {false, true}) {
        g_marker_forward_after_wait.store(after_wait);
        const uint64_t frame = after_wait ? 1201 : 1101;
        const unsigned residences_before = s_residence_publications;

        s_call_order.clear();
        SendMarker(&device, frame, SIMULATION_START);
        CHECK(s_call_order == (after_wait ? Order{{'O', frame}, {'F', frame}}
                                          : Order{{'F', frame}, {'O', frame}}));
        CHECK(s_residence_publications == residences_before + 1);
        CHECK(after_wait ? s_last_residence_us == 0.0 : s_last_residence_us > 0.0);

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
    }
    g_marker_forward_after_wait.store(false);
    s_on_marker_sleep_ms = 0;
    s_record_call_order = false;
    NvAPI_ClearFeedbackDevice();
}

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

    // H2 and H3 plans: PRESENT_START reports the arrival only; OtherRung
    // calls do nothing and never consume the plan.
    for (PacingRung later : {PacingRung::NativeHold, PacingRung::GpuQueueWait}) {
        const uint64_t id = later == PacingRung::NativeHold ? 807 : 808;
        const auto later_plan = PublishPlan(Key(id), later, 5'000.0);
        attribution = SendPresentStart(&device, id);
        events = DrainStageEvents();
        CHECK(attribution.plan_status == Status(PacingPlanStatus::Unknown));
        CHECK(attribution.marker_output == PacingOutputKind::None);
        CHECK(events.marker_arrivals == 1 && events.output_releases == 0);
        PresentationGateResult other{};
        s_gate_qpc = 0;
        PresentGate_Execute(later_plan.key, PacingRung::MarkerHold, RealQpc(), nullptr, &other);
        CHECK(!other.claimed && !other.held && other.status == PacingPlanStatus::Unknown);
        CHECK(DrainStageEvents().output_releases == 0);
        if (later != PacingRung::NativeHold) continue;
        // H2 core (NativeGate_Execute delegates here after the NativeArrival
        // event): the NativeHold claim holds once, then reports Duplicate.
        PresentationGateResult native{};
        s_gate_qpc = 0;
        PresentGate_Execute(later_plan.key, PacingRung::NativeHold, RealQpc(), nullptr, &native);
        events = DrainStageEvents();
        CHECK(native.claimed && native.held && native.status == PacingPlanStatus::Held);
        CHECK(native.target_qpc == later_plan.output_target_qpc && native.release_qpc >= native.target_qpc);
        CHECK(events.output_releases == 1 && events.release.rung == PacingRung::NativeHold);
        CHECK(TrackedOutput(later_plan) == native.release_qpc);
        s_gate_qpc = 0;
        PresentGate_Execute(later_plan.key, PacingRung::NativeHold, RealQpc(), nullptr, &native);
        CHECK(!native.claimed && native.status == PacingPlanStatus::Duplicate);
        CHECK(DrainStageEvents().output_releases == 0);
    }

    // H2 at a native Present the route did not select: the NativeHold plan is
    // claimed and reported Undeliverable at the decision time, never held.
    const auto declined_plan = PublishPlan(Key(809), PacingRung::NativeHold, 5'000.0);
    PresentationGateResult declined{};
    PresentGate_DeclineOutput(declined_plan.key, PacingRung::MarkerHold, RealQpc(), &declined);
    CHECK(!declined.claimed && declined.status == PacingPlanStatus::Unknown); // OtherRung
    const int64_t decision_qpc = RealQpc();
    PresentGate_DeclineOutput(declined_plan.key, PacingRung::NativeHold, decision_qpc, &declined);
    events = DrainStageEvents();
    CHECK(declined.claimed && !declined.held && declined.status == PacingPlanStatus::Undeliverable);
    CHECK(events.output_releases == 1 && events.release.qpc == decision_qpc &&
        events.release.status == PacingPlanStatus::Undeliverable);
    CHECK(TrackedOutput(declined_plan) == decision_qpc);
    PresentGate_DeclineOutput(declined_plan.key, PacingRung::NativeHold, decision_qpc, &declined);
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
        // Every rung (and no plan at all) feeds the route.
        const uint64_t variant = frame % 4;
        if (variant != 3) {
            const PacingRung rung = variant == 0 ? PacingRung::SourceOnly :
                variant == 1 ? PacingRung::MarkerHold : PacingRung::NativeHold;
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
    // A marker hold (the render handoff offset samples every rung), then a
    // frame without a plan: no event, but its RENDERSUBMIT_START is still the
    // newest render handoff.
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
    uint64_t handoff_frame = 0;
    int64_t handoff_qpc = 0;
    CHECK(RenderHandoff_Latest(static_cast<uint8_t>(PacingIdentityDomain::NvApiFrame),
        Key(2103).producer_epoch, &handoff_frame, &handoff_qpc));
    CHECK(handoff_frame == 2103 && handoff_qpc > 0);
    CHECK(s_forwarded_markers - forwarded_before == 12);
    NvAPI_ClearFeedbackDevice();
}

// Render-thread start hold: a direct RENDERSUBMIT_START waits for the anchor's
// Ĝ(n − 1) − (P/2 + F) when it lies ahead by at most the real frame interval,
// then reaches the driver once, after the hold; the hold is a stage event
// before RenderSubmitStart and a telemetry wait record. A target further away
// or already past is applied without waiting (0 hold); another producer
// epoch, the key off and the bypass off do not apply it at all.
void TestRenderStartHold() {
    FakeDevice device;
    CHECK(NvAPI_CaptureFeedbackDevice(&device));
    s_orig_set_latency_marker = ForwardMarker;
    const auto saved_pipeline = PacingPipeline_GetConfig();
    auto pipeline = saved_pipeline;
    pipeline.render_start_hold = true;
    PacingPipeline_SetConfig(pipeline);
    g_reflex_sleep_bypass = true;
    g_user_target_fps = 157;
    s_paces_source = true;
    g_effective_interval_us = 50'000.0; // I = 500,000 ticks
    const uint32_t epoch = Key(1).producer_epoch;
    constexpr int64_t kStart = 90'000; // P/2 + F = 9 ms
    const auto publish_anchor = [&](uint64_t gpu_end_frame, int64_t gpu_end_qpc, uint32_t anchor_epoch) {
        GpuFloorAnchor anchor;
        anchor.valid = true;
        anchor.identity_domain = static_cast<uint8_t>(PacingIdentityDomain::NvApiFrame);
        anchor.producer_epoch = anchor_epoch;
        anchor.gpu_end_frame = gpu_end_frame;
        anchor.gpu_end_qpc = gpu_end_qpc;
        anchor.qpc_frequency = 10'000'000;
        anchor.period_us = 10'000.0;
        anchor.start_us = 9'000.0;
        RenderHandoff_PublishGpuFloor(anchor);
    };
    const auto send = [&](uint64_t frame) {
        PublishPlan(Key(frame), PacingRung::SourceOnly, 2'000.0);
        DrainStageEvents();
        s_wait_records.clear();
        s_call_order.clear();
        s_record_call_order = true;
        s_render_start_waits = 0;
        SendMarker(&device, frame, RENDERSUBMIT_START);
        s_record_call_order = false;
    };
    using Order = std::vector<std::pair<char, uint64_t>>;
    const unsigned forwarded_before = s_forwarded_markers;

    // Target 31 ms ahead (within I): waited, then forwarded once; hold event first.
    const int64_t expected_target = RealQpc() + 400'000;
    publish_anchor(2999, expected_target, epoch);
    send(3000);
    CHECK(s_render_start_waits == 1 && s_render_start_wait_target == expected_target - kStart);
    CHECK(s_call_order == (Order{{'W', 0}, {'F', 3000}}));
    CHECK(s_wait_records.size() == 1 && s_wait_records[0].kind == PacingWaitKind::RenderStartHold &&
        s_wait_records[0].key.frame_id == 3000 && s_wait_records[0].key.producer_epoch == epoch &&
        s_wait_records[0].exit_qpc > s_wait_records[0].entry_qpc);
    const int64_t hold_ticks = s_wait_records[0].exit_qpc - s_wait_records[0].entry_qpc;
    PacingStageEvent event{};
    CHECK(FramePlan_PopStageEvent(&event) && event.kind == PacingStageEventKind::RenderStartHold &&
        event.key.frame_id == 3000 && event.hold_ticks == hold_ticks && event.qpc == s_wait_records[0].exit_qpc);
    PacingStageEvent submit{};
    CHECK(FramePlan_PopStageEvent(&submit) && submit.kind == PacingStageEventKind::RenderSubmitStart &&
        submit.qpc >= event.qpc && submit.grid_index == event.grid_index);
    CHECK(!FramePlan_PopStageEvent(&submit));

    // Target further ahead than I: a misprediction, applied without waiting.
    publish_anchor(3000, RealQpc() + 1'500'000, epoch);
    send(3001);
    CHECK(s_render_start_waits == 0 && s_call_order == (Order{{'F', 3001}}));
    CHECK(s_wait_records.size() == 1 && s_wait_records[0].exit_qpc == s_wait_records[0].entry_qpc);
    CHECK(DrainStageEvents().other == 1); // RenderSubmitStart only; no hold event

    // Target already past: applied without waiting.
    publish_anchor(3001, RealQpc(), epoch);
    send(3002);
    CHECK(s_render_start_waits == 0 && s_wait_records.size() == 1 &&
        s_wait_records[0].exit_qpc == s_wait_records[0].entry_qpc);

    // Not applied: another producer epoch, the bypass off, the key off.
    publish_anchor(3002, RealQpc() + 120'000, epoch + 1);
    send(3003);
    CHECK(s_render_start_waits == 0 && s_wait_records.empty() && s_call_order == (Order{{'F', 3003}}));
    publish_anchor(3003, RealQpc() + 120'000, epoch);
    g_reflex_sleep_bypass = false;
    send(3004);
    CHECK(s_render_start_waits == 0 && s_wait_records.empty() && s_call_order == (Order{{'F', 3004}}));
    g_reflex_sleep_bypass = true;
    pipeline.render_start_hold = false;
    PacingPipeline_SetConfig(pipeline);
    publish_anchor(3004, RealQpc() + 120'000, epoch);
    send(3005);
    CHECK(s_render_start_waits == 0 && s_wait_records.empty() && s_call_order == (Order{{'F', 3005}}));

    // Exactly one driver call per marker, held or not.
    CHECK(s_forwarded_markers - forwarded_before == 6);
    GpuFloorAnchor none;
    RenderHandoff_PublishGpuFloor(none);
    PacingPipeline_SetConfig(saved_pipeline);
    g_user_target_fps = 0;
    g_effective_interval_us = 0.0;
    NvAPI_ClearFeedbackDevice();
}

NvAPI_Status __cdecl AsyncMarkerSleeps(IUnknown*, NV_ASYNC_FRAME_MARKER_PARAMS*) {
    Sleep(3);
    return NVAPI_OK;
}

// Driver Present hold: with the developer A/B held_gpu_floor each
// SetAsyncFrameMarker call of a planned frame is also a DriverPresentHold
// stage event keyed by the async marker's frame ID, with the call's end and
// duration (the telemetry wait record is the same call). The key off and an
// unplanned frame push nothing; the call's result passes through.
void TestAsyncMarkerDriverHold() {
    s_orig_set_async_frame_marker = AsyncMarkerSleeps;
    const auto saved_pipeline = PacingPipeline_GetConfig();
    auto pipeline = saved_pipeline;
    const auto call = [](uint64_t frame, uint32_t type) {
        NV_ASYNC_FRAME_MARKER_PARAMS params{};
        params.frameID = frame;
        params.markerType = type;
        s_wait_records.clear();
        CHECK(Hook_SetAsyncFrameMarker(nullptr, &params) == NVAPI_OK);
    };
    PublishPlan(Key(4000), PacingRung::GpuQueueWait, 2'000.0);
    DrainStageEvents();

    pipeline.held_gpu_floor = false;
    PacingPipeline_SetConfig(pipeline);
    call(4000, 12);
    CHECK(s_wait_records.size() == 1 && s_wait_records[0].kind == PacingWaitKind::AsyncFrameMarker);
    PacingStageEvent event{};
    CHECK(!FramePlan_PopStageEvent(&event));

    pipeline.held_gpu_floor = true;
    PacingPipeline_SetConfig(pipeline);
    call(4000, 12);
    CHECK(s_wait_records.size() == 1 && s_wait_records[0].exit_qpc > s_wait_records[0].entry_qpc);
    CHECK(FramePlan_PopStageEvent(&event) && event.kind == PacingStageEventKind::DriverPresentHold &&
        event.key == Key(4000) && event.rung == PacingRung::GpuQueueWait &&
        event.qpc == s_wait_records[0].exit_qpc &&
        event.hold_ticks == s_wait_records[0].exit_qpc - s_wait_records[0].entry_qpc);
    CHECK(!FramePlan_PopStageEvent(&event));

    call(4001, 12); // no plan for this frame ID
    CHECK(s_wait_records.size() == 1 && !FramePlan_PopStageEvent(&event));

    PacingPipeline_SetConfig(saved_pipeline);
    s_orig_set_async_frame_marker = nullptr;
}

void TestNativeEndpointCancellation() {
    // H2 core with route revalidation: the hold never needs the owner and a
    // revoked route cancels it (Stale) with its partial residence retained.
    const auto plan = PublishPlan(Key(500), PacingRung::NativeHold, 8'000.0);
    DrainStageEvents();
    s_gate_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s_gate_continue = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK(s_gate_entered && s_gate_continue);
    std::atomic<bool> current{true};
    const PresentationGateOptions options{
        [](const void* value) noexcept { return static_cast<const std::atomic<bool>*>(value)->load(); },
        &current};
    s_pause_gate = true;
    const unsigned holds_before = s_gate_hold_publications;
    PresentationGateResult result{};
    std::thread render([&] {
        s_gate_qpc = 0;
        PresentGate_Execute(plan.key, PacingRung::NativeHold, RealQpc(), &options, &result);
    });
    CHECK(WaitForSingleObject(s_gate_entered, 5000) == WAIT_OBJECT_0);
    { SchedulerOwnerGuard owner; CHECK(static_cast<bool>(owner)); }
    current = false; // settings/swapchain/FG token revoked while the native hold spins
    SetEvent(s_gate_continue);
    render.join();
    const auto events = DrainStageEvents();
    CHECK(result.claimed && result.held && result.status == PacingPlanStatus::Stale);
    CHECK(result.release_qpc < result.target_qpc);
    CHECK(s_gate_hold_publications == holds_before + 1); // partial owned residence retained
    CHECK(events.output_releases == 1 && events.release.status == PacingPlanStatus::Stale &&
        events.release.rung == PacingRung::NativeHold);
    CHECK(TrackedOutput(plan) == result.release_qpc);
    CloseHandle(s_gate_entered);
    CloseHandle(s_gate_continue);
    s_gate_entered = s_gate_continue = nullptr;
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
IUnknown* SwapMgr_AcquirePresentingDevice() noexcept { return nullptr; }
ActiveAPI SwapMgr_GetActiveAPI() { return ActiveAPI::None; } // native calls bootstrap feedback
void Log_Write(LogLevel, const char*, ...) {}
// Frame-ID-keyed SIMULATION_START samples for the CPU-latency readout; tail
// markers (drained from the deferred queue) are counted.
void MarkerLog::Record(uint32_t type, int64_t ts, uint64_t id) noexcept {
    if (type == SIMULATION_START) {
        s_simulation_start_id[id % 8] = id;
        s_simulation_start_qpc[id % 8] = ts;
    }
    if (type == RENDERSUBMIT_END) s_last_render_submit_end_qpc = ts;
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
// Hook_Sleep's pre-Sleep placement and render handoff limit (scheduler.cpp,
// sleep.cpp are not linked): controlled by the s_pre_sleep_active and
// s_handoff_frame stubs; no pre-wait is ever planned.
void Scheduler_RecordReflexSleep(int64_t) noexcept {}
bool Scheduler_PreSleepPlacementActive() noexcept { return s_pre_sleep_active; }
bool Scheduler_PacesSource() noexcept { return s_paces_source; }
bool Scheduler_RenderHandoffFrame(uint8_t* domain, uint32_t* producer_epoch, uint64_t* frame_id) noexcept {
    if (s_handoff_frame == 0) return false;
    *domain = 1;
    *producer_epoch = 7;
    *frame_id = s_handoff_frame;
    return true;
}
void Scheduler_RenderHandoffStalled() noexcept { ++s_handoff_stalls; }
bool Scheduler_PlanReflexSleep(int64_t, bool, int64_t handoff_qpc, int64_t*, uint64_t*) noexcept {
    s_planned_handoff_qpc = handoff_qpc;
    return false;
}
std::atomic<double> g_effective_interval_us{0.0};
void Scheduler_EndReflexSleep(uint64_t, int64_t) noexcept {}
bool PreSleepWaitUntil(int64_t) { return false; }
bool RenderStartWaitUntil(int64_t target_qpc) {
    ++s_render_start_waits;
    s_render_start_wait_target = target_qpc;
    if (s_record_call_order) s_call_order.push_back({'W', 0});
    while (RealQpc() < target_qpc) Sleep(0);
    return true;
}
void PacingTelemetry_PushWait(const PacingWaitRecord& record) noexcept { s_wait_records.push_back(record); }
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
#ifdef _WIN64
bool PCL_MarkersFlowing() { return false; }
bool IsNvSmoothMotionActive() { return s_smooth_motion; }
#endif

int main(int argc, char** argv) {
    TestIdentityNormalization();
    TestCyclesAndBounds();
    TestFailedQueries();
    TestStableAliasCapture();
    TestReentrantRevalidation();
    TestRealAddressReuse();
    TestDiagnosticAdmission();
    TestCpuLatency();
    TestMarkerForwardOrder();
    TestSourceAdmissionWithBusyTail();
    TestPresentStartExecutors();
    TestEnforcementDuringGateAndCancellation();
    TestNativeRouteFromEveryMarker();
    TestFrameMarkerStageEvents();
    TestRenderStartHold();
    TestAsyncMarkerDriverHold();
    TestNativeEndpointCancellation();
    TestReflexCallForwarding();
    TestMarkersOptimizeOverride();
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
    if (argc == 2) ReplayDawnwalker(argv[1]);
    CHECK(g_dev.load() == nullptr);
    std::puts("feedback device identity/publication tests passed (production-linked; no live driver)");
    return 0;
}
