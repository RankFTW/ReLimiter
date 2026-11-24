#include "logger.h"
#include "shutdown_admission.h"
#include "startup_breadcrumb.h"
#include "log_level_state.h"
#include "nvapi_marker_census.h"
#include "relimiter_version.h"
#include <reshade.hpp>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <atomic>

static HANDLE s_hfile = INVALID_HANDLE_VALUE;
// The requested level and a temporary floor of it in one atomic word (log_level_state.h): Log_Write's rejected path
// is one load.
static LogLevelState s_levels{static_cast<uint8_t>(LogLevel::Info)};
// Lifecycle operations never run on logging producers or under loader lock.
static std::mutex s_lifecycle_mutex;
static std::mutex s_mutex;
static ShutdownAdmission s_write_admission;

class LogWriteGuard {
public:
    LogWriteGuard() noexcept : admitted_(s_write_admission.TryEnter()) {}
    ~LogWriteGuard() {
        if (admitted_) s_write_admission.Leave();
    }
    bool IsAdmitted() const noexcept { return admitted_; }
private:
    bool admitted_;
};

static LARGE_INTEGER s_start_qpc;
static double s_qpc_freq_inv = 0.0;
static uint64_t s_qpc_frequency = 0;
static std::atomic<bool> s_init_phase{true};
static char s_process_name[MAX_PATH] = "unknown";
static char s_log_path[MAX_PATH] = "";

// ── Buffered logging ──
// Log messages are appended to a ring buffer. A background thread flushes
// a batch within 500 ms of its first line, sleeping indefinitely when empty.
// During init phase, writes are immediate (crash safety before game starts).
static constexpr size_t LOG_BUF_SIZE = 64 * 1024;  // 64KB ring buffer
static char s_log_buf[LOG_BUF_SIZE];
static char s_flush_buf[LOG_BUF_SIZE];
static size_t s_log_buf_pos = 0;
static std::mutex s_buf_mutex;
static HANDLE s_flush_thread = nullptr;
static std::condition_variable s_flush_changed;
static bool s_flush_running = false; // under s_buf_mutex
static std::chrono::steady_clock::time_point s_flush_deadline;

static void FlushBuffer() {
    // Serialize file access and the shared flush scratch buffer with init-phase
    // synchronous writers and final close, but never with buffered producers.
    std::lock_guard<std::mutex> file_lock(s_mutex);
    size_t flush_size = 0;
    {
        std::lock_guard<std::mutex> lock(s_buf_mutex);
        flush_size = s_log_buf_pos;
        if (flush_size > 0) {
            memcpy(s_flush_buf, s_log_buf, flush_size);
            s_log_buf_pos = 0;
        }
    }

    // Never hold the producer mutex across filesystem I/O. A slow disk must
    // not block a marker, Present, or driver hook attempting to log.
    if (flush_size > 0 && s_hfile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(
            s_hfile,
            s_flush_buf,
            static_cast<DWORD>(flush_size),
            &written,
            nullptr);
        FlushFileBuffers(s_hfile);
    }
}

// A debug log batch is the consumer for the changed marker census. Do not
// poll the marker thread (or wake an otherwise idle worker) for diagnostics.
static void LogNvApiMarkerCensus(int64_t* last_qpc,
    std::array<uint64_t, NvApiMarkerCensus::Types * 4>* counts) {
    if (s_levels.Effective() < static_cast<uint8_t>(LogLevel::Debug)) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (*last_qpc && now.QuadPart - *last_qpc < static_cast<int64_t>(s_qpc_frequency) * 5) return;
    *last_qpc = now.QuadPart;
    for (uint32_t type = 0; type < NvApiMarkerCensus::Types; ++type) {
        for (uint32_t stage = 0; stage < 4; ++stage) {
            NvApiMarkerCensus::Snapshot snapshot;
            uint64_t& logged_count = (*counts)[type * 4 + stage];
            if (!g_nvapi_marker_census.Read(type, static_cast<MarkerCensusStage>(stage), &snapshot) ||
                snapshot.count == logged_count) continue;
            logged_count = snapshot.count;
            const auto& first = snapshot.first;
            const auto& last = snapshot.last;
            LOG_DEBUG("NvAPI census type=%u stage=%u count=%llu sample_misses=%llu "
                "first_frame=%llu first_qpc=%lld first_caller=0x%llx first_device=0x%llx "
                "last_frame=%llu last_qpc=%lld caller=0x%llx device=0x%llx thread=%u producer=%u reason=%u selected=%u frequency=%llu",
                type, stage, snapshot.count, snapshot.misses, first.frame, first.qpc, first.caller, first.device,
                last.frame, last.qpc, last.caller, last.device, last.thread, last.producer, last.reason,
                last.selected, s_qpc_frequency);
        }
    }
}

