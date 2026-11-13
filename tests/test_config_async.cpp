// Windows/MSVC C++17 production-backed tests, NOT a copied writer model.
// Includes the actual ../src/config.cpp; CMake also links the real pacing
// pipeline resolver (pacing_resolver.cpp) that ApplyConfig configures.
// Requires src include directory, NOMINMAX, WIN32_LEAN_AND_MEAN, /utf-8,
// /permissive-, /W4, and the usual static test CRT. No production .cpp linkage,
// shell32, ReShade, or external libraries are needed (kernel32 is standard).
// Register a CTest timeout of 30 seconds. Run x64 AND Win32.
// Profile/shell I/O is entirely in memory; CreateThread/Wait/Close use real
// Windows handles. Gates replace sleeps and make interleavings deterministic.
// This validates config implementation and its caller retention contract, NOT
// actual AddonUninit/rollback ordering, filesystem durability, or frame pacing.

#include <Windows.h>
#include <ShlObj.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace config_test {
void Check(bool passed, const char* expression, int line) {
    if (passed) return;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    std::abort();
}
#define CHECK(expression) config_test::Check(!!(expression), #expression, __LINE__)

using Key = std::tuple<std::string, std::string, std::string>;
struct Write {
    Key key;
    std::string value;
    DWORD thread_id;
};
std::mutex mutex;
std::condition_variable changed;
std::map<Key, std::string> disk;
std::vector<Write> writes;
bool hold_start = true;
bool hold_io = false;
bool entered_io = false;
bool fail_create = false;
bool fail_writes = false;
bool throw_writes = false;
unsigned creates = 0;
unsigned closes = 0;
std::atomic<bool> logger_alive{ true };
std::atomic<unsigned> logger_shutdowns{ 0 };
std::atomic<unsigned> worker_logs{ 0 };
DWORD caller_thread = 0;
constexpr const char* local_path = "C:\\config-test\\ReLimiter.ini";
constexpr const char* shared_path = "C:\\config-test\\Local\\RHI\\ReLimiter_Presets\\presets.ini";

DWORD WINAPI ModulePath(HMODULE, LPSTR out, DWORD size) {
    return static_cast<DWORD>(snprintf(out, size, "%s", "C:\\config-test\\ReLimiter.addon64"));
}
// The INI exists once a test put keys on the fake disk (first launch otherwise).
DWORD WINAPI Attributes(LPCSTR path) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& entry : disk)
        if (std::get<0>(entry.first) == path) return FILE_ATTRIBUTE_NORMAL;
    return INVALID_FILE_ATTRIBUTES;
}
HRESULT WINAPI Folder(HWND, int, HANDLE, DWORD, LPSTR out) {
    strcpy(out, "C:\\config-test\\Local");
    return S_OK;
}
int WINAPI Directory(HWND, LPCSTR, const SECURITY_ATTRIBUTES*) { return ERROR_SUCCESS; }
DWORD WINAPI Read(LPCSTR section, LPCSTR key, LPCSTR fallback,
                  LPSTR out, DWORD size, LPCSTR path) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = disk.find({ path, section, key });
    const char* value = found == disk.end() ? fallback : found->second.c_str();
    const int length = snprintf(out, size, "%s", value ? value : "");
    return static_cast<DWORD>(length < static_cast<int>(size) ? length : size - 1);
}
UINT WINAPI ReadInt(LPCSTR section, LPCSTR key, INT fallback, LPCSTR path) {
    char out[64] = {};
    char text[64] = {};
    snprintf(text, sizeof(text), "%d", fallback);
    Read(section, key, text, out, sizeof(out), path);
    return static_cast<UINT>(atoi(out));
}
BOOL WINAPI WriteProfile(LPCSTR section, LPCSTR key, LPCSTR value, LPCSTR path) {
    std::unique_lock<std::mutex> lock(mutex);
    entered_io = true;
    changed.notify_all();
    CHECK(changed.wait_for(lock, std::chrono::seconds(10), [] { return !hold_io; }));
    writes.push_back({ { path, section, key ? key : "" },
                      value ? value : "", GetCurrentThreadId() });
    if (throw_writes) throw std::runtime_error("injected profile failure");
    if (fail_writes) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (!key) {
        for (auto it = disk.begin(); it != disk.end();) {
            if (std::get<0>(it->first) == path && std::get<1>(it->first) == section)
                it = disk.erase(it);
            else
                ++it;
        }
    } else {
        disk[{ path, section, key }] = value ? value : "";
    }
    return TRUE;
}

struct ThreadStart { LPTHREAD_START_ROUTINE procedure; LPVOID parameter; };
DWORD WINAPI GatedThread(void* context) {
    std::unique_ptr<ThreadStart> start(static_cast<ThreadStart*>(context));
    {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock, std::chrono::seconds(10), [] { return !hold_start; }));
    }
    return start->procedure(start->parameter);
}
HANDLE WINAPI Create(LPSECURITY_ATTRIBUTES attributes, SIZE_T stack,
                     LPTHREAD_START_ROUTINE procedure, LPVOID parameter,
                     DWORD flags, LPDWORD id) {
    ++creates;
    if (fail_create) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    auto start = std::make_unique<ThreadStart>(ThreadStart{ procedure, parameter });
    const HANDLE thread = CreateThread(attributes, stack, GatedThread, start.get(), flags, id);
    if (thread) start.release();
    return thread;
}
BOOL WINAPI Close(HANDLE handle) {
    // Closing before a successful join is a resource-retention regression.
    CHECK(WaitForSingleObject(handle, 0) == WAIT_OBJECT_0);
    ++closes;
    return CloseHandle(handle);
}
} // namespace config_test

// Headers are already included, so these substitutions affect config.cpp's
// callsites only. Keep all persistence, snapshot and worker policy unmodified.
#define GetModuleFileNameA config_test::ModulePath
#define GetFileAttributesA config_test::Attributes
#define SHGetFolderPathA config_test::Folder
#define SHCreateDirectoryExA config_test::Directory
#define GetPrivateProfileStringA config_test::Read
#define GetPrivateProfileIntA config_test::ReadInt
#define WritePrivateProfileStringA config_test::WriteProfile
#define CreateThread config_test::Create
#define CloseHandle config_test::Close
#include "../src/config.cpp"
#undef GetModuleFileNameA
#undef GetFileAttributesA
#undef SHGetFolderPathA
#undef SHCreateDirectoryExA
#undef GetPrivateProfileStringA
#undef GetPrivateProfileIntA
#undef WritePrivateProfileStringA
#undef CreateThread
#undef CloseHandle

// Unrelated runtime services: typed definitions against production headers.
std::atomic<int> g_user_target_fps{ 0 };
std::atomic<int> g_background_fps{ 0 };
std::atomic<int> g_fg_off_fps{ 0 };
#ifdef _WIN64
std::atomic<int> g_fg_mode{ 0 };
#endif
AdaptiveWakeGuard g_adaptive_wake_guard;
void OLEDCare_SetConfig(bool, int) {}
void VSync_ApplyOpenGL() {}
#ifdef _WIN64
void ReflexInject_SetEnabled(bool) noexcept {}
#endif
void Log_SetLevel(LogLevel) {}
LogLevel Log_ParseLevel(const char*) { return LogLevel::Warn; }
const char* Log_GetProcessName() { return "config-test"; }
// The Sleep-mode reconcile's key note (nvapi_hooks.cpp is not linked here).
void NvAPI_PrepareUnifiedKey(bool) noexcept {}
void Log_Write(LogLevel, const char*, ...) {
    CHECK(config_test::logger_alive.load());
    if (GetCurrentThreadId() != config_test::caller_thread) ++config_test::worker_logs;
}
bool Log_Shutdown() {
    CHECK(!s_config_writer);
    CHECK(!s_save_busy && s_pending_save.Empty() && s_failed_save.Empty());
    config_test::logger_alive = false;
    ++config_test::logger_shutdowns;
    return true;
}

namespace config_test {
// Same conditional ownership contract required of the actual addon caller.
// Source/integration tests must separately verify its real placement there.
bool OwnerShutdown(DWORD timeout = 5000) {
    if (!Config_Shutdown(timeout)) return false;
    return Log_Shutdown();
}
// unified_pacer is on by default while the unified pacer is tested (kPacingUnifiedPacerDefault); the
// tests of the other pacing keys compare the pipeline with the user's values that the key would
// force, so Reset seeds unified_pacer=false (the earlier pipeline) unless the test seeds the key
// itself or turns the seeding off (the default tests do).
bool seed_earlier_pipeline = true;
// One [FrameLimiter] key seeded into the fake local INI before LoadConfig.
struct IniEntry {
    const char* key;
    const char* value;
};
void Reset(bool creation_failure = false, bool initial_write_failure = false,
           const std::vector<IniEntry>& ini = {}) {
    CHECK(!s_config_writer); // Never discard state while a real thread exists.
    {
        std::lock_guard<std::mutex> lock(s_save_mutex);
        CHECK(!s_save_busy);
        // TEST FIXTURE ONLY: discard a previously asserted permanent failure.
        // Production intentionally has no API for discarding retained payloads.
        s_pending_save = {};
        s_failed_save = {};
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        disk.clear();
        for (const auto& entry : ini) disk[{ local_path, "FrameLimiter", entry.key }] = entry.value;
        const decltype(disk)::key_type unified_key{ local_path, "FrameLimiter", "unified_pacer" };
        if (seed_earlier_pipeline && disk.count(unified_key) == 0) disk[unified_key] = "false";
        writes.clear();
        hold_start = true;
        hold_io = false;
        entered_io = false;
        fail_create = creation_failure;
        fail_writes = initial_write_failure;
        throw_writes = false;
        creates = closes = 0;
    }
    logger_alive = true;
    logger_shutdowns = worker_logs = 0;
    g_config = {};
    PacingPipelineConfig stale{};
    stale.adaptive_cap = true; // LoadConfig must apply the loaded adaptive_cap
    PacingPipeline_SetConfig(stale);
    g_reflex_sleep_bypass.store(false);
    g_bypass_low_latency_off.store(true); // LoadConfig must apply the loaded value
    g_marker_forward_after_wait.store(true); // LoadConfig must apply the loaded order
    LoadConfig(nullptr);
    CHECK(creates == 1);
    CHECK((s_config_writer == nullptr) == creation_failure);
    // LoadConfig's synchronous initial save is intentional and separately
    // exercised here; subsequent assertions exclude those initialization writes.
    CHECK(!writes.empty());
    for (const auto& write : writes) CHECK(write.thread_id == caller_thread);
    std::lock_guard<std::mutex> lock(mutex);
    writes.clear();
    entered_io = false;
}
// Developer overrides other than unified_pacer, which Reset seeds false for the tests of the other keys.
int OverridesBesidesUnified() {
    return Config_DeveloperOverrides() - (g_config.unified_pacer != kPacingUnifiedPacerDefault ? 1 : 0);
}
// The developer reset returns unified_pacer to its default (on); the tests of the other keys read
// the earlier pipeline, so they put the key back to false (the seeding of Reset) afterwards.
void ResetDeveloperKeepingEarlierPipeline() {
    Config_ResetDeveloperSettings();
    char error[64] = {};
    if (seed_earlier_pipeline) CHECK(Config_SetLiveKey("unified_pacer", "false", error, sizeof(error)));
}
void ReleaseStart() {
    std::lock_guard<std::mutex> lock(mutex);
    hold_start = false;
    changed.notify_all();
}
void WaitForBlockedWrite() {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(changed.wait_for(lock, std::chrono::seconds(5), [] { return entered_io; }));
}
void ReleaseIO() {
    std::lock_guard<std::mutex> lock(mutex);
    hold_io = false;
    changed.notify_all();
}
void WaitForFailedBatch() {
    std::unique_lock<std::mutex> lock(s_save_mutex);
    CHECK(s_save_wakeup.wait_for(lock, std::chrono::seconds(5), [] {
        return s_pending_save.Empty() && !s_save_busy && !s_failed_save.Empty();
    }));
}
unsigned Attempts() {
    std::lock_guard<std::mutex> lock(mutex);
    unsigned count = 0;
    for (const auto& write : writes)
        if (std::get<2>(write.key) == "target_fps") ++count;
    return count;
}
std::string Value(const char* path, const char* section, const char* key) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = disk.find({ path, section, key });
    return found == disk.end() ? "" : found->second;
}
void CheckWorkerWrites() {
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(!writes.empty());
    for (const auto& write : writes) CHECK(write.thread_id != caller_thread);
}

