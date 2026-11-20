#pragma once

#include <Windows.h>

struct PMTraceSession;

ULONG NativePclEtw_StartSession(
    PMTraceSession& session,
    const wchar_t* session_name) noexcept;
