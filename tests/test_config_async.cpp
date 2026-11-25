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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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
// Failure injection for one file: every write to fail_path fails (an unwritable file); only the key deletions
// (a null value) to fail_delete_path fail. Neither is cleared by Launch: a test sets and clears them itself.
std::string fail_path;
std::string fail_delete_path;
bool throw_writes = false;
unsigned creates = 0;
unsigned closes = 0;
unsigned reads = 0; // every profile read the production code made (counted under `mutex`)
std::atomic<int> applied_log_level{ -1 }; // the LogLevel the production code last applied (-1: none since launch)
std::atomic<bool> logger_alive{ true };
std::atomic<unsigned> logger_shutdowns{ 0 };
std::atomic<unsigned> worker_logs{ 0 };
DWORD caller_thread = 0;
std::mutex log_mutex;
std::vector<std::string> warn_lines; // every Warn line since the last launch
std::vector<std::string> info_lines; // every Info line the production code logged since the last launch
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
    ++reads;
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
    const bool path_fails = (!fail_path.empty() && fail_path == path) ||
        (!fail_delete_path.empty() && fail_delete_path == path && key && !value);
    if (fail_writes || path_fails) {
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
    } else if (!value) {
        disk.erase(Key{ path, section, key }); // a null value deletes the key, as the real API does
    } else {
        disk[{ path, section, key }] = value;
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
// display_state.cpp publishes the limiter's cap from the saved one; here no display is detected, so it is the saved one.
void DisplayState_PublishTargetFps() noexcept { g_user_target_fps.store(g_config.target_fps); }
void VSync_ApplyOpenGL() {}
#ifdef _WIN64
void ReflexInject_SetEnabled(bool) noexcept {}
#endif
void Log_SetLevel(LogLevel level) { config_test::applied_log_level = static_cast<int>(level); }
LogLevel Log_ParseLevel(const char* text) {
    for (const LogLevel level : { LogLevel::Error, LogLevel::Warn, LogLevel::Info, LogLevel::Debug }) {
        static const char* const names[] = { "error", "warn", "info", "debug" };
        if (std::strcmp(text, names[static_cast<int>(level)]) == 0) return level;
    }
    return LogLevel::Warn;
}
const char* Log_GetProcessName() { return "config-test"; }
// The Sleep-mode reconcile's key note (nvapi_hooks.cpp is not linked here).
void NvAPI_PrepareUnifiedKey(bool) noexcept {}
void Log_Write(LogLevel level, const char* format, ...) {
    CHECK(config_test::logger_alive.load());
    if (GetCurrentThreadId() != config_test::caller_thread) ++config_test::worker_logs;
    if (level != LogLevel::Info && level != LogLevel::Warn) return;
    char text[4096] = {};
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    std::lock_guard<std::mutex> lock(config_test::log_mutex);
    (level == LogLevel::Warn ? config_test::warn_lines : config_test::info_lines).emplace_back(text);
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
// One key of any file and section seeded into the fake disk by the next fresh launch (Reset), then forgotten:
// the OSD_Preset_<n> sections of the local INI and the shared presets file.
struct DiskEntry {
    const char* path;
    const char* section;
    const char* key;
    const char* value;
};
std::vector<DiskEntry> extra_seed;
// One launch of the configuration on the fake disk. keep_disk false seeds the fake INI from `ini` first (a launch
// without any key on the disk is a first launch); keep_disk true loads the file the previous launch left behind.
void Launch(bool creation_failure, bool initial_write_failure, bool keep_disk, const std::vector<IniEntry>& ini) {
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
        if (!keep_disk) {
            disk.clear();
            for (const auto& entry : ini) disk[{ local_path, "FrameLimiter", entry.key }] = entry.value;
            for (const auto& entry : extra_seed) disk[{ entry.path, entry.section, entry.key }] = entry.value;
            extra_seed.clear();
        }
        writes.clear();
        hold_start = true;
        hold_io = false;
        entered_io = false;
        fail_create = creation_failure;
        fail_writes = initial_write_failure;
        throw_writes = false;
        creates = closes = 0;
    }
    {
        std::lock_guard<std::mutex> lock(log_mutex);
        info_lines.clear();
        warn_lines.clear();
    }
    logger_alive = true;
    applied_log_level = -1;
    logger_shutdowns = worker_logs = 0;
    g_config = {};
    PacingPipelineConfig stale{};
    stale.test_mode = PacingTestMode::ReflexOnly; // LoadConfig must apply the loaded mode
    stale.gpu_probes = false;
    PacingPipeline_SetConfig(stale);
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
void Reset(bool creation_failure = false, bool initial_write_failure = false,
           const std::vector<IniEntry>& ini = {}) {
    Launch(creation_failure, initial_write_failure, false, ini);
}
// The next launch of the same file: LoadConfig again on what the previous launch wrote.
void Relaunch() {
    Launch(false, false, true, {});
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

// The state every observer of the kept pacing settings is in: the queue probe's gate and its law feed, the probe source,
// the marker stamps, the margin end, the gate probes, and the pipeline's pacer numbers and owner.
struct Observed {
    bool gpu_queue_probe, law_feed;
    int upstream, probe_source, margin_end;
    bool gpu_probes, queue_law;
    int queue_rank, need_frames, allow_us;
    double hold_share;
    bool unified;
};
Observed Observe() {
    const PacingPipelineConfig pipeline = PacingPipeline_GetConfig();
    return Observed{ g_gpu_queue_probe.load(), g_gpu_queue_law_feed.load(), g_unified_queue_upstream.load(),
        g_gpu_queue_probe_source.load(), g_unified_queue_margin_end.load(), pipeline.gpu_probes,
        pipeline.unified_queue_law, pipeline.unified_queue_rank, pipeline.unified_queue_need_frames,
        pipeline.unified_queue_allow_us, pipeline.unified_hold_share, pipeline.unified };
}
bool SameObserved(const Observed& a, const Observed& b) {
    return a.gpu_queue_probe == b.gpu_queue_probe && a.law_feed == b.law_feed && a.upstream == b.upstream &&
        a.probe_source == b.probe_source && a.margin_end == b.margin_end && a.gpu_probes == b.gpu_probes &&
        a.queue_law == b.queue_law && a.queue_rank == b.queue_rank && a.need_frames == b.need_frames &&
        a.allow_us == b.allow_us && a.hold_share == b.hold_share && a.unified == b.unified;
}
bool SamePipeline(const PacingPipelineConfig& a, const PacingPipelineConfig& b) {
    return a.gpu_probes == b.gpu_probes && a.unified == b.unified && a.test_mode == b.test_mode &&
        a.unified_hold_share == b.unified_hold_share && a.unified_queue_law == b.unified_queue_law &&
        a.unified_queue_rank == b.unified_queue_rank && a.unified_queue_need_frames == b.unified_queue_need_frames &&
        a.gpu_queue_probe_source == b.gpu_queue_probe_source && a.unified_queue_upstream == b.unified_queue_upstream &&
        a.unified_queue_margin_end == b.unified_queue_margin_end && a.unified_queue_allow_us == b.unified_queue_allow_us;
}

// Everything a launch publishes of the kept settings (the caps, the preference, the owner and the test mode, the
// pipeline and its observers), to compare two launches.
struct Loaded {
    int overrides, target_fps, background_fps, fg_off_fps, user_target, user_background, user_fg_off;
    PacingPreference preference;
    PacingTestMode raw_mode, mode;
    bool unified, sleep_bypass;
    PacingPipelineConfig pipeline;
    Observed observed;
};
Loaded Capture() {
    return Loaded{ Config_DeveloperOverrides(), g_config.target_fps, g_config.background_fps, g_config.fg_off_fps,
        g_user_target_fps.load(), g_background_fps.load(), g_fg_off_fps.load(), g_config.pacing_preference,
        g_config.pacing_test_mode, PacingPipeline_TestMode(), PacingPipeline_Unified(), PacingPipeline_SleepBypass(),
        PacingPipeline_GetConfig(), Observe() };
}
bool SameLoaded(const Loaded& a, const Loaded& b) {
    return a.overrides == b.overrides && a.target_fps == b.target_fps && a.background_fps == b.background_fps &&
        a.fg_off_fps == b.fg_off_fps && a.user_target == b.user_target && a.user_background == b.user_background &&
        a.user_fg_off == b.user_fg_off && a.preference == b.preference && a.raw_mode == b.raw_mode &&
        a.mode == b.mode && a.unified == b.unified && a.sleep_bypass == b.sleep_bypass &&
        SamePipeline(a.pipeline, b.pipeline) && SameObserved(a.observed, b.observed);
}

// How many Warn lines of the last launch contain the text.
size_t WarnCount(const char* needle) {
    std::lock_guard<std::mutex> lock(log_mutex);
    size_t count = 0;
    for (const std::string& line : warn_lines)
        if (line.find(needle) != std::string::npos) ++count;
    return count;
}
// The info line that names the removed settings an old INI still held (empty when the launch logged none); the
// production logger is replaced by this file's Log_Write, which keeps every Info line of the launch.
constexpr const char* removed_info_prefix = "Config: settings that no longer exist were ignored: ";
std::string RemovedInfoLine() {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::string found;
    for (const std::string& line : info_lines) {
        if (line.rfind(removed_info_prefix, 0) != 0) continue;
        CHECK(found.empty()); // at most one such line per launch
        found = line;
    }
    return found;
}
// True when the line lists key=value as one whole entry.
bool ListedInLine(const std::string& line, const char* key, const char* value) {
    const std::string token = std::string(key) + "=" + value;
    for (size_t at = line.find(token); at != std::string::npos; at = line.find(token, at + 1)) {
        const bool starts = at >= 2 && (line.compare(at - 2, 2, ": ") == 0 || line.compare(at - 2, 2, ", ") == 0);
        const size_t end = at + token.size();
        const bool ends = end == line.size() || line.compare(end, 2, ", ") == 0;
        if (starts && ends) return true;
    }
    return false;
}

// The resolver holds exactly the live config: the owner derived from the effective test mode, the effective rank and
// share of the pacing preference, and every kept developer field (32-bit clamping included), and the observers.
void CheckPipelineMatchesConfig() {
    const PacingPipelineConfig pipeline = PacingPipeline_GetConfig();
    const bool is64 = sizeof(void*) == 8;
    // The owner is derived from the effective test mode (developer_settings gate): 64-bit and no test mode.
    const PacingTestMode mode = PacingPipeline_EffectiveTestMode(g_config.pacing_test_mode, g_config.developer_settings);
    const bool unified = is64 && mode == PacingTestMode::Off;
    CHECK(pipeline.test_mode == mode);
    CHECK(pipeline.unified == unified);
    CHECK(PacingPipeline_Unified() == unified);
    CHECK(PacingPipeline_TestMode() == mode);
    CHECK(PacingPipeline_SleepBypass() == (mode == PacingTestMode::Off));
    CHECK(pipeline.gpu_probes == g_config.gpu_present_gate_probes);
    // Keys unified_hold_share and unified_queue_rank reach the pipeline as the effective pair the pacing preference resolves
    // with them (g_config keeps the raw values); the hold share is 64-bit only, forced to the default (0) on Win32.
    const PacingPreferenceResolution effective = PacingPipeline_ResolvePreference(
        g_config.pacing_preference, g_config.unified_queue_rank, g_config.unified_hold_share, g_config.fg_lead_extra_us);
    CHECK(pipeline.unified_hold_share == (is64 ? effective.hold_share : 0.0));
    CHECK(pipeline.fg_lead_extra_us == effective.lead_extra_us);
    // The queue law forces the probe on and feeds the law while the unified pacer owns pacing (64-bit only);
    // the probe key is a 64-bit measurement; the user's values stay in g_config.
    const bool law_in_use = g_config.unified_queue_law && pipeline.unified;
    CHECK(g_gpu_queue_probe.load() == ((g_config.gpu_queue_probe && is64) || law_in_use));
    CHECK(g_gpu_queue_law_feed.load() == law_in_use);
    CHECK(pipeline.unified_queue_law == (g_config.unified_queue_law && is64));
    CHECK(pipeline.unified_queue_rank == effective.queue_rank);
    CHECK(pipeline.unified_queue_need_frames == g_config.unified_queue_need_frames);
    // The upstream stamping key: 64-bit only (0 on Win32), in the pipeline snapshot and the hook gate.
    CHECK(pipeline.unified_queue_upstream == (is64 ? g_config.unified_queue_upstream : 0));
    CHECK(g_unified_queue_upstream.load() == (is64 ? g_config.unified_queue_upstream : 0));
    CHECK(g_gpu_queue_probe_source.load() == (is64 ? g_config.gpu_queue_probe_source : 0));
    CHECK(pipeline.gpu_queue_probe_source == (is64 ? g_config.gpu_queue_probe_source : 0));
    CHECK(pipeline.unified_queue_margin_end == (is64 ? g_config.unified_queue_margin_end : 0));
    CHECK(g_unified_queue_margin_end.load() == (is64 ? g_config.unified_queue_margin_end : 0));
    CHECK(pipeline.unified_queue_allow_us == g_config.unified_queue_allow_us);
}
// The production behavior: no developer override, the unified owner on 64-bit, no test mode, the Reflex Sleep bypass.
void CheckProductionDefaults() {
    CHECK(Config_DeveloperOverrides() == 0);
    CHECK(PacingPipeline_Unified() == (sizeof(void*) == 8));
    CHECK(PacingPipeline_TestMode() == PacingTestMode::Off && PacingPipeline_SleepBypass());
    CheckPipelineMatchesConfig();
}

void TestPacingPipelineDefaults() {
    const bool is64 = sizeof(void*) == 8;
    Reset(); // a first launch: no INI on the disk
    CHECK(Config_IsFirstLaunch());
    CHECK(g_config.gpu_present_gate_probes && !g_config.advanced_settings);
    CHECK(!g_config.developer_settings && Config_DeveloperOverrides() == 0);
    CHECK(g_config.pacing_test_mode == PacingTestMode::Off && g_config.pacing_preference == PacingPreference::Balanced);
    // Recommended defaults: the unified pacer owns pacing (64-bit), no test mode, the Reflex Sleep bypass is in effect.
    CHECK(PacingPipeline_Unified() == is64 && PacingPipeline_TestMode() == PacingTestMode::Off);
    CHECK(PacingPipeline_SleepBypass());
    CHECK(!g_config.flip_metering_off && !g_config.control_file);
    // The queue probe, the queue law and the probe's source key are on by default (one constant each).
    CHECK(g_config.gpu_queue_probe && kPacingGpuQueueProbeDefault);
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
    CHECK(g_gpu_queue_probe_source.load() == (is64 ? 1 : 0));
    CHECK(g_config.unified_queue_margin_end == 1 && kPacingUnifiedQueueMarginEndDefault == 1);
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_margin_end") == "1");
    CHECK(g_unified_queue_margin_end.load() == (is64 ? 1 : 0));
    CHECK(g_config.unified_queue_upstream == 0 && kPacingUnifiedQueueUpstreamDefault == 0);
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_upstream") == "0");
    CHECK(g_unified_queue_upstream.load() == 0);
    CHECK(g_gpu_queue_law_feed.load() == is64 && g_gpu_queue_probe.load() == is64);
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_probe") == "true");
    // Present-first ordering is on by default (one constant); the published switch follows the configuration.
    CHECK(g_config.gpu_wait_present_first && kPacingGpuWaitPresentFirstDefault && g_gpu_wait_present_first.load());
    CHECK(Value(local_path, "FrameLimiter", "gpu_wait_present_first") == "true");
    // The start limit is on by default as well (one constant); the published switch follows the configuration.
    CHECK(g_config.gpu_wait_start_limit && kPacingGpuWaitStartLimitDefault && g_gpu_wait_start_limit.load());
    CHECK(Value(local_path, "FrameLimiter", "gpu_wait_start_limit") == "true");
    // The lead queue correction is on by default too (one constant); the published switch follows the configuration.
    CHECK(g_config.lead_queue_correction && kPacingLeadQueueCorrectionDefault && g_lead_queue_correction.load());
    CHECK(Value(local_path, "FrameLimiter", "lead_queue_correction") == "true");
    // The frame generation ordering is on by default: the switch, the trailing release, the hold on the separate
    // presenter's route and the breaks. The INI only reads them (never saves them), and the published switches follow the
    // configuration; the trailing release and the breaks never publish on in a 32-bit process.
    CHECK(g_config.fg_present_order && g_config.fg_present_order_trailing && g_config.fg_separate_presenter_hold);
    CHECK(g_config.fg_render_break_every == 1 && g_config.fg_render_break_window == -1);
    CHECK(g_fg_present_order.load() == is64 && g_fg_present_order_trailing.load() == is64);
    CHECK(g_fg_render_break_every.load() == (is64 ? 1 : 0) && g_fg_render_break_window.load() == -1);
    CHECK(g_fg_separate_presenter_hold.load());
    for (const char* key : {"fg_present_order", "fg_present_order_trailing", "fg_separate_presenter_hold",
                            "fg_render_break_every", "fg_render_break_window"})
        CHECK(Value(local_path, "FrameLimiter", key).empty());
    const Config recommended{}; // the struct's own initializers agree with the INI read defaults above
    CHECK(recommended.fg_present_order && recommended.fg_present_order_trailing && recommended.fg_separate_presenter_hold);
    CHECK(recommended.fg_render_break_every == 1 && recommended.fg_render_break_window == -1);
    CheckProductionDefaults();
    // The startup rewrite saves every key, including the developer keys.
    CHECK(Value(local_path, "FrameLimiter", "flip_metering_off") == "false");
    CHECK(Value(local_path, "FrameLimiter", "control_file") == "false");
    CHECK(Value(local_path, "FrameLimiter", "developer_settings") == "false");
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "7");
    CHECK(Value(local_path, "FrameLimiter", "pacing_preference") == "balanced");
    CHECK(Value(local_path, "FrameLimiter", "pacing_test_mode") == "off");
    CHECK(Value(local_path, "FrameLimiter", "gpu_present_gate_probes") == "true");
    ReleaseStart();CHECK(OwnerShutdown());
}

// An INI from before config_version 5 saved the old defaults of the queue law, the queue probe and the
// probe's source key (false, false, 0): it moves to the new defaults once and keeps every other value; a
// version-5 INI keeps its explicit choices; keys the INI does not carry take the new defaults; a first
// launch (no INI) takes the defaults and is written at version 7.
void TestQueueDefaultsMigration() {
    const bool is64 = sizeof(void*) == 8;
    // An INI of version 4 as the earlier build wrote it.
    Reset(false, false, {{"config_version", "4"}, {"unified_queue_law", "false"},
        {"gpu_queue_probe", "false"}, {"gpu_queue_probe_source", "0"}, {"unified_queue_rank", "3"},
        {"unified_queue_allow_us", "2500"}, {"target_fps", "100"}});
    CHECK(g_config.unified_queue_law && g_config.gpu_queue_probe && g_config.gpu_queue_probe_source == 1);
    CHECK(g_config.unified_queue_rank == 3 && g_config.unified_queue_allow_us == 2500 && g_config.target_fps == 100);
    CheckPipelineMatchesConfig();
    CHECK(g_gpu_queue_probe.load() == is64 && g_gpu_queue_law_feed.load() == is64);
    CHECK(g_gpu_queue_probe_source.load() == (is64 ? 1 : 0));
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "7");
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_law") == "true");
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_probe") == "true");
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_probe_source") == "1");
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_rank") == "3");
    ReleaseStart();CHECK(OwnerShutdown());

    // One key different is enough, and an INI without a version is older still.
    Reset(false, false, {{"gpu_queue_probe_source", "0"}});
    CHECK(g_config.unified_queue_law && g_config.gpu_queue_probe && g_config.gpu_queue_probe_source == 1);
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_probe_source") == "1");
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "7");
    ReleaseStart();CHECK(OwnerShutdown());

    // Keys the old INI does not carry take the new defaults.
    Reset(false, false, {{"config_version", "4"}});
    CHECK(g_config.unified_queue_law && g_config.gpu_queue_probe && g_config.gpu_queue_probe_source == 1);
    ReleaseStart();CHECK(OwnerShutdown());

    // Version 5: the explicit choices stay, saved as they are.
    Reset(false, false, {{"config_version", "5"}, {"unified_queue_law", "false"},
        {"gpu_queue_probe", "false"}, {"gpu_queue_probe_source", "0"}});
    CHECK(!g_config.unified_queue_law && !g_config.gpu_queue_probe && g_config.gpu_queue_probe_source == 0);
    CHECK(!g_gpu_queue_probe.load() && !g_gpu_queue_law_feed.load() && g_gpu_queue_probe_source.load() == 0);
    CheckPipelineMatchesConfig();
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_law") == "false");
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_probe") == "false");
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_probe_source") == "0");
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "7");
    CHECK(Config_DeveloperOverrides() == 3);
    ReleaseStart();CHECK(OwnerShutdown());

    // A first launch (no INI at all) takes the defaults and writes version 7.
    Reset();
    CHECK(g_config.unified_queue_law && g_config.gpu_queue_probe && g_config.gpu_queue_probe_source == 1);
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "7");
    ReleaseStart();CHECK(OwnerShutdown());
}