// The resolver (which carries adaptive_cap to the period governor) holds
// exactly the live config.
void CheckPipelineMatchesConfig() {
    const PacingPipelineConfig pipeline = PacingPipeline_GetConfig();
    // Key unified_pacer (64-bit) forces the mechanisms the law replaces (PacingPipeline_ClampConfig);
    // g_config keeps the user's values and the pipeline holds the forced ones.
    // The owner is derived from the effective test mode (developer_settings gate) too: a test mode takes it from the key.
    const PacingTestMode mode = PacingPipeline_EffectiveTestMode(g_config.pacing_test_mode, g_config.developer_settings);
    CHECK(pipeline.test_mode == mode);
    const bool forced = g_config.unified_pacer && sizeof(void*) == 8 && mode == PacingTestMode::Off;
    CHECK(pipeline.smoothing == (forced ? PacingSmoothing::On : g_config.smoothing));
    CHECK(pipeline.rung_override == (forced ? PacingRungOverride::Auto : g_config.output_rung));
    CHECK(pipeline.adaptive_cap == (forced ? false : g_config.adaptive_cap));
    CHECK(pipeline.adaptive_cap_percentile == g_config.adaptive_cap_percentile);
    CHECK(pipeline.placement_override == g_config.source_placement);
    CHECK(pipeline.gpu_probes == g_config.gpu_present_gate_probes);
    CHECK(pipeline.stall_inject_ms == (forced ? 0.0 : g_config.stall_inject_ms));
    CHECK(pipeline.stall_inject_period_s == g_config.stall_inject_period_s);
    CHECK(pipeline.gpu_wait_interleave_s == (forced ? 0.0 : g_config.gpu_wait_interleave_s));
    CHECK(pipeline.gpu_queue_wait_render_queue ==
        (forced ? PacingRenderQueue::Keep : g_config.gpu_queue_wait_render_queue));
    CHECK(pipeline.lead_coverage == (forced ? PacingLeadCoverage::Q99 : g_config.lead_coverage));
    CHECK(pipeline.fg_late_spacing == (forced ? true : g_config.fg_late_spacing));
    CHECK(pipeline.gpu_paced_cadence == (forced ? false : g_config.gpu_paced_cadence));
    // Key unified_pacer forces the hold room off (config clamp), the user's value stays in g_config.
    CHECK(pipeline.adaptive_cap_hold_room ==
        (g_config.adaptive_cap_hold_room && !forced));
    CHECK(pipeline.unified_pacer == forced);
    CHECK(g_reflex_sleep_bypass.load() == PacingPipeline_EffectiveSleepBypass(g_config.reflex_sleep_bypass, mode));
    // Keys unified_every_frame_service and unified_queue_target_us: 64-bit only, the clamp forces
    // them off / to the default on Win32; the user's values stay in g_config.
    CHECK(pipeline.unified_every_frame_service == (g_config.unified_every_frame_service && sizeof(void*) == 8));
    CHECK(pipeline.unified_queue_target_us ==
        (sizeof(void*) == 8 ? g_config.unified_queue_target_us : kPacingUnifiedQueueTargetDefaultUs));
    // Keys unified_hold_share and unified_queue_rank reach the pipeline as the effective pair the pacing preference resolves
    // with them (g_config keeps the raw values); the hold share is 64-bit only, forced to the default (0) on Win32.
    const PacingPreferenceResolution effective = PacingPipeline_ResolvePreference(
        g_config.pacing_preference, g_config.unified_queue_rank, g_config.unified_hold_share);
    CHECK(pipeline.unified_hold_share == (sizeof(void*) == 8 ? effective.hold_share : 0.0));
    CHECK(pipeline.pre_sleep_precise == (forced ? false : g_config.pre_sleep_precise));
    CHECK(pipeline.driver_limiter_hold == (forced ? false : g_config.driver_limiter_hold));
    CHECK(g_driver_low_latency_off.load() == g_config.driver_low_latency_off);
    CHECK(g_sleep_status_every_frame.load() == g_config.sleep_status_every_frame);
    CHECK(g_gpu_start_probe.load() == g_config.gpu_start_probe);
    // The queue law forces the probe on and feeds the law while the unified pacer is on (64-bit only);
    // the probe key is a 64-bit measurement; the user's values stay in g_config.
    const bool is64 = sizeof(void*) == 8;
    const bool law_in_use = g_config.unified_queue_law && pipeline.unified_pacer;
    CHECK(g_gpu_queue_probe.load() == ((g_config.gpu_queue_probe && is64) || law_in_use));
    CHECK(g_gpu_queue_law_feed.load() == law_in_use);
    CHECK(pipeline.unified_queue_law == (g_config.unified_queue_law && is64));
    CHECK(pipeline.unified_queue_rank == effective.queue_rank);
    CHECK(pipeline.unified_queue_need_frames == g_config.unified_queue_need_frames);
    // The upstream stamping key: 64-bit only (0 on Win32), in the pipeline snapshot and the hook gate.
    CHECK(pipeline.unified_queue_upstream == (sizeof(void*) == 8 ? g_config.unified_queue_upstream : 0));
    CHECK(g_unified_queue_upstream.load() == (sizeof(void*) == 8 ? g_config.unified_queue_upstream : 0));
    CHECK(g_gpu_queue_probe_source.load() == (sizeof(void*) == 8 ? g_config.gpu_queue_probe_source : 0));
    CHECK(pipeline.gpu_queue_probe_source == (sizeof(void*) == 8 ? g_config.gpu_queue_probe_source : 0));
    CHECK(pipeline.unified_queue_margin_end == (sizeof(void*) == 8 ? g_config.unified_queue_margin_end : 0));
    CHECK(g_unified_queue_margin_end.load() == (sizeof(void*) == 8 ? g_config.unified_queue_margin_end : 0));
    CHECK(pipeline.unified_queue_allow_us == g_config.unified_queue_allow_us);
    CHECK(g_marker_forward_after_wait.load() ==
        (g_config.marker_forward_order == PacingMarkerForwardOrder::AfterWait));
}

void TestPacingPipelineDefaults() {
    seed_earlier_pipeline = false; // the fresh configuration, unified_pacer at its default
    Reset();
    CHECK(g_config.smoothing == PacingSmoothing::On);
    CHECK(g_config.output_rung == PacingRungOverride::Auto);
    CHECK(!g_config.adaptive_cap && g_config.gpu_present_gate_probes && !g_config.advanced_settings);
    CHECK(!g_config.developer_settings && OverridesBesidesUnified() == 0);
    CHECK(g_config.stall_inject_ms == 0.0 && g_config.stall_inject_period_s == 5.0);
    // Recommended defaults: the Reflex Sleep bypass and pre-Sleep placement.
    CHECK(g_config.reflex_sleep_bypass && g_reflex_sleep_bypass.load());
    CHECK(g_config.marker_forward_order == PacingMarkerForwardOrder::BeforeWait);
    CHECK(g_config.gpu_queue_wait_render_queue == PacingRenderQueue::Keep);
    CHECK(g_config.lead_coverage == PacingLeadCoverage::Q99);
    CHECK(g_config.fg_late_spacing);
    CHECK(!g_config.gpu_paced_cadence);
    CHECK(!g_config.bypass_low_latency_off && !g_bypass_low_latency_off.load());
    CHECK(!g_config.flip_metering_off);
    CHECK(!g_config.pre_sleep_precise && !g_config.driver_low_latency_off && !g_config.driver_limiter_hold &&
          !g_config.sleep_status_every_frame && !g_config.gpu_start_probe);
    // The queue probe, the queue law and the probe's source key are on by default (one constant each).
    CHECK(g_config.gpu_queue_probe && kPacingGpuQueueProbeDefault);
    // The unified pacer is on by default while it is tested (one constant); the pipeline holds it on 64-bit only.
    CHECK(kPacingUnifiedPacerDefault && g_config.unified_pacer);
    // The earlier law (false, 4000) is an explicit setting; the defaults are the every-frame estimate and 2000 µs.
    CHECK(g_config.unified_every_frame_service && kPacingUnifiedEveryFrameServiceDefault);
    CHECK(g_config.unified_queue_target_us == 2000.0 && kPacingUnifiedQueueTargetDefaultUs == 2000.0);
    CHECK(g_config.unified_hold_share == 0.0 && kPacingUnifiedHoldShareDefault == 0.0);
    CHECK(g_config.unified_queue_law && g_config.unified_queue_rank == 2 && g_config.unified_queue_allow_us == 4000);
    CHECK(kPacingUnifiedQueueLawDefault && kPacingUnifiedQueueRankDefault == 2 &&
          kPacingUnifiedQueueAllowDefaultUs == 4000);
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_law") == "true");
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_rank") == "2");
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_allow_us") == "4000");
    CHECK(g_config.unified_queue_need_frames == 4 && kPacingUnifiedQueueNeedFramesDefault == 4);
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_need_frames") == "4");
    CHECK(g_config.gpu_queue_probe_source == 1 && kPacingGpuQueueProbeSourceDefault == 1);
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_probe_source") == "1");
    CHECK(g_gpu_queue_probe_source.load() == (sizeof(void*) == 8 ? 1 : 0));
    CHECK(g_config.unified_queue_margin_end == 1 && kPacingUnifiedQueueMarginEndDefault == 1);
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_margin_end") == "1");
    CHECK(g_unified_queue_margin_end.load() == (sizeof(void*) == 8 ? 1 : 0));
    CHECK(g_config.unified_queue_upstream == 0 && kPacingUnifiedQueueUpstreamDefault == 0);
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_upstream") == "0");
    CHECK(g_unified_queue_upstream.load() == 0);
    CHECK(g_gpu_queue_law_feed.load() == (sizeof(void*) == 8) && g_gpu_queue_probe.load() == (sizeof(void*) == 8));
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_probe") == "true");
    CHECK(g_config.adaptive_cap_hold_room);
    CHECK(g_config.source_placement == PacingPlacementOverride::PreSleep);
    CheckPipelineMatchesConfig();
    // The startup rewrite saves every new key, including the developer keys.
    CHECK(Value(local_path, "FrameLimiter", "marker_forward_order") == "before_wait");
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_wait_render_queue") == "keep");
    CHECK(Value(local_path, "FrameLimiter", "lead_coverage") == "99");
    CHECK(Value(local_path, "FrameLimiter", "fg_late_spacing") == "true");
    CHECK(Value(local_path, "FrameLimiter", "gpu_paced_cadence") == "false");
    CHECK(Value(local_path, "FrameLimiter", "bypass_low_latency_off") == "false");
    CHECK(Value(local_path, "FrameLimiter", "flip_metering_off") == "false");
    for (const char* key : {"pre_sleep_precise", "driver_low_latency_off", "driver_limiter_hold",
            "sleep_status_every_frame", "gpu_start_probe"})
        CHECK(Value(local_path, "FrameLimiter", key) == "false");
    CHECK(Value(local_path, "FrameLimiter", "unified_pacer") == "true");
    CHECK(Value(local_path, "FrameLimiter", "unified_every_frame_service") == "true");
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_target_us") == "2000.000000");
    CHECK(Value(local_path, "FrameLimiter", "adaptive_cap_hold_room") == "true");
    CHECK(Value(local_path, "FrameLimiter", "source_placement") == "pre_sleep");
    CHECK(Value(local_path, "FrameLimiter", "reflex_sleep_bypass") == "true");
    CHECK(Value(local_path, "FrameLimiter", "developer_settings") == "false");
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "5");
    CHECK(Value(local_path, "FrameLimiter", "smoothing") == "on");
    CHECK(Value(local_path, "FrameLimiter", "adaptive_cap") == "false");
    CHECK(Value(local_path, "FrameLimiter", "output_rung") == "auto");
    CHECK(Value(local_path, "FrameLimiter", "stall_inject_ms") == "0.000000");
    CHECK(Value(local_path, "FrameLimiter", "stall_inject_period_s") == "5.000000");
    CHECK(Value(local_path, "FrameLimiter", "gpu_present_gate_probes") == "true");
    seed_earlier_pipeline = true;
    ReleaseStart();CHECK(OwnerShutdown());
}

// An INI from before config_version 2 saved the old defaults of the Reflex
// Sleep bypass (off) and source placement (marker): it moves to the new
// defaults once and keeps every other value; a version-2 INI keeps its choice.
// A version-2 INI (test builds) saved lead_coverage=95: it moves to 99 once;
// a version-3 INI keeps 95 as the developer A/B. A version-3 INI saved
// fg_late_spacing=false: it moves to true once; a version-4 INI keeps false.
void TestRecommendedDefaultsMigration() {
    Reset(false, false, {{"reflex_sleep_bypass", "false"}, {"source_placement", "marker"},
        {"gpu_wait_headroom_us", "2000"}, {"adaptive_cap", "true"}});
    CHECK(g_config.reflex_sleep_bypass && g_reflex_sleep_bypass.load());
    CHECK(g_config.source_placement == PacingPlacementOverride::PreSleep);
    CHECK(g_config.gpu_wait_headroom_us == 2000.0 && g_config.adaptive_cap);
    CheckPipelineMatchesConfig();
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "5");
    CHECK(Value(local_path, "FrameLimiter", "reflex_sleep_bypass") == "true");
    CHECK(Value(local_path, "FrameLimiter", "source_placement") == "pre_sleep");
    ReleaseStart();CHECK(OwnerShutdown());

    Reset(false, false, {{"config_version", "2"}, {"reflex_sleep_bypass", "false"},
        {"source_placement", "marker"}, {"lead_coverage", "95"}});
    CHECK(!g_config.reflex_sleep_bypass && !g_reflex_sleep_bypass.load());
    CHECK(g_config.source_placement == PacingPlacementOverride::Marker);
    CHECK(g_config.lead_coverage == PacingLeadCoverage::Q99);
    CheckPipelineMatchesConfig();
    CHECK(Value(local_path, "FrameLimiter", "lead_coverage") == "99");
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "5");
    ReleaseStart();CHECK(OwnerShutdown());

    Reset(false, false, {{"config_version", "3"}, {"lead_coverage", "95"}, {"fg_late_spacing", "false"}});
    CHECK(g_config.lead_coverage == PacingLeadCoverage::Q95 && g_config.fg_late_spacing);
    CheckPipelineMatchesConfig();
    CHECK(Value(local_path, "FrameLimiter", "lead_coverage") == "95");
    CHECK(Value(local_path, "FrameLimiter", "fg_late_spacing") == "true");
    ReleaseStart();CHECK(OwnerShutdown());

    Reset(false, false, {{"config_version", "4"}, {"fg_late_spacing", "false"}});
    CHECK(!g_config.fg_late_spacing);
    CHECK(Value(local_path, "FrameLimiter", "fg_late_spacing") == "false");
    ReleaseStart();CHECK(OwnerShutdown());
}

// An INI from before config_version 5 saved the old defaults of the queue law, the queue probe and the
// probe's source key (false, false, 0): it moves to the new defaults once and keeps every other value; a
// version-5 INI keeps its explicit choices; keys the INI does not carry take the new defaults; a first
// launch (no INI) takes the defaults and is written at version 5.
void TestQueueDefaultsMigration() {
    const bool is64 = sizeof(void*) == 8;
    // An INI of version 4 as the earlier build wrote it.
    Reset(false, false, {{"config_version", "4"}, {"unified_pacer", "true"}, {"unified_queue_law", "false"},
        {"gpu_queue_probe", "false"}, {"gpu_queue_probe_source", "0"}, {"unified_queue_rank", "3"},
        {"unified_queue_allow_us", "2500"}, {"target_fps", "100"}});
    CHECK(g_config.unified_queue_law && g_config.gpu_queue_probe && g_config.gpu_queue_probe_source == 1);
    CHECK(g_config.unified_queue_rank == 3 && g_config.unified_queue_allow_us == 2500 && g_config.target_fps == 100);
    CheckPipelineMatchesConfig();
    CHECK(g_gpu_queue_probe.load() == is64 && g_gpu_queue_law_feed.load() == is64);
    CHECK(g_gpu_queue_probe_source.load() == (is64 ? 1 : 0));
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "5");
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_law") == "true");
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_probe") == "true");
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_probe_source") == "1");
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_rank") == "3");
    ReleaseStart();CHECK(OwnerShutdown());

    // One key different is enough, and an INI without a version is older still.
    Reset(false, false, {{"unified_pacer", "true"}, {"gpu_queue_probe_source", "0"}});
    CHECK(g_config.unified_queue_law && g_config.gpu_queue_probe && g_config.gpu_queue_probe_source == 1);
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_probe_source") == "1");
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "5");
    ReleaseStart();CHECK(OwnerShutdown());

    // Keys the old INI does not carry take the new defaults.
    Reset(false, false, {{"config_version", "4"}, {"unified_pacer", "true"}});
    CHECK(g_config.unified_queue_law && g_config.gpu_queue_probe && g_config.gpu_queue_probe_source == 1);
    ReleaseStart();CHECK(OwnerShutdown());

    // Version 5: the explicit choices stay, saved as they are.
    Reset(false, false, {{"config_version", "5"}, {"unified_pacer", "true"}, {"unified_queue_law", "false"},
        {"gpu_queue_probe", "false"}, {"gpu_queue_probe_source", "0"}});
    CHECK(!g_config.unified_queue_law && !g_config.gpu_queue_probe && g_config.gpu_queue_probe_source == 0);
    CHECK(!g_gpu_queue_probe.load() && !g_gpu_queue_law_feed.load() && g_gpu_queue_probe_source.load() == 0);
    CheckPipelineMatchesConfig();
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_law") == "false");
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_probe") == "false");
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_probe_source") == "0");
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "5");
    CHECK(OverridesBesidesUnified() == 3);
    ReleaseStart();CHECK(OwnerShutdown());

    // A first launch (no INI at all) takes the defaults and writes version 5.
    seed_earlier_pipeline = false;
    Reset();
    CHECK(g_config.unified_queue_law && g_config.gpu_queue_probe && g_config.gpu_queue_probe_source == 1);
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "5");
    seed_earlier_pipeline = true;
    ReleaseStart();CHECK(OwnerShutdown());
}