static DWORD WINAPI LogFlushThread(LPVOID) {
    int64_t census_qpc = 0;
    std::array<uint64_t, NvApiMarkerCensus::Types * 4> census_counts{};
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(s_buf_mutex);
            s_flush_changed.wait(lock, [] { return !s_flush_running || s_log_buf_pos != 0; });
            if (!s_flush_running) break;
            // Batch subsequent lines without waking for each one. Shutdown interrupts the delay.
            s_flush_changed.wait_until(lock, s_flush_deadline, [] { return !s_flush_running; });
        }
        LogNvApiMarkerCensus(&census_qpc, &census_counts);
        FlushBuffer();
    }
    FlushBuffer();
    return 0;
}

static void StartFlushThread() {
    if (s_flush_thread) return;
    std::lock_guard<std::mutex> lock(s_buf_mutex);
    s_flush_running = true;
    s_flush_thread = CreateThread(nullptr, 0, LogFlushThread, nullptr, 0, nullptr);
    if (!s_flush_thread)
        s_flush_running = false;
}

static bool StopFlushThread() {
    if (!s_flush_thread) return true;
    {
        std::lock_guard<std::mutex> lock(s_buf_mutex);
        s_flush_running = false;
    }
    s_flush_changed.notify_one();
    DWORD wait_status = WaitForSingleObject(s_flush_thread, 2000);
    if (wait_status != WAIT_OBJECT_0) {
        CancelSynchronousIo(s_flush_thread);
        wait_status = WaitForSingleObject(s_flush_thread, 500);
    }
    if (wait_status != WAIT_OBJECT_0) return false;
    CloseHandle(s_flush_thread);
    s_flush_thread = nullptr;
    return true;
}

static void BufferedWrite(const char* str, int len) {
    std::lock_guard<std::mutex> lock(s_buf_mutex);
    if (static_cast<size_t>(len) > LOG_BUF_SIZE) return;  // message too large
    const bool was_empty = s_log_buf_pos == 0;
    // If buffer would overflow, drop oldest content
    if (s_log_buf_pos + static_cast<size_t>(len) > LOG_BUF_SIZE)
        s_log_buf_pos = 0;
    memcpy(s_log_buf + s_log_buf_pos, str, len);
    s_log_buf_pos += len;
    if (was_empty) {
        s_flush_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        s_flush_changed.notify_one();
    }
}

static const char* LevelTag(LogLevel lv) {
    switch (lv) {
    case LogLevel::Error: return "ERR ";
    case LogLevel::Warn:  return "WARN";
    case LogLevel::Info:  return "INFO";
    case LogLevel::Debug: return "DBG ";
    }
    return "????";
}

static double ElapsedMs() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - s_start_qpc.QuadPart) * s_qpc_freq_inv * 1000.0;
}

static void RawWrite(const char* str, int len) {
    if (s_hfile == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(s_hfile, str, static_cast<DWORD>(len), &written, nullptr);
}

static void RawFlush() {
    if (s_hfile != INVALID_HANDLE_VALUE) FlushFileBuffers(s_hfile);
}

// "<dir>\relimiter_<exe>.log" -> "<dir>\relimiter_<exe><suffix>" (suffix: ".prev.log" or ".crumb").
static bool SiblingPath(const char* log_path, const char* suffix, char* out, size_t cap) {
    const size_t len = strlen(log_path);
    if (len < 4 || _stricmp(log_path + len - 4, ".log") != 0) return false;
    const int n = snprintf(out, cap, "%.*s%s", static_cast<int>(len - 4), log_path, suffix);
    return n > 0 && static_cast<size_t>(n) < cap;
}

static bool TryOpen(const char* path) {
    // Keep the previous launch's log: relimiter_<exe>.prev.log in the same directory, replacing an older one.
    // Failure is not an error (no previous log, the file is held by a live launch, a read-only folder).
    char prev_path[MAX_PATH];
    if (SiblingPath(path, ".prev.log", prev_path, sizeof(prev_path)))
        MoveFileExA(path, prev_path, MOVEFILE_REPLACE_EXISTING);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        s_hfile = h;
        return true;
    }
    return false;
}