// developer_settings is read and saved; every developer key away from its default is counted (the 15 developer
// fields), and Reset returns exactly those keys to the defaults and applies them, leaving the user's settings alone.
void TestDeveloperSettingsReset() {
    const bool is64 = sizeof(void*) == 8;
    Reset(false, false, {{"config_version", "6"}, {"developer_settings", "true"}, {"target_fps", "90"},
        {"pacing_preference", "smooth"}, {"gpu_present_gate_probes", "false"}, {"flip_metering_off", "true"},
        {"control_file", "true"}, {"gpu_queue_probe", "false"}, {"gpu_queue_probe_source", "0"},
        {"unified_queue_margin_end", "0"}, {"pacing_test_mode", "driver_limiter"}, {"unified_hold_share", "0.1"},
        {"unified_queue_law", "false"}, {"unified_queue_rank", "3"}, {"unified_queue_need_frames", "6"},
        {"unified_queue_upstream", "1"}, {"unified_queue_allow_us", "2500"}, {"hang_report_dump", "false"},
        {"hang_report_test_after_s", "30"}});
    CHECK(g_config.developer_settings);
    CHECK(Value(local_path, "FrameLimiter", "developer_settings") == "true");
    CHECK(Config_DeveloperOverrides() == 15);
    CHECK(PacingPipeline_TestMode() == PacingTestMode::DriverLimiter && !PacingPipeline_Unified());
    CheckPipelineMatchesConfig();
    Config_ResetDeveloperSettings();
    CHECK(Config_DeveloperOverrides() == 0);
    // The user's settings stay: the cap, the preference and developer_settings itself.
    CHECK(g_config.developer_settings && g_config.target_fps == 90 && g_config.pacing_preference == PacingPreference::Smooth);
    CHECK(g_config.gpu_present_gate_probes && !g_config.flip_metering_off && !g_config.control_file);
    CHECK(g_config.gpu_queue_probe && g_config.gpu_queue_probe_source == 1 && g_config.unified_queue_margin_end == 1);
    CHECK(g_config.pacing_test_mode == PacingTestMode::Off && g_config.unified_hold_share == 0.0);
    CHECK(g_config.unified_queue_law && g_config.unified_queue_rank == 2 && g_config.unified_queue_need_frames == 4);
    CHECK(g_config.unified_queue_upstream == 0 && g_config.unified_queue_allow_us == 4000);
    CHECK(PacingPipeline_TestMode() == PacingTestMode::Off && PacingPipeline_Unified() == is64);
    CHECK(PacingPipeline_SleepBypass());
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());
}

// Developer gpu_present_gate_probes: false loads, reaches the pipeline, is saved, counts as one developer override, is
// a live key and resets to true.
void TestGpuPresentGateProbesKey() {
    Reset(false, false, {{"config_version", "6"}, {"gpu_present_gate_probes", "false"}});
    CHECK(!g_config.gpu_present_gate_probes && !PacingPipeline_GetConfig().gpu_probes);
    CheckPipelineMatchesConfig();
    CHECK(Value(local_path, "FrameLimiter", "gpu_present_gate_probes") == "false");
    CHECK(Config_DeveloperOverrides() == 1);
    char error[128] = {};
    CHECK(Config_SetLiveKey("gpu_present_gate_probes", "on", error, sizeof(error)) && g_config.gpu_present_gate_probes);
    CHECK(PacingPipeline_GetConfig().gpu_probes && Config_DeveloperOverrides() == 0);
    CHECK(Config_SetLiveKey("gpu_present_gate_probes", "0", error, sizeof(error)) && !g_config.gpu_present_gate_probes);
    CHECK(!PacingPipeline_GetConfig().gpu_probes);
    CHECK(!Config_SetLiveKey("gpu_present_gate_probes", "maybe", error, sizeof(error)) && !g_config.gpu_present_gate_probes);
    Config_ResetDeveloperSettings();
    CHECK(g_config.gpu_present_gate_probes && PacingPipeline_GetConfig().gpu_probes);
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());
}

// Developer gpu_wait_present_first is on by default (kPacingGpuWaitPresentFirstDefault): a launch without the key
// publishes it and logs nothing about it; false loads, is saved, is published, is a live key, counts as the one
// developer override and logs one Warn line. An interleave logs one line more whichever way this key is set: with
// the start limit on (its default) the interleave alternates the start limit, not this key, so the line is the start
// limit's (TestGpuWaitStartLimitIsOnByDefault has the case with the start limit off). The lines are counted, not
// read.
void TestGpuWaitPresentFirstIsOnByDefault() {
    const auto launch = [](const std::vector<IniEntry>& extra) {
        std::vector<IniEntry> ini{{"config_version", "6"}};
        ini.insert(ini.end(), extra.begin(), extra.end());
        Reset(false, false, ini);
        return WarnCount("");
    };
    const size_t quiet = launch({});
    CHECK(g_config.gpu_wait_present_first && g_gpu_wait_present_first.load() && Config_DeveloperOverrides() == 0);
    CHECK(Value(local_path, "FrameLimiter", "gpu_wait_present_first") == "true");
    ReleaseStart();CHECK(OwnerShutdown());

    CHECK(launch({{"gpu_wait_present_first", "false"}}) == quiet + 1);
    CHECK(!g_config.gpu_wait_present_first && !g_gpu_wait_present_first.load());
    CHECK(Value(local_path, "FrameLimiter", "gpu_wait_present_first") == "false");
    CHECK(Config_DeveloperOverrides() == 1);
    char error[128] = {};
    CHECK(Config_SetLiveKey("gpu_wait_present_first", "on", error, sizeof(error)) && g_config.gpu_wait_present_first);
    CHECK(g_gpu_wait_present_first.load() && Config_DeveloperOverrides() == 0);
    CHECK(Config_SetLiveKey("gpu_wait_present_first", "off", error, sizeof(error)) && !g_config.gpu_wait_present_first);
    CHECK(!g_gpu_wait_present_first.load() && Config_DeveloperOverrides() == 1);
    Config_ResetDeveloperSettings();
    CHECK(g_config.gpu_wait_present_first && g_gpu_wait_present_first.load() && Config_DeveloperOverrides() == 0);
    ReleaseStart();CHECK(OwnerShutdown());

    CHECK(launch({{"gpu_wait_ab_interleave_s", "30"}}) == quiet + 1);
    CHECK(g_config.gpu_wait_present_first && g_gpu_wait_present_first.load());
    CHECK(g_gpu_wait_ab_interleave_s.load() == 30 && Config_DeveloperOverrides() == 1);
    ReleaseStart();CHECK(OwnerShutdown());

    CHECK(launch({{"gpu_wait_present_first", "false"}, {"gpu_wait_ab_interleave_s", "30"}}) == quiet + 2);
    CHECK(!g_gpu_wait_present_first.load() && Config_DeveloperOverrides() == 2);
    ReleaseStart();CHECK(OwnerShutdown());
}

// Developer gpu_wait_start_limit is on by default (kPacingGpuWaitStartLimitDefault): a launch without the key
// publishes it and logs nothing about it; false loads, is saved, is published, is a live key, counts as one developer
// override and logs one Warn line. With it on, an interleave logs one line of its own (it alternates the start limit
// and leaves present-first on its key); with it off the interleave alternates present-first, as it did before the
// start limit existed, and logs that line besides the key's own. The lines are counted, not read.
void TestGpuWaitStartLimitIsOnByDefault() {
    const auto launch = [](const std::vector<IniEntry>& extra) {
        std::vector<IniEntry> ini{{"config_version", "6"}};
        ini.insert(ini.end(), extra.begin(), extra.end());
        Reset(false, false, ini);
        return WarnCount("");
    };
    const size_t quiet = launch({});
    CHECK(g_config.gpu_wait_start_limit && g_gpu_wait_start_limit.load() && Config_DeveloperOverrides() == 0);
    CHECK(Value(local_path, "FrameLimiter", "gpu_wait_start_limit") == "true");
    ReleaseStart();CHECK(OwnerShutdown());

    CHECK(launch({{"gpu_wait_start_limit", "false"}}) == quiet + 1);
    CHECK(!g_config.gpu_wait_start_limit && !g_gpu_wait_start_limit.load());
    CHECK(Value(local_path, "FrameLimiter", "gpu_wait_start_limit") == "false");
    CHECK(Config_DeveloperOverrides() == 1);
    char error[128] = {};
    CHECK(Config_SetLiveKey("gpu_wait_start_limit", "on", error, sizeof(error)) && g_config.gpu_wait_start_limit);
    CHECK(g_gpu_wait_start_limit.load() && Config_DeveloperOverrides() == 0);
    CHECK(Config_SetLiveKey("gpu_wait_start_limit", "off", error, sizeof(error)) && !g_config.gpu_wait_start_limit);
    CHECK(!g_gpu_wait_start_limit.load() && Config_DeveloperOverrides() == 1);
    Config_ResetDeveloperSettings();
    CHECK(g_config.gpu_wait_start_limit && g_gpu_wait_start_limit.load() && Config_DeveloperOverrides() == 0);
    ReleaseStart();CHECK(OwnerShutdown());

    // The interleave with the key on alternates the start limit: one line, present-first untouched.
    CHECK(launch({{"gpu_wait_ab_interleave_s", "30"}}) == quiet + 1);
    CHECK(g_gpu_wait_start_limit.load() && g_gpu_wait_present_first.load() && g_gpu_wait_ab_interleave_s.load() == 30);
    ReleaseStart();CHECK(OwnerShutdown());

    // With the key off the interleave alternates present-first: the key's own line and the interleave's.
    CHECK(launch({{"gpu_wait_start_limit", "false"}, {"gpu_wait_ab_interleave_s", "30"}}) == quiet + 2);
    CHECK(!g_gpu_wait_start_limit.load() && g_gpu_wait_present_first.load() && g_gpu_wait_ab_interleave_s.load() == 30);
    ReleaseStart();CHECK(OwnerShutdown());
}

// Developer lead_queue_correction is on by default (kPacingLeadQueueCorrectionDefault): a launch without the key
// publishes it and logs nothing about it; false loads, is saved, is published, is a live key, counts as one developer
// override and logs one Warn line. The lines are counted, not read.
void TestLeadQueueCorrectionIsOnByDefault() {
    const auto launch = [](const std::vector<IniEntry>& extra) {
        std::vector<IniEntry> ini{{"config_version", "6"}};
        ini.insert(ini.end(), extra.begin(), extra.end());
        Reset(false, false, ini);
        return WarnCount("");
    };
    const size_t quiet = launch({});
    CHECK(g_config.lead_queue_correction && g_lead_queue_correction.load() && Config_DeveloperOverrides() == 0);
    CHECK(Value(local_path, "FrameLimiter", "lead_queue_correction") == "true");
    ReleaseStart();CHECK(OwnerShutdown());

    CHECK(launch({{"lead_queue_correction", "false"}}) == quiet + 1);
    CHECK(!g_config.lead_queue_correction && !g_lead_queue_correction.load());
    CHECK(Value(local_path, "FrameLimiter", "lead_queue_correction") == "false");
    CHECK(Config_DeveloperOverrides() == 1);
    char error[128] = {};
    CHECK(Config_SetLiveKey("lead_queue_correction", "on", error, sizeof(error)) && g_config.lead_queue_correction);
    CHECK(g_lead_queue_correction.load() && Config_DeveloperOverrides() == 0);
    CHECK(Config_SetLiveKey("lead_queue_correction", "off", error, sizeof(error)) && !g_config.lead_queue_correction);
    CHECK(!g_lead_queue_correction.load() && Config_DeveloperOverrides() == 1);
    Config_ResetDeveloperSettings();
    CHECK(g_config.lead_queue_correction && g_lead_queue_correction.load() && Config_DeveloperOverrides() == 0);
    ReleaseStart();CHECK(OwnerShutdown());
}