// developer_settings is read and saved; every developer key away from its
// default is counted, and Reset returns exactly those keys to the defaults
// and applies them, leaving the user's settings alone.
void TestDeveloperSettingsReset() {
    Reset(false, false, {{"config_version", "2"}, {"developer_settings", "true"}, {"smoothing", "off"},
        {"output_rung", "gpu"}, {"gpu_wait_headroom_us", "2000"}, {"reflex_sleep_bypass", "false"},
        {"source_placement", "marker"}, {"adaptive_cap", "true"}, {"adaptive_cap_percentile", "90"},
        {"unified_pacer", "false"}});
    CHECK(g_config.developer_settings);
    CHECK(Value(local_path, "FrameLimiter", "developer_settings") == "true");
    CHECK(Config_DeveloperOverrides() == 6);
    Config_ResetDeveloperSettings();
    CHECK(Config_DeveloperOverrides() == 0);
    CHECK(g_config.smoothing == PacingSmoothing::On && g_config.output_rung == PacingRungOverride::Auto);
    CHECK(g_config.gpu_wait_headroom_us == kPacingGpuWaitHeadroomDefaultUs);
    CHECK(g_config.reflex_sleep_bypass && g_reflex_sleep_bypass.load());
    CHECK(g_config.source_placement == PacingPlacementOverride::PreSleep);
    CHECK(g_config.adaptive_cap && g_config.adaptive_cap_percentile == 90 && g_config.developer_settings);
    CHECK(g_config.unified_pacer == kPacingUnifiedPacerDefault); // the reset returns the key to its default
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());
}

void TestSmoothingParsing() {
    struct Case { const char* text; PacingSmoothing expected; };
    for (const Case& test : {Case{"off", PacingSmoothing::Off}, Case{"on", PacingSmoothing::On},
            Case{"Low", PacingSmoothing::On}, Case{"medium", PacingSmoothing::On},
            Case{"HIGH", PacingSmoothing::On}, Case{"extreme", PacingSmoothing::On},
            Case{"", PacingSmoothing::On}}) {
        Reset(false, false, {{"smoothing", test.text}});
        CHECK(g_config.smoothing == test.expected);
        CheckPipelineMatchesConfig();
        CHECK(Value(local_path, "FrameLimiter", "smoothing") == PacingSmoothing_Text(test.expected));
        ReleaseStart();CHECK(OwnerShutdown());
    }
    // Runtime edits are validated, applied and snapshotted at request time.
    Reset();
    g_config.smoothing = static_cast<PacingSmoothing>(9);
    g_config.output_rung = static_cast<PacingRungOverride>(9);
    ValidateConfig();
    CHECK(g_config.smoothing == PacingSmoothing::On && g_config.output_rung == PacingRungOverride::Auto);
    g_config.smoothing = PacingSmoothing::On;
    g_config.adaptive_cap = true;
    Config_ApplyPacingPipeline();
    CheckPipelineMatchesConfig();
    RequestSaveConfig();
    g_config.smoothing = PacingSmoothing::Off;
    g_config.adaptive_cap = false;
    ReleaseStart();CHECK(OwnerShutdown());
    CheckWorkerWrites();
    CHECK(Value(local_path, "FrameLimiter", "smoothing") == "on");
    CHECK(Value(local_path, "FrameLimiter", "adaptive_cap") == "true");
}

// source_placement parses case-insensitively, reads the legacy
// `auto` as marker, falls back to the pre_sleep default, saves canonical
// text and reaches the resolver configuration (version-2 INIs: no migration).
void TestSourcePlacementParsing() {
    struct Case { const char* text; PacingPlacementOverride expected; };
    for (const Case& test : {Case{"auto", PacingPlacementOverride::Marker},
            Case{"MARKER", PacingPlacementOverride::Marker},
            Case{"driver", PacingPlacementOverride::Driver},
            Case{"pre_sleep", PacingPlacementOverride::PreSleep},
            Case{"bogus", PacingPlacementOverride::PreSleep},
            Case{"", PacingPlacementOverride::PreSleep}}) {
        Reset(false, false, {{"config_version", "2"}, {"source_placement", test.text}});
        CHECK(g_config.source_placement == test.expected);
        CheckPipelineMatchesConfig();
        CHECK(Value(local_path, "FrameLimiter", "source_placement") ==
            PacingPlacementOverride_Text(test.expected));
        ReleaseStart();CHECK(OwnerShutdown());
    }
    Reset();
    g_config.source_placement = static_cast<PacingPlacementOverride>(7);
    ValidateConfig();
    CHECK(g_config.source_placement == PacingPlacementOverride::PreSleep);
    g_config.source_placement = PacingPlacementOverride::Marker;
    Config_ApplyPacingPipeline();
    CheckPipelineMatchesConfig();
    RequestSaveConfig();
    ReleaseStart();CHECK(OwnerShutdown());
    CheckWorkerWrites();
    CHECK(Value(local_path, "FrameLimiter", "source_placement") == "marker");
}

// marker_forward_order parses case-insensitively, falls back to
// before_wait, saves canonical text and reaches the marker hook's switch.
void TestMarkerForwardOrderParsing() {
    struct Case { const char* text; PacingMarkerForwardOrder expected; };
    for (const Case& test : {Case{"after_wait", PacingMarkerForwardOrder::AfterWait},
            Case{"AFTER_WAIT", PacingMarkerForwardOrder::AfterWait},
            Case{"before_wait", PacingMarkerForwardOrder::BeforeWait},
            Case{"later", PacingMarkerForwardOrder::BeforeWait},
            Case{"", PacingMarkerForwardOrder::BeforeWait}}) {
        Reset(false, false, {{"marker_forward_order", test.text}});
        CHECK(g_config.marker_forward_order == test.expected);
        CheckPipelineMatchesConfig();
        CHECK(Value(local_path, "FrameLimiter", "marker_forward_order") ==
            PacingMarkerForwardOrder_Text(test.expected));
        ReleaseStart();CHECK(OwnerShutdown());
    }
    // Runtime edits: out-of-range values validate to before_wait; the OSD
    // path (Config_ApplyPacingPipeline) switches the hook and saves the text.
    Reset();
    g_config.marker_forward_order = static_cast<PacingMarkerForwardOrder>(7);
    ValidateConfig();
    CHECK(g_config.marker_forward_order == PacingMarkerForwardOrder::BeforeWait);
    g_config.marker_forward_order = PacingMarkerForwardOrder::AfterWait;
    Config_ApplyPacingPipeline();
    CHECK(g_marker_forward_after_wait.load());
    RequestSaveConfig();
    ReleaseStart();CHECK(OwnerShutdown());
    CheckWorkerWrites();
    CHECK(Value(local_path, "FrameLimiter", "marker_forward_order") == "after_wait");
}

// Developer A/B gpu_queue_wait_render_queue: parses case-insensitively, falls
// back to keep, saves canonical text and reaches the pipeline configuration.
void TestRenderQueueParsing() {
    struct Case { const char* text; PacingRenderQueue expected; };
    for (const Case& test : {Case{"remove", PacingRenderQueue::Remove},
            Case{"REMOVE_HELD", PacingRenderQueue::RemoveHeld},
            Case{"keep", PacingRenderQueue::Keep},
            Case{"on", PacingRenderQueue::Keep},
            Case{"", PacingRenderQueue::Keep}}) {
        Reset(false, false, {{"gpu_queue_wait_render_queue", test.text}});
        CHECK(g_config.gpu_queue_wait_render_queue == test.expected);
        CheckPipelineMatchesConfig();
        CHECK(Value(local_path, "FrameLimiter", "gpu_queue_wait_render_queue") ==
            PacingRenderQueue_Text(test.expected));
        ReleaseStart();CHECK(OwnerShutdown());
    }
    // Runtime edits: out-of-range values validate to keep; the OSD path
    // (Config_ApplyPacingPipeline) publishes the setting and saves the text.
    Reset();
    g_config.gpu_queue_wait_render_queue = static_cast<PacingRenderQueue>(7);
    ValidateConfig();
    CHECK(g_config.gpu_queue_wait_render_queue == PacingRenderQueue::Keep);
    g_config.gpu_queue_wait_render_queue = PacingRenderQueue::RemoveHeld;
    Config_ApplyPacingPipeline();
    CHECK(PacingPipeline_GetConfig().gpu_queue_wait_render_queue == PacingRenderQueue::RemoveHeld);
    RequestSaveConfig();
    ReleaseStart();CHECK(OwnerShutdown());
    CheckWorkerWrites();
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_wait_render_queue") == "remove_held");
}

// Developer A/B lead_coverage: 95 loads (in a version-3 INI), reaches the
// pipeline, counts as a developer override and resets to 99; 50 loads and
// is saved as written; other text falls back to 99.
void TestLeadCoverageKey() {
    Reset(false, false, {{"config_version", "3"}, {"lead_coverage", "95"}});
    CHECK(g_config.lead_coverage == PacingLeadCoverage::Q95);
    CheckPipelineMatchesConfig();
    CHECK(Value(local_path, "FrameLimiter", "lead_coverage") == "95");
    CHECK(OverridesBesidesUnified() == 1);
    ResetDeveloperKeepingEarlierPipeline();
    CHECK(g_config.lead_coverage == PacingLeadCoverage::Q99);
    CHECK(PacingPipeline_GetConfig().lead_coverage == PacingLeadCoverage::Q99);
    ReleaseStart();CHECK(OwnerShutdown());
    Reset(false, false, {{"config_version", "4"}, {"lead_coverage", "50"}});
    CHECK(g_config.lead_coverage == PacingLeadCoverage::Q50);
    CheckPipelineMatchesConfig();
    CHECK(Value(local_path, "FrameLimiter", "lead_coverage") == "50");
    ReleaseStart();CHECK(OwnerShutdown());
    Reset(false, false, {{"config_version", "3"}, {"lead_coverage", "97"}});
    CHECK(g_config.lead_coverage == PacingLeadCoverage::Q99);
    CHECK(Value(local_path, "FrameLimiter", "lead_coverage") == "99");
    ReleaseStart();CHECK(OwnerShutdown());
}

// Developer A/B fg_late_spacing: false loads (in a version-4 INI), reaches the
// pipeline, counts as a developer override and resets to true.
void TestFgLateSpacingKey() {
    Reset(false, false, {{"config_version", "4"}, {"fg_late_spacing", "false"}});
    CHECK(!g_config.fg_late_spacing);
    CheckPipelineMatchesConfig();
    CHECK(Value(local_path, "FrameLimiter", "fg_late_spacing") == "false");
    CHECK(OverridesBesidesUnified() == 1);
    ResetDeveloperKeepingEarlierPipeline();
    CHECK(g_config.fg_late_spacing && PacingPipeline_GetConfig().fg_late_spacing);
    ReleaseStart();CHECK(OwnerShutdown());
}

// Developer A/B adaptive_cap_hold_room: false loads, reaches the pipeline, is
// saved, counts as a developer override and resets to true.
void TestAdaptiveCapHoldRoomKey() {
    Reset(false, false, {{"config_version", "4"}, {"adaptive_cap_hold_room", "false"}});
    CHECK(!g_config.adaptive_cap_hold_room);
    CheckPipelineMatchesConfig();
    CHECK(!PacingPipeline_GetConfig().adaptive_cap_hold_room);
    CHECK(Value(local_path, "FrameLimiter", "adaptive_cap_hold_room") == "false");
    CHECK(OverridesBesidesUnified() == 1);
    ResetDeveloperKeepingEarlierPipeline();
    CHECK(g_config.adaptive_cap_hold_room && PacingPipeline_GetConfig().adaptive_cap_hold_room);
    ReleaseStart();CHECK(OwnerShutdown());
}

// Developer A/B gpu_paced_cadence: true loads, reaches the pipeline, counts as
// a developer override and resets to false.
void TestGpuPacedCadenceKey() {
    Reset(false, false, {{"config_version", "4"}, {"gpu_paced_cadence", "true"}});
    CHECK(g_config.gpu_paced_cadence);
    CheckPipelineMatchesConfig();
    CHECK(PacingPipeline_GetConfig().gpu_paced_cadence);
    CHECK(Value(local_path, "FrameLimiter", "gpu_paced_cadence") == "true");
    CHECK(OverridesBesidesUnified() == 1);
    ResetDeveloperKeepingEarlierPipeline();
    CHECK(!g_config.gpu_paced_cadence && !PacingPipeline_GetConfig().gpu_paced_cadence);
    ReleaseStart();CHECK(OwnerShutdown());
}

// Developer A/B bypass_low_latency_off: true loads, reaches the NvAPI hooks'
// atomic, is saved, counts as a developer override and resets to false.
void TestBypassLowLatencyOffKey() {
    Reset(false, false, {{"config_version", "4"}, {"bypass_low_latency_off", "true"}});
    CHECK(g_config.bypass_low_latency_off && g_bypass_low_latency_off.load());
    CheckPipelineMatchesConfig();
    CHECK(Value(local_path, "FrameLimiter", "bypass_low_latency_off") == "true");
    CHECK(OverridesBesidesUnified() == 1);
    ResetDeveloperKeepingEarlierPipeline();
    CHECK(!g_config.bypass_low_latency_off && !g_bypass_low_latency_off.load());
    ReleaseStart();CHECK(OwnerShutdown());
}

// Developer flip_metering_off: true loads, is saved, counts as a developer
// override and resets to false (the block itself is installed at init only).
void TestFlipMeteringOffKey() {
    Reset(false, false, {{"config_version", "4"}, {"flip_metering_off", "true"}});
    CHECK(g_config.flip_metering_off);
    CHECK(Value(local_path, "FrameLimiter", "flip_metering_off") == "true");
    CHECK(OverridesBesidesUnified() == 1);
    ResetDeveloperKeepingEarlierPipeline();
    CHECK(!g_config.flip_metering_off);
    ReleaseStart();CHECK(OwnerShutdown());
}

