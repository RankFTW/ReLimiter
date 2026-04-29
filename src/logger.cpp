#include "logger.h"
#include <reshade.hpp>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <atomic>

static HANDLE s_hfile = INVALID_HANDLE_VALUE;
static LogLevel s_level = LogLevel::Info;
static std::mutex s_mutex;
static LARGE_INTEGER s_start_qpc;
static double s_qpc_freq_inv = 0.0;
static bool s_init_phase = true;
static char s_process_name[MAX_PATH] = "unknown";

// ── Buffered logging ──
// Log messages are appended to a ring buffer. A background thread flushes
// to disk every 500ms. This keeps file I/O off the render thread entirely.
// During init phase, writes are immediate (crash safety before game starts).
static constexpr size_t LOG_BUF_SIZE = 64 * 1024;  // 64KB ring buffer
static char s_log_buf[LOG_BUF_SIZE];
static size_t s_log_buf_pos = 0;
static std::mutex s_buf_mutex;
static HANDLE s_flush_thread = nullptr;
static std::atomic<bool> s_flush_running{false};

static void FlushBuffer() {
    std::lock_guard<std::mutex> lock(s_buf_mutex);
    if (s_log_buf_pos > 0 && s_hfile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(s_hfile, s_log_buf, static_cast<DWORD>(s_log_buf_pos), &written, nullptr);
        FlushFileBuffers(s_hfile);
        s_log_buf_pos = 0;
    }
}

static DWORD WINAPI LogFlushThread(LPVOID) {
    while (s_flush_running.load(std::memory_order_relaxed)) {
        Sleep(500);
        FlushBuffer();
    }
    FlushBuffer();  // final flush on shutdown
    return 0;
}

static void StartFlushThread() {
    if (s_flush_thread) return;
    s_flush_running.store(true, std::memory_order_relaxed);
    s_flush_thread = CreateThread(nullptr, 0, LogFlushThread, nullptr, 0, nullptr);
}

static void StopFlushThread() {
    if (!s_flush_thread) return;
    s_flush_running.store(false, std::memory_order_relaxed);
    WaitForSingleObject(s_flush_thread, 2000);
    CloseHandle(s_flush_thread);
    s_flush_thread = nullptr;
}

static void BufferedWrite(const char* str, int len) {
    std::lock_guard<std::mutex> lock(s_buf_mutex);
    if (static_cast<size_t>(len) > LOG_BUF_SIZE) return;  // message too large
    // If buffer would overflow, drop oldest content
    if (s_log_buf_pos + static_cast<size_t>(len) > LOG_BUF_SIZE)
        s_log_buf_pos = 0;
    memcpy(s_log_buf + s_log_buf_pos, str, len);
    s_log_buf_pos += len;
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

static bool TryOpen(const char* path) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        s_hfile = h;
        return true;
    }
    return false;
}

void Log_Init(HMODULE hModule, LogLevel level) {
    if (s_hfile != INVALID_HANDLE_VALUE) return; // already initialized

    s_level = level;
    s_init_phase = true;

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
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
    return;

opened:
    {
        char header[512];
        int n = snprintf(header, sizeof(header),
            "=== relimiter log [%s] (PID %lu) ===\r\nLog: %s\r\n",
            exe_name, pid, path);
        if (n > 0) RawWrite(header, n);
        RawFlush();
    }
}

void Log_Shutdown() {
    StopFlushThread();
    FlushBuffer();
    if (s_hfile != INVALID_HANDLE_VALUE) {
        char msg[] = "=== relimiter log closed ===\r\n";
        RawWrite(msg, static_cast<int>(strlen(msg)));
        RawFlush();
        CloseHandle(s_hfile);
        s_hfile = INVALID_HANDLE_VALUE;
    }
}

void Log_Write(LogLevel level, const char* fmt, ...) {
    if (level > s_level) return;

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
        if (s_init_phase) {
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
    if (s_init_phase && level < s_level) return;
    s_level = level;
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
    s_init_phase = false;
    // Flush any remaining init-phase messages, then start background flushing
    FlushBuffer();
    StartFlushThread();
}

const char* Log_GetProcessName() {
    return s_process_name;
}