// Opens the log under the lifecycle and file locks. True: the log is open and `previous` holds the verdict on the
// previous launch's start-up breadcrumb (reported by Log_Init once the locks are released, because Log_Write
// takes the file lock during the init phase).
static bool InitLocked(HMODULE hModule, LogLevel level, BreadcrumbVerdict* previous) {
    std::lock_guard<std::mutex> lifecycle_lock(s_lifecycle_mutex);
    if (s_flush_thread || !s_write_admission.IsDrained()) return false;
    std::lock_guard<std::mutex> file_lock(s_mutex);
    if (s_hfile != INVALID_HANDLE_VALUE) return false; // already initialized

    s_levels.Reset(static_cast<uint8_t>(level));
    s_init_phase.store(true, std::memory_order_release);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    s_qpc_frequency = static_cast<uint64_t>(freq.QuadPart);
    s_qpc_freq_inv = 1.0 / static_cast<double>(freq.QuadPart);
    QueryPerformanceCounter(&s_start_qpc);

    char exe_path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    const char* exe_name = strrchr(exe_path, '\\');
    exe_name = exe_name ? exe_name + 1 : exe_path;

    // Store process name without extension for filename use
    strncpy(s_process_name, exe_name, MAX_PATH - 1);
    s_process_name[MAX_PATH - 1] = '\0';
    char* dot = strrchr(s_process_name, '.');
    if (dot) *dot = '\0';

    DWORD pid = GetCurrentProcessId();
    char path[MAX_PATH] = {};

    // Try 1: next to the game executable
    {
        char exe_dir[MAX_PATH] = {};
        strncpy(exe_dir, exe_path, MAX_PATH - 1);
        char* slash = strrchr(exe_dir, '\\');
        if (slash) *(slash + 1) = '\0';
        snprintf(path, sizeof(path), "%srelimiter_%s.log", exe_dir, s_process_name);
        if (TryOpen(path)) goto opened;
    }

    // Try 2: next to the addon DLL
    {
        char dll_dir[MAX_PATH] = {};
        GetModuleFileNameA(hModule, dll_dir, MAX_PATH);
        char* slash = strrchr(dll_dir, '\\');
        if (slash) *(slash + 1) = '\0';
        snprintf(path, sizeof(path), "%srelimiter_%s.log", dll_dir, s_process_name);
        if (TryOpen(path)) goto opened;
    }

    // Try 3: %TEMP%
    {
        char temp[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, temp);
        snprintf(path, sizeof(path), "%srelimiter_%s.log", temp, s_process_name);
        if (TryOpen(path)) goto opened;
    }

    OutputDebugStringA("[relimiter] LOG INIT FAILED — could not open any log file\n");
    s_write_admission.Open(); // still allow ReShade warning/error forwarding
    return false;

opened:
    strncpy(s_log_path, path, MAX_PATH - 1);
    s_log_path[MAX_PATH - 1] = '\0';
    {
        char header[512];
        int n = snprintf(header, sizeof(header),
            "=== relimiter log [%s] (PID %lu) ===\r\nReLimiter %s (%s)\r\nLog: %s\r\n",
            exe_name, pid, RELIMITER_VERSION_STRING, sizeof(void*) == 8 ? "64-bit" : "32-bit", path);
        if (n > 0) RawWrite(header, n);
        RawFlush();
    }
    {
        // The breadcrumb file sits next to the log that opened; it is read (and reset) after the new log is open.
        char crumb_path[MAX_PATH];
        if (SiblingPath(path, ".crumb", crumb_path, sizeof(crumb_path)))
            *previous = Breadcrumb_Attach(crumb_path);
    }
    s_write_admission.Open();
    return true;
}