// Developer measurement keys pre_sleep_precise, driver_low_latency_off,
// driver_limiter_hold, sleep_status_every_frame and gpu_start_probe: off by
// default, each loads, reaches its consumer (the pipeline or the atomics of the
// hooks and the GPU probe), is saved, counts as one developer override, is a
// live key and resets to false. (gpu_queue_probe is on by default: see TestUnifiedQueueLawKeys.)
void TestMeasurementKeys() {
    struct Case { const char* key; bool PacingPipelineConfig::*pipeline; bool Config::*field; };
    for (const Case& test : {Case{"pre_sleep_precise", &PacingPipelineConfig::pre_sleep_precise, &Config::pre_sleep_precise},
            Case{"driver_low_latency_off", nullptr, &Config::driver_low_latency_off},
            Case{"driver_limiter_hold", &PacingPipelineConfig::driver_limiter_hold, &Config::driver_limiter_hold},
            Case{"sleep_status_every_frame", nullptr, &Config::sleep_status_every_frame},
            Case{"gpu_start_probe", nullptr, &Config::gpu_start_probe}}) {
        Reset(false, false, {{"config_version", "4"}, {test.key, "true"}});
        CHECK(g_config.*test.field);
        // The key is 64-bit only: the clamp forces it false on Win32.
        if (test.pipeline && (sizeof(void*) == 8 ||
                (test.field != &Config::unified_pacer && test.field != &Config::unified_every_frame_service))) {
            CHECK(PacingPipeline_GetConfig().*test.pipeline);
        }
        CheckPipelineMatchesConfig();
        CHECK(g_driver_low_latency_off.load() == (std::strcmp(test.key, "driver_low_latency_off") == 0));
        CHECK(g_sleep_status_every_frame.load() == (std::strcmp(test.key, "sleep_status_every_frame") == 0));
        CHECK(g_gpu_start_probe.load() == (std::strcmp(test.key, "gpu_start_probe") == 0));
        CHECK(g_gpu_queue_probe.load() == (sizeof(void*) == 8)); // the default probe key, no law (earlier pipeline)
        CHECK(Value(local_path, "FrameLimiter", test.key) == "true");
        CHECK(OverridesBesidesUnified() == 1);
        // A live key: strict parse, published at once, not saved by the command.
        char error[128] = {};
        CHECK(Config_SetLiveKey(test.key, "off", error, sizeof(error)) && !(g_config.*test.field));
        CheckPipelineMatchesConfig();
        CHECK(OverridesBesidesUnified() == 0);
        CHECK(Config_SetLiveKey(test.key, "1", error, sizeof(error)) && g_config.*test.field);
        CheckPipelineMatchesConfig();
        CHECK(!Config_SetLiveKey(test.key, "maybe", error, sizeof(error)) && g_config.*test.field);
        ResetDeveloperKeepingEarlierPipeline();
        CHECK(!(g_config.*test.field));
        CheckPipelineMatchesConfig();
        CHECK(OverridesBesidesUnified() == 0);
        ReleaseStart();CHECK(OwnerShutdown());
    }
}

// unified_queue_target_us (developer, only read under unified_pacer): default 2000 µs, loads from
// the INI, is clamped to [0, 20000] by ValidateConfig (a non-finite INI value is the default),
// is saved, counts as one developer override only away from 2000 (4000 is the earlier law),
// is a live key (a non-finite or unparsable value is
// refused and leaves the field alone; an out-of-range value is clamped), reaches the pipeline
// (the default on Win32) and resets to 2000.
void TestUnifiedQueueTargetKey() {
    struct Case { const char* value; double expected; };
    for (const Case& test : {Case{"6000", 6000.0}, Case{"4000", 4000.0}, Case{"2000", 2000.0}, Case{"0", 0.0},
            Case{"20000", 20000.0}, Case{"25000", 20000.0}, Case{"-5", 0.0}, Case{"inf", 2000.0}}) {
        Reset(false, false, {{"config_version", "4"}, {"unified_queue_target_us", test.value}});
        CHECK(g_config.unified_queue_target_us == test.expected);
        CheckPipelineMatchesConfig();
        CHECK(PacingPipeline_GetConfig().unified_queue_target_us ==
            (sizeof(void*) == 8 ? test.expected : kPacingUnifiedQueueTargetDefaultUs));
        CHECK(OverridesBesidesUnified() == (test.expected != 2000.0 ? 1 : 0));
        char saved[32] = {};
        snprintf(saved, sizeof(saved), "%.6f", test.expected);
        CHECK(Value(local_path, "FrameLimiter", "unified_queue_target_us") == saved);
        ReleaseStart();CHECK(OwnerShutdown());
    }
    // "nan" loads as some finite value inside the range, whatever the C library's atof gives.
    Reset(false, false, {{"config_version", "4"}, {"unified_queue_target_us", "nan"}});
    CHECK(std::isfinite(g_config.unified_queue_target_us) && g_config.unified_queue_target_us >= 0.0 &&
          g_config.unified_queue_target_us <= kPacingUnifiedQueueTargetMaxUs);
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());

    // Live key and ValidateConfig for a value set directly.
    Reset(false, false, {{"config_version", "4"}});
    char error[128] = {};
    CHECK(Config_SetLiveKey("unified_queue_target_us", "7500", error, sizeof(error)));
    CHECK(g_config.unified_queue_target_us == 7500.0 && OverridesBesidesUnified() == 1);
    CheckPipelineMatchesConfig();
    CHECK(PacingPipeline_GetConfig().unified_queue_target_us ==
        (sizeof(void*) == 8 ? 7500.0 : kPacingUnifiedQueueTargetDefaultUs));
    CHECK(Config_SetLiveKey("unified_queue_target_us", "99999", error, sizeof(error)));
    CHECK(g_config.unified_queue_target_us == 20000.0);
    CHECK(Config_SetLiveKey("unified_queue_target_us", "-1", error, sizeof(error)));
    CHECK(g_config.unified_queue_target_us == 0.0);
    CheckPipelineMatchesConfig();
    CHECK(Config_SetLiveKey("unified_queue_target_us", "2500", error, sizeof(error)));
    // Invalid values are refused and leave the field alone (NaN and infinity never parse).
    for (const char* bad : {"nan", "inf", "-inf", "abc", "12ms"}) {
        CHECK(!Config_SetLiveKey("unified_queue_target_us", bad, error, sizeof(error)) && error[0] != '\0');
        CHECK(g_config.unified_queue_target_us == 2500.0);
        CheckPipelineMatchesConfig();
    }
    g_config.unified_queue_target_us = std::numeric_limits<double>::quiet_NaN();
    ValidateConfig();
    CHECK(g_config.unified_queue_target_us == 2000.0);
    g_config.unified_queue_target_us = 1e9;
    ValidateConfig();
    CHECK(g_config.unified_queue_target_us == 20000.0);
    g_config.unified_queue_target_us = 3000.0;
    ResetDeveloperKeepingEarlierPipeline();
    CHECK(g_config.unified_queue_target_us == 2000.0 && OverridesBesidesUnified() == 0);
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());
}

// unified_pacer (developer, on by default while it is tested: kPacingUnifiedPacerDefault): the
// default is on with no developer override, the pipeline holds it on 64-bit only (Win32 forces it
// false without the override count, the reset or the warning seeing a deviation: they compare
// g_config with its default), false in the INI is the earlier pipeline and one developer override,
// it is a live key and the reset returns it to the default.
void TestUnifiedPacerKey() {
    const bool wide = sizeof(void*) == 8;
    seed_earlier_pipeline = false;
    Reset(false, false, {{"config_version", "4"}});
    seed_earlier_pipeline = true;
    CHECK(g_config.unified_pacer == kPacingUnifiedPacerDefault && kPacingUnifiedPacerDefault);
    CHECK(Config_DeveloperOverrides() == 0);
    CHECK(PacingPipeline_GetConfig().unified_pacer == wide);
    CHECK(PacingPipeline_UnifiedPacerKey() == wide);
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());
    // A fresh INI without the key reads the default; false is the earlier pipeline.
    Reset(false, false, {{"config_version", "4"}, {"unified_pacer", "false"}});
    CHECK(!g_config.unified_pacer && Config_DeveloperOverrides() == 1);
    CHECK(!PacingPipeline_GetConfig().unified_pacer && !PacingPipeline_UnifiedPacerKey());
    CHECK(Value(local_path, "FrameLimiter", "unified_pacer") == "false");
    CheckPipelineMatchesConfig();
    char error[128] = {};
    CHECK(Config_SetLiveKey("unified_pacer", "on", error, sizeof(error)));
    CHECK(g_config.unified_pacer && Config_DeveloperOverrides() == 0);
    CHECK(PacingPipeline_GetConfig().unified_pacer == wide);
    CheckPipelineMatchesConfig();
    CHECK(Config_SetLiveKey("unified_pacer", "0", error, sizeof(error)));
    CHECK(!g_config.unified_pacer && Config_DeveloperOverrides() == 1);
    CHECK(!PacingPipeline_GetConfig().unified_pacer);
    CHECK(!Config_SetLiveKey("unified_pacer", "maybe", error, sizeof(error)) && !g_config.unified_pacer);
    Config_ResetDeveloperSettings();
    CHECK(g_config.unified_pacer == kPacingUnifiedPacerDefault && Config_DeveloperOverrides() == 0);
    CHECK(PacingPipeline_GetConfig().unified_pacer == wide);
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());
}

// unified_every_frame_service (developer, only read under unified_pacer): on by default; off is the
// earlier estimator and counts as one developer override. Loads, is saved, is a live key, reaches the
// pipeline (forced false on Win32, where the unified pacer does not exist) and resets to on.
void TestUnifiedEveryFrameServiceKey() {
    const bool wide = sizeof(void*) == 8;
    Reset(false, false, {{"config_version", "4"}});
    CHECK(g_config.unified_every_frame_service && OverridesBesidesUnified() == 0);
    CHECK(PacingPipeline_GetConfig().unified_every_frame_service == wide);
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());
    Reset(false, false, {{"config_version", "4"}, {"unified_every_frame_service", "false"}});
    CHECK(!g_config.unified_every_frame_service && OverridesBesidesUnified() == 1);
    CHECK(!PacingPipeline_GetConfig().unified_every_frame_service);
    CheckPipelineMatchesConfig();
    CHECK(Value(local_path, "FrameLimiter", "unified_every_frame_service") == "false");
    char error[128] = {};
    CHECK(Config_SetLiveKey("unified_every_frame_service", "on", error, sizeof(error)));
    CHECK(g_config.unified_every_frame_service && OverridesBesidesUnified() == 0);
    CHECK(PacingPipeline_GetConfig().unified_every_frame_service == wide);
    CheckPipelineMatchesConfig();
    CHECK(Config_SetLiveKey("unified_every_frame_service", "0", error, sizeof(error)));
    CHECK(!g_config.unified_every_frame_service && OverridesBesidesUnified() == 1);
    CHECK(!PacingPipeline_GetConfig().unified_every_frame_service);
    CHECK(!Config_SetLiveKey("unified_every_frame_service", "maybe", error, sizeof(error)) &&
          !g_config.unified_every_frame_service);
    ResetDeveloperKeepingEarlierPipeline();
    CHECK(g_config.unified_every_frame_service && OverridesBesidesUnified() == 0);
    CHECK(PacingPipeline_GetConfig().unified_every_frame_service == wide);
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());
}

// unified_hold_share (developer, only read under unified_pacer): default 0 (off, no override), loads
// from the INI, is clamped to [0, 0.25] (a non-finite value is 0), is saved, counts as one developer
// override when above 0, is a live key (unparsable and non-finite values are refused, an
// out-of-range one is clamped), reaches the pipeline (0 on Win32) and resets to 0.
void TestUnifiedHoldShareKey() {
    const bool wide = sizeof(void*) == 8;
    struct Case { const char* value; double expected; };
    for (const Case& test : {Case{"0.1", 0.1}, Case{"0.25", 0.25}, Case{"0", 0.0}, Case{"1", 0.25},
            Case{"0.98", 0.25}, Case{"-0.5", 0.0}, Case{"inf", 0.0}}) {
        Reset(false, false, {{"config_version", "4"}, {"unified_hold_share", test.value}});
        CHECK(g_config.unified_hold_share == test.expected);
        CheckPipelineMatchesConfig();
        CHECK(PacingPipeline_GetConfig().unified_hold_share == (wide ? test.expected : 0.0));
        CHECK(OverridesBesidesUnified() == (test.expected != 0.0 ? 1 : 0));
        char saved[32] = {};
        snprintf(saved, sizeof(saved), "%.6f", test.expected);
        CHECK(Value(local_path, "FrameLimiter", "unified_hold_share") == saved);
        ReleaseStart();CHECK(OwnerShutdown());
    }
    // "nan" loads as some finite value inside the range, whatever the C library's atof gives.
    Reset(false, false, {{"config_version", "4"}, {"unified_hold_share", "nan"}});
    CHECK(std::isfinite(g_config.unified_hold_share) && g_config.unified_hold_share >= 0.0 &&
          g_config.unified_hold_share <= kPacingUnifiedHoldShareMax);
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());

    Reset(false, false, {{"config_version", "4"}});
    CHECK(g_config.unified_hold_share == 0.0 && OverridesBesidesUnified() == 0);
    char error[128] = {};
    CHECK(Config_SetLiveKey("unified_hold_share", "0.2", error, sizeof(error)));
    CHECK(g_config.unified_hold_share == 0.2 && OverridesBesidesUnified() == 1);
    CheckPipelineMatchesConfig();
    CHECK(PacingPipeline_GetConfig().unified_hold_share == (wide ? 0.2 : 0.0));
    CHECK(Config_SetLiveKey("unified_hold_share", "5", error, sizeof(error)) && g_config.unified_hold_share == 0.25);
    CHECK(Config_SetLiveKey("unified_hold_share", "-1", error, sizeof(error)) && g_config.unified_hold_share == 0.0);
    CHECK(Config_SetLiveKey("unified_hold_share", "0.05", error, sizeof(error)));
    for (const char* bad : {"nan", "inf", "-inf", "abc", "0.05x"}) {
        CHECK(!Config_SetLiveKey("unified_hold_share", bad, error, sizeof(error)) && error[0] != '\0');
        CHECK(g_config.unified_hold_share == 0.05);
        CheckPipelineMatchesConfig();
    }
    g_config.unified_hold_share = std::numeric_limits<double>::quiet_NaN();
    ValidateConfig();
    CHECK(g_config.unified_hold_share == 0.0);
    g_config.unified_hold_share = 7.0;
    ValidateConfig();
    CHECK(g_config.unified_hold_share == 0.25);
    g_config.unified_hold_share = 0.2;
    ResetDeveloperKeepingEarlierPipeline();
    CHECK(g_config.unified_hold_share == 0.0 && OverridesBesidesUnified() == 0);
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());
}

