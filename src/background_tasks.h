#pragma once

#include <Windows.h>

using BackgroundTaskProc = DWORD(WINAPI*)(LPVOID);

// Start a short-lived task owned by the addon. Returns false when shutdown has
// begun, the fixed task registry is full, or thread creation fails.
bool BackgroundTasks_Start(BackgroundTaskProc procedure, void* context) noexcept;

// Bounded-wait for tracked tasks. Timed-out handles remain owned and the caller
// must retain the addon module until process exit.
bool BackgroundTasks_Shutdown(DWORD timeout_ms = 2000) noexcept;