// The frame generation ordering switches that became defaults (fg_present_order_trailing, fg_render_break_every 1,
// fg_separate_presenter_hold) are on by default and are not developer overrides: the INI only reads them. A launch
// without them publishes them and logs nothing about them; one that is off (the breaks also at a value other than 1)
// loads, is published, is a live key and logs one Warn line. The lines are counted, not read.
void TestFgOrderingSwitchesAreOnByDefault() {
    const bool is64 = sizeof(void*) == 8;
    const auto launch = [](const std::vector<IniEntry>& extra) {
        std::vector<IniEntry> ini{{"config_version", "6"}};
        ini.insert(ini.end(), extra.begin(), extra.end());
        Reset(false, false, ini);
        return WarnCount("");
    };
    const size_t quiet = launch({});
    CHECK(g_config.fg_present_order_trailing && g_config.fg_separate_presenter_hold && g_config.fg_render_break_every == 1);
    CHECK(g_fg_present_order_trailing.load() == is64 && g_fg_render_break_every.load() == (is64 ? 1 : 0));
    CHECK(g_fg_separate_presenter_hold.load() && Config_DeveloperOverrides() == 0);
    ReleaseStart();CHECK(OwnerShutdown());

    CHECK(launch({{"fg_present_order_trailing", "false"}}) == quiet + 1);
    CHECK(!g_config.fg_present_order_trailing && !g_fg_present_order_trailing.load() && Config_DeveloperOverrides() == 0);
    char error[128] = {};
    CHECK(Config_SetLiveKey("fg_present_order_trailing", "on", error, sizeof(error)) && g_config.fg_present_order_trailing);
    CHECK(g_fg_present_order_trailing.load() == is64);
    CHECK(Config_SetLiveKey("fg_present_order_trailing", "off", error, sizeof(error)) && !g_fg_present_order_trailing.load());
    ReleaseStart();CHECK(OwnerShutdown());

    CHECK(launch({{"fg_render_break_every", "0"}}) == quiet + 1);
    CHECK(g_config.fg_render_break_every == 0 && g_fg_render_break_every.load() == 0 && Config_DeveloperOverrides() == 0);
    CHECK(Config_SetLiveKey("fg_render_break_every", "1", error, sizeof(error)) && g_config.fg_render_break_every == 1);
    CHECK(g_fg_render_break_every.load() == (is64 ? 1 : 0));
    ReleaseStart();CHECK(OwnerShutdown());

    CHECK(launch({{"fg_render_break_every", "2"}}) == quiet + 1);
    CHECK(g_config.fg_render_break_every == 2 && g_fg_render_break_every.load() == (is64 ? 2 : 0));
    ReleaseStart();CHECK(OwnerShutdown());

    CHECK(launch({{"fg_separate_presenter_hold", "false"}}) == quiet + 1);
    CHECK(!g_config.fg_separate_presenter_hold && !g_fg_separate_presenter_hold.load() && Config_DeveloperOverrides() == 0);
    CHECK(Config_SetLiveKey("fg_separate_presenter_hold", "on", error, sizeof(error)) && g_fg_separate_presenter_hold.load());
    ReleaseStart();CHECK(OwnerShutdown());

    CHECK(launch({{"fg_present_order_trailing", "false"}, {"fg_render_break_every", "0"},
                  {"fg_separate_presenter_hold", "false"}}) == quiet + 3);
    CHECK(!g_fg_present_order_trailing.load() && g_fg_render_break_every.load() == 0 && !g_fg_separate_presenter_hold.load());
    ReleaseStart();CHECK(OwnerShutdown());
}

// Developer flip_metering_off: true loads, is saved, counts as a developer
// override and resets to false (the block itself is installed at init only).
void TestFlipMeteringOffKey() {
    Reset(false, false, {{"config_version", "4"}, {"flip_metering_off", "true"}});
    CHECK(g_config.flip_metering_off);
    CHECK(Value(local_path, "FrameLimiter", "flip_metering_off") == "true");
    CHECK(Config_DeveloperOverrides() == 1);
    Config_ResetDeveloperSettings();
    CHECK(!g_config.flip_metering_off);
    ReleaseStart();CHECK(OwnerShutdown());
}

// Stall report keys: hang_report (a normal switch, off by default, not a developer override), hang_report_dump (on by
// default) and the developer hang_report_test_after_s (0 = off): each loads, is saved at its default too, the last two
// count as developer overrides and the developer reset returns them to their defaults. None is a live key.
void TestHangReportKeys() {
    Reset(false, false, {{"config_version", "4"}});
    CHECK(!g_config.hang_report && g_config.hang_report_dump && g_config.hang_report_test_after_s == 0);
    CHECK(Config_DeveloperOverrides() == 0);
    CHECK(Value(local_path, "FrameLimiter", "hang_report") == "false");
    CHECK(Value(local_path, "FrameLimiter", "hang_report_dump") == "true");
    CHECK(Value(local_path, "FrameLimiter", "hang_report_test_after_s") == "0");
    ReleaseStart();CHECK(OwnerShutdown());
    Reset(false, false, {{"config_version", "4"}, {"hang_report", "true"}});
    CHECK(g_config.hang_report && Config_DeveloperOverrides() == 0);
    CHECK(Value(local_path, "FrameLimiter", "hang_report") == "true");
    Config_ResetDeveloperSettings();
    CHECK(g_config.hang_report);   // the switch is the user's, the developer reset leaves it
    ReleaseStart();CHECK(OwnerShutdown());
    Reset(false, false, {{"config_version", "4"}, {"hang_report_test_after_s", "30"}});
    CHECK(g_config.hang_report_test_after_s == 30);
    CHECK(Value(local_path, "FrameLimiter", "hang_report_test_after_s") == "30");
    CHECK(Config_DeveloperOverrides() == 1);
    Config_ResetDeveloperSettings();
    CHECK(g_config.hang_report_test_after_s == 0 && Config_DeveloperOverrides() == 0);
    ReleaseStart();CHECK(OwnerShutdown());
    for (const char* value : {"-5", "100000"}) {
        Reset(false, false, {{"config_version", "4"}, {"hang_report_test_after_s", value}});
        CHECK(g_config.hang_report_test_after_s == (value[0] == '-' ? 0 : 3600));
        ReleaseStart();CHECK(OwnerShutdown());
    }
    Reset(false, false, {{"config_version", "4"}, {"hang_report_dump", "false"}});
    CHECK(!g_config.hang_report_dump);
    CHECK(Value(local_path, "FrameLimiter", "hang_report_dump") == "false");
    CHECK(Config_DeveloperOverrides() == 1);
    Config_ResetDeveloperSettings();
    CHECK(g_config.hang_report_dump && Config_DeveloperOverrides() == 0);
    // Launch-only keys: none is a live key.
    char error[128] = {};
    for (const char* key : {"hang_report", "hang_report_dump", "hang_report_test_after_s"})
        CHECK(!Config_SetLiveKey(key, "1", error, sizeof(error)));
    ReleaseStart();CHECK(OwnerShutdown());
}

// unified_hold_share (developer, only read while the unified pacer owns pacing): default 0 (off, no override), loads
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
        CHECK(Config_DeveloperOverrides() == (test.expected != 0.0 ? 1 : 0));
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
    CHECK(g_config.unified_hold_share == 0.0 && Config_DeveloperOverrides() == 0);
    char error[128] = {};
    CHECK(Config_SetLiveKey("unified_hold_share", "0.2", error, sizeof(error)));
    CHECK(g_config.unified_hold_share == 0.2 && Config_DeveloperOverrides() == 1);
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
    Config_ResetDeveloperSettings();
    CHECK(g_config.unified_hold_share == 0.0 && Config_DeveloperOverrides() == 0);
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());
}

// unified_queue_law, unified_queue_rank, unified_queue_allow_us (developer, only read while the unified
// pacer owns pacing): the law is on by default and the numbers 2 and 4000; they load, are clamped
// (rank 1..8, allowance 0..20000), are saved, count as developer overrides only away from their
// defaults, are live keys (an unparsable value is refused, an out-of-range one clamped), reach the
// pipeline (the law is forced off on Win32), the law forces the probe atomic on and sets the feed
// atomic (64-bit, while the unified pacer owns pacing), and reset to the defaults. The probe key is on by
// default too; a test mode takes the ownership, so nothing is fed and the law is not needed.
void TestUnifiedQueueLawKeys() {
    const bool wide = sizeof(void*) == 8;
    // Defaults with the unified pacer on: the law and the probe published, no override.
    Reset(false, false, {{"config_version", "5"}});
    CHECK(g_config.unified_queue_law && g_config.gpu_queue_probe && Config_DeveloperOverrides() == 0);
    CHECK(PacingPipeline_GetConfig().unified_queue_law == wide);
    CHECK(g_gpu_queue_probe.load() == wide && g_gpu_queue_law_feed.load() == wide);
    CheckPipelineMatchesConfig();
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_law") == "true");
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_probe") == "true");
    ReleaseStart();CHECK(OwnerShutdown());
    // A test mode takes the ownership: the law is not in use, nothing is fed; the probe key alone keeps the probe on.
    Reset(false, false, {{"config_version", "5"}, {"developer_settings", "true"}, {"pacing_test_mode", "reflex_only"}});
    CHECK(g_config.unified_queue_law && !g_gpu_queue_law_feed.load() && g_gpu_queue_probe.load() == wide);
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());
    // The law and the probe key off from the INI: two overrides, nothing published.
    Reset(false, false, {{"config_version", "5"}, {"unified_queue_law", "false"},
        {"gpu_queue_probe", "false"}});
    CHECK(!g_config.unified_queue_law && !g_config.gpu_queue_probe && Config_DeveloperOverrides() == 2);
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
    CHECK(g_config.unified_queue_law && g_config.gpu_queue_probe && Config_DeveloperOverrides() == 0);
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
        CHECK(Config_DeveloperOverrides() == (test.expected != 2 ? 1 : 0));
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
        CHECK(Config_DeveloperOverrides() == (test.expected != 4000 ? 1 : 0));
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
    CHECK(Config_DeveloperOverrides() == 2);
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
    Config_ResetDeveloperSettings();
    CHECK(g_config.unified_queue_rank == 2 && g_config.unified_queue_allow_us == 4000 &&
          g_config.unified_queue_law && Config_DeveloperOverrides() == 0);
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
        CHECK(Config_DeveloperOverrides() == (test.expected != 4 ? 1 : 0));
        char saved[32] = {};
        snprintf(saved, sizeof(saved), "%d", test.expected);
        CHECK(Value(local_path, "FrameLimiter", "unified_queue_need_frames") == saved);
        ReleaseStart();CHECK(OwnerShutdown());
    }
    Reset(false, false, {{"config_version", "4"}});
    char error[128] = {};
    CHECK(Config_SetLiveKey("unified_queue_need_frames", "6", error, sizeof(error)) &&
          g_config.unified_queue_need_frames == 6);
    CHECK(Config_DeveloperOverrides() == 1);
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
    Config_ResetDeveloperSettings();
    CHECK(g_config.unified_queue_need_frames == 4 && Config_DeveloperOverrides() == 0);
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
        CHECK(Config_DeveloperOverrides() == (test.expected != 0 ? 1 : 0));
        char saved[32] = {};
        snprintf(saved, sizeof(saved), "%d", test.expected);
        CHECK(Value(local_path, "FrameLimiter", "unified_queue_upstream") == saved);
        ReleaseStart();CHECK(OwnerShutdown());
    }
    // An INI that still holds the removed keys loads as without them.
    Reset(false, false, {{"config_version", "4"}, {"unified_queue_upstream_lead_us", "1500"},
        {"unified_queue_upstream_feed_permille", "300"}});
    CHECK(g_config.unified_queue_upstream == 0 && Config_DeveloperOverrides() == 0);
    CheckPipelineMatchesConfig();
    ReleaseStart();CHECK(OwnerShutdown());
    Reset(false, false, {{"config_version", "4"}});
    CHECK(g_unified_queue_upstream.load() == 0);
    char error[128] = {};
    CHECK(Config_SetLiveKey("unified_queue_upstream", "1", error, sizeof(error)) &&
          g_config.unified_queue_upstream == 1);
    CHECK(g_unified_queue_upstream.load() == (wide ? 1 : 0) && Config_DeveloperOverrides() == 1);
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
    Config_ResetDeveloperSettings();
    CHECK(g_config.unified_queue_upstream == 0 && g_unified_queue_upstream.load() == 0 &&
          Config_DeveloperOverrides() == 0);
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
        CHECK(Config_DeveloperOverrides() == (test.expected != 1 ? 1 : 0));
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
    CHECK(g_gpu_queue_probe_source.load() == 0 && Config_DeveloperOverrides() == 1);
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
    Config_ResetDeveloperSettings();
    CHECK(g_config.gpu_queue_probe_source == 1 && g_gpu_queue_probe_source.load() == (wide ? 1 : 0) &&
          Config_DeveloperOverrides() == 0);
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
        CHECK(Config_DeveloperOverrides() == (test.expected != 1 ? 1 : 0));
        char saved[32] = {};
        snprintf(saved, sizeof(saved), "%d", test.expected);
        CHECK(Value(local_path, "FrameLimiter", "unified_queue_margin_end") == saved);
        ReleaseStart();CHECK(OwnerShutdown());
    }
    Reset(false, false, {{"config_version", "5"}});
    CHECK(g_unified_queue_margin_end.load() == (wide ? 1 : 0) && g_config.unified_queue_margin_end == 1);
    CHECK(Config_DeveloperOverrides() == 0);
    char error[128] = {};
    CHECK(Config_SetLiveKey("unified_queue_margin_end", "0", error, sizeof(error)) &&
          g_config.unified_queue_margin_end == 0);
    CHECK(g_unified_queue_margin_end.load() == 0 && Config_DeveloperOverrides() == 1);
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
    Config_ResetDeveloperSettings();
    CHECK(g_config.unified_queue_margin_end == 1 && g_unified_queue_margin_end.load() == (wide ? 1 : 0) &&
          Config_DeveloperOverrides() == 0);
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
    CHECK(Config_SetLiveKey("background_fps", "45", error, sizeof(error)));
    CHECK(g_config.background_fps == 45 && g_background_fps.load() == 45);
    CHECK(Config_SetLiveKey("background_fps", "5", error, sizeof(error)));
    CHECK(g_config.background_fps == 20 && g_background_fps.load() == 20);
    CHECK(Config_SetLiveKey("fg_off_fps", "48", error, sizeof(error)));
    CHECK(g_config.fg_off_fps == 48 && g_fg_off_fps.load() == 48);
    CHECK(Config_SetLiveKey("pacing_preference", "SMOOTH", error, sizeof(error)));
    CHECK(Config_SetLiveKey("unified_queue_rank", "3", error, sizeof(error)));
    CHECK(Config_SetLiveKey("unified_queue_law", "off", error, sizeof(error)));
    CHECK(g_config.pacing_preference == PacingPreference::Smooth && g_config.unified_queue_rank == 3);
    CHECK(!g_config.unified_queue_law);
    CheckPipelineMatchesConfig();

    CHECK(!Config_SetLiveKey("pacing_preference", "sideways", error, sizeof(error)) && error[0] != '\0');
    CHECK(g_config.pacing_preference == PacingPreference::Smooth);
    CHECK(!Config_SetLiveKey("target_fps", "60fps", error, sizeof(error)) && g_config.target_fps == 30);
    CHECK(!Config_SetLiveKey("target_fps", "", error, sizeof(error)) && g_config.target_fps == 30);
    CHECK(!Config_SetLiveKey("unified_queue_law", "maybe", error, sizeof(error)) && !g_config.unified_queue_law);
    CHECK(!Config_SetLiveKey("flip_metering_off", "true", error, sizeof(error)) && !g_config.flip_metering_off);
    CHECK(!Config_SetLiveKey("control_file", "true", error, sizeof(error)) && !g_config.control_file);
    CHECK(!Config_SetLiveKey("no_such_key", "1", error, sizeof(error)));
    CHECK(!Config_SetLiveKey("", "1", error, sizeof(error)) && !Config_SetLiveKey("target_fps", nullptr, error, sizeof(error)));
    CheckPipelineMatchesConfig();

    // No save queued: draining the writer leaves the loaded cap in the INI.
    ReleaseStart();CHECK(OwnerShutdown());
    CHECK(Value(local_path, "FrameLimiter", "target_fps") == "100");
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
        // Smooth's second lever (the FG input wait's floor at the commanded interval) and the floor's slack default.
        CHECK(g_fg_late_spacing_commanded.load() == (test.expected == PacingPreference::Smooth));
        CHECK(g_fg_late_spacing_slack.load() == kPacingFgLateSpacingSlackDefault && kPacingFgLateSpacingSlackDefault == 100);
        // The flip gate's cover and fit are on by default where the gate exists (64-bit), and published as 0 elsewhere.
        CHECK(kPacingFgFlipGateCoverDefault == 95 && kPacingFgFlipGateFitDefault == 75);
        CHECK(g_fg_flip_gate_cover.load() == (sizeof(void*) == 8 ? kPacingFgFlipGateCoverDefault : 0));
        CHECK(g_fg_flip_gate_fit.load() == (sizeof(void*) == 8 ? kPacingFgFlipGateFitDefault : 0));
        CHECK(g_fg_flip_gate_entry.load() == 0);
        // Smooth's third lever (the FG input wait's extra lead) reaches the pipeline; no other position sets it.
        CHECK(pipeline.fg_lead_extra_us ==
            (test.expected == PacingPreference::Smooth ? kPacingPreferenceSmoothLeadExtraUs : kPacingFgLeadExtraDefaultUs));
        CHECK(Config_DeveloperOverrides() == 0); // a user setting, not a developer override
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
    CHECK(Config_DeveloperOverrides() == 2); // the two developer keys, not the preference
    // The developer reset returns the developer keys and leaves the preference.
    Config_ResetDeveloperSettings();
    CHECK(Config_DeveloperOverrides() == 0);
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
    // Back at balanced the floor keeps the display-side interval; the developer key turns the commanded one on there,
    // and the slack key reaches the executor's value (0: the earlier eighth).
    CHECK(!g_fg_late_spacing_commanded.load());
    CHECK(Config_SetLiveKey("fg_late_spacing_commanded", "true", error, sizeof(error)) && g_fg_late_spacing_commanded.load());
    CHECK(Config_SetLiveKey("fg_late_spacing_commanded", "false", error, sizeof(error)) && !g_fg_late_spacing_commanded.load());
    CHECK(Config_SetLiveKey("fg_late_spacing_slack", "0", error, sizeof(error)) && g_fg_late_spacing_slack.load() == 0);
    CHECK(Config_SetLiveKey("fg_late_spacing_slack", "100", error, sizeof(error)) && g_fg_late_spacing_slack.load() == 100);
    // The extra lead's developer key reaches the pipeline, clamped, in any position, and 0 gives the position's own.
    CHECK(Config_SetLiveKey("fg_lead_extra_us", "1500", error, sizeof(error)) && PacingPipeline_GetConfig().fg_lead_extra_us == 1500);
    CHECK(Config_SetLiveKey("fg_lead_extra_us", "9000", error, sizeof(error)) &&
          g_config.fg_lead_extra_us == kPacingFgLeadExtraMaxUs && PacingPipeline_GetConfig().fg_lead_extra_us == kPacingFgLeadExtraMaxUs);
    CHECK(Config_SetLiveKey("fg_lead_extra_us", "0", error, sizeof(error)) &&
          PacingPipeline_GetConfig().fg_lead_extra_us == kPacingFgLeadExtraDefaultUs);
    CHECK(Config_DeveloperOverrides() == 0);

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

