#pragma once

#include <Windows.h>
#include <cstdarg>

// Structured file logger with ReShade message integration.
// FR-7: Initialization logging + runtime event logging.

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

// Initialize logger. Opens relimiter_<exe>.log (next to the game exe, else the addon DLL, else %TEMP%) after moving
// the previous log to relimiter_<exe>.prev.log, then attaches the start-up breadcrumb (relimiter_<exe>.crumb, see
// startup_breadcrumb.h) and warns once when the previous launch ended during start-up.
void Log_Init(HMODULE hModule, LogLevel level);
// Outside loader lock only. Closes admission, drains synchronous/buffered
// producers, joins the flush worker, then closes the file. False retains state;
// the caller must retain the module and must not reinitialize the logger.
bool Log_Shutdown();

// Core write function.
void Log_Write(LogLevel level, const char* fmt, ...);

// Set level at runtime (e.g., from config reload).
void Log_SetLevel(LogLevel level);
LogLevel Log_ParseLevel(const char* str);

// Call after init sequence to allow config-driven level changes
void Log_EndInitPhase();

// Returns the process name without extension (e.g. "game" for "game.exe").
// Valid after Log_Init; returns "unknown" if called before init.
const char* Log_GetProcessName();

// Convenience macros
#define LOG_ERROR(fmt, ...) Log_Write(LogLevel::Error, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  Log_Write(LogLevel::Warn,  fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  Log_Write(LogLevel::Info,  fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) Log_Write(LogLevel::Debug, fmt, ##__VA_ARGS__)