// unified_queue_law, unified_queue_rank, unified_queue_allow_us (developer, only read under
// unified_pacer): the law is on by default and the numbers 2 and 4000; they load, are clamped
// (rank 1..8, allowance 0..20000), are saved, count as developer overrides only away from their
// defaults, are live keys (an unparsable value is refused, an out-of-range one clamped), reach the
// pipeline (the law is forced off on Win32), the law forces the probe atomic on and sets the feed
// atomic (64-bit, while the unified pacer is on), and reset to the defaults. The probe key is on by
// default too; the earlier pipeline (unified_pacer false) neither feeds nor needs the law.
void TestUnifiedQueueLawKeys() {
    const bool wide = sizeof(void*) == 8;
    // Defaults with the unified pacer on: the law and the probe published, no override.
    Reset(false, false, {{"config_version", "5"}, {"unified_pacer", "true"}});
    CHECK(g_config.unified_queue_law && g_config.gpu_queue_probe && OverridesBesidesUnified() == 0);
    CHECK(PacingPipeline_GetConfig().unified_queue_law == wide);
    CHECK(g_gpu_queue_probe.load() == wide && g_gpu_queue_law_feed.load() == wide);
    CheckPipelineMatchesConfig();
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_law") == "true");
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_probe") == "true");
    ReleaseStart();CHECK(OwnerShutdown());
    // The earlier pipeline: the law is not in use, nothing is fed; the probe key alone keeps the probe on.
    Reset(false, false, {{"config_version", "5"}});
    CHECK(g_config.unified_queue_law && !g_gpu_queue_law_feed.load() && g_gpu_queue_probe.load() == wide);
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());
    // The law and the probe key off from the INI: two overrides, nothing published.
    Reset(false, false, {{"config_version", "5"}, {"unified_pacer", "true"}, {"unified_queue_law", "false"},
        {"gpu_queue_probe", "false"}});
    CHECK(!g_config.unified_queue_law && !g_config.gpu_queue_probe && OverridesBesidesUnified() == 2);
    CHECK(!PacingPipeline_GetConfig().unified_queue_law);
    CHECK(!g_gpu_queue_probe.load() && !g_gpu_queue_law_feed.load());
    CheckPipelineMatchesConfig();
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_law") == "false");
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_probe") == "false");
    char error[128] = {};
    // The law forces the probe on although the user's probe key is off, and feeds it.
    CHECK(Config_SetLiveKey("unified_queue_law", "on", error, sizeof(error)) && g_config.unified_queue_law);
    CHECK(g_gpu_queue_probe.load() == wide && g_gpu_queue_law_feed.load() == wide && !g_config.gpu_queue_probe);
    CheckPipelineMatchesConfig();
    CHECK(Config_SetLiveKey("unified_queue_law", "0", error, sizeof(error)) && !g_config.unified_queue_law);
    CHECK(!g_gpu_queue_probe.load() && !g_gpu_queue_law_feed.load());
    CheckPipelineMatchesConfig();
    // The user's own probe key survives the law switching off.
    CHECK(Config_SetLiveKey("gpu_queue_probe", "on", error, sizeof(error)));
    CHECK(g_gpu_queue_probe.load() == wide && !g_gpu_queue_law_feed.load());
    CHECK(Config_SetLiveKey("unified_queue_law", "1", error, sizeof(error)));
    CHECK(g_gpu_queue_probe.load() == wide && g_gpu_queue_law_feed.load() == wide);
    CHECK(Config_SetLiveKey("unified_queue_law", "0", error, sizeof(error)));
    CHECK(g_gpu_queue_probe.load() == wide && !g_gpu_queue_law_feed.load());
    CHECK(!Config_SetLiveKey("unified_queue_law", "maybe", error, sizeof(error)) && !g_config.unified_queue_law);
    CHECK(!Config_SetLiveKey("gpu_queue_probe", "maybe", error, sizeof(error)) && g_config.gpu_queue_probe);
    // The developer reset returns both to on.
    Config_ResetDeveloperSettings();
    CHECK(g_config.unified_queue_law && g_config.gpu_queue_probe && OverridesBesidesUnified() == 0);
    CHECK(g_gpu_queue_probe.load() == wide && g_gpu_queue_law_feed.load() == wide);
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());

    struct Case { const char* value; int expected; };
    for (const Case& test : {Case{"1", 1}, Case{"8", 8}, Case{"4", 4}, Case{"2", 2}, Case{"0", 1}, Case{"-3", 1},
            Case{"9", 8}, Case{"500", 8}}) {
        Reset(false, false, {{"config_version", "4"}, {"unified_queue_rank", test.value}});
        CHECK(g_config.unified_queue_rank == test.expected);
        CheckPipelineMatchesConfig();
        CHECK(PacingPipeline_GetConfig().unified_queue_rank == test.expected);
        CHECK(OverridesBesidesUnified() == (test.expected != 2 ? 1 : 0));
        char saved[32] = {};
        snprintf(saved, sizeof(saved), "%d", test.expected);
        CHECK(Value(local_path, "FrameLimiter", "unified_queue_rank") == saved);
        ReleaseStart();CHECK(OwnerShutdown());
    }
    for (const Case& test : {Case{"0", 0}, Case{"1500", 1500}, Case{"4000", 4000}, Case{"20000", 20000},
            Case{"20001", 20000}, Case{"999999", 20000}, Case{"-1", 0}}) {
        Reset(false, false, {{"config_version", "4"}, {"unified_queue_allow_us", test.value}});
        CHECK(g_config.unified_queue_allow_us == test.expected);
        CheckPipelineMatchesConfig();
        CHECK(PacingPipeline_GetConfig().unified_queue_allow_us == test.expected);
        CHECK(OverridesBesidesUnified() == (test.expected != 4000 ? 1 : 0));
        char saved[32] = {};
        snprintf(saved, sizeof(saved), "%d", test.expected);
        CHECK(Value(local_path, "FrameLimiter", "unified_queue_allow_us") == saved);
        ReleaseStart();CHECK(OwnerShutdown());
    }

    Reset(false, false, {{"config_version", "4"}});
    CHECK(Config_SetLiveKey("unified_queue_rank", "5", error, sizeof(error)) && g_config.unified_queue_rank == 5);
    CHECK(Config_SetLiveKey("unified_queue_rank", "50", error, sizeof(error)) && g_config.unified_queue_rank == 8);
    CHECK(Config_SetLiveKey("unified_queue_rank", "3", error, sizeof(error)));
    CHECK(Config_SetLiveKey("unified_queue_allow_us", "2500", error, sizeof(error)) &&
          g_config.unified_queue_allow_us == 2500);
    CHECK(Config_SetLiveKey("unified_queue_allow_us", "30000", error, sizeof(error)) &&
          g_config.unified_queue_allow_us == 20000);
    CHECK(Config_SetLiveKey("unified_queue_allow_us", "2500", error, sizeof(error)));
    CheckPipelineMatchesConfig();
    CHECK(OverridesBesidesUnified() == 2);
    for (const char* bad : {"abc", "2x", ""}) {
        CHECK(!Config_SetLiveKey("unified_queue_rank", bad, error, sizeof(error)) && g_config.unified_queue_rank == 3);
        CHECK(!Config_SetLiveKey("unified_queue_allow_us", bad, error, sizeof(error)) &&
              g_config.unified_queue_allow_us == 2500);
    }
    g_config.unified_queue_rank = 40;
    g_config.unified_queue_allow_us = -9;
    ValidateConfig();
    CHECK(g_config.unified_queue_rank == 8 && g_config.unified_queue_allow_us == 0);
    g_config.unified_queue_rank = -2;
    ValidateConfig();
    CHECK(g_config.unified_queue_rank == 1);
    ResetDeveloperKeepingEarlierPipeline();
    CHECK(g_config.unified_queue_rank == 2 && g_config.unified_queue_allow_us == 4000 &&
          g_config.unified_queue_law && OverridesBesidesUnified() == 0);
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());
}

// unified_queue_need_frames (developer, only read with unified_queue_law): default 4; loads, is
// clamped to 1..16, is saved, counts as one developer override only away from 4, is a live key (an
// unparsable value is refused, an out-of-range one clamped), reaches the pipeline on both
// architectures (it has no effect without the law) and resets to 4.
void TestUnifiedQueueNeedFramesKey() {
    struct Case { const char* value; int expected; };
    for (const Case& test : {Case{"1", 1}, Case{"4", 4}, Case{"16", 16}, Case{"8", 8}, Case{"0", 1},
            Case{"-7", 1}, Case{"17", 16}, Case{"1000", 16}}) {
        Reset(false, false, {{"config_version", "4"}, {"unified_queue_need_frames", test.value}});
        CHECK(g_config.unified_queue_need_frames == test.expected);
        CheckPipelineMatchesConfig();
        CHECK(PacingPipeline_GetConfig().unified_queue_need_frames == test.expected);
        CHECK(OverridesBesidesUnified() == (test.expected != 4 ? 1 : 0));
        char saved[32] = {};
        snprintf(saved, sizeof(saved), "%d", test.expected);
        CHECK(Value(local_path, "FrameLimiter", "unified_queue_need_frames") == saved);
        ReleaseStart();CHECK(OwnerShutdown());
    }
    Reset(false, false, {{"config_version", "4"}});
    char error[128] = {};
    CHECK(Config_SetLiveKey("unified_queue_need_frames", "6", error, sizeof(error)) &&
          g_config.unified_queue_need_frames == 6);
    CHECK(OverridesBesidesUnified() == 1);
    CHECK(Config_SetLiveKey("unified_queue_need_frames", "99", error, sizeof(error)) &&
          g_config.unified_queue_need_frames == 16);
    CHECK(Config_SetLiveKey("unified_queue_need_frames", "-1", error, sizeof(error)) &&
          g_config.unified_queue_need_frames == 1);
    CHECK(Config_SetLiveKey("unified_queue_need_frames", "3", error, sizeof(error)));
    CheckPipelineMatchesConfig();
    for (const char* bad : {"abc", "2x", ""}) {
        CHECK(!Config_SetLiveKey("unified_queue_need_frames", bad, error, sizeof(error)) &&
              g_config.unified_queue_need_frames == 3);
    }
    g_config.unified_queue_need_frames = 40;
    ValidateConfig();
    CHECK(g_config.unified_queue_need_frames == 16);
    g_config.unified_queue_need_frames = 0;
    ValidateConfig();
    CHECK(g_config.unified_queue_need_frames == 1);
    ResetDeveloperKeepingEarlierPipeline();
    CHECK(g_config.unified_queue_need_frames == 4 && OverridesBesidesUnified() == 0);
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());
}

// unified_queue_upstream (developer): default 0 (off); loads, is clamped to 0..1 (the removed modes 2
// and 3 of an older INI load as 1, silently), is saved, counts as one developer override only away
// from 0, is a live key (an unparsable value is refused, an out-of-range one clamped), reaches the
// pipeline snapshot and the hook gate atomic (64-bit; 0 on Win32) and resets to 0. The removed keys
// unified_queue_upstream_lead_us and unified_queue_upstream_feed_permille of an older INI are ignored.
void TestUnifiedQueueUpstreamKey() {
    const bool wide = sizeof(void*) == 8;
    CHECK(kPacingUnifiedQueueUpstreamMax == 1);
    struct Case { const char* value; int expected; };
    for (const Case& test : {Case{"0", 0}, Case{"1", 1}, Case{"2", 1}, Case{"3", 1}, Case{"4", 1}, Case{"99", 1},
            Case{"-1", 0}, Case{"-50", 0}}) {
        Reset(false, false, {{"config_version", "4"}, {"unified_queue_upstream", test.value}});
        CHECK(g_config.unified_queue_upstream == test.expected);
        CheckPipelineMatchesConfig();
        CHECK(PacingPipeline_GetConfig().unified_queue_upstream == (wide ? test.expected : 0));
        CHECK(g_unified_queue_upstream.load() == (wide ? test.expected : 0));
        CHECK(OverridesBesidesUnified() == (test.expected != 0 ? 1 : 0));
        char saved[32] = {};
        snprintf(saved, sizeof(saved), "%d", test.expected);
        CHECK(Value(local_path, "FrameLimiter", "unified_queue_upstream") == saved);
        ReleaseStart();CHECK(OwnerShutdown());
    }
    // An INI that still holds the removed keys loads as without them.
    Reset(false, false, {{"config_version", "4"}, {"unified_queue_upstream_lead_us", "1500"},
        {"unified_queue_upstream_feed_permille", "300"}});
    CHECK(g_config.unified_queue_upstream == 0 && OverridesBesidesUnified() == 0);
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());
    Reset(false, false, {{"config_version", "4"}});
    CHECK(g_unified_queue_upstream.load() == 0);
    char error[128] = {};
    CHECK(Config_SetLiveKey("unified_queue_upstream", "1", error, sizeof(error)) &&
          g_config.unified_queue_upstream == 1);
    CHECK(g_unified_queue_upstream.load() == (wide ? 1 : 0) && OverridesBesidesUnified() == 1);
    CHECK(Config_SetLiveKey("unified_queue_upstream", "7", error, sizeof(error)) &&
          g_config.unified_queue_upstream == 1);
    CHECK(Config_SetLiveKey("unified_queue_upstream", "2", error, sizeof(error)) &&
          g_config.unified_queue_upstream == 1);
    CHECK(g_unified_queue_upstream.load() == (wide ? 1 : 0));
    CHECK(Config_SetLiveKey("unified_queue_upstream", "0", error, sizeof(error)) &&
          g_unified_queue_upstream.load() == 0);
    CHECK(Config_SetLiveKey("unified_queue_upstream", "3", error, sizeof(error)) &&
          g_config.unified_queue_upstream == 1);
    CheckPipelineMatchesConfig();
    for (const char* bad : {"abc", "1x", ""}) {
        CHECK(!Config_SetLiveKey("unified_queue_upstream", bad, error, sizeof(error)) &&
              g_config.unified_queue_upstream == 1);
    }
    g_config.unified_queue_upstream = 9;
    ValidateConfig();
    CHECK(g_config.unified_queue_upstream == 1);
    g_config.unified_queue_upstream = -3;
    ValidateConfig();
    CHECK(g_config.unified_queue_upstream == 0);
    g_config.unified_queue_upstream = 1;
    ResetDeveloperKeepingEarlierPipeline();
    CHECK(g_config.unified_queue_upstream == 0 && g_unified_queue_upstream.load() == 0 &&
          OverridesBesidesUnified() == 0);
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());
}

// gpu_queue_probe_source (developer): default 1; loads, is clamped to 0..1, is saved, counts as one
// developer override only away from 1, is a live key (an unparsable value is refused, an
// out-of-range one clamped), reaches the pipeline snapshot and the probe's atomic (64-bit; 0 on
// Win32; the probe key itself is independent) and resets to 1.
void TestGpuQueueProbeSourceKey() {
    const bool wide = sizeof(void*) == 8;
    struct Case { const char* value; int expected; };
    for (const Case& test : {Case{"0", 0}, Case{"1", 1}, Case{"2", 1}, Case{"99", 1}, Case{"-1", 0}, Case{"-9", 0}}) {
        Reset(false, false, {{"config_version", "5"}, {"gpu_queue_probe_source", test.value}});
        CHECK(g_config.gpu_queue_probe_source == test.expected);
        CheckPipelineMatchesConfig();
        CHECK(PacingPipeline_GetConfig().gpu_queue_probe_source == (wide ? test.expected : 0));
        CHECK(g_gpu_queue_probe_source.load() == (wide ? test.expected : 0));
        CHECK(g_config.gpu_queue_probe);
        CHECK(OverridesBesidesUnified() == (test.expected != 1 ? 1 : 0));
        char saved[32] = {};
        snprintf(saved, sizeof(saved), "%d", test.expected);
        CHECK(Value(local_path, "FrameLimiter", "gpu_queue_probe_source") == saved);
        ReleaseStart();CHECK(OwnerShutdown());
    }
    Reset(false, false, {{"config_version", "5"}});
    CHECK(g_gpu_queue_probe_source.load() == (wide ? 1 : 0));
    char error[128] = {};
    CHECK(Config_SetLiveKey("gpu_queue_probe_source", "0", error, sizeof(error)) &&
          g_config.gpu_queue_probe_source == 0);
    CHECK(g_gpu_queue_probe_source.load() == 0 && OverridesBesidesUnified() == 1);
    CHECK(Config_SetLiveKey("gpu_queue_probe_source", "5", error, sizeof(error)) &&
          g_config.gpu_queue_probe_source == 1);
    CHECK(g_gpu_queue_probe_source.load() == (wide ? 1 : 0));
    CHECK(Config_SetLiveKey("gpu_queue_probe_source", "0", error, sizeof(error)));
    CheckPipelineMatchesConfig();
    for (const char* bad : {"abc", "1x", ""}) {
        CHECK(!Config_SetLiveKey("gpu_queue_probe_source", bad, error, sizeof(error)) &&
              g_config.gpu_queue_probe_source == 0);
    }
    g_config.gpu_queue_probe_source = 9;
    ValidateConfig();
    CHECK(g_config.gpu_queue_probe_source == 1);
    g_config.gpu_queue_probe_source = -3;
    ValidateConfig();
    CHECK(g_config.gpu_queue_probe_source == 0);
    ResetDeveloperKeepingEarlierPipeline();
    CHECK(g_config.gpu_queue_probe_source == 1 && g_gpu_queue_probe_source.load() == (wide ? 1 : 0) &&
          OverridesBesidesUnified() == 0);
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());
}