// pacing_test_mode (developer key): loads each text (case-insensitive), an invalid or empty text resets to off, saves
// canonical text, honoured only with developer_settings (the raw value stays in g_config and the INI), a live key,
// counted in the developer overrides and reset by the developer reset; the effective mode takes the ownership from
// the unified pacer and the Reflex Sleep bypass, and each mode leaves every other published observer as the production
// default does except the two the owner gates (the law's feed and `unified`).
void TestPacingTestMode() {
    const bool wide = sizeof(void*) == 8;
    struct Case { const char* text; PacingTestMode expected; };
    for (const Case& test : {Case{nullptr, PacingTestMode::Off}, Case{"off", PacingTestMode::Off},
            Case{"reflex_only", PacingTestMode::ReflexOnly}, Case{"DRIVER_LIMITER", PacingTestMode::DriverLimiter},
            Case{"reflex", PacingTestMode::Off}, Case{"", PacingTestMode::Off}}) {
        std::vector<IniEntry> ini{{"developer_settings", "true"}};
        if (test.text) ini.push_back({"pacing_test_mode", test.text});
        Reset(false, false, ini);
        CHECK(g_config.pacing_test_mode == test.expected);
        CheckPipelineMatchesConfig();
        CHECK(PacingPipeline_TestMode() == test.expected);
        CHECK(PacingPipeline_Unified() == (wide && test.expected == PacingTestMode::Off));
        CHECK(PacingPipeline_SleepBypass() == (test.expected == PacingTestMode::Off));
        CHECK(Value(local_path, "FrameLimiter", "pacing_test_mode") == PacingTestMode_Text(test.expected));
        CHECK(Config_DeveloperOverrides() == (test.expected == PacingTestMode::Off ? 0 : 1));
        ReleaseStart();CHECK(OwnerShutdown());
    }

    // The gate: without developer_settings the saved mode is not honoured and the raw value stays.
    Reset(false, false, {{"pacing_test_mode", "reflex_only"}});
    CHECK(!g_config.developer_settings && g_config.pacing_test_mode == PacingTestMode::ReflexOnly);
    CheckPipelineMatchesConfig();
    CHECK(PacingPipeline_TestMode() == PacingTestMode::Off);
    CHECK(PacingPipeline_Unified() == wide);
    CHECK(PacingPipeline_SleepBypass());
    CHECK(Value(local_path, "FrameLimiter", "pacing_test_mode") == "reflex_only");
    {
        const Observed ignored = Observe();
        char error[128] = {};
        CHECK(Config_SetLiveKey("pacing_test_mode", "off", error, sizeof(error)));
        CHECK(SameObserved(ignored, Observe()));
    }
    ReleaseStart();CHECK(OwnerShutdown());

    // The live key, the override count and the developer reset.
    Reset(false, false, {{"developer_settings", "true"}});
    char error[128] = {};
    CHECK(Config_SetLiveKey("pacing_test_mode", "Driver_Limiter", error, sizeof(error)) && error[0] == '\0');
    CHECK(g_config.pacing_test_mode == PacingTestMode::DriverLimiter && PacingPipeline_TestMode() == PacingTestMode::DriverLimiter);
    CheckPipelineMatchesConfig();
    CHECK(!PacingPipeline_Unified() && !PacingPipeline_SleepBypass());
    CHECK(!Config_SetLiveKey("pacing_test_mode", "turbo", error, sizeof(error)) && error[0] != '\0');
    CHECK(g_config.pacing_test_mode == PacingTestMode::DriverLimiter);
    CHECK(Config_DeveloperOverrides() == 1);
    Config_ResetDeveloperSettings();
    CHECK(Config_DeveloperOverrides() == 0 && g_config.pacing_test_mode == PacingTestMode::Off);
    CHECK(PacingPipeline_TestMode() == PacingTestMode::Off);
    CHECK(PacingPipeline_Unified() == wide && PacingPipeline_SleepBypass());
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

    // Observers: the production default (no test mode, the law, every developer key as below) is the reference; each
    // test mode, with the same remaining keys, differs exactly where the owner gates an observer.
    const std::vector<IniEntry> shared{{"config_version", "5"}, {"developer_settings", "true"}, {"gpu_queue_probe", "false"},
        {"unified_hold_share", "0.0625"}, {"unified_queue_rank", "3"}, {"unified_queue_need_frames", "6"},
        {"unified_queue_allow_us", "3000"}, {"gpu_present_gate_probes", "true"}, {"unified_queue_upstream", "1"}};
    Reset(false, false, shared);
    const Observed production = Observe();
    CHECK(production.unified == wide);
    CHECK(production.law_feed == wide); // the law's feed follows the owner
    CHECK(production.gpu_queue_probe == wide); // forced on by the law although the user's probe key is off
    ReleaseStart();CHECK(OwnerShutdown());
    Observed expected = production;
    expected.unified = false;
    expected.law_feed = false;
    expected.gpu_queue_probe = false; // the law no longer forces the probe on; the user's key is off
    for (const char* mode : {"reflex_only", "driver_limiter"}) {
        std::vector<IniEntry> ini = shared;
        ini.push_back({"pacing_test_mode", mode});
        Reset(false, false, ini);
        CHECK(SameObserved(Observe(), expected));
        CHECK(!PacingPipeline_Unified() && !PacingPipeline_SleepBypass());
        CheckPipelineMatchesConfig();
        ReleaseStart();CHECK(OwnerShutdown());
    }
}

// ── Removal of the selectable pacing settings (config_version 6) ──
// The 30 settings config_version 6 removed, written once here as the fixture of the contract (nothing reads the production
// table): the value an earlier build wrote when the user had touched the key, and the value it had by default (a
// default is not reported as a loss).
struct RemovedSetting {
    const char* key;
    const char* value;
    const char* old_default;
};
constexpr RemovedSetting removed_settings[] = {
    {"adaptive_cap", "true", "false"}, {"adaptive_cap_percentile", "99", "75"}, {"adaptive_cap_hold_room", "false", "true"},
    {"osd_show_adaptive_smoothing", "true", "false"}, {"adaptive_smoothing", "true", "false"}, {"smoothing", "off", "on"},
    {"output_rung", "gpu", "auto"}, {"source_placement", "driver", "pre_sleep"}, {"reflex_sleep_bypass", "false", "true"},
    {"bypass_low_latency_off", "true", "false"}, {"driver_low_latency_off", "true", "false"},
    {"driver_limiter_hold", "true", "false"}, {"pre_sleep_precise", "true", "false"}, {"gpu_paced_cadence", "true", "false"},
    {"lead_coverage", "95", "99"}, {"lead_spike_floor", "false", "true"}, {"fg_late_spacing", "false", "true"},
    {"gpu_queue_wait_render_queue", "remove", "keep"}, {"gpu_wait_interleave_s", "2.5", "0"},
    {"stall_inject_ms", "12.5", "0"}, {"stall_inject_period_s", "2", "5"}, {"marker_forward_order", "after_wait", "before_wait"},
    {"sleep_status_every_frame", "true", "false"}, {"gpu_start_probe", "true", "false"},
    {"gpu_priority_probe", "true", "false"}, {"fg_presenter_probe_us", "500", "0"}, {"unified_pacer", "false", "true"},
    {"unified_every_frame_service", "false", "true"}, {"unified_queue_target_us", "4000", "2000"},
    {"gpu_wait_headroom_us", "2000", "1000"}};
static_assert(sizeof(removed_settings) / sizeof(removed_settings[0]) == 30, "the contract lists 30 removed settings");

// No removed key is on the fake disk, in any section of the INI, and the INI is at version 7.
void CheckNoRemovedKeysOnDisk() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& entry : disk) {
            if (std::get<0>(entry.first) != local_path) continue;
            for (const RemovedSetting& setting : removed_settings) CHECK(std::get<2>(entry.first) != setting.key);
            CHECK(std::get<2>(entry.first) != "show_adaptive_smoothing"); // the preset key of the removed OSD element
        }
    }
    for (const RemovedSetting& setting : removed_settings) CHECK(Value(local_path, "FrameLimiter", setting.key).empty());
    CHECK(Value(local_path, "FrameLimiter", "hook_installer_thread").empty());
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "7");
}
// A version-5 INI as an earlier build wrote it: the kept settings the user set, every removed key away from its default.
std::vector<IniEntry> OldIniWithRemovedKeys(const char* developer_settings, const char* test_mode) {
    std::vector<IniEntry> ini{{"config_version", "5"}, {"target_fps", "75"}, {"background_fps", "40"},
        {"fg_off_fps", "48"}, {"pacing_preference", "smooth"}, {"developer_settings", developer_settings}};
    for (const RemovedSetting& setting : removed_settings) ini.push_back({setting.key, setting.value});
    if (test_mode) ini.push_back({"pacing_test_mode", test_mode});
    return ini;
}
// What the loaded old INI must have become: the kept caps and the preference preserved, the pipeline the production
// one for the effective test mode (the unified owner on 64-bit only while no test mode is on).
void CheckLoadedOldIni(PacingTestMode expected_mode, int expected_overrides) {
    CHECK(g_config.target_fps == 75 && g_user_target_fps.load() == 75);
    CHECK(g_config.background_fps == 40 && g_background_fps.load() == 40);
    CHECK(g_config.fg_off_fps == 48 && g_fg_off_fps.load() == 48);
    CHECK(g_config.pacing_preference == PacingPreference::Smooth);
    CHECK(Config_DeveloperOverrides() == expected_overrides);
    CHECK(PacingPipeline_TestMode() == expected_mode);
    CHECK(PacingPipeline_Unified() == (sizeof(void*) == 8 && expected_mode == PacingTestMode::Off));
    CHECK(PacingPipeline_GetConfig().unified == PacingPipeline_Unified());
    CHECK(PacingPipeline_SleepBypass() == (expected_mode == PacingTestMode::Off));
    CheckPipelineMatchesConfig();
}
void RunMigrationMatrix(const char* developer_settings, const char* test_mode, PacingTestMode expected_mode) {
    const int overrides = test_mode ? 1 : 0; // the raw saved mode counts, effective or not
    Reset(false, false, OldIniWithRemovedKeys(developer_settings, test_mode));
    CHECK(!Config_IsFirstLaunch());
    // One info line names every removed key that held something else, each with its value.
    const std::string line = RemovedInfoLine();
    CHECK(!line.empty());
    for (const RemovedSetting& setting : removed_settings) CHECK(ListedInLine(line, setting.key, setting.value));
    CHECK(static_cast<size_t>(std::count(line.begin(), line.end(), '=')) ==
          sizeof(removed_settings) / sizeof(removed_settings[0]));
    CheckLoadedOldIni(expected_mode, overrides);
    const Loaded first = Capture();
    ReleaseStart();CHECK(OwnerShutdown());
    // The rewritten file carries none of the removed keys and every kept one as loaded.
    CheckNoRemovedKeysOnDisk();
    CHECK(Value(local_path, "FrameLimiter", "target_fps") == "75");
    CHECK(Value(local_path, "FrameLimiter", "background_fps") == "40");
    CHECK(Value(local_path, "FrameLimiter", "fg_off_fps") == "48");
    CHECK(Value(local_path, "FrameLimiter", "pacing_preference") == "smooth");
    CHECK(Value(local_path, "FrameLimiter", "developer_settings") == developer_settings);
    CHECK(Value(local_path, "FrameLimiter", "pacing_test_mode") == (test_mode ? test_mode : "off"));
    // A second launch of the rewritten file loads the identical state and has nothing to report.
    Relaunch();
    CHECK(!Config_IsFirstLaunch());
    CHECK(RemovedInfoLine().empty());
    CheckLoadedOldIni(expected_mode, overrides);
    CHECK(SameLoaded(first, Capture()));
    ReleaseStart();CHECK(OwnerShutdown());
    CheckNoRemovedKeysOnDisk();
}

// A version-5 INI that sets every removed key to a non-default value loads to the production behavior: no developer
// override, the unified owner (64-bit), no test mode, the Reflex Sleep bypass in effect, the kept caps and preference
// preserved; the INI is rewritten at version 7 without any removed key and loads again to the same state. With
// developer_settings on and every old arm key set (unified_pacer=false, source_placement=driver,
// reflex_sleep_bypass=false, adaptive_cap=true, ...) it is still no test mode; only an explicit pacing_test_mode
// enters one, and only with developer_settings.
void TestRemovedKeysMigrationMatrix() {
    RunMigrationMatrix("false", nullptr, PacingTestMode::Off);
    RunMigrationMatrix("true", nullptr, PacingTestMode::Off);
    RunMigrationMatrix("true", "reflex_only", PacingTestMode::ReflexOnly);
    RunMigrationMatrix("false", "reflex_only", PacingTestMode::Off);
    RunMigrationMatrix("true", "driver_limiter", PacingTestMode::DriverLimiter);
    RunMigrationMatrix("false", "driver_limiter", PacingTestMode::Off);
}

// A save after the load never resurrects a removed key: the synchronous save and the writer thread's save both write
// the kept settings only, whichever kept key changed (a live key or a field).
void TestSaveNeverResurrectsRemovedKeys() {
    Reset(false, false, OldIniWithRemovedKeys("true", nullptr));
    CheckNoRemovedKeysOnDisk();
    char error[128] = {};
    CHECK(Config_SetLiveKey("unified_queue_rank", "3", error, sizeof(error)));
    g_config.target_fps = 90;
    SaveConfig();
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_rank") == "3");
    CHECK(Value(local_path, "FrameLimiter", "target_fps") == "90");
    CheckNoRemovedKeysOnDisk();
    {
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(!writes.empty());
        for (const auto& write : writes)
            for (const RemovedSetting& setting : removed_settings) CHECK(std::get<2>(write.key) != setting.key);
        writes.clear();
    }
    // The writer thread.
    CHECK(Config_SetLiveKey("pacing_preference", "latency", error, sizeof(error)));
    g_config.target_fps = 120;
    RequestSaveConfig();
    ReleaseStart();CHECK(OwnerShutdown());
    CheckWorkerWrites();
    CHECK(Value(local_path, "FrameLimiter", "pacing_preference") == "latency");
    CHECK(Value(local_path, "FrameLimiter", "target_fps") == "120");
    CheckNoRemovedKeysOnDisk();
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& write : writes)
        for (const RemovedSetting& setting : removed_settings) CHECK(std::get<2>(write.key) != setting.key);
}

