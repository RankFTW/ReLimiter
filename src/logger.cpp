#include "logger.h"
#include <reshade.hpp>
#include <cstdio>
#include <cstring>
#include <mutex>

static HANDLE s_hfile = INVALID_HANDLE_VALUE;
static LogLevel s_level = LogLevel::Info;
static std::mutex s_mutex;
static LARGE_INTEGER s_start_qpc;
static double s_qpc_freq_inv = 0.0;
static bool s_init_phase = true;
static char s_process_name[MAX_PATH] = "unknown";

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

    std::lock_guard<std::mutex> lock(s_mutex);
    if (n > 0) RawWrite(line, n);
    RawFlush(); // always flush for crash safety

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
}

const char* Log_GetProcessName() {
    return s_process_name;
}