// unified_queue_margin_end (developer, probe only): default 1; loads, is clamped to 0..1, is saved, counts
// as one developer override only away from 1, is a live key (an unparsable value is refused, an
// out-of-range one clamped), reaches the pipeline snapshot and the probe's atomic (64-bit; 0 on Win32)
// and resets to 1. An INI written by a build where the default was 0 keeps its 0 (no migration).
void TestUnifiedQueueMarginEndKey() {
    const bool wide = sizeof(void*) == 8;
    struct Case { const char* value; int expected; };
    for (const Case& test : {Case{"0", 0}, Case{"1", 1}, Case{"2", 1}, Case{"99", 1}, Case{"-1", 0}, Case{"-9", 0}}) {
        Reset(false, false, {{"config_version", "5"}, {"unified_queue_margin_end", test.value}});
        CHECK(g_config.unified_queue_margin_end == test.expected);
        CheckPipelineMatchesConfig();
        CHECK(PacingPipeline_GetConfig().unified_queue_margin_end == (wide ? test.expected : 0));
        CHECK(g_unified_queue_margin_end.load() == (wide ? test.expected : 0));
        CHECK(OverridesBesidesUnified() == (test.expected != 1 ? 1 : 0));
        char saved[32] = {};
        snprintf(saved, sizeof(saved), "%d", test.expected);
        CHECK(Value(local_path, "FrameLimiter", "unified_queue_margin_end") == saved);
        ReleaseStart();CHECK(OwnerShutdown());
    }
    Reset(false, false, {{"config_version", "5"}});
    CHECK(g_unified_queue_margin_end.load() == (wide ? 1 : 0) && g_config.unified_queue_margin_end == 1);
    CHECK(OverridesBesidesUnified() == 0);
    char error[128] = {};
    CHECK(Config_SetLiveKey("unified_queue_margin_end", "0", error, sizeof(error)) &&
          g_config.unified_queue_margin_end == 0);
    CHECK(g_unified_queue_margin_end.load() == 0 && OverridesBesidesUnified() == 1);
    CHECK(Config_SetLiveKey("unified_queue_margin_end", "7", error, sizeof(error)) &&
          g_config.unified_queue_margin_end == 1);
    CHECK(Config_SetLiveKey("unified_queue_margin_end", "-4", error, sizeof(error)) &&
          g_config.unified_queue_margin_end == 0);
    CHECK(Config_SetLiveKey("unified_queue_margin_end", "1", error, sizeof(error)));
    CheckPipelineMatchesConfig();
    for (const char* bad : {"abc", "1x", ""}) {
        CHECK(!Config_SetLiveKey("unified_queue_margin_end", bad, error, sizeof(error)) &&
              g_config.unified_queue_margin_end == 1);
    }
    g_config.unified_queue_margin_end = 9;
    ValidateConfig();
    CHECK(g_config.unified_queue_margin_end == 1);
    g_config.unified_queue_margin_end = -3;
    ValidateConfig();
    CHECK(g_config.unified_queue_margin_end == 0);
    g_config.unified_queue_margin_end = 0;
    ResetDeveloperKeepingEarlierPipeline();
    CHECK(g_config.unified_queue_margin_end == 1 && g_unified_queue_margin_end.load() == (wide ? 1 : 0) &&
          OverridesBesidesUnified() == 0);
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());
}

// Developer control file: Config_SetLiveKey sets one live key as the OSD does
// (validated like an INI value, published to the cap atomics and the
// pipeline) and queues no save; it refuses unparsable values, launch-only and
// unknown keys and leaves the field alone then.
void TestLiveKeySetter() {
    Reset(false, false, {{"config_version", "4"}, {"target_fps", "100"}});
    char error[128] = {};
    CHECK(Config_SetLiveKey("target_fps", "60", error, sizeof(error)) && error[0] == '\0');
    CHECK(g_config.target_fps == 60 && g_user_target_fps.load() == 60);
    CHECK(Config_SetLiveKey("target_fps", "10", error, sizeof(error)));
    CHECK(g_config.target_fps == 30 && g_user_target_fps.load() == 30);
    CHECK(Config_SetLiveKey("source_placement", "MARKER", error, sizeof(error)));
    CHECK(Config_SetLiveKey("reflex_sleep_bypass", "off", error, sizeof(error)));
    CHECK(Config_SetLiveKey("gpu_wait_headroom_us", "2000", error, sizeof(error)));
    CHECK(Config_SetLiveKey("adaptive_cap", "TRUE", error, sizeof(error)));
    CHECK(g_config.source_placement == PacingPlacementOverride::Marker);
    CHECK(!g_config.reflex_sleep_bypass && !g_reflex_sleep_bypass.load());
    CHECK(g_config.gpu_wait_headroom_us == 2000.0 && g_config.adaptive_cap);
    CheckPipelineMatchesConfig();

    CHECK(!Config_SetLiveKey("source_placement", "sideways", error, sizeof(error)) && error[0] != '\0');
    CHECK(g_config.source_placement == PacingPlacementOverride::Marker);
    CHECK(!Config_SetLiveKey("target_fps", "60fps", error, sizeof(error)) && g_config.target_fps == 30);
    CHECK(!Config_SetLiveKey("target_fps", "", error, sizeof(error)) && g_config.target_fps == 30);
    CHECK(!Config_SetLiveKey("adaptive_cap", "maybe", error, sizeof(error)) && g_config.adaptive_cap);
    CHECK(!Config_SetLiveKey("flip_metering_off", "true", error, sizeof(error)) && !g_config.flip_metering_off);
    CHECK(!Config_SetLiveKey("no_such_key", "1", error, sizeof(error)));

    // No save queued: draining the writer leaves the loaded cap in the INI.
    ReleaseStart();CHECK(OwnerShutdown());
    CHECK(Value(local_path, "FrameLimiter", "target_fps") == "100");
}

// pacing_method=marker becomes output_rung=marker; automatic, native and
// invalid methods become auto. A present output_rung always wins.
void TestPacingMethodMigration() {
    struct Case { const char* method; const char* rung; PacingRungOverride expected; };
    for (const Case& test : {Case{"marker", nullptr, PacingRungOverride::Marker},
            Case{"automatic", nullptr, PacingRungOverride::Auto},
            Case{"native", nullptr, PacingRungOverride::Auto},
            Case{"invalid", nullptr, PacingRungOverride::Auto},
            Case{"marker", "gpu", PacingRungOverride::Gpu},
            Case{"marker", "auto", PacingRungOverride::Auto},
            Case{nullptr, "native", PacingRungOverride::Native},
            Case{nullptr, "SOURCE", PacingRungOverride::SourceOnly},
            Case{"marker", "bogus", PacingRungOverride::Auto}}) {
        std::vector<IniEntry> ini;
        if (test.method) ini.push_back({"pacing_method", test.method});
        if (test.rung) ini.push_back({"output_rung", test.rung});
        Reset(false, false, ini);
        CHECK(g_config.output_rung == test.expected);
        CheckPipelineMatchesConfig();
        CHECK(Value(local_path, "FrameLimiter", "output_rung") == PacingRungOverride_Text(test.expected));
        CHECK(Value(local_path, "FrameLimiter", "pacing_method").empty());
        ReleaseStart();CHECK(OwnerShutdown());
    }
}

// adaptive_cap loads the legacy adaptive_smoothing value only when absent.
void TestAdaptiveCapMigration() {
    struct Case { const char* legacy; const char* current; bool expected; };
    for (const Case& test : {Case{nullptr, nullptr, false}, Case{"true", nullptr, true},
            Case{"1", nullptr, true}, Case{"false", nullptr, false},
            Case{"true", "false", false}, Case{"false", "true", true}, Case{nullptr, "yes", true}}) {
        std::vector<IniEntry> ini;
        if (test.legacy) ini.push_back({"adaptive_smoothing", test.legacy});
        if (test.current) ini.push_back({"adaptive_cap", test.current});
        Reset(false, false, ini);
        CHECK(g_config.adaptive_cap == test.expected);
        CheckPipelineMatchesConfig();
        CHECK(Value(local_path, "FrameLimiter", "adaptive_cap") == (test.expected ? "true" : "false"));
        CHECK(Value(local_path, "FrameLimiter", "adaptive_smoothing").empty());
        ReleaseStart();CHECK(OwnerShutdown());
    }
}

// adaptive_cap_percentile defaults to 75, keeps the tiers 75, 90 and 99,
// snaps any other value to the nearest tier (up to 82 → 75, 83–94 → 90,
// 95 and above → 99) and saves the tier.
void TestAdaptiveCapPercentile() {
    struct Case { const char* value; int expected; };
    for (const Case& test : {Case{nullptr, 75}, Case{"75", 75}, Case{"90", 90}, Case{"99", 99},
            Case{"50", 75}, Case{"82", 75}, Case{"83", 90}, Case{"94", 90}, Case{"95", 99},
            Case{"10", 75}, Case{"150", 99}}) {
        std::vector<IniEntry> ini;
        if (test.value) ini.push_back({"adaptive_cap_percentile", test.value});
        Reset(false, false, ini);
        CHECK(g_config.adaptive_cap_percentile == test.expected);
        CheckPipelineMatchesConfig();
        CHECK(Value(local_path, "FrameLimiter", "adaptive_cap_percentile") == std::to_string(test.expected));
        ReleaseStart();CHECK(OwnerShutdown());
    }
}

// pacing_preference (user key): loads each text (case-insensitive), an invalid or empty text resets to balanced, saves
// canonical text; the pipeline carries the effective rank and hold share while g_config and the INI keep the raw developer
// keys; a developer key that differs from its default decides its own value, one at its default inherits the preference,
// and the two levers are independent; the live key; not a developer setting, and the developer reset leaves it alone.
void TestPacingPreference() {
    struct Case { const char* text; PacingPreference expected; };
    for (const Case& test : {Case{nullptr, PacingPreference::Balanced}, Case{"balanced", PacingPreference::Balanced},
            Case{"latency", PacingPreference::Latency}, Case{"SMOOTH", PacingPreference::Smooth},
            Case{"fast", PacingPreference::Balanced}, Case{"", PacingPreference::Balanced}}) {
        std::vector<IniEntry> ini;
        if (test.text) ini.push_back({"pacing_preference", test.text});
        Reset(false, false, ini);
        CHECK(g_config.pacing_preference == test.expected);
        CheckPipelineMatchesConfig();
        CHECK(Value(local_path, "FrameLimiter", "pacing_preference") == PacingPreference_Text(test.expected));
        // The effective pair is the preference's; the developer keys and the INI keep their raw values.
        CHECK(g_config.unified_queue_rank == kPacingUnifiedQueueRankDefault);
        CHECK(g_config.unified_hold_share == kPacingUnifiedHoldShareDefault);
        CHECK(std::stoi(Value(local_path, "FrameLimiter", "unified_queue_rank")) == kPacingUnifiedQueueRankDefault);
        CHECK(std::stod(Value(local_path, "FrameLimiter", "unified_hold_share")) == kPacingUnifiedHoldShareDefault);
        const PacingPipelineConfig pipeline = PacingPipeline_GetConfig();
        CHECK(pipeline.unified_queue_rank ==
            (test.expected == PacingPreference::Latency ? kPacingPreferenceLatencyRank : kPacingUnifiedQueueRankDefault));
        CHECK(pipeline.unified_hold_share ==
            (test.expected == PacingPreference::Smooth && sizeof(void*) == 8 ? kPacingPreferenceSmoothHoldShare
                                                                              : kPacingUnifiedHoldShareDefault));
        CHECK(OverridesBesidesUnified() == 0); // a user setting, not a developer override
        ReleaseStart();CHECK(OwnerShutdown());
    }

    // Live key, precedence, independence and the developer reset.
    Reset(false, false, {{"config_version", "4"}});
    char error[128] = {};
    CHECK(Config_SetLiveKey("pacing_preference", "Smooth", error, sizeof(error)) && error[0] == '\0');
    CHECK(g_config.pacing_preference == PacingPreference::Smooth);
    CheckPipelineMatchesConfig();
    CHECK(PacingPipeline_GetConfig().unified_queue_rank == kPacingUnifiedQueueRankDefault); // Smooth moves only the share
    CHECK(!Config_SetLiveKey("pacing_preference", "turbo", error, sizeof(error)) && error[0] != '\0');
    CHECK(g_config.pacing_preference == PacingPreference::Smooth);
    CHECK(Config_SetLiveKey("pacing_preference", "latency", error, sizeof(error)));
    CheckPipelineMatchesConfig();
    CHECK(PacingPipeline_GetConfig().unified_queue_rank == kPacingPreferenceLatencyRank);
    CHECK(PacingPipeline_GetConfig().unified_hold_share == kPacingUnifiedHoldShareDefault); // Latency moves only the rank
    // A developer key away from its default decides its own value whatever the preference.
    CHECK(Config_SetLiveKey("unified_queue_rank", "3", error, sizeof(error)));
    CHECK(PacingPipeline_GetConfig().unified_queue_rank == 3 && g_config.unified_queue_rank == 3);
    CHECK(Config_SetLiveKey("pacing_preference", "smooth", error, sizeof(error)));
    CHECK(PacingPipeline_GetConfig().unified_queue_rank == 3); // Smooth does not touch the rank
    CHECK(Config_SetLiveKey("unified_hold_share", "0.0625", error, sizeof(error)));
    CheckPipelineMatchesConfig();
    CHECK(g_config.unified_hold_share == 0.0625);
    if (sizeof(void*) == 8) CHECK(PacingPipeline_GetConfig().unified_hold_share == 0.0625);
    CHECK(OverridesBesidesUnified() == 2); // the two developer keys, not the preference
    // The developer reset returns the developer keys and leaves the preference.
    ResetDeveloperKeepingEarlierPipeline();
    CHECK(OverridesBesidesUnified() == 0);
    CHECK(g_config.pacing_preference == PacingPreference::Smooth);
    CheckPipelineMatchesConfig();
    CHECK(PacingPipeline_GetConfig().unified_hold_share ==
        (sizeof(void*) == 8 ? kPacingPreferenceSmoothHoldShare : kPacingUnifiedHoldShareDefault));
    // A developer key explicitly at its default inherits the preference (to force the exact pair, select balanced first).
    CHECK(Config_SetLiveKey("unified_queue_rank", "2", error, sizeof(error)));
    CHECK(Config_SetLiveKey("pacing_preference", "latency", error, sizeof(error)));
    CHECK(PacingPipeline_GetConfig().unified_queue_rank == kPacingPreferenceLatencyRank);
    CHECK(Config_SetLiveKey("pacing_preference", "balanced", error, sizeof(error)));
    CHECK(PacingPipeline_GetConfig().unified_queue_rank == kPacingUnifiedQueueRankDefault);
    CheckPipelineMatchesConfig();

    // Save round trip: the preference is saved as text, the raw developer keys stay raw.
    g_config.pacing_preference = PacingPreference::Latency;
    Config_ApplyPacingPipeline();
    RequestSaveConfig();
    ReleaseStart();CHECK(OwnerShutdown());
    CheckWorkerWrites();
    CHECK(Value(local_path, "FrameLimiter", "pacing_preference") == "latency");
    CHECK(std::stoi(Value(local_path, "FrameLimiter", "unified_queue_rank")) == kPacingUnifiedQueueRankDefault);

    // An out-of-range value in memory is reset with a warning on validation.
    Reset();
    g_config.pacing_preference = static_cast<PacingPreference>(9);
    ValidateConfig();
    CHECK(g_config.pacing_preference == PacingPreference::Balanced);
    ReleaseStart();CHECK(OwnerShutdown());
}