// Every removed key is refused by Config_SetLiveKey with the error text of an unknown key and changes nothing; every
// kept pacing live key still works; pacing_test_mode accepts off, reflex_only and driver_limiter only.
void TestRemovedKeysAreNotLiveKeys() {
    const bool wide = sizeof(void*) == 8;
    Reset(false, false, {{"config_version", "6"}, {"developer_settings", "true"}});
    char unknown[128] = {};
    CHECK(!Config_SetLiveKey("no_such_key", "1", unknown, sizeof(unknown)));
    CHECK(std::strstr(unknown, "not a live key") != nullptr);
    const PacingPipelineConfig pipeline = PacingPipeline_GetConfig();
    const Observed observed = Observe();
    const int overrides = Config_DeveloperOverrides();
    const auto refused = [&](const char* key, const char* value) {
        char error[128] = {};
        CHECK(!Config_SetLiveKey(key, value, error, sizeof(error)));
        CHECK(std::strcmp(error, unknown) == 0);
        CHECK(SamePipeline(pipeline, PacingPipeline_GetConfig()) && SameObserved(observed, Observe()));
        CHECK(Config_DeveloperOverrides() == overrides);
        CHECK(g_config.target_fps == 0 && g_config.pacing_test_mode == PacingTestMode::Off);
        CHECK(g_config.pacing_preference == PacingPreference::Balanced);
        CHECK(PacingPipeline_Unified() == wide && PacingPipeline_SleepBypass());
    };
    for (const RemovedSetting& setting : removed_settings)
        for (const char* value : {setting.value, "on", "0"}) refused(setting.key, value);
    for (const char* value : {"on", "true", "0"}) refused("show_adaptive_smoothing", value);
    CheckPipelineMatchesConfig();

    // The kept live keys still work, each published where it was.
    struct LiveCase { const char* key; const char* value; bool (*applied)(); };
    const LiveCase live_cases[] = {
        {"target_fps", "60", [] { return g_config.target_fps == 60 && g_user_target_fps.load() == 60; }},
        {"background_fps", "45", [] { return g_config.background_fps == 45 && g_background_fps.load() == 45; }},
        {"fg_off_fps", "48", [] { return g_config.fg_off_fps == 48 && g_fg_off_fps.load() == 48; }},
        {"pacing_preference", "latency", [] {
            return g_config.pacing_preference == PacingPreference::Latency &&
                PacingPipeline_GetConfig().unified_queue_rank == kPacingPreferenceLatencyRank; }},
        {"gpu_present_gate_probes", "off", [] {
            return !g_config.gpu_present_gate_probes && !PacingPipeline_GetConfig().gpu_probes; }},
        {"gpu_queue_probe", "off", [] { return !g_config.gpu_queue_probe; }},
        {"gpu_queue_probe_source", "0", [] {
            return g_config.gpu_queue_probe_source == 0 && g_gpu_queue_probe_source.load() == 0; }},
        {"unified_queue_margin_end", "0", [] {
            return g_config.unified_queue_margin_end == 0 && g_unified_queue_margin_end.load() == 0; }},
        {"unified_hold_share", "0.1", [] { return g_config.unified_hold_share == 0.1; }},
        {"unified_queue_law", "off", [] { return !g_config.unified_queue_law && !g_gpu_queue_law_feed.load(); }},
        {"unified_queue_rank", "3", [] {
            return g_config.unified_queue_rank == 3 && PacingPipeline_GetConfig().unified_queue_rank == 3; }},
        {"unified_queue_need_frames", "6", [] { return PacingPipeline_GetConfig().unified_queue_need_frames == 6; }},
        {"unified_queue_upstream", "1", [] { return g_config.unified_queue_upstream == 1; }},
        {"unified_queue_allow_us", "2500", [] { return PacingPipeline_GetConfig().unified_queue_allow_us == 2500; }},
    };
    for (const LiveCase& test : live_cases) {
        char error[128] = {};
        CHECK(Config_SetLiveKey(test.key, test.value, error, sizeof(error)) && error[0] == '\0');
        CHECK(test.applied());
        CheckPipelineMatchesConfig();
    }

    // pacing_test_mode: off, reflex_only and driver_limiter (any case) are accepted and published; anything else is refused.
    struct ModeCase { const char* text; PacingTestMode expected; };
    for (const ModeCase& test : {ModeCase{"reflex_only", PacingTestMode::ReflexOnly},
            ModeCase{"driver_limiter", PacingTestMode::DriverLimiter}, ModeCase{"off", PacingTestMode::Off},
            ModeCase{"Reflex_Only", PacingTestMode::ReflexOnly}, ModeCase{"OFF", PacingTestMode::Off}}) {
        char error[128] = {};
        CHECK(Config_SetLiveKey("pacing_test_mode", test.text, error, sizeof(error)) && error[0] == '\0');
        CHECK(g_config.pacing_test_mode == test.expected && PacingPipeline_TestMode() == test.expected);
        CHECK(PacingPipeline_Unified() == (wide && test.expected == PacingTestMode::Off));
        CHECK(PacingPipeline_SleepBypass() == (test.expected == PacingTestMode::Off));
        CheckPipelineMatchesConfig();
    }
    char error[128] = {};
    CHECK(Config_SetLiveKey("pacing_test_mode", "driver_limiter", error, sizeof(error)));
    for (const char* bad : {"auto", "turbo", "reflex", "driver", "limiter", "on", "1", ""}) {
        CHECK(!Config_SetLiveKey("pacing_test_mode", bad, error, sizeof(error)) && error[0] != '\0');
        CHECK(g_config.pacing_test_mode == PacingTestMode::DriverLimiter);
        CHECK(PacingPipeline_TestMode() == PacingTestMode::DriverLimiter && !PacingPipeline_Unified());
    }
    ReleaseStart();CHECK(OwnerShutdown());
}

// A first launch (no INI) writes version 7 and no removed key and logs nothing about removed settings. A version-5 INI
// that carries only default-valued removed keys loads to the same defaults and logs nothing either; one key away from
// its default is the only one the info line names (a default spelled differently is still a default); a version-6 INI
// that still carries removed keys (edited by hand) drops them silently.
void TestFirstLaunchAndDefaultValuedRemovedKeys() {
    Reset();
    CHECK(Config_IsFirstLaunch());
    CHECK(RemovedInfoLine().empty());
    CheckNoRemovedKeysOnDisk();
    CheckProductionDefaults();
    const Loaded fresh = Capture();
    ReleaseStart();CHECK(OwnerShutdown());

    std::vector<IniEntry> old_defaults{{"config_version", "5"}};
    for (const RemovedSetting& setting : removed_settings) old_defaults.push_back({setting.key, setting.old_default});
    Reset(false, false, old_defaults);
    CHECK(!Config_IsFirstLaunch());
    CHECK(RemovedInfoLine().empty()); // nothing is logged for defaults
    CheckNoRemovedKeysOnDisk();
    CheckProductionDefaults();
    CHECK(SameLoaded(fresh, Capture()));
    ReleaseStart();CHECK(OwnerShutdown());

    // One key away from its default is the only one named; other spellings of a default do not count.
    std::vector<IniEntry> one_away = old_defaults;
    for (IniEntry& entry : one_away) {
        if (std::strcmp(entry.key, "adaptive_cap") == 0) entry.value = "true";
        if (std::strcmp(entry.key, "adaptive_cap_percentile") == 0) entry.value = "75.0";
        if (std::strcmp(entry.key, "unified_pacer") == 0) entry.value = "yes";
        if (std::strcmp(entry.key, "smoothing") == 0) entry.value = "ON";
    }
    Reset(false, false, one_away);
    CHECK(RemovedInfoLine() == std::string(removed_info_prefix) + "adaptive_cap=true");
    CheckNoRemovedKeysOnDisk();
    CheckProductionDefaults();
    CHECK(SameLoaded(fresh, Capture()));
    ReleaseStart();CHECK(OwnerShutdown());

    // A version-6 INI is past the removal: nothing is reported, whatever it carries, and the keys are dropped.
    Reset(false, false, {{"config_version", "6"}, {"adaptive_cap", "true"}, {"smoothing", "off"}, {"unified_pacer", "false"}});
    CHECK(RemovedInfoLine().empty());
    CheckNoRemovedKeysOnDisk();
    CheckProductionDefaults();
    CHECK(SameLoaded(fresh, Capture()));
    ReleaseStart();CHECK(OwnerShutdown());

    // A version-5 INI without any removed key reports nothing.
    Reset(false, false, {{"config_version", "5"}, {"target_fps", "60"}});
    CHECK(RemovedInfoLine().empty() && g_config.target_fps == 60);
    CheckNoRemovedKeysOnDisk();
    ReleaseStart();CHECK(OwnerShutdown());
}

// config_version 7 removed hook_installer_thread (the installer thread is off in the build, hooks.h). Every earlier
// build wrote the key: true, its default then, is dropped without a word; false is named once; neither comes back.
void TestHookInstallerThreadKeyIsDropped() {
    Reset(false, false, {{"config_version", "6"}, {"target_fps", "75"}, {"hook_installer_thread", "true"}});
    CHECK(!Config_IsFirstLaunch() && RemovedInfoLine().empty() && g_config.target_fps == 75);
    ReleaseStart();CHECK(OwnerShutdown());
    CheckNoRemovedKeysOnDisk();
    CHECK(Value(local_path, "FrameLimiter", "target_fps") == "75");

    // A version-6 INI: the keys version 6 removed are past their report, this one is not.
    Reset(false, false, {{"config_version", "6"}, {"adaptive_cap", "true"}, {"hook_installer_thread", "false"}});
    CHECK(RemovedInfoLine() == std::string(removed_info_prefix) + "hook_installer_thread=false");
    ReleaseStart();CHECK(OwnerShutdown());
    CheckNoRemovedKeysOnDisk();
    Relaunch(); // the rewritten file, at version 7
    CHECK(RemovedInfoLine().empty());
    ReleaseStart();CHECK(OwnerShutdown());
    CheckNoRemovedKeysOnDisk();

    // An older INI names it beside the keys version 6 removed, in the one line.
    Reset(false, false, {{"config_version", "5"}, {"adaptive_cap", "true"}, {"hook_installer_thread", "false"}});
    const std::string line = RemovedInfoLine();
    CHECK(ListedInLine(line, "adaptive_cap", "true") && ListedInLine(line, "hook_installer_thread", "false") &&
          std::count(line.begin(), line.end(), '=') == 2);
    ReleaseStart();CHECK(OwnerShutdown());
    CheckNoRemovedKeysOnDisk();

    // A version-7 INI that carries the key (edited by hand): not read, not reported, dropped.
    Reset(false, false, {{"config_version", "7"}, {"hook_installer_thread", "true"}});
    CHECK(RemovedInfoLine().empty());
    ReleaseStart();CHECK(OwnerShutdown());
    CheckNoRemovedKeysOnDisk();
}

// ── The preset key of the removed OSD element ──
// The OSD_Preset_<n> sections live outside [FrameLimiter], so the start-up wipe never reached them: the version step
// deletes show_adaptive_smoothing there (local INI and shared presets file) at every start, and writes nothing else.
// Two preset slots with the fields the loader reads; the removed key is seeded (true in slot 1, false in slot 2) or not.
std::vector<DiskEntry> PresetSections(const char* path, bool with_removed_key) {
    struct Slot {
        const char* section; const char* name; const char* x; const char* y; const char* scale; const char* opacity;
        const char* fps; const char* frametime; const char* removed;
    };
    const Slot slots[] = {
        {"OSD_Preset_1", "Alpha", "0.25", "0.5", "1.5", "0.75", "true", "false", "true"},
        {"OSD_Preset_2", "Beta", "0.125", "0.0625", "0.5", "0.25", "false", "true", "false"}};
    std::vector<DiskEntry> entries;
    for (const Slot& slot : slots) {
        entries.push_back({path, slot.section, "name", slot.name});
        entries.push_back({path, slot.section, "osd_x", slot.x});
        entries.push_back({path, slot.section, "osd_y", slot.y});
        entries.push_back({path, slot.section, "osd_scale", slot.scale});
        entries.push_back({path, slot.section, "osd_opacity", slot.opacity});
        entries.push_back({path, slot.section, "show_fps", slot.fps});
        entries.push_back({path, slot.section, "show_frametime", slot.frametime});
        if (with_removed_key) entries.push_back({path, slot.section, "show_adaptive_smoothing", slot.removed});
    }
    return entries;
}
std::vector<DiskEntry> operator+(std::vector<DiskEntry> a, const std::vector<DiskEntry>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}
std::map<Key, std::string> AsFields(const std::vector<DiskEntry>& entries) {
    std::map<Key, std::string> fields;
    for (const DiskEntry& entry : entries) fields[Key{ entry.path, entry.section, entry.key }] = entry.value;
    return fields;
}
// Every OSD_Preset_<n> key of one file on the fake disk.
std::map<Key, std::string> PresetFields(const char* path) {
    std::lock_guard<std::mutex> lock(mutex);
    std::map<Key, std::string> fields;
    for (const auto& entry : disk)
        if (std::get<0>(entry.first) == path && std::get<1>(entry.first).rfind("OSD_Preset_", 0) == 0)
            fields.insert(entry);
    return fields;
}
size_t EntriesOf(const char* path) {
    std::lock_guard<std::mutex> lock(mutex);
    size_t count = 0;
    for (const auto& entry : disk)
        if (std::get<0>(entry.first) == path) ++count;
    return count;
}
// The loaded presets are the two seeded slots, intact.
void CheckPresetsIntact() {
    const OSDPreset& alpha = OSDPreset_GetSlot(0);
    CHECK(alpha.occupied && std::strcmp(alpha.name, "Alpha") == 0);
    CHECK(alpha.osd_x == 0.25f && alpha.osd_y == 0.5f && alpha.osd_scale == 1.5f && alpha.osd_opacity == 0.75f);
    CHECK(alpha.show_fps && !alpha.show_frametime);
    const OSDPreset& beta = OSDPreset_GetSlot(1);
    CHECK(beta.occupied && std::strcmp(beta.name, "Beta") == 0);
    CHECK(beta.osd_x == 0.125f && beta.osd_y == 0.0625f && beta.osd_scale == 0.5f && beta.osd_opacity == 0.25f);
    CHECK(!beta.show_fps && beta.show_frametime);
}
void CheckPresetKeyGone(const char* path) {
    CHECK(Value(path, "OSD_Preset_1", "show_adaptive_smoothing").empty());
    CHECK(Value(path, "OSD_Preset_2", "show_adaptive_smoothing").empty());
}

void TestRemovedPresetKeyIsDropped() {
    // (1) Version-5 local INI: the key is gone from both sections, every other preset field is as it was, the
    // presets load intact; the shared presets file was never seeded and is not created.
    extra_seed = PresetSections(local_path, true);
    Reset(false, false, {{"config_version", "5"}, {"target_fps", "60"}});
    CHECK(!Config_IsFirstLaunch());
    CheckPresetKeyGone(local_path);
    CHECK(PresetFields(local_path) == AsFields(PresetSections(local_path, false)));
    CheckPresetsIntact();
    CHECK(EntriesOf(shared_path) == 0);
    ReleaseStart();CHECK(OwnerShutdown());
    CHECK(PresetFields(local_path) == AsFields(PresetSections(local_path, false)));
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "7");

    // (2) shared_presets on, the key in the SHARED presets file (and none in the local INI): gone there, the other
    // fields unchanged, the shared presets loaded intact.
    extra_seed = PresetSections(shared_path, true);
    Reset(false, false, {{"config_version", "5"}, {"shared_presets", "true"}});
    CheckPresetKeyGone(shared_path);
    CHECK(PresetFields(shared_path) == AsFields(PresetSections(shared_path, false)));
    CHECK(PresetFields(local_path).empty());
    CheckPresetsIntact();
    ReleaseStart();CHECK(OwnerShutdown());
    CHECK(PresetFields(shared_path) == AsFields(PresetSections(shared_path, false)));

    // (2b) The key in both files, shared_presets off: both are cleaned, nothing else changes.
    extra_seed = PresetSections(local_path, true) + PresetSections(shared_path, true);
    Reset(false, false, {{"config_version", "5"}});
    CheckPresetKeyGone(local_path);
    CheckPresetKeyGone(shared_path);
    CHECK(PresetFields(local_path) == AsFields(PresetSections(local_path, false)));
    CHECK(PresetFields(shared_path) == AsFields(PresetSections(shared_path, false)));
    ReleaseStart();CHECK(OwnerShutdown());

    // (3) A shared presets file that does not exist is not created by the migration; with shared_presets on the
    // start-up save writes only its [Presets] section there, never a preset section.
    extra_seed = PresetSections(local_path, true);
    Reset(false, false, {{"config_version", "5"}});
    CheckPresetKeyGone(local_path);
    CHECK(EntriesOf(shared_path) == 0);
    ReleaseStart();CHECK(OwnerShutdown());
    extra_seed = PresetSections(local_path, true);
    Reset(false, false, {{"config_version", "5"}, {"shared_presets", "true"}});
    CheckPresetKeyGone(local_path);
    CHECK(PresetFields(shared_path).empty());
    ReleaseStart();CHECK(OwnerShutdown());

    // (4) The clean-up runs at every start, whatever the INI's version: a version-6 INI loses the key too, the other
    // fields stay.
    extra_seed = PresetSections(local_path, true) + PresetSections(shared_path, true);
    Reset(false, false, {{"config_version", "6"}});
    CheckPresetKeyGone(local_path);
    CheckPresetKeyGone(shared_path);
    CHECK(PresetFields(local_path) == AsFields(PresetSections(local_path, false)));
    CHECK(PresetFields(shared_path) == AsFields(PresetSections(shared_path, false)));
    CheckPresetsIntact();
    ReleaseStart();CHECK(OwnerShutdown());

    // (5) A first launch writes no preset section anywhere.
    Reset();
    CHECK(Config_IsFirstLaunch());
    CHECK(PresetFields(local_path).empty() && PresetFields(shared_path).empty());
    CHECK(EntriesOf(shared_path) == 0);
    ReleaseStart();CHECK(OwnerShutdown());
    CHECK(PresetFields(local_path).empty() && PresetFields(shared_path).empty());
}

