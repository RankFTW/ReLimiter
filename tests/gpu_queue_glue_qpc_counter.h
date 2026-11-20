#pragma once

// Force-included (MSVC /FI, tests/CMakeLists.txt) into the two glue translation units that read the clock
// (src/gpu_queue_probe_d3d12.cpp, src/streamline_queue_hooks.cpp) of the glue test and bench executables only: every
// QueryPerformanceCounter call of the glue goes through a counter that the bench reports per call
// (glue::g_thread.qpc_reads, thread-local). Nothing else changes; the addon is not compiled with it.

#include <Windows.h>

BOOL GlueCountedQpc(LARGE_INTEGER* value) noexcept;
#define QueryPerformanceCounter(value) GlueCountedQpc(value)