// The state every observer of the key is in: the queue probe's gate and its law feed, the probe source, the marker
// stamps, the margin end, readiness and gate probes, the other published developer flags, and the pipeline's pacer
// numbers. A test mode must leave all of it as its old-key arm does.
struct Observed {
    bool gpu_queue_probe, law_feed, start_probe, sleep_status_every_frame, bypass_low_latency_off, driver_low_latency_off,
        marker_forward_after_wait;
    int upstream, probe_source, margin_end, presenter_probe_us;
    bool gpu_probes, queue_law, every_frame_service;
    int queue_rank, need_frames, allow_us;
    double hold_share, queue_target_us;
    bool unified;
};
Observed Observe() {
    const PacingPipelineConfig pipeline = PacingPipeline_GetConfig();
    return Observed{ g_gpu_queue_probe.load(), g_gpu_queue_law_feed.load(), g_gpu_start_probe.load(),
        g_sleep_status_every_frame.load(), g_bypass_low_latency_off.load(), g_driver_low_latency_off.load(),
        g_marker_forward_after_wait.load(), g_unified_queue_upstream.load(), g_gpu_queue_probe_source.load(),
        g_unified_queue_margin_end.load(), g_fg_presenter_probe_us.load(), pipeline.gpu_probes, pipeline.unified_queue_law,
        pipeline.unified_every_frame_service, pipeline.unified_queue_rank, pipeline.unified_queue_need_frames,
        pipeline.unified_queue_allow_us, pipeline.unified_hold_share, pipeline.unified_queue_target_us,
        pipeline.unified_pacer };
}
bool SameObserved(const Observed& a, const Observed& b) {
    return a.gpu_queue_probe == b.gpu_queue_probe && a.law_feed == b.law_feed && a.start_probe == b.start_probe &&
        a.sleep_status_every_frame == b.sleep_status_every_frame && a.bypass_low_latency_off == b.bypass_low_latency_off &&
        a.driver_low_latency_off == b.driver_low_latency_off && a.marker_forward_after_wait == b.marker_forward_after_wait &&
        a.upstream == b.upstream && a.probe_source == b.probe_source && a.margin_end == b.margin_end &&
        a.presenter_probe_us == b.presenter_probe_us && a.gpu_probes == b.gpu_probes && a.queue_law == b.queue_law &&
        a.every_frame_service == b.every_frame_service && a.queue_rank == b.queue_rank &&
        a.need_frames == b.need_frames && a.allow_us == b.allow_us && a.hold_share == b.hold_share &&
        a.queue_target_us == b.queue_target_us && a.unified == b.unified;
}

// pacing_test_mode (developer key): loads each text (case-insensitive), an invalid or empty text resets to off, saves
// canonical text, honoured only with developer_settings (the raw value stays in g_config and the INI), a live key,
// counted in the developer overrides and reset by the developer reset; the effective mode takes the ownership from
// unified_pacer, and each mode leaves every published observer in the state of its old-key arm.
void TestPacingTestMode() {
    struct Case { const char* text; PacingTestMode expected; };
    for (const Case& test : {Case{nullptr, PacingTestMode::Off}, Case{"off", PacingTestMode::Off},
            Case{"reflex_only", PacingTestMode::ReflexOnly}, Case{"DRIVER_LIMITER", PacingTestMode::DriverLimiter},
            Case{"reflex", PacingTestMode::Off}, Case{"", PacingTestMode::Off}}) {
        std::vector<IniEntry> ini{{"developer_settings", "true"}, {"unified_pacer", "true"}};
        if (test.text) ini.push_back({"pacing_test_mode", test.text});
        Reset(false, false, ini);
        CHECK(g_config.pacing_test_mode == test.expected);
        CheckPipelineMatchesConfig();
        CHECK(PacingPipeline_TestMode() == test.expected);
        CHECK(PacingPipeline_UnifiedPacerKey() == (sizeof(void*) == 8 && test.expected == PacingTestMode::Off));
        CHECK(Value(local_path, "FrameLimiter", "pacing_test_mode") == PacingTestMode_Text(test.expected));
        CHECK(OverridesBesidesUnified() == (test.expected == PacingTestMode::Off ? 0 : 1));
        ReleaseStart();CHECK(OwnerShutdown());
    }

    // The gate: without developer_settings the saved mode is not honoured and the raw value stays.
    Reset(false, false, {{"unified_pacer", "true"}, {"pacing_test_mode", "reflex_only"}});
    CHECK(!g_config.developer_settings && g_config.pacing_test_mode == PacingTestMode::ReflexOnly);
    CheckPipelineMatchesConfig();
    CHECK(PacingPipeline_TestMode() == PacingTestMode::Off);
    CHECK(PacingPipeline_UnifiedPacerKey() == (sizeof(void*) == 8));
    CHECK(g_reflex_sleep_bypass.load() == g_config.reflex_sleep_bypass);
    CHECK(Value(local_path, "FrameLimiter", "pacing_test_mode") == "reflex_only");
    {
        const Observed ignored = Observe();
        char error[128] = {};
        CHECK(Config_SetLiveKey("pacing_test_mode", "off", error, sizeof(error)));
        CHECK(SameObserved(ignored, Observe()));
    }
    ReleaseStart();CHECK(OwnerShutdown());

    // The live key, the override count and the developer reset.
    Reset(false, false, {{"developer_settings", "true"}, {"unified_pacer", "true"}});
    char error[128] = {};
    CHECK(Config_SetLiveKey("pacing_test_mode", "Driver_Limiter", error, sizeof(error)) && error[0] == '\0');
    CHECK(g_config.pacing_test_mode == PacingTestMode::DriverLimiter && PacingPipeline_TestMode() == PacingTestMode::DriverLimiter);
    CheckPipelineMatchesConfig();
    CHECK(!PacingPipeline_UnifiedPacerKey());
    CHECK(!Config_SetLiveKey("pacing_test_mode", "turbo", error, sizeof(error)) && error[0] != '\0');
    CHECK(g_config.pacing_test_mode == PacingTestMode::DriverLimiter);
    CHECK(OverridesBesidesUnified() == 1);
    ResetDeveloperKeepingEarlierPipeline();
    CHECK(OverridesBesidesUnified() == 0 && g_config.pacing_test_mode == PacingTestMode::Off);
    CHECK(PacingPipeline_TestMode() == PacingTestMode::Off);
    ReleaseStart();CHECK(OwnerShutdown());

    // Save round trip.
    Reset(false, false, {{"developer_settings", "true"}});
    g_config.pacing_test_mode = PacingTestMode::ReflexOnly;
    Config_ApplyPacingPipeline();
    RequestSaveConfig();
    ReleaseStart();CHECK(OwnerShutdown());
    CheckWorkerWrites();
    CHECK(Value(local_path, "FrameLimiter", "pacing_test_mode") == "reflex_only");
    Reset();
    g_config.pacing_test_mode = static_cast<PacingTestMode>(9);
    ValidateConfig();
    CHECK(g_config.pacing_test_mode == PacingTestMode::Off);
    ReleaseStart();CHECK(OwnerShutdown());

    // Observers: each mode, with the same remaining keys, leaves them as its arm does. The production default (the
    // key on, the law, the probe and every other developer key at its default) is the reference that the arms differ from.
    const std::vector<IniEntry> shared{{"config_version", "5"}, {"developer_settings", "true"}, {"gpu_queue_probe", "false"},
        {"unified_hold_share", "0.0625"}, {"unified_queue_rank", "3"}, {"unified_queue_need_frames", "6"},
        {"unified_queue_allow_us", "3000"}, {"gpu_present_gate_probes", "true"}, {"unified_queue_upstream", "1"},
        {"sleep_status_every_frame", "true"}, {"gpu_start_probe", "true"}, {"fg_presenter_probe_us", "500"}};
    const auto with = [&](std::vector<IniEntry> extra) {
        std::vector<IniEntry> ini = shared;
        ini.insert(ini.end(), extra.begin(), extra.end());
        return ini;
    };
    Reset(false, false, with({{"unified_pacer", "true"}, {"target_fps", "60"}}));
    const Observed production = Observe();
    CHECK(production.unified == (sizeof(void*) == 8));
    CHECK(production.law_feed == (sizeof(void*) == 8)); // the law's feed follows the owner
    ReleaseStart();CHECK(OwnerShutdown());

    // reflex_only against its arm (unified_pacer=false, reflex_sleep_bypass=true, target_fps=0).
    Reset(false, false, with({{"unified_pacer", "true"}, {"pacing_test_mode", "reflex_only"}, {"target_fps", "60"}}));
    const Observed reflex_mode = Observe();
    const bool reflex_mode_bypass = g_reflex_sleep_bypass.load();
    ReleaseStart();CHECK(OwnerShutdown());
    Reset(false, false, with({{"unified_pacer", "false"}, {"reflex_sleep_bypass", "true"}, {"target_fps", "0"}}));
    const Observed reflex_arm = Observe();
    const bool reflex_arm_bypass = g_reflex_sleep_bypass.load();
    ReleaseStart();CHECK(OwnerShutdown());
    CHECK(SameObserved(reflex_mode, reflex_arm));
    CHECK(!reflex_mode.unified && !reflex_mode.law_feed);
    CHECK(!reflex_mode_bypass && reflex_arm_bypass); // the bypass is off in effect (the arm's is inert at cap 0)

    // driver_limiter against its arm (unified_pacer=false, source_placement=driver, reflex_sleep_bypass=false).
    Reset(false, false, with({{"unified_pacer", "true"}, {"pacing_test_mode", "driver_limiter"}, {"target_fps", "60"}}));
    const Observed limiter_mode = Observe();
    const bool limiter_mode_bypass = g_reflex_sleep_bypass.load();
    ReleaseStart();CHECK(OwnerShutdown());
    Reset(false, false, with({{"unified_pacer", "false"}, {"source_placement", "driver"},
        {"reflex_sleep_bypass", "false"}, {"target_fps", "60"}}));
    const Observed limiter_arm = Observe();
    const bool limiter_arm_bypass = g_reflex_sleep_bypass.load();
    ReleaseStart();CHECK(OwnerShutdown());
    CHECK(SameObserved(limiter_mode, limiter_arm));
    CHECK(!limiter_mode.unified && !limiter_mode.law_feed);
    CHECK(!limiter_mode_bypass && !limiter_arm_bypass);
    // And both differ from the production owner exactly where the key gates observers (the law's feed).
    CHECK(production.law_feed != limiter_mode.law_feed || sizeof(void*) != 8);
}

void TestDeveloperKeysClamped() {
    struct Case { const char* ms; const char* period; double expected_ms; double expected_period; };
    for (const Case& test : {Case{"12.5", "2", 12.5, 2.0}, Case{"500", "120", 200.0, 60.0},
            Case{"-3", "0", 0.0, 1.0}, Case{"nan", "inf", 0.0, 5.0}}) {
        Reset(false, false, {{"stall_inject_ms", test.ms}, {"stall_inject_period_s", test.period},
            {"gpu_present_gate_probes", "false"}, {"reflex_sleep_bypass", "true"}});
        CHECK(g_config.stall_inject_ms == test.expected_ms);
        CHECK(g_config.stall_inject_period_s == test.expected_period);
        CHECK(!g_config.gpu_present_gate_probes && !PacingPipeline_GetConfig().gpu_probes);
        CHECK(g_config.reflex_sleep_bypass && g_reflex_sleep_bypass.load());
        CheckPipelineMatchesConfig();
        CHECK(Value(local_path, "FrameLimiter", "gpu_present_gate_probes") == "false");
        CHECK(Value(local_path, "FrameLimiter", "reflex_sleep_bypass") == "true");
        ReleaseStart();CHECK(OwnerShutdown());
    }
    Reset();
    g_config.stall_inject_ms = 1e9;
    g_config.stall_inject_period_s = -1.0;
    ValidateConfig();ApplyConfig();
    CHECK(g_config.stall_inject_ms == 200.0 && g_config.stall_inject_period_s == 1.0);
    CheckPipelineMatchesConfig();
    RequestSaveConfig();ReleaseStart();CHECK(OwnerShutdown());
    CHECK(Value(local_path, "FrameLimiter", "stall_inject_ms") == "200.000000");
    CHECK(Value(local_path, "FrameLimiter", "stall_inject_period_s") == "1.000000");
}

// gpu_wait_interleave_s: 0, negative or non-finite is off (the default);
// otherwise [1, 10] s.
void TestGpuWaitInterleaveKey() {
    struct Case { const char* value; double expected; };
    for (const Case& test : {Case{"0", 0.0}, Case{"2.5", 2.5}, Case{"0.3", 1.0}, Case{"30", 10.0},
            Case{"-1", 0.0}, Case{"nan", 0.0}}) {
        Reset(false, false, {{"gpu_wait_interleave_s", test.value}});
        CHECK(g_config.gpu_wait_interleave_s == test.expected);
        CheckPipelineMatchesConfig();
        ReleaseStart();CHECK(OwnerShutdown());
    }
}