// A deletion that fails is not abandoned: the key stays on disk, one warning names the file, the rest of the start
// goes on (the INI is rewritten at version 7), and the next start removes the key once the file is writable again.
void TestFailedPresetKeyDeletionIsRetriedAtTheNextStart() {
    const char* const warning = "obsolete preset key";
    // (a) The key in the shared presets file, which cannot be written at the first launch.
    extra_seed = PresetSections(shared_path, true);
    fail_path = shared_path;
    Reset(false, false, {{"config_version", "5"}});
    CHECK(WarnCount(warning) == 1 && WarnCount(shared_path) == 1);
    CHECK(Value(shared_path, "OSD_Preset_1", "show_adaptive_smoothing") == "true");
    CHECK(Value(shared_path, "OSD_Preset_2", "show_adaptive_smoothing") == "false");
    CHECK(PresetFields(shared_path) == AsFields(PresetSections(shared_path, true)));
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "7");
    CHECK(PresetFields(local_path).empty());
    ReleaseStart();CHECK(OwnerShutdown());
    fail_path.clear();
    Relaunch(); // the same fake disk, the file writable now, the INI already at version 7
    CHECK(WarnCount(warning) == 0);
    CheckPresetKeyGone(shared_path);
    CHECK(PresetFields(shared_path) == AsFields(PresetSections(shared_path, false)));
    ReleaseStart();CHECK(OwnerShutdown());

    // (c) The same for the local INI: its key deletions fail (its other writes succeed), the presets still load,
    // the next start removes the key.
    extra_seed = PresetSections(local_path, true);
    fail_delete_path = local_path;
    Reset(false, false, {{"config_version", "5"}});
    CHECK(WarnCount(warning) == 1 && WarnCount(local_path) == 1);
    CHECK(PresetFields(local_path) == AsFields(PresetSections(local_path, true)));
    CheckPresetsIntact();
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "7");
    ReleaseStart();CHECK(OwnerShutdown());
    fail_delete_path.clear();
    Relaunch();
    CHECK(WarnCount(warning) == 0);
    CheckPresetKeyGone(local_path);
    CHECK(PresetFields(local_path) == AsFields(PresetSections(local_path, false)));
    CheckPresetsIntact();
    ReleaseStart();CHECK(OwnerShutdown());
}

// Keys retired by the earlier reworks. An existing INI may still carry them: loading ignores them and
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
    "smoothing_bias_us", "smoothing_percentile", "gpu_readiness_observer",
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
        {"gpu_readiness_observer", "true"},
        {"dmfg_output_cap", "144"}, {"gpu_paced_hold_margin", "75"},
        {"gpu_paced_release_smoothing", "true"}, {"zz_never_a_key", "1"}});
    // None of them takes effect; the pipeline matches a fresh INI.
    CHECK(g_config.gpu_present_gate_probes);
    CheckProductionDefaults();
    for (const char* key : retired_keys) CHECK(Value(local_path, "FrameLimiter", key).empty());
    CHECK(Value(local_path, "FrameLimiter", "zz_never_a_key").empty()); // the clean rewrite drops any key it does not write
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

// ── Save policy: the INI is rewritten clean, key by key ──
using Fields = std::map<std::string, std::string>;
// Every key and value of one section of one file on the fake disk.
Fields SectionFields(const char* path, const char* section) {
    std::lock_guard<std::mutex> lock(mutex);
    Fields fields;
    for (const auto& entry : disk)
        if (std::get<0>(entry.first) == path && std::get<1>(entry.first) == section)
            fields[std::get<2>(entry.first)] = entry.second;
    return fields;
}
// True when the section holds exactly these keys, no more and no fewer (the differences are named on stderr).
bool HasExactlyKeys(const Fields& fields, const std::vector<std::string>& keys) {
    Fields expected;
    for (const std::string& key : keys) expected[key] = "";
    bool same = expected.size() == keys.size(); // the literal list has no duplicate
    for (const auto& entry : fields)
        if (!expected.count(entry.first)) {
            std::fprintf(stderr, "unexpected key: %s\n", entry.first.c_str());
            same = false;
        }
    for (const auto& entry : expected)
        if (!fields.count(entry.first)) {
            std::fprintf(stderr, "missing key: %s\n", entry.first.c_str());
            same = false;
        }
    return same;
}
bool FieldIs(const Fields& fields, const std::string& key, const std::string& value) {
    const auto found = fields.find(key);
    if (found != fields.end() && found->second == value) return true;
    std::fprintf(stderr, "key %s: expected '%s', found '%s'\n", key.c_str(), value.c_str(),
                 found == fields.end() ? "<absent>" : found->second.c_str());
    return false;
}
size_t WriteCount() {
    std::lock_guard<std::mutex> lock(mutex);
    return writes.size();
}
unsigned ReadCount() {
    std::lock_guard<std::mutex> lock(mutex);
    return reads;
}
// The values written to one key of any file, in order.
std::vector<std::string> WrittenValues(const char* key) {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::string> values;
    for (const auto& write : writes)
        if (std::get<2>(write.key) == key) values.push_back(write.value);
    return values;
}

// The 27 OSD element toggles of the live config and of a preset, written once here as the fixture of the contract:
// the INI key of the config flag, the key of the preset section, and the two fields.
struct OsdToggle {
    const char* config_key;
    const char* preset_key;
    bool Config::* config_field;
    bool OSDPreset::* preset_field;
};
#define OSD_TOGGLE(name) { "osd_show_" #name, "show_" #name, &Config::osd_show_##name, &OSDPreset::show_##name }
const OsdToggle osd_toggles[] = {
    OSD_TOGGLE(clock),
    OSD_TOGGLE(fps), OSD_TOGGLE(frametime), OSD_TOGGLE(frametime_graph), OSD_TOGGLE(fg), OSD_TOGGLE(limiter),
    OSD_TOGGLE(jitter), OSD_TOGGLE(cpu_latency), OSD_TOGGLE(native_pcl), OSD_TOGGLE(native_pcl_breakdown),
    OSD_TOGGLE(1pct_low), OSD_TOGGLE(pacing_quality),
    OSD_TOGGLE(0_1pct_low), OSD_TOGGLE(gpu_render_time), OSD_TOGGLE(total_frame_cost), OSD_TOGGLE(fg_time),
    OSD_TOGGLE(gpu_temp), OSD_TOGGLE(gpu_clock), OSD_TOGGLE(gpu_usage), OSD_TOGGLE(vram), OSD_TOGGLE(cpu_usage),
    OSD_TOGGLE(ram), OSD_TOGGLE(dlss_quality), OSD_TOGGLE(dlss_features), OSD_TOGGLE(dlss_resolution),
    OSD_TOGGLE(dlss_presets), OSD_TOGGLE(dlss_versions)};
#undef OSD_TOGGLE
constexpr size_t osd_toggle_count = sizeof(osd_toggles) / sizeof(osd_toggles[0]);
static_assert(osd_toggle_count == 27, "the contract lists 27 OSD element toggles");

// The 63 keys of the [FrameLimiter] section that are not OSD element toggles (90 with the 27 toggles), each with a value
// away from its default as an earlier launch left it, and the value the clean rewrite saves back. The keybind keys are
// written to the local INI only while the presets are not shared. Derived by running the writer, then pinned. Which keys
// exist is not decided here: ExpectedIniKeys reads the config_key lines of the shared fixture.
struct IniKey {
    const char* key;
    const char* seeded;
    const char* written;
};
const IniKey non_default_ini[] = {
    {"target_fps", "90", "90"}, {"background_fps", "45", "45"}, {"fg_off_fps", "60", "60"},
    {"vsync_mode", "on", "on"}, {"enforcement_marker", "Present", "Present"},
    {"initial_wake_guard_us", "1200", "1200.000000"}, {"window_mode", "borderless", "borderless"},
    {"fake_fullscreen", "true", "true"}, {"selected_monitor", "2", "2"}, {"focus_lock", "true", "true"},
    {"oled_care_key", "F11", "F11"}, {"oled_care_all_monitors", "false", "false"}, {"oled_care_idle_minutes", "5", "5"},
    {"osd_enabled", "true", "true"}, {"osd_x", "0.25", "0.250000"}, {"osd_y", "0.75", "0.750000"},
    {"osd_opacity", "0.5", "0.500000"}, {"osd_scale", "1.5", "1.500000"}, {"osd_drop_shadow", "false", "false"},
    {"osd_text_brightness", "0.5", "0.500000"}, {"osd_toggle_key", "F9", "F9"}, {"osd_position_cycle_key", "F10", "F10"},
    {"shared_presets", "false", "false"}, // true is the shared-file variant below
    {"osd_preset_prev_key", "F1", "F1"}, {"osd_preset_next_key", "F2", "F2"}, {"blackout_key", "F3", "F3"},
    {"pacing_preference", "smooth", "smooth"}, {"advanced_settings", "true", "true"},
    {"developer_settings", "true", "true"}, {"config_version", "7", "7"}, {"flip_metering_off", "true", "true"},
    {"vtable_copy_at_present", "false", "false"},
    {"gpu_wait_late_guard", "true", "true"}, {"gpu_wait_present_first", "false", "false"},
    {"gpu_wait_start_limit", "false", "false"},
    {"lead_queue_correction", "false", "false"}, {"fg_flip_gate_cover", "80", "80"},
    {"fg_flip_gate_entry", "1", "1"}, {"fg_flip_gate_fit", "90", "90"},
    {"gpu_wait_ab_interleave_s", "30", "30"},
    {"gpu_queue_probe", "false", "false"}, {"gpu_queue_probe_source", "0", "0"},
    {"pacing_test_mode", "driver_limiter", "driver_limiter"}, {"unified_hold_share", "0.125", "0.125000"},
    {"unified_queue_law", "false", "false"}, {"unified_queue_rank", "3", "3"}, {"unified_queue_need_frames", "6", "6"},
    {"unified_queue_upstream", "1", "1"}, {"unified_queue_margin_end", "0", "0"}, {"unified_queue_allow_us", "2500", "2500"},
    {"control_file", "true", "true"}, {"dynamic_mfg_passthrough", "true", "true"}, {"reflex_inject", "true", "true"},
    {"flip_model_override", "true", "true"}, {"log_level", "debug", "debug"},
    {"csv_enabled", "true", "false"}, // telemetry needs an explicit activation each launch
    {"native_pcl_enabled", "true", "true"}, {"gpu_present_gate_probes", "false", "false"},
    {"dlss_info_hooks", "false", "false"},
    {"hang_report", "true", "true"}, {"hang_report_dump", "false", "false"}, {"hang_report_test_after_s", "30", "30"},
    {"streamline_compat", "force_false", "force_false"}};
constexpr size_t non_default_ini_count = sizeof(non_default_ini) / sizeof(non_default_ini[0]);
static_assert(non_default_ini_count == 63, "the contract lists 63 further keys");
bool IsKeybindKey(const std::string& key) {
    return key == "osd_preset_prev_key" || key == "osd_preset_next_key" || key == "blackout_key" ||
           key == "osd_position_cycle_key" || key == "oled_care_key" || key == "oled_care_all_monitors" ||
           key == "oled_care_idle_minutes";
}
// The keys of the [FrameLimiter] section the build reads and writes: the config_key lines of
// tests/data/pacing_schema.txt (next to this file), which tests/test_pacing_schedule.py compares with the settings the
// capture kit may write. A first launch writes exactly these; a key added to or removed from the build fails this test
// until the fixture follows.
std::vector<std::string> FixtureIniKeys() {
    std::string path = __FILE__;
    path.erase(path.find_last_of("/\\") + 1);
    std::ifstream file(path + "data/pacing_schema.txt");
    CHECK(file.is_open());
    std::vector<std::string> keys;
    const std::string prefix = "config_key ";
    for (std::string line; std::getline(file, line);) {
        if (line.compare(0, prefix.size(), prefix) != 0) continue;
        std::string key = line.substr(prefix.size());
        while (!key.empty() && (key.back() == '\r' || key.back() == ' ')) key.pop_back();
        keys.push_back(key);
    }
    CHECK(!keys.empty());
    return keys;
}
// Every key the clean rewrite writes to the [FrameLimiter] section.
std::vector<std::string> ExpectedIniKeys(bool shared_presets) {
    std::vector<std::string> keys;
    for (const std::string& key : FixtureIniKeys())
        if (!(shared_presets && IsKeybindKey(key))) keys.push_back(key);
    return keys;
}

// A first launch writes every key at its default and nothing else; every key set away from its default loads, is
// written back as loaded (a key the writer omitted would be erased by the clean rewrite), and a second launch of
// the rewritten file changes nothing; an unknown key does not survive; the telemetry switch is never carried over.
void TestIniWritesExactlyTheKeysThatExist() {
    Reset();
    CHECK(Config_IsFirstLaunch());
    CHECK(HasExactlyKeys(SectionFields(local_path, "FrameLimiter"), ExpectedIniKeys(false)));
    ReleaseStart();CHECK(OwnerShutdown());
    CHECK(HasExactlyKeys(SectionFields(local_path, "FrameLimiter"), ExpectedIniKeys(false)));

    std::vector<IniEntry> seed;
    for (const IniKey& entry : non_default_ini) seed.push_back({entry.key, entry.seeded});
    for (const OsdToggle& toggle : osd_toggles)
        seed.push_back({toggle.config_key, std::strcmp(toggle.config_key, "osd_show_fps") == 0 ? "false" : "true"});
    seed.push_back({"zz_never_a_key", "1"});
    const auto check_written = [](const Fields& fields) {
        CHECK(HasExactlyKeys(fields, ExpectedIniKeys(false)));
        for (const IniKey& entry : non_default_ini) CHECK(FieldIs(fields, entry.key, entry.written));
        for (const OsdToggle& toggle : osd_toggles)
            CHECK(FieldIs(fields, toggle.config_key, std::strcmp(toggle.config_key, "osd_show_fps") == 0 ? "false" : "true"));
    };
    Reset(false, false, seed);
    CHECK(!Config_IsFirstLaunch());
    check_written(SectionFields(local_path, "FrameLimiter"));
    CHECK(Value(local_path, "FrameLimiter", "zz_never_a_key").empty());
    // What the loaded config holds, key by key.
    CHECK(g_config.target_fps == 90 && g_config.background_fps == 45 && g_config.fg_off_fps == 60);
    CHECK(g_config.vsync_mode == "on" && g_config.enforcement_marker == "Present" && g_config.window_mode == "borderless");
    CHECK(g_config.initial_wake_guard_us == 1200.0 && g_config.selected_monitor == 2 && g_config.oled_care_idle_minutes == 5);
    CHECK(g_config.fake_fullscreen && g_config.focus_lock && !g_config.oled_care_all_monitors && g_config.osd_enabled);
    CHECK(g_config.osd_x == 0.25f && g_config.osd_y == 0.75f && g_config.osd_opacity == 0.5f && g_config.osd_scale == 1.5f);
    CHECK(!g_config.osd_drop_shadow && g_config.osd_text_brightness == 0.5f);
    CHECK(g_config.osd_toggle_key == "F9" && g_config.osd_position_cycle_key == "F10" && g_config.oled_care_key == "F11");
    CHECK(g_config.osd_preset_prev_key == "F1" && g_config.osd_preset_next_key == "F2" && g_config.blackout_key == "F3");
    CHECK(!g_config.shared_presets && g_config.advanced_settings && g_config.developer_settings);
    CHECK(g_config.pacing_preference == PacingPreference::Smooth && g_config.pacing_test_mode == PacingTestMode::DriverLimiter);
    CHECK(g_config.flip_metering_off && g_config.control_file && !g_config.gpu_present_gate_probes);
    CHECK(g_config.gpu_wait_late_guard && !g_config.gpu_wait_present_first && g_config.gpu_wait_ab_interleave_s == 30);
    CHECK(!g_gpu_wait_present_first.load());
    CHECK(!g_config.lead_queue_correction && !g_lead_queue_correction.load());
    CHECK(!g_config.gpu_queue_probe && g_config.gpu_queue_probe_source == 0 && g_config.unified_queue_margin_end == 0);
    CHECK(g_config.unified_hold_share == 0.125 && !g_config.unified_queue_law && g_config.unified_queue_rank == 3);
    CHECK(g_config.unified_queue_need_frames == 6 && g_config.unified_queue_upstream == 1 && g_config.unified_queue_allow_us == 2500);
    CHECK(g_config.dynamic_mfg_passthrough && g_config.reflex_inject && g_config.flip_model_override);
    CHECK(g_config.log_level == "debug" && g_config.native_pcl_enabled && !g_config.csv_enabled);
    CHECK(!g_config.dlss_info_hooks && !g_config.streamline_compat);
    for (const OsdToggle& toggle : osd_toggles)
        CHECK(g_config.*(toggle.config_field) == (std::strcmp(toggle.config_key, "osd_show_fps") != 0));
#ifdef _WIN64
    g_fg_mode.store(0); // dynamic_mfg_passthrough took the mode over
#endif
    ReleaseStart();CHECK(OwnerShutdown());
    const Fields saved = SectionFields(local_path, "FrameLimiter");
    check_written(saved);
    // The rewritten file loads to the same file.
    Relaunch();
    CHECK(SectionFields(local_path, "FrameLimiter") == saved);
#ifdef _WIN64
    g_fg_mode.store(0);
#endif
    ReleaseStart();CHECK(OwnerShutdown());
    CHECK(SectionFields(local_path, "FrameLimiter") == saved);
}

