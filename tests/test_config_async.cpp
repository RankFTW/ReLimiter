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
void VSync_ApplyOpenGL() {}
#ifdef _WIN64
void ReflexInject_SetEnabled(bool) noexcept {}
#endif
void Log_SetLevel(LogLevel) {}
LogLevel Log_ParseLevel(const char*) { return LogLevel::Warn; }
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
        g_config.pacing_preference, g_config.unified_queue_rank, g_config.unified_hold_share);
    CHECK(pipeline.unified_hold_share == (is64 ? effective.hold_share : 0.0));
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
    CheckProductionDefaults();
    // The startup rewrite saves every key, including the developer keys.
    CHECK(Value(local_path, "FrameLimiter", "flip_metering_off") == "false");
    CHECK(Value(local_path, "FrameLimiter", "control_file") == "false");
    CHECK(Value(local_path, "FrameLimiter", "developer_settings") == "false");
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "6");
    CHECK(Value(local_path, "FrameLimiter", "pacing_preference") == "balanced");
    CHECK(Value(local_path, "FrameLimiter", "pacing_test_mode") == "off");
    CHECK(Value(local_path, "FrameLimiter", "gpu_present_gate_probes") == "true");
    ReleaseStart();CHECK(OwnerShutdown());
}

// An INI from before config_version 5 saved the old defaults of the queue law, the queue probe and the
// probe's source key (false, false, 0): it moves to the new defaults once and keeps every other value; a
// version-5 INI keeps its explicit choices; keys the INI does not carry take the new defaults; a first
// launch (no INI) takes the defaults and is written at version 6.
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
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "6");
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_law") == "true");
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_probe") == "true");
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_probe_source") == "1");
    CHECK(Value(local_path, "FrameLimiter", "unified_queue_rank") == "3");
    ReleaseStart();CHECK(OwnerShutdown());

    // One key different is enough, and an INI without a version is older still.
    Reset(false, false, {{"gpu_queue_probe_source", "0"}});
    CHECK(g_config.unified_queue_law && g_config.gpu_queue_probe && g_config.gpu_queue_probe_source == 1);
    CHECK(Value(local_path, "FrameLimiter", "gpu_queue_probe_source") == "1");
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "6");
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
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "6");
    CHECK(Config_DeveloperOverrides() == 3);
    ReleaseStart();CHECK(OwnerShutdown());

    // A first launch (no INI at all) takes the defaults and writes version 6.
    Reset();
    CHECK(g_config.unified_queue_law && g_config.gpu_queue_probe && g_config.gpu_queue_probe_source == 1);
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "6");
    ReleaseStart();CHECK(OwnerShutdown());
}