// Keys retired by the timing-core rework (pacing_method and adaptive_smoothing
// migrate first). An existing INI may still carry them: loading ignores them and
// LoadConfig's clean rewrite drops them, so the pipeline matches a fresh INI.
constexpr const char* retired_keys[] = {
    "ready_present_mode", "ready_present_slack_us", "ready_present_max_hold_us",
    "readiness_phase_mode", "readiness_phase_max_advance_us", "adaptive_cycle_trial",
    "gpu_present_gate_hold", "gpu_present_gate_phase_controller", "reflex_gpu_combined",
    "reflex_pre_sleep_pacing", "marker_gate_bypass",
    "pacing_method", "estimator_a_mode", "estimator_a_fixed_reserve_us",
    "native_arrival_predictor_mode", "presentation_gate_location", "gpu_present_gate_mode",
    "gpu_present_gate_slack_us", "gpu_present_gate_max_delay_us",
    "adaptive_deadline_reserve_mode", "smoothing_window", "smoothing_release_speed",
    "smoothing_bias_us", "smoothing_percentile", "gpu_readiness_observer", "adaptive_smoothing",
    "dmfg_output_cap", "gpu_paced_hold_margin", "gpu_paced_release_smoothing"};

void TestRetiredKeysIgnoredAndDropped() {
    Reset(false, false, {{"ready_present_mode", "active"}, {"ready_present_slack_us", "2000"},
        {"ready_present_max_hold_us", "500"}, {"readiness_phase_mode", "active"},
        {"readiness_phase_max_advance_us", "0"}, {"adaptive_cycle_trial", "true"},
        {"gpu_present_gate_hold", "cpu_present"}, {"gpu_present_gate_phase_controller", "true"},
        {"reflex_gpu_combined", "true"}, {"reflex_pre_sleep_pacing", "true"},
        {"marker_gate_bypass", "true"}, {"pacing_method", "native"},
        {"estimator_a_mode", "shadow"}, {"estimator_a_fixed_reserve_us", "90"},
        {"native_arrival_predictor_mode", "off"}, {"presentation_gate_location", "marker"},
        {"gpu_present_gate_mode", "shadow"}, {"gpu_present_gate_slack_us", "2000"},
        {"gpu_present_gate_max_delay_us", "500"}, {"adaptive_deadline_reserve_mode", "active"},
        {"smoothing_window", "dual"}, {"smoothing_release_speed", "0.25"},
        {"smoothing_bias_us", "1000"}, {"smoothing_percentile", "0.5"},
        {"gpu_readiness_observer", "true"}, {"adaptive_smoothing", "true"},
        {"dmfg_output_cap", "144"}, {"gpu_paced_hold_margin", "75"},
        {"gpu_paced_release_smoothing", "true"}});
    // Only the migrations take effect; the pipeline otherwise matches a fresh INI.
    CHECK(g_config.adaptive_cap && g_config.output_rung == PacingRungOverride::Auto);
    CHECK(g_config.smoothing == PacingSmoothing::On && g_config.gpu_present_gate_probes);
    CHECK(g_config.stall_inject_ms == 0.0 && g_config.stall_inject_period_s == 5.0);
    CheckPipelineMatchesConfig();
    CHECK(Value(local_path, "FrameLimiter", "adaptive_cap") == "true");
    for (const char* key : retired_keys) CHECK(Value(local_path, "FrameLimiter", key).empty());
    RequestSaveConfig();ReleaseStart();CHECK(OwnerShutdown());
    CheckWorkerWrites();
    for (const char* key : retired_keys) CHECK(Value(local_path, "FrameLimiter", key).empty());
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& write : writes)
        for (const char* key : retired_keys) CHECK(std::get<2>(write.key) != key);
}
void TestImmutableCoalescingAndDestinations() {
    Reset();
    g_config.target_fps = 101;
    RequestSaveConfig();
    auto& first = OSDPreset_GetSlot(0);
    first.occupied = true;
    strcpy(first.name, "discarded");
    auto& second = OSDPreset_GetSlot(1);
    second.occupied = true;
    strcpy(second.name, "local-compacted");
    OSDPreset_SaveSlot(0);
    OSDPreset_DeleteSlot(0); // Latest local image includes compaction + empty tail.
    {
        std::lock_guard<std::mutex> lock(mutex);
        disk[{ local_path, "OSD_Preset_16", "name" }] = "stale-tail";
    }
    g_config.shared_presets = true;
    strcpy(OSDPreset_GetSlot(0).name, "shared-snapshot");
    OSDPreset_SaveSlot(0);
    g_config.target_fps = 202;
    g_config.osd_preset_prev_key = "captured-key";
    RequestSaveConfig();
    // Mutate every source after publication, before the worker can take it.
    g_config.target_fps = 999;
    g_config.shared_presets = false;
    g_config.osd_preset_prev_key = "mutated-key";
    strcpy(OSDPreset_GetSlot(0).name, "mutated-slot");
    strcpy(s_ini_path, "C:\\wrong.ini");
    strcpy(s_shared_presets_path, "C:\\wrong-shared.ini");
    ReleaseStart();
    CHECK(OwnerShutdown());
    CHECK(Attempts() == 1);
    CHECK(Value(local_path, "FrameLimiter", "target_fps") == "202");
    CHECK(Value(shared_path, "Presets", "prev_key") == "captured-key");
    CHECK(Value(local_path, "OSD_Preset_1", "name") == "local-compacted");
    CHECK(Value(local_path, "OSD_Preset_2", "name").empty());
    CHECK(Value(local_path, "OSD_Preset_16", "name").empty());
    CHECK(Value(shared_path, "OSD_Preset_1", "name") == "shared-snapshot");
    CHECK(closes == 1 && logger_shutdowns == 1);
    CheckWorkerWrites();
}

void TestBlockedIOTimeoutAndInFlightReplacement() {
    Reset();
    {
        std::lock_guard<std::mutex> lock(mutex);
        hold_io = true;
    }
    g_config.target_fps = 111;
    RequestSaveConfig();
    ReleaseStart();
    WaitForBlockedWrite();
    // The writer owns s_ini_mutex here. These UI producers must still return.
    g_config.target_fps = 222;
    g_config.osd_preset_next_key = "local-key";
    RequestSaveConfig();
    OSDPreset_GetSlot(0).occupied = true;
    strcpy(OSDPreset_GetSlot(0).name, "nonblocking");
    OSDPreset_SaveSlot(0);
    OSDPreset_DeleteSlot(0);
    const HANDLE retained = s_config_writer;
    CHECK(!OwnerShutdown(0));
    CHECK(s_config_writer == retained && closes == 0);
    CHECK(logger_alive && logger_shutdowns == 0);
    CHECK(!Config_FlushPending()); // Must not wait for the blocked worker.
    SaveConfig();                 // Nor perform new synchronous writes.
    OSDPreset_LoadAll();
    LoadConfig(nullptr);          // Retained writer cannot be reinitialized.
    CHECK(creates == 1);
    ReleaseIO();
    CHECK(OwnerShutdown());
    CHECK(Attempts() == 2);
    CHECK(Value(local_path, "FrameLimiter", "target_fps") == "222");
    CHECK(Value(local_path, "FrameLimiter", "osd_preset_next_key") == "local-key");
    CHECK(closes == 1 && logger_shutdowns == 1);
    CheckWorkerWrites();
}

void TestFailedOnlyRetry(bool recover, bool exception) {
    Reset();
    {
        std::lock_guard<std::mutex> lock(mutex);
        fail_writes = !exception;
        throw_writes = exception;
    }
    g_config.target_fps = 333;
    RequestSaveConfig();
    ReleaseStart();
    WaitForFailedBatch();
    CHECK(Attempts() == 1);
    // Failed-only state must sleep until a request/stop, not wake itself forever.
    s_save_wakeup.notify_all();
    if (recover) {
        std::lock_guard<std::mutex> lock(mutex);
        fail_writes = throw_writes = false;
    }
    CHECK(OwnerShutdown() == recover);
    CHECK(Attempts() == 2); // Exactly one final failed-only retry.
    CHECK(!s_config_writer && closes == 1);
    CHECK(worker_logs > 0);
    CheckWorkerWrites();
    if (recover) {
        CHECK(Value(local_path, "FrameLimiter", "target_fps") == "333");
        CHECK(!logger_alive && logger_shutdowns == 1);
    } else {
        CHECK(s_failed_save.config && s_failed_save.config->settings.target_fps == 333);
        CHECK(logger_alive && logger_shutdowns == 0);
        CHECK(!OwnerShutdown(0));
        CHECK(!Config_FlushPending());
        SaveConfig();
        LoadConfig(nullptr);
        CHECK(Attempts() == 2 && creates == 1); // No post-join caller retry.
        CHECK(s_failed_save.config && s_failed_save.config->settings.target_fps == 333);
    }
}

void TestStoppingPendingFailure() {
    Reset();
    fail_writes = true; // Worker still gated, so fixture write is serialized.
    g_config.target_fps = 444;
    RequestSaveConfig();
    CHECK(!OwnerShutdown(0)); // Stop arrives BEFORE the worker takes its batch.
    ReleaseStart();
    CHECK(!OwnerShutdown());
    CHECK(Attempts() == 2); // Initial drain plus one retry, not zero or infinite.
    CHECK(s_failed_save.config && s_failed_save.config->settings.target_fps == 444);
    CHECK(closes == 1 && logger_alive);
    CheckWorkerWrites();
}

void TestNewerRequestSupersedesFailure() {
    Reset();
    fail_writes = true; // Worker is still gated.
    g_config.target_fps = 123;
    OSDPreset_GetSlot(0).occupied = true;
    strcpy(OSDPreset_GetSlot(0).name, "failed-local");
    OSDPreset_SaveSlot(0);
    RequestSaveConfig();
    ReleaseStart();
    WaitForFailedBatch();
    CHECK(Attempts() == 1);
    {
        std::lock_guard<std::mutex> lock(mutex);
        fail_writes = false;
    }
    g_config.target_fps = 789;
    g_config.shared_presets = true;
    strcpy(OSDPreset_GetSlot(0).name, "new-shared");
    RequestSaveConfig();
    OSDPreset_SaveSlot(0);
    CHECK(OwnerShutdown());
    CHECK(Attempts() == 2);
    CHECK(Value(local_path, "FrameLimiter", "target_fps") == "789");
    CHECK(Value(local_path, "OSD_Preset_1", "name") == "failed-local");
    CHECK(Value(shared_path, "OSD_Preset_1", "name") == "new-shared");
    CheckWorkerWrites();
}

void TestTimeoutDuringFinalRetry() {
    Reset();
    fail_writes = true; // Worker is still gated.
    g_config.target_fps = 456;
    RequestSaveConfig();
    ReleaseStart();
    WaitForFailedBatch();
    {
        std::lock_guard<std::mutex> lock(mutex);
        fail_writes = false;
        entered_io = false;
        hold_io = true;
    }
    const HANDLE retained = s_config_writer;
    CHECK(!OwnerShutdown(0));
    WaitForBlockedWrite(); // Final retry must run on the worker, not the caller.
    CHECK(!OwnerShutdown(0));
    CHECK(s_config_writer == retained && closes == 0);
    CHECK(logger_alive && logger_shutdowns == 0);
    CHECK(!Config_FlushPending());
    ReleaseIO();
    CHECK(OwnerShutdown());
    CHECK(Attempts() == 2);
    CHECK(Value(local_path, "FrameLimiter", "target_fps") == "456");
    CHECK(closes == 1 && logger_shutdowns == 1);
    CheckWorkerWrites();
}

void TestInitialFailureDrainedByWorker() {
    Reset(false, true); // LoadConfig's synchronous save failed before CreateThread.
    CHECK(s_failed_save.config);
    fail_writes = false; // Worker still gated.
    CHECK(!OwnerShutdown(0));
    ReleaseStart();
    CHECK(OwnerShutdown());
    CHECK(Attempts() == 1); // Failed-only startup payload gets a final attempt.
    CheckWorkerWrites();
}

void TestCreationFailureAndExplicitQuiescentFlush() {
    Reset(true);
    g_config.target_fps = 555;
    RequestSaveConfig();
    OSDPreset_GetSlot(0).occupied = true;
    strcpy(OSDPreset_GetSlot(0).name, "retained");
    OSDPreset_SaveSlot(0);
    CHECK(writes.empty() && creates == 1);
    CHECK(!OwnerShutdown(0));
    CHECK(!Config_FlushPending());
    SaveConfig();
    LoadConfig(nullptr);
    CHECK(writes.empty() && creates == 1 && closes == 0);
    CHECK(s_pending_save.config && s_pending_save.config->settings.target_fps == 555);
    CHECK(s_pending_save.presets[0]);
    CHECK(logger_alive && logger_shutdowns == 0);

    Reset(true);
    g_config.target_fps = 666;
    RequestSaveConfig();
    CHECK(Config_FlushPending()); // Explicit PRE-stop quiescent I/O remains legal.
    CHECK(Attempts() == 1);
    CHECK(Value(local_path, "FrameLimiter", "target_fps") == "666");
    CHECK(OwnerShutdown());
    CHECK(closes == 0 && logger_shutdowns == 1);
}
} // namespace config_test

int main() {
    using namespace config_test;
    caller_thread = GetCurrentThreadId();
    TestPacingPipelineDefaults();
    TestRecommendedDefaultsMigration();
    TestQueueDefaultsMigration();
    TestDeveloperSettingsReset();
    TestSmoothingParsing();
    TestMarkerForwardOrderParsing();
    TestRenderQueueParsing();
    TestLeadCoverageKey();
    TestFgLateSpacingKey();
    TestGpuPacedCadenceKey();
    TestAdaptiveCapHoldRoomKey();
    TestBypassLowLatencyOffKey();
    TestMeasurementKeys();
    TestUnifiedPacerKey();
    TestUnifiedQueueTargetKey();
    TestUnifiedEveryFrameServiceKey();
    TestUnifiedHoldShareKey();
    TestUnifiedQueueLawKeys();
    TestUnifiedQueueNeedFramesKey();
    TestUnifiedQueueUpstreamKey();
    TestGpuQueueProbeSourceKey();
    TestUnifiedQueueMarginEndKey();
    TestFlipMeteringOffKey();
    TestLiveKeySetter();
    TestSourcePlacementParsing();
    TestPacingMethodMigration();
    TestAdaptiveCapMigration();
    TestAdaptiveCapPercentile();
    TestPacingPreference();
    TestPacingTestMode();
    TestDeveloperKeysClamped();
    TestGpuWaitInterleaveKey();
    TestRetiredKeysIgnoredAndDropped();
    TestImmutableCoalescingAndDestinations();
    TestBlockedIOTimeoutAndInFlightReplacement();
    TestFailedOnlyRetry(true, false);
    TestFailedOnlyRetry(false, false);
    TestFailedOnlyRetry(false, true);
    TestStoppingPendingFailure();
    TestNewerRequestSupersedesFailure();
    TestTimeoutDuringFinalRetry();
    TestInitialFailureDrainedByWorker();
    TestCreationFailureAndExplicitQuiescentFlush();
    std::puts("Production config async tests passed (fake I/O, real Windows threads)");
    return 0;
}