void Log_Init(HMODULE hModule, LogLevel level) {
    BreadcrumbVerdict previous;
    // InitLocked releases the lifecycle and file locks before the warning is written (Log_Write takes the file lock).
    if (!InitLocked(hModule, level, &previous)) return;
    char message[160];
    if (Breadcrumb_Describe(previous, message, sizeof(message)) > 0)
        LOG_WARN("%s", message);
}

bool Log_Shutdown() {
    std::lock_guard<std::mutex> lifecycle_lock(s_lifecycle_mutex);
    s_write_admission.Close();
    if (!StopFlushThread()) {
        OutputDebugStringA(
            "ReLimiter: logger worker did not stop; file state retained.\n");
        return false;
    }
    const ULONGLONG start = GetTickCount64();
    while (!s_write_admission.IsDrained()) {
        if (GetTickCount64() - start >= 2500) {
            OutputDebugStringA(
                "ReLimiter: logger producers did not drain; file state retained.\n");
            return false;
        }
        Sleep(1);
    }
    FlushBuffer();
    std::lock_guard<std::mutex> file_lock(s_mutex);
    if (s_hfile != INVALID_HANDLE_VALUE) {
        char msg[] = "=== relimiter log closed ===\r\n";
        RawWrite(msg, static_cast<int>(strlen(msg)));
        RawFlush();
        CloseHandle(s_hfile);
        s_hfile = INVALID_HANDLE_VALUE;
    }
    return true;
}

bool Log_IsEnabled(LogLevel level) noexcept {
    return static_cast<uint8_t>(level) <= s_levels.Effective();
}

void Log_Write(LogLevel level, const char* fmt, ...) {
    if (!Log_IsEnabled(level)) return;
    LogWriteGuard writer;
    if (!writer.IsAdmitted()) return;

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len < 0) return;

    double ms = ElapsedMs();

    char line[1200];
    int n = snprintf(line, sizeof(line), "[%10.3f] [%s] %s\r\n", ms, LevelTag(level), buf);

    if (n > 0) {
        if (s_init_phase.load(std::memory_order_acquire)) {
            // During init: write directly for crash safety
            std::lock_guard<std::mutex> lock(s_mutex);
            RawWrite(line, n);
            RawFlush();
        } else {
            // After init: buffer for background flush
            BufferedWrite(line, n);
        }
    }

    // Forward errors/warnings to ReShade log
    if (level <= LogLevel::Warn) {
        reshade::log::message(
            level == LogLevel::Error ? reshade::log::level::error : reshade::log::level::warning,
            buf);
    }
}

void Log_SetLevel(LogLevel level) {
    if (s_init_phase.load(std::memory_order_acquire) &&
        level < static_cast<LogLevel>(s_levels.Effective())) return;
    // The request is stored whatever a temporary floor says; the floor only decides what is effective now.
    s_levels.Request(static_cast<uint8_t>(level));
}

void Log_BeginLevelFloor(LogLevel level) {
    s_levels.BeginFloor(static_cast<uint8_t>(level));
}

void Log_EndLevelFloor() {
    s_levels.EndFloor();
}

LogLevel Log_ParseLevel(const char* str) {
    if (!str) return LogLevel::Info;
    if (_stricmp(str, "error") == 0) return LogLevel::Error;
    if (_stricmp(str, "warn") == 0)  return LogLevel::Warn;
    if (_stricmp(str, "info") == 0)  return LogLevel::Info;
    if (_stricmp(str, "debug") == 0) return LogLevel::Debug;
    return LogLevel::Info;
}

void Log_EndInitPhase() {
    std::lock_guard<std::mutex> lifecycle_lock(s_lifecycle_mutex);
    if (s_write_admission.IsClosed() ||
        !s_init_phase.load(std::memory_order_acquire)) return;
    // Keep synchronous logging if the worker cannot start.
    StartFlushThread();
    if (s_flush_thread) s_init_phase.store(false, std::memory_order_release);
}

const char* Log_GetProcessName() {
    return s_process_name;
}

const char* Log_GetFilePath() {
    return s_log_path;
}