// developer_settings is read and saved; every developer key away from its default is counted (the 13 developer
// fields), and Reset returns exactly those keys to the defaults and applies them, leaving the user's settings alone.
void TestDeveloperSettingsReset() {
    const bool is64 = sizeof(void*) == 8;
    Reset(false, false, {{"config_version", "6"}, {"developer_settings", "true"}, {"target_fps", "90"},
        {"pacing_preference", "smooth"}, {"gpu_present_gate_probes", "false"}, {"flip_metering_off", "true"},
        {"control_file", "true"}, {"gpu_queue_probe", "false"}, {"gpu_queue_probe_source", "0"},
        {"unified_queue_margin_end", "0"}, {"pacing_test_mode", "driver_limiter"}, {"unified_hold_share", "0.1"},
        {"unified_queue_law", "false"}, {"unified_queue_rank", "3"}, {"unified_queue_need_frames", "6"},
        {"unified_queue_upstream", "1"}, {"unified_queue_allow_us", "2500"}});
    CHECK(g_config.developer_settings);
    CHECK(Value(local_path, "FrameLimiter", "developer_settings") == "true");
    CHECK(Config_DeveloperOverrides() == 13);
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
// A non-default value of every setting kRemovedKeys lists: what an earlier build wrote when the user had touched it.
struct RemovedSetting {
    const char* key;
    const char* value;
};
constexpr RemovedSetting removed_settings[] = {
    {"adaptive_cap", "true"}, {"adaptive_cap_percentile", "99"}, {"adaptive_cap_hold_room", "false"},
    {"osd_show_adaptive_smoothing", "true"}, {"adaptive_smoothing", "true"}, {"smoothing", "off"},
    {"output_rung", "gpu"}, {"source_placement", "driver"}, {"reflex_sleep_bypass", "false"},
    {"bypass_low_latency_off", "true"}, {"driver_low_latency_off", "true"}, {"driver_limiter_hold", "true"},
    {"pre_sleep_precise", "true"}, {"gpu_paced_cadence", "true"}, {"lead_coverage", "95"},
    {"lead_spike_floor", "false"}, {"fg_late_spacing", "false"}, {"gpu_queue_wait_render_queue", "remove"},
    {"gpu_wait_interleave_s", "2.5"}, {"stall_inject_ms", "12.5"}, {"stall_inject_period_s", "2"},
    {"marker_forward_order", "after_wait"}, {"sleep_status_every_frame", "true"}, {"gpu_start_probe", "true"},
    {"gpu_priority_probe", "true"}, {"fg_presenter_probe_us", "500"}, {"unified_pacer", "false"},
    {"unified_every_frame_service", "false"}, {"unified_queue_target_us", "4000"}, {"gpu_wait_headroom_us", "2000"}};

// The list above is exactly production's table, and every value in it is away from the key's old default.
void CheckRemovedSettingsCoverTheTable() {
    constexpr size_t count = sizeof(removed_settings) / sizeof(removed_settings[0]);
    CHECK(count == sizeof(kRemovedKeys) / sizeof(kRemovedKeys[0]));
    for (const RemovedSetting& setting : removed_settings) {
        const RemovedKey* match = nullptr;
        for (const RemovedKey& key : kRemovedKeys)
            if (std::strcmp(key.name, setting.key) == 0) match = &key;
        CHECK(match != nullptr);
        CHECK(!SameAsDefault(setting.value, match->default_text));
    }
}
// No removed key is on the fake disk, in any section of the INI, and the INI is at version 6.
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
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "6");
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
// preserved; the INI is rewritten at version 6 without any removed key and loads again to the same state. With
// developer_settings on and every old arm key set (unified_pacer=false, source_placement=driver,
// reflex_sleep_bypass=false, adaptive_cap=true, ...) it is still no test mode; only an explicit pacing_test_mode
// enters one, and only with developer_settings.
void TestRemovedKeysMigrationMatrix() {
    CheckRemovedSettingsCoverTheTable();
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

// A first launch (no INI) writes version 6 and no removed key and logs nothing about removed settings. A version-5 INI
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
    for (const RemovedKey& key : kRemovedKeys) old_defaults.push_back({key.name, key.default_text});
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
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "6");

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
// goes on (the INI is rewritten at version 6), and the next start removes the key once the file is writable again.
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
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "6");
    CHECK(PresetFields(local_path).empty());
    ReleaseStart();CHECK(OwnerShutdown());
    fail_path.clear();
    Relaunch(); // the same fake disk, the file writable now, the INI already at version 6
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
    CHECK(Value(local_path, "FrameLimiter", "config_version") == "6");
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
        {"gpu_paced_release_smoothing", "true"}});
    // None of them takes effect; the pipeline matches a fresh INI.
    CHECK(g_config.gpu_present_gate_probes);
    CheckProductionDefaults();
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
    TestQueueDefaultsMigration();
    TestDeveloperSettingsReset();
    TestGpuPresentGateProbesKey();
    TestUnifiedHoldShareKey();
    TestUnifiedQueueLawKeys();
    TestUnifiedQueueNeedFramesKey();
    TestUnifiedQueueUpstreamKey();
    TestGpuQueueProbeSourceKey();
    TestUnifiedQueueMarginEndKey();
    TestFlipMeteringOffKey();
    TestLiveKeySetter();
    TestPacingPreference();
    TestPacingTestMode();
    TestRemovedKeysMigrationMatrix();
    TestSaveNeverResurrectsRemovedKeys();
    TestRemovedKeysAreNotLiveKeys();
    TestFirstLaunchAndDefaultValuedRemovedKeys();
    TestRemovedPresetKeyIsDropped();
    TestFailedPresetKeyDeletionIsRetriedAtTheNextStart();
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