// With shared presets the keybinds (preset cycling, Move OSD, blackout, OLED Care) and the OLED Care settings live in
// the shared file's [Presets] section: they are loaded from it over the local INI's copy, never written to the local
// INI, and changes go to the shared file; with the presets local the keys are in the local INI and the shared file is
// not touched.
void TestSharedPresetsKeepTheKeybindsInTheSharedFile() {
    const Fields seeded_shared{{"prev_key", "F1"}, {"next_key", "F2"}, {"position_cycle_key", "F4"},
        {"blackout_key", "F3"}, {"oled_care_key", "F6"}, {"oled_care_all_monitors", "false"},
        {"oled_care_idle_minutes", "7"}};
    std::vector<DiskEntry> shared_keys;
    for (const auto& field : seeded_shared) shared_keys.push_back({shared_path, "Presets", field.first.c_str(), field.second.c_str()});
    extra_seed = shared_keys;
    Reset(false, false, {{"config_version", "6"}, {"shared_presets", "true"}, {"osd_preset_prev_key", "stale"},
        {"osd_preset_next_key", "stale"}, {"blackout_key", "stale"}, {"osd_position_cycle_key", "stale"},
        {"oled_care_key", "stale"}, {"oled_care_all_monitors", "true"}, {"oled_care_idle_minutes", "30"}});
    CHECK(g_config.osd_preset_prev_key == "F1" && g_config.osd_preset_next_key == "F2" && g_config.blackout_key == "F3");
    CHECK(g_config.osd_position_cycle_key == "F4" && g_config.oled_care_key == "F6");
    CHECK(!g_config.oled_care_all_monitors && g_config.oled_care_idle_minutes == 7);
    CHECK(HasExactlyKeys(SectionFields(local_path, "FrameLimiter"), ExpectedIniKeys(true)));
    CHECK(FieldIs(SectionFields(local_path, "FrameLimiter"), "shared_presets", "true"));
    CHECK(SectionFields(shared_path, "Presets") == seeded_shared);
    g_config.osd_preset_prev_key = "F5";
    g_config.osd_position_cycle_key = "F8";
    g_config.oled_care_idle_minutes = 12;
    RequestSaveConfig();
    ReleaseStart();CHECK(OwnerShutdown());
    CheckWorkerWrites();
    CHECK(Value(shared_path, "Presets", "prev_key") == "F5");
    CHECK(Value(shared_path, "Presets", "next_key") == "F2" && Value(shared_path, "Presets", "blackout_key") == "F3");
    CHECK(Value(shared_path, "Presets", "position_cycle_key") == "F8");
    CHECK(Value(shared_path, "Presets", "oled_care_idle_minutes") == "12");
    CHECK(Value(shared_path, "Presets", "oled_care_all_monitors") == "false");
    CHECK(HasExactlyKeys(SectionFields(local_path, "FrameLimiter"), ExpectedIniKeys(true)));

    extra_seed = shared_keys;
    Reset(false, false, {{"config_version", "6"}, {"osd_preset_prev_key", "L1"}, {"osd_preset_next_key", "L2"},
        {"blackout_key", "L3"}, {"osd_position_cycle_key", "L4"}, {"oled_care_key", "L6"},
        {"oled_care_idle_minutes", "9"}});
    CHECK(g_config.osd_preset_prev_key == "L1" && g_config.osd_preset_next_key == "L2" && g_config.blackout_key == "L3");
    CHECK(g_config.osd_position_cycle_key == "L4" && g_config.oled_care_key == "L6");
    CHECK(g_config.oled_care_all_monitors && g_config.oled_care_idle_minutes == 9);
    CHECK(HasExactlyKeys(SectionFields(local_path, "FrameLimiter"), ExpectedIniKeys(false)));
    CHECK(Value(local_path, "FrameLimiter", "osd_preset_prev_key") == "L1");
    CHECK(Value(local_path, "FrameLimiter", "osd_preset_next_key") == "L2");
    CHECK(Value(local_path, "FrameLimiter", "blackout_key") == "L3");
    CHECK(Value(local_path, "FrameLimiter", "osd_position_cycle_key") == "L4");
    CHECK(Value(local_path, "FrameLimiter", "oled_care_key") == "L6");
    CHECK(Value(local_path, "FrameLimiter", "oled_care_idle_minutes") == "9");
    CHECK(SectionFields(shared_path, "Presets") == seeded_shared);
    ReleaseStart();CHECK(OwnerShutdown());
    CHECK(SectionFields(shared_path, "Presets") == seeded_shared);
}

// The default preset of the shared file sets the OSD layout at every launch with shared presets on, over the local
// INI's; Save and Clear write and remove its section in the shared file; with the presets local it is neither applied
// nor offered, and Clear leaves the other games' default alone.
void TestSharedDefaultPresetAppliesAtLaunch() {
    const std::vector<DiskEntry> seeded_default{{shared_path, "OSD_Default_Preset", "name", "Default"},
        {shared_path, "OSD_Default_Preset", "osd_x", "0.9"}, {shared_path, "OSD_Default_Preset", "osd_scale", "1.5"},
        {shared_path, "OSD_Default_Preset", "show_gpu_temp", "true"}};
    extra_seed = seeded_default;
    Reset(false, false, {{"config_version", "6"}, {"shared_presets", "true"}, {"osd_show_fps", "true"},
        {"osd_x", "0.1"}});
    CHECK(OSDPreset_HasDefault() && std::strcmp(OSDPreset_GetDefault().name, "Default") == 0);
    CHECK(!g_config.osd_show_fps && g_config.osd_show_gpu_temp);
    CHECK(g_config.osd_x == 0.9f && g_config.osd_scale == 1.5f);
    // The rewritten local INI carries the applied layout.
    CHECK(Value(local_path, "FrameLimiter", "osd_show_fps") == "false");

    g_config.osd_show_ram = true;
    OSDPreset update = OSDPreset_FromConfig();
    std::snprintf(update.name, sizeof(update.name), "%s", "Mine");
    OSDPreset_SaveDefault(update);
    ReleaseStart();CHECK(OwnerShutdown());
    CheckWorkerWrites();
    CHECK(Value(shared_path, "OSD_Default_Preset", "name") == "Mine");
    CHECK(Value(shared_path, "OSD_Default_Preset", "show_ram") == "true");
    CHECK(SectionFields(local_path, "OSD_Default_Preset").empty());

    extra_seed = seeded_default;
    Reset(false, false, {{"config_version", "6"}, {"shared_presets", "true"}});
    OSDPreset_ClearDefault();
    CHECK(!OSDPreset_HasDefault());
    ReleaseStart();CHECK(OwnerShutdown());
    CheckWorkerWrites();
    CHECK(SectionFields(shared_path, "OSD_Default_Preset").empty());

    extra_seed = seeded_default;
    Reset(false, false, {{"config_version", "6"}, {"osd_show_fps", "true"}, {"osd_x", "0.1"}});
    CHECK(!OSDPreset_HasDefault());
    CHECK(g_config.osd_show_fps && !g_config.osd_show_gpu_temp && g_config.osd_x == 0.1f);
    OSDPreset_SaveDefault(OSDPreset_FromConfig());
    OSDPreset_ClearDefault();
    CHECK(!OSDPreset_HasDefault());
    ReleaseStart();CHECK(OwnerShutdown());
    CHECK(Value(shared_path, "OSD_Default_Preset", "name") == "Default");
}

// The log level of the INI is kept at start and applied; the telemetry (CSV) switch is never carried over a start, but a
// change made while running is saved.
void TestStartupKeepsLogLevelAndDisablesCsv() {
    Reset(false, false, {{"config_version", "6"}, {"log_level", "debug"}, {"csv_enabled", "true"}});
    CHECK(g_config.log_level == "debug" && applied_log_level == static_cast<int>(LogLevel::Debug));
    CHECK(!g_config.csv_enabled);
    CHECK(Value(local_path, "FrameLimiter", "log_level") == "debug");
    CHECK(Value(local_path, "FrameLimiter", "csv_enabled") == "false");
    g_config.csv_enabled = true; // the OSD switch
    RequestSaveConfig();
    ReleaseStart();CHECK(OwnerShutdown());
    CHECK(Value(local_path, "FrameLimiter", "csv_enabled") == "true");
    Relaunch();
    CHECK(!g_config.csv_enabled && g_config.log_level == "debug" && applied_log_level == static_cast<int>(LogLevel::Debug));
    CHECK(Value(local_path, "FrameLimiter", "csv_enabled") == "false");
    ReleaseStart();CHECK(OwnerShutdown());
    // An unknown level is reset to warn and applied as such.
    Reset(false, false, {{"config_version", "6"}, {"log_level", "verbose"}});
    CHECK(g_config.log_level == "warn" && applied_log_level == static_cast<int>(LogLevel::Warn));
    CHECK(Value(local_path, "FrameLimiter", "log_level") == "warn");
    ReleaseStart();CHECK(OwnerShutdown());
}

// Each removed setting alone in an old INI: it is named as lost, absent from the rewritten INI (the whole section equals a
// first launch's), changes nothing the config or the pipeline holds, and a later save never writes it.
void TestEachRemovedKeyAloneIsDroppedAndNeverSaved() {
    Reset();
    const Fields fresh_fields = SectionFields(local_path, "FrameLimiter");
    const Loaded fresh = Capture();
    ReleaseStart();CHECK(OwnerShutdown());
    for (const RemovedSetting& setting : removed_settings) {
        Reset(false, false, {{"config_version", "5"}, {setting.key, setting.value}});
        CHECK(!Config_IsFirstLaunch());
        CHECK(RemovedInfoLine() == std::string(removed_info_prefix) + setting.key + "=" + setting.value);
        CHECK(Value(local_path, "FrameLimiter", setting.key).empty());
        CHECK(SectionFields(local_path, "FrameLimiter") == fresh_fields);
        CHECK(SameLoaded(fresh, Capture()));
        g_config.target_fps = 77;
        RequestSaveConfig();
        ReleaseStart();CHECK(OwnerShutdown());
        CheckWorkerWrites();
        CHECK(Value(local_path, "FrameLimiter", "target_fps") == "77");
        CHECK(Value(local_path, "FrameLimiter", setting.key).empty());
        CHECK(WrittenValues(setting.key).empty());
    }
}

// The live keys the INI carries are the ones below, written once here as the fixture of the contract: each is accepted
// with a value of its own, and every other key the INI carries is refused as not live and changes nothing. (The frame
// generation ordering switches are live too; the INI only reads them, so they are not part of this list.)
struct LiveKey {
    const char* key;
    const char* value;
};
const LiveKey live_keys[] = {
    {"target_fps", "60"}, {"background_fps", "45"}, {"fg_off_fps", "48"}, {"pacing_preference", "smooth"},
    {"gpu_present_gate_probes", "off"}, {"gpu_queue_probe", "off"}, {"gpu_queue_probe_source", "0"},
    {"unified_queue_margin_end", "0"}, {"unified_hold_share", "0.1"}, {"pacing_test_mode", "reflex_only"},
    {"unified_queue_law", "off"}, {"unified_queue_rank", "3"}, {"unified_queue_need_frames", "6"},
    {"gpu_wait_late_guard", "on"}, {"gpu_wait_present_first", "off"}, {"gpu_wait_start_limit", "off"},
    {"lead_queue_correction", "off"}, {"fg_flip_gate_cover", "80"}, {"fg_flip_gate_entry", "1"},
    {"fg_flip_gate_fit", "90"}, {"gpu_wait_ab_interleave_s", "30"},
    {"unified_queue_upstream", "1"}, {"unified_queue_allow_us", "2500"}};
static_assert(sizeof(live_keys) / sizeof(live_keys[0]) == 23, "the contract lists 23 live keys");
bool IsLiveKey(const std::string& key) {
    for (const LiveKey& live : live_keys)
        if (key == live.key) return true;
    return false;
}
void TestOnlyTheListedKeysAreLive() {
    Reset(false, false, {{"config_version", "6"}, {"developer_settings", "true"}});
    char error[128] = {};
    for (const LiveKey& live : live_keys) {
        CHECK(Config_SetLiveKey(live.key, live.value, error, sizeof(error)) && error[0] == '\0');
        CHECK(!Config_SetLiveKey(live.key, "not-a-value", error, sizeof(error)) && error[0] != '\0');
    }
    SaveConfig(); // the INI now carries every live key as set
    const Fields before = SectionFields(local_path, "FrameLimiter");
    CHECK(HasExactlyKeys(before, ExpectedIniKeys(false)));
    for (const LiveKey& live : live_keys) CHECK(before.count(live.key) == 1);
    size_t refused = 0;
    for (const auto& entry : before) {
        if (IsLiveKey(entry.first)) continue;
        char unknown[128] = {};
        CHECK(!Config_SetLiveKey(entry.first.c_str(), "1", unknown, sizeof(unknown)));
        CHECK(std::strstr(unknown, "not a live key") != nullptr);
        ++refused;
    }
    CHECK(refused == 90 - 23); // every key of the section that is not a live one
    SaveConfig();
    CHECK(SectionFields(local_path, "FrameLimiter") == before); // nothing a refused key named changed
    ReleaseStart();CHECK(OwnerShutdown());
}

// The developer fields Config_DeveloperOverrides counts and Config_ResetDeveloperSettings returns to their defaults are
// these 20 (a name and the way to move the field away from its default and test that it is back); nothing else counts, and
// the user's settings stay through the reset.
struct DeveloperField {
    const char* name;
    void (*move_away)();
    bool (*at_default)();
};
const DeveloperField developer_fields[] = {
    {"gpu_present_gate_probes", [] { g_config.gpu_present_gate_probes = false; }, [] { return g_config.gpu_present_gate_probes; }},
    {"flip_metering_off", [] { g_config.flip_metering_off = true; }, [] { return !g_config.flip_metering_off; }},
    {"vtable_copy_at_present", [] { g_config.vtable_copy_at_present = false; }, [] { return g_config.vtable_copy_at_present; }},
    {"gpu_wait_late_guard", [] { g_config.gpu_wait_late_guard = true; }, [] { return !g_config.gpu_wait_late_guard; }},
    {"gpu_wait_present_first", [] { g_config.gpu_wait_present_first = false; }, [] { return g_config.gpu_wait_present_first; }},
    {"gpu_wait_start_limit", [] { g_config.gpu_wait_start_limit = false; }, [] { return g_config.gpu_wait_start_limit; }},
    {"lead_queue_correction", [] { g_config.lead_queue_correction = false; }, [] { return g_config.lead_queue_correction; }},
    {"fg_flip_gate_cover", [] { g_config.fg_flip_gate_cover = 0; }, [] { return g_config.fg_flip_gate_cover == kPacingFgFlipGateCoverDefault; }},
    {"fg_flip_gate_entry", [] { g_config.fg_flip_gate_entry = 1; }, [] { return g_config.fg_flip_gate_entry == 0; }},
    {"fg_flip_gate_fit", [] { g_config.fg_flip_gate_fit = 0; }, [] { return g_config.fg_flip_gate_fit == kPacingFgFlipGateFitDefault; }},
    {"gpu_wait_ab_interleave_s", [] { g_config.gpu_wait_ab_interleave_s = 30; }, [] { return g_config.gpu_wait_ab_interleave_s == 0; }},
    {"control_file", [] { g_config.control_file = true; }, [] { return !g_config.control_file; }},
    {"gpu_queue_probe", [] { g_config.gpu_queue_probe = false; }, [] { return g_config.gpu_queue_probe; }},
    {"gpu_queue_probe_source", [] { g_config.gpu_queue_probe_source = 0; }, [] { return g_config.gpu_queue_probe_source == 1; }},
    {"unified_queue_margin_end", [] { g_config.unified_queue_margin_end = 0; }, [] { return g_config.unified_queue_margin_end == 1; }},
    {"pacing_test_mode", [] { g_config.pacing_test_mode = PacingTestMode::ReflexOnly; },
        [] { return g_config.pacing_test_mode == PacingTestMode::Off; }},
    {"unified_hold_share", [] { g_config.unified_hold_share = 0.1; }, [] { return g_config.unified_hold_share == 0.0; }},
    {"unified_queue_law", [] { g_config.unified_queue_law = false; }, [] { return g_config.unified_queue_law; }},
    {"unified_queue_rank", [] { g_config.unified_queue_rank = 3; }, [] { return g_config.unified_queue_rank == 2; }},
    {"unified_queue_need_frames", [] { g_config.unified_queue_need_frames = 6; }, [] { return g_config.unified_queue_need_frames == 4; }},
    {"unified_queue_upstream", [] { g_config.unified_queue_upstream = 1; }, [] { return g_config.unified_queue_upstream == 0; }},
    {"unified_queue_allow_us", [] { g_config.unified_queue_allow_us = 2500; }, [] { return g_config.unified_queue_allow_us == 4000; }},
    {"hang_report_dump", [] { g_config.hang_report_dump = false; }, [] { return g_config.hang_report_dump; }},
    {"hang_report_test_after_s", [] { g_config.hang_report_test_after_s = 30; }, [] { return g_config.hang_report_test_after_s == 0; }}};
