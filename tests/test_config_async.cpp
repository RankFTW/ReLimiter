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
    CHECK(pipeline.smoothing == g_config.smoothing);
    CHECK(pipeline.rung_override == g_config.output_rung);
    CHECK(pipeline.adaptive_cap == g_config.adaptive_cap);
    CHECK(pipeline.adaptive_cap_percentile == g_config.adaptive_cap_percentile);
    CHECK(pipeline.placement_override == g_config.source_placement);
    CHECK(pipeline.gpu_probes == g_config.gpu_present_gate_probes);
    CHECK(pipeline.stall_inject_ms == g_config.stall_inject_ms);
    CHECK(pipeline.stall_inject_period_s == g_config.stall_inject_period_s);
    CHECK(pipeline.gpu_wait_interleave_s == g_config.gpu_wait_interleave_s);
    CHECK(pipeline.gpu_queue_wait_render_queue == g_config.gpu_queue_wait_render_queue);
    CHECK(pipeline.lead_coverage == g_config.lead_coverage);
    CHECK(pipeline.fg_late_spacing == g_config.fg_late_spacing);
    CHECK(g_marker_forward_after_wait.load() ==
        (g_config.marker_forward_order == PacingMarkerForwardOrder::AfterWait));
}

void TestPacingPipelineDefaults() {
    Reset();
    CHECK(g_config.smoothing == PacingSmoothing::On);
    CHECK(g_config.output_rung == PacingRungOverride::Auto);
    CHECK(!g_config.adaptive_cap && g_config.gpu_present_gate_probes && !g_config.advanced_settings);
    CHECK(!g_config.developer_settings && Config_DeveloperOverrides() == 0);
    CHECK(g_config.stall_inject_ms == 0.0 && g_config.stall_inject_period_s == 5.0);
    // Recommended defaults: the Reflex Sleep bypass and pre-Sleep placement.
    CHECK(g_config.reflex_sleep_bypass && g_reflex_sleep_bypass.load());
    CHECK(g_config.marker_forward_order == PacingMarkerForwardOrder::BeforeWait);
    CHECK(g_config.gpu_queue_wait_render_queue == PacingRenderQueue::Keep);
    CHECK(g_config.lead_coverage == PacingLeadCoverage::Q99);
    CHECK(g_config.fg_late_spacing);
    CHECK(g_config.source_placement == PacingPlacementOverride::PreSleep);
    CheckPipelineMatchesConfig();
    // The startup rewrite saves every new key, including the developer keys.
    CHECK(Value(local_path, "FrameLimiter", "marker_forward_order") == "before_wait");
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_wait_render_queue") == "keep");
    CHECK(Value(local_path, "FrameLimiter", "lead_coverage") == "99");
    CHECK(Value(local_path, "FrameLimiter", "fg_late_spacing") == "true");
    CHECK(Value(local_path, "FrameLimiter", "source_placement") == "pre_sleep");
    CHECK(Value(local_path, "FrameLimiter", "reflex_sleep_bypass") == "true");
    CHECK(Value(local_path, "FrameLimiter", "developer_settings") == "false");
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "4");
    CHECK(Value(local_path, "FrameLimiter", "smoothing") == "on");
    CHECK(Value(local_path, "FrameLimiter", "adaptive_cap") == "false");
    CHECK(Value(local_path, "FrameLimiter", "output_rung") == "auto");
    CHECK(Value(local_path, "FrameLimiter", "stall_inject_ms") == "0.000000");
    CHECK(Value(local_path, "FrameLimiter", "stall_inject_period_s") == "5.000000");
    CHECK(Value(local_path, "FrameLimiter", "gpu_present_gate_probes") == "true");
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
        {"gpu_wait_budget_us", "4000"}, {"adaptive_cap", "true"}});
    CHECK(g_config.reflex_sleep_bypass && g_reflex_sleep_bypass.load());
    CHECK(g_config.source_placement == PacingPlacementOverride::PreSleep);
    CHECK(g_config.gpu_wait_budget_us == 4000.0 && g_config.adaptive_cap);
    CheckPipelineMatchesConfig();
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "4");
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
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "4");
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

// developer_settings is read and saved; every developer key away from its
// default is counted, and Reset returns exactly those keys to the defaults
// and applies them, leaving the user's settings alone.
void TestDeveloperSettingsReset() {
    Reset(false, false, {{"config_version", "2"}, {"developer_settings", "true"}, {"smoothing", "off"},
        {"output_rung", "gpu"}, {"gpu_wait_budget_us", "4000"}, {"reflex_sleep_bypass", "false"},
        {"source_placement", "marker"}, {"adaptive_cap", "true"}, {"adaptive_cap_percentile", "90"}});
    CHECK(g_config.developer_settings);
    CHECK(Value(local_path, "FrameLimiter", "developer_settings") == "true");
    CHECK(Config_DeveloperOverrides() == 5);
    Config_ResetDeveloperSettings();
    CHECK(Config_DeveloperOverrides() == 0);
    CHECK(g_config.smoothing == PacingSmoothing::On && g_config.output_rung == PacingRungOverride::Auto);
    CHECK(g_config.gpu_wait_budget_us == kPacingGpuWaitBudgetDefaultUs);
    CHECK(g_config.reflex_sleep_bypass && g_reflex_sleep_bypass.load());
    CHECK(g_config.source_placement == PacingPlacementOverride::PreSleep);
    CHECK(g_config.adaptive_cap && g_config.adaptive_cap_percentile == 90 && g_config.developer_settings);
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
// pipeline, counts as a developer override and resets to 99; other text
// falls back to 99.
void TestLeadCoverageKey() {
    Reset(false, false, {{"config_version", "3"}, {"lead_coverage", "95"}});
    CHECK(g_config.lead_coverage == PacingLeadCoverage::Q95);
    CheckPipelineMatchesConfig();
    CHECK(Value(local_path, "FrameLimiter", "lead_coverage") == "95");
    CHECK(Config_DeveloperOverrides() == 1);
    Config_ResetDeveloperSettings();
    CHECK(g_config.lead_coverage == PacingLeadCoverage::Q99);
    CHECK(PacingPipeline_GetConfig().lead_coverage == PacingLeadCoverage::Q99);
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
    CHECK(Config_DeveloperOverrides() == 1);
    Config_ResetDeveloperSettings();
    CHECK(g_config.fg_late_spacing && PacingPipeline_GetConfig().fg_late_spacing);
    ReleaseStart();CHECK(OwnerShutdown());
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
    "dmfg_output_cap"};

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
        {"dmfg_output_cap", "144"}});
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
    TestDeveloperSettingsReset();
    TestSmoothingParsing();
    TestMarkerForwardOrderParsing();
    TestRenderQueueParsing();
    TestLeadCoverageKey();
    TestFgLateSpacingKey();
    TestSourcePlacementParsing();
    TestPacingMethodMigration();
    TestAdaptiveCapMigration();
    TestAdaptiveCapPercentile();
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