static_assert(sizeof(developer_fields) / sizeof(developer_fields[0]) == 24, "the contract lists 24 developer fields");
void TestDeveloperFieldsAreCountedAndResetOneByOne() {
    Reset();
    CHECK(Config_DeveloperOverrides() == 0);
    for (const DeveloperField& field : developer_fields) {
        field.move_away();
        CHECK(!field.at_default());
        CHECK(Config_DeveloperOverrides() == 1);
        Config_ResetDeveloperSettings();
        CHECK(field.at_default() && Config_DeveloperOverrides() == 0);
        CheckPipelineMatchesConfig();
    }
    // The user's settings are not developer overrides and the reset leaves them.
    g_config.target_fps = 90;
    g_config.pacing_preference = PacingPreference::Smooth;
    g_config.developer_settings = true;
    g_config.advanced_settings = true;
    g_config.csv_enabled = true;
    g_config.log_level = "debug";
    g_config.osd_enabled = true;
    g_config.reflex_inject = true;
    CHECK(Config_DeveloperOverrides() == 0);
    Config_ResetDeveloperSettings();
    CHECK(g_config.target_fps == 90 && g_config.pacing_preference == PacingPreference::Smooth);
    CHECK(g_config.developer_settings && g_config.advanced_settings && g_config.csv_enabled);
    CHECK(g_config.log_level == "debug" && g_config.osd_enabled && g_config.reflex_inject);
    ReleaseStart();CHECK(OwnerShutdown());
}

// Every OSD element toggle is carried by the config, by a preset made from it, by each way of applying a preset (all
// fields, toggles only), by the INI (load and save) and by the preset sections (load and save): one toggle at a time, so a
// swapped or missing key shows.
void TestOsdElementTogglesCoverLoadSaveAndEveryPresetPath() {
    const auto only_config = [](size_t k) {
        for (size_t i = 0; i < osd_toggle_count; ++i)
            if (g_config.*(osd_toggles[i].config_field) != (i == k)) return false;
        return true;
    };
    const auto only_preset = [](const OSDPreset& p, size_t k) {
        for (size_t i = 0; i < osd_toggle_count; ++i)
            if (p.*(osd_toggles[i].preset_field) != (i == k)) return false;
        return true;
    };
    const auto set_all = [](bool value) {
        for (const OsdToggle& toggle : osd_toggles) g_config.*(toggle.config_field) = value;
    };
    Reset();
    for (size_t k = 0; k < osd_toggle_count; ++k) {
        set_all(false);
        g_config.*(osd_toggles[k].config_field) = true;
        OSDPreset preset = OSDPreset_FromConfig();
        CHECK(preset.occupied && only_preset(preset, k));
        preset.osd_x = 0.25f; preset.osd_y = 0.75f; preset.osd_scale = 1.5f; preset.osd_opacity = 0.5f;
        // Toggles only: the position, scale and opacity stay.
        set_all(true);
        g_config.osd_x = 0.5f; g_config.osd_y = 0.5f; g_config.osd_scale = 1.0f; g_config.osd_opacity = 0.6f;
        OSDPreset_ApplyTogglesOnly(preset);
        CHECK(only_config(k));
        CHECK(g_config.osd_x == 0.5f && g_config.osd_y == 0.5f && g_config.osd_scale == 1.0f && g_config.osd_opacity == 0.6f);
        // All fields: the position, scale and opacity follow too.
        set_all(true);
        OSDPreset_ApplyToConfig(preset);
        CHECK(only_config(k));
        CHECK(g_config.osd_x == 0.25f && g_config.osd_y == 0.75f && g_config.osd_scale == 1.5f && g_config.osd_opacity == 0.5f);
    }
    ReleaseStart();CHECK(OwnerShutdown());

    // Through the INI and the preset section of the disk.
    std::vector<std::string> preset_keys{"name", "osd_x", "osd_y", "osd_scale", "osd_opacity"};
    for (const OsdToggle& toggle : osd_toggles) preset_keys.push_back(toggle.preset_key);
    for (size_t k = 0; k < osd_toggle_count; ++k) {
        std::vector<IniEntry> ini{{"config_version", "6"}};
        for (size_t i = 0; i < osd_toggle_count; ++i) ini.push_back({osd_toggles[i].config_key, i == k ? "true" : "false"});
        extra_seed = {{local_path, "OSD_Preset_1", "name", "One"}, {local_path, "OSD_Preset_1", osd_toggles[k].preset_key, "true"}};
        Reset(false, false, ini);
        CHECK(only_config(k));
        const OSDPreset& loaded = OSDPreset_GetSlot(0);
        CHECK(loaded.occupied && std::strcmp(loaded.name, "One") == 0 && only_preset(loaded, k));
        OSDPreset_SaveSlot(0);
        ReleaseStart();CHECK(OwnerShutdown());
        const Fields app = SectionFields(local_path, "FrameLimiter");
        for (size_t i = 0; i < osd_toggle_count; ++i) CHECK(FieldIs(app, osd_toggles[i].config_key, i == k ? "true" : "false"));
        const Fields preset = SectionFields(local_path, "OSD_Preset_1");
        CHECK(HasExactlyKeys(preset, preset_keys));
        for (size_t i = 0; i < osd_toggle_count; ++i) CHECK(FieldIs(preset, osd_toggles[i].preset_key, i == k ? "true" : "false"));
        CHECK(SectionFields(local_path, "OSD_Preset_2").empty());
    }
}

// Jitter replaces PQI and Smoothness: an INI or a preset section that still has either on, and no jitter key, loads
// with Jitter on; a jitter key it carries wins; the rewritten INI carries the old keys no more.
void TestJitterTakesOverPqiAndSmoothness() {
    for (const char* old_key : {"osd_show_pqi", "osd_show_smoothness"}) {
        Reset(false, false, {{"config_version", "6"}, {old_key, "true"}});
        CHECK(g_config.osd_show_jitter);
        ReleaseStart();CHECK(OwnerShutdown());
        CHECK(Value(local_path, "FrameLimiter", "osd_show_jitter") == "true");
        CHECK(Value(local_path, "FrameLimiter", old_key).empty());
    }
    Reset(false, false, {{"config_version", "6"}, {"osd_show_pqi", "true"}, {"osd_show_smoothness", "true"},
        {"osd_show_jitter", "false"}});
    CHECK(!g_config.osd_show_jitter);
    ReleaseStart();CHECK(OwnerShutdown());
    Reset(false, false, {{"config_version", "6"}, {"osd_show_pqi", "false"}, {"osd_show_pqi_breakdown", "true"}});
    CHECK(!g_config.osd_show_jitter);
    ReleaseStart();CHECK(OwnerShutdown());
    CHECK(Value(local_path, "FrameLimiter", "osd_show_pqi_breakdown").empty());

    // A preset saved with the old keys.
    extra_seed = {{local_path, "OSD_Preset_1", "name", "Old"}, {local_path, "OSD_Preset_1", "show_smoothness", "true"},
        {local_path, "OSD_Preset_2", "name", "Kept"}, {local_path, "OSD_Preset_2", "show_pqi", "true"},
        {local_path, "OSD_Preset_2", "show_jitter", "false"}, {local_path, "OSD_Preset_3", "name", "Off"},
        {local_path, "OSD_Preset_3", "show_pqi_breakdown", "true"}};
    Reset(false, false, {{"config_version", "6"}});
    CHECK(OSDPreset_GetSlot(0).show_jitter);
    CHECK(!OSDPreset_GetSlot(1).show_jitter);
    CHECK(!OSDPreset_GetSlot(2).show_jitter);
    ReleaseStart();CHECK(OwnerShutdown());
}

// Deleting a preset compacts the slots (the ones after it move down), keeps at least the initial slots, and clears the
// sections that no longer hold a preset, so a deleted preset does not come back from a stale section; the shared and the
// local presets are separate destinations.
void TestPresetDeleteCompactsAndClearsTheTail() {
    Reset();
    ReleaseStart();
    const auto name_slot = [](int slot, const char* name) {
        OSDPreset& preset = OSDPreset_GetSlot(slot);
        preset.occupied = true;
        std::snprintf(preset.name, sizeof(preset.name), "%s", name);
    };
    name_slot(0, "A");
    name_slot(1, "B");
    name_slot(2, "C");
    const int added = OSDPreset_AddSlot();
    CHECK(added == 3);
    name_slot(added, "D");
    OSDPreset_SaveSlot(0);
    CHECK(Config_FlushPending());
    CHECK(Value(local_path, "OSD_Preset_1", "name") == "A" && Value(local_path, "OSD_Preset_2", "name") == "B");
    CHECK(Value(local_path, "OSD_Preset_3", "name") == "C" && Value(local_path, "OSD_Preset_4", "name") == "D");

    OSDPreset_DeleteSlot(1);
    CHECK(OSDPreset_GetCount() == 3 && std::strcmp(OSDPreset_GetSlot(1).name, "C") == 0);
    CHECK(Config_FlushPending());
    CHECK(Value(local_path, "OSD_Preset_1", "name") == "A" && Value(local_path, "OSD_Preset_2", "name") == "C");
    CHECK(Value(local_path, "OSD_Preset_3", "name") == "D");
    CHECK(SectionFields(local_path, "OSD_Preset_4").empty());

    OSDPreset_DeleteSlot(0);
    OSDPreset_DeleteSlot(0);
    CHECK(OSDPreset_GetCount() == 3 && OSDPreset_GetSlot(0).occupied && std::strcmp(OSDPreset_GetSlot(0).name, "D") == 0);
    CHECK(!OSDPreset_GetSlot(1).occupied && !OSDPreset_GetSlot(2).occupied);
    CHECK(Config_FlushPending());
    CHECK(Value(local_path, "OSD_Preset_1", "name") == "D");
    CHECK(SectionFields(local_path, "OSD_Preset_2").empty() && SectionFields(local_path, "OSD_Preset_3").empty());

    // Shared presets: the image goes to the shared file and the local file keeps what it had.
    g_config.shared_presets = true;
    OSDPreset_SaveSlot(0);
    CHECK(Config_FlushPending());
    CHECK(Value(shared_path, "OSD_Preset_1", "name") == "D");
    OSDPreset_DeleteSlot(0);
    CHECK(Config_FlushPending());
    CHECK(SectionFields(shared_path, "OSD_Preset_1").empty());
    CHECK(Value(local_path, "OSD_Preset_1", "name") == "D");
    CHECK(OwnerShutdown());
}

// The producers a UI thread calls (a settings or preset request, adding and deleting a slot, the destination switch) do no
// profile read, no profile write and create no thread: the disk is the worker's alone.
void TestUiRequestsDoNoCallerIo() {
    Reset(); // the worker is held before its first batch
    const unsigned reads_before = ReadCount();
    g_config.target_fps = 61;
    RequestSaveConfig();
    OSDPreset_GetSlot(0).occupied = true;
    std::snprintf(OSDPreset_GetSlot(0).name, sizeof(OSDPreset_GetSlot(0).name), "%s", "ui");
    OSDPreset_SaveSlot(0);
    const int added = OSDPreset_AddSlot();
    CHECK(added >= 0);
    OSDPreset_DeleteSlot(added);
    g_config.shared_presets = true;
    OSDPreset_SaveSlot(0);
    CHECK(WriteCount() == 0 && ReadCount() == reads_before && creates == 1);
    ReleaseStart();CHECK(OwnerShutdown());
    CheckWorkerWrites();
    CHECK(Value(local_path, "FrameLimiter", "target_fps") == "61");
    CHECK(Value(shared_path, "OSD_Preset_1", "name") == "ui");
}

// A synchronous save and a preset reload first drain what is queued, so an older request never overwrites a newer save
// and a queued preset is not replaced by a stale disk image (no worker here: the drain is the caller's, before the stop).
void TestSyncSaveAndReloadDrainQueuedWritesFirst() {
    Reset(true);
    g_config.target_fps = 70;
    RequestSaveConfig();
    g_config.target_fps = 80;
    SaveConfig();
    const std::vector<std::string> targets = WrittenValues("target_fps");
    CHECK(targets.size() == 2 && targets[0] == "70" && targets[1] == "80");
    CHECK(Value(local_path, "FrameLimiter", "target_fps") == "80");
    CHECK(s_pending_save.Empty() && s_failed_save.Empty());

    OSDPreset_GetSlot(0).occupied = true;
    std::snprintf(OSDPreset_GetSlot(0).name, sizeof(OSDPreset_GetSlot(0).name), "%s", "queued");
    OSDPreset_SaveSlot(0);
    CHECK(Value(local_path, "OSD_Preset_1", "name").empty());
    OSDPreset_LoadAll();
    CHECK(OSDPreset_GetSlot(0).occupied && std::strcmp(OSDPreset_GetSlot(0).name, "queued") == 0);
    CHECK(Value(local_path, "OSD_Preset_1", "name") == "queued");
    CHECK(OwnerShutdown());
}

// After the stop the synchronous entry points do no I/O at all; a request made then stays queued and unwritten; a new start
// after a clean stop works again.
void TestStoppedEntryPointsDoNoIo() {
    Reset();
    ReleaseStart();
    CHECK(Config_Shutdown());
    {
        std::lock_guard<std::mutex> lock(mutex);
        writes.clear();
    }
    const unsigned reads_before = ReadCount();
    g_config.target_fps = 99;
    SaveConfig();
    OSDPreset_LoadAll();
    CHECK(WriteCount() == 0 && ReadCount() == reads_before);
    CHECK(Config_FlushPending()); // nothing queued: drained, still no I/O
    RequestSaveConfig();
    CHECK(!Config_FlushPending()); // queued, and not written after the stop
    CHECK(!Config_Shutdown(0));
    CHECK(WriteCount() == 0 && Value(local_path, "FrameLimiter", "target_fps") != "99");
    // The next start (after the fixture discards the retained request) opens I/O again.
    Relaunch();
    g_config.target_fps = 98;
    RequestSaveConfig();
    ReleaseStart();CHECK(OwnerShutdown());
    CHECK(Value(local_path, "FrameLimiter", "target_fps") == "98");
}
} // namespace config_test

int main() {
    using namespace config_test;
    caller_thread = GetCurrentThreadId();
    TestPacingPipelineDefaults();
    TestQueueDefaultsMigration();
    TestDeveloperSettingsReset();
    TestGpuPresentGateProbesKey();
    TestGpuWaitPresentFirstIsOnByDefault();
    TestGpuWaitStartLimitIsOnByDefault();
    TestLeadQueueCorrectionIsOnByDefault();
    TestFgOrderingSwitchesAreOnByDefault();
    TestUnifiedHoldShareKey();
    TestUnifiedQueueLawKeys();
    TestUnifiedQueueNeedFramesKey();
    TestUnifiedQueueUpstreamKey();
    TestGpuQueueProbeSourceKey();
    TestUnifiedQueueMarginEndKey();
    TestFlipMeteringOffKey();
    TestHangReportKeys();
    TestLiveKeySetter();
    TestPacingPreference();
    TestPacingTestMode();
    TestRemovedKeysMigrationMatrix();
    TestSaveNeverResurrectsRemovedKeys();
    TestRemovedKeysAreNotLiveKeys();
    TestFirstLaunchAndDefaultValuedRemovedKeys();
    TestHookInstallerThreadKeyIsDropped();
    TestRemovedPresetKeyIsDropped();
    TestFailedPresetKeyDeletionIsRetriedAtTheNextStart();
    TestRetiredKeysIgnoredAndDropped();
    TestIniWritesExactlyTheKeysThatExist();
    TestSharedPresetsKeepTheKeybindsInTheSharedFile();
    TestSharedDefaultPresetAppliesAtLaunch();
    TestStartupKeepsLogLevelAndDisablesCsv();
    TestEachRemovedKeyAloneIsDroppedAndNeverSaved();
    TestOnlyTheListedKeysAreLive();
    TestDeveloperFieldsAreCountedAndResetOneByOne();
    TestOsdElementTogglesCoverLoadSaveAndEveryPresetPath();
    TestJitterTakesOverPqiAndSmoothness();
    TestPresetDeleteCompactsAndClearsTheTail();
    TestUiRequestsDoNoCallerIo();
    TestSyncSaveAndReloadDrainQueuedWritesFirst();
    TestStoppedEntryPointsDoNoIo();
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