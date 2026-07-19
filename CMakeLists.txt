cmake_minimum_required(VERSION 3.20)
project(relimiter LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(RELIMITER_ENABLE_MCP
    "Build read-only live pacing telemetry and the MCP stdio bridge"
    OFF)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RELIMITER_VERSION LIMIT_COUNT 1)
string(STRIP "${RELIMITER_VERSION}" RELIMITER_VERSION)
if(NOT RELIMITER_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "VERSION must contain a semantic version such as 3.3.1")
endif()
set(RELIMITER_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(RELIMITER_VERSION_MINOR "${CMAKE_MATCH_2}")
set(RELIMITER_VERSION_PATCH "${CMAKE_MATCH_3}")
set(RELIMITER_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${RELIMITER_GENERATED_INCLUDE_DIR}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/relimiter_version.h.in"
    "${RELIMITER_GENERATED_INCLUDE_DIR}/relimiter_version.h"
    @ONLY)

# ── Architecture detection ──
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(RELIMITER_64BIT TRUE)
    set(ADDON_SUFFIX ".addon64")
    message(STATUS "ReLimiter: building 64-bit addon")
else()
    set(RELIMITER_64BIT FALSE)
    set(ADDON_SUFFIX ".addon32")
    message(STATUS "ReLimiter: building 32-bit addon")
endif()

# ── Output ──
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Sources (shared — builds for both 32 and 64-bit) ──
set(ADDON_SOURCES
    src/dllmain.cpp
    src/cadence_meter.cpp
    src/ceiling_margin.cpp
    src/config.cpp
    src/correlator.cpp
    src/damping.cpp
    src/display_poll_thread.cpp
    src/display_resolver.cpp
    src/display_state.cpp
    src/enforcement_dispatcher.cpp
    src/feedback.cpp
    src/flip_metering.cpp
    src/flip_model.cpp
    src/flush.cpp
    src/frame_latency_controller.cpp
    src/health.cpp
    src/hooks.cpp
    src/hw_monitor.cpp
    src/hw_spin.cpp
    src/loadlib_hooks.cpp
    src/marker_log.cpp
    src/native_pcl_measurement.cpp
    src/nvapi_hooks.cpp
    src/osd.cpp
    src/pll.cpp
    src/predictor.cpp
    src/regime_detector.cpp
    src/scheduler.cpp
    src/sleep.cpp
    src/sleep_mode.cpp
    src/stress_detector.cpp
    src/swapchain_manager.cpp
    src/system_hardening.cpp
    src/tier.cpp
    src/timer_hooks.cpp
    src/tsc_cal.cpp
    src/vblank_thread.cpp
    src/wake_guard.cpp
    src/logger.cpp
    src/csv_writer.cpp
    src/pqi.cpp
    src/baseline.cpp
    src/presentation_gate.cpp
    src/vsync_control.cpp
    src/vk_enforce.cpp
    src/blackout.cpp
    src/focus_lock.cpp
)

if(RELIMITER_ENABLE_MCP)
    list(APPEND ADDON_SOURCES src/live_debug.cpp)
endif()

# ── 64-bit only sources ──
if(RELIMITER_64BIT)
    list(APPEND ADDON_SOURCES
        src/adaptive_smoothing.cpp
        src/fg_divisor.cpp
        src/frame_splitting.cpp
        src/ngx_hooks.cpp
        src/streamline_hooks.cpp
        src/pcl_hooks.cpp
        src/reflex_inject.cpp
        src/dlss_presets.cpp
    )
endif()

# ── MinHook (compiled as part of the addon) ──
set(MINHOOK_SOURCES
    deps/reshade/deps/minhook/src/buffer.c
    deps/reshade/deps/minhook/src/hook.c
    deps/reshade/deps/minhook/src/trampoline.c
    deps/reshade/deps/minhook/src/hde/hde32.c
    deps/reshade/deps/minhook/src/hde/hde64.c
)

# ── PresentMon PresentData (native NVIDIA PCL + display correlation) ──
set(PRESENTMON_PRESENTDATA_SOURCES
    deps/presentmon/PresentData/Debug.cpp
    deps/presentmon/PresentData/GpuTrace.cpp
    deps/presentmon/PresentData/NvidiaTraceConsumer.cpp
    deps/presentmon/PresentData/PresentMonTraceConsumer.cpp
    deps/presentmon/PresentData/TraceConsumer.cpp
    deps/presentmon/PresentData/PresentMonTraceSession.cpp
    deps/presentmon/PresentData/TraceLogging.cpp
    src/native_pcl_collector.cpp
    src/native_pcl_etw_cleanup.cpp
    src/native_pcl_etw_diagnostics.cpp
    src/native_pcl_etw_provider_setup.cpp
    src/native_pcl_etw_session.cpp
    src/presentmon_runtime_compat.cpp
)

add_library(presentmon_presentdata STATIC ${PRESENTMON_PRESENTDATA_SOURCES})
# PresentMon's disabled logging facade still declares std::stacktrace types.
# Keep that upstream-only requirement isolated; ReLimiter itself remains C++20.
target_compile_features(presentmon_presentdata PRIVATE cxx_std_23)
target_include_directories(presentmon_presentdata PUBLIC
    ${CMAKE_SOURCE_DIR}/deps/presentmon
    ${CMAKE_SOURCE_DIR}/deps/presentmon/PresentData
)
target_compile_definitions(presentmon_presentdata PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0601
    NTDDI_VERSION=0x06010000
    PMLOG_BUILD_LEVEL=None
)
if(MSVC)
    set_property(TARGET presentmon_presentdata PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(presentmon_presentdata PRIVATE
        /W4
        /utf-8
        /wd4100
        /wd4201
        /wd4244
        $<$<CONFIG:Debug>:/Zi /Od /RTC1>
        $<$<CONFIG:Release>:/O2 /GL /GS->
    )
endif()

# ── ImGui: DO NOT compile — ReShade provides its own ImGui instance.
# We only use the headers for function declarations. ReShade resolves
# the actual ImGui symbols at runtime through its addon API.
# Compiling our own copy creates duplicate symbols that crash on overlay open.
#set(IMGUI_SOURCES
#    deps/reshade/deps/imgui/imgui.cpp
#    deps/reshade/deps/imgui/imgui_draw.cpp
#    deps/reshade/deps/imgui/imgui_tables.cpp
#    deps/reshade/deps/imgui/imgui_widgets.cpp
#)

# ── MinHook as separate static library (matches working example) ──
add_library(minhook_static STATIC ${MINHOOK_SOURCES})
target_compile_definitions(minhook_static PRIVATE MH_STATIC=1)
target_include_directories(minhook_static PRIVATE
    ${CMAKE_SOURCE_DIR}/deps/reshade/deps/minhook/include)
if(MSVC)
    set_property(TARGET minhook_static PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ── DLL target ──
add_library(relimiter SHARED
    ${ADDON_SOURCES}
    src/version.rc
)

# ── Output as .addon32 or .addon64 ──
set_target_properties(relimiter PROPERTIES
    SUFFIX "${ADDON_SUFFIX}"
    PREFIX ""
)

# ── Include paths ──
target_include_directories(relimiter PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${RELIMITER_GENERATED_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/deps/reshade/include
    ${CMAKE_SOURCE_DIR}/deps/reshade/deps/imgui
    ${CMAKE_SOURCE_DIR}/deps/reshade/deps/minhook/include
    ${CMAKE_SOURCE_DIR}/deps/nvapi
    ${CMAKE_SOURCE_DIR}/deps/vulkan-headers/include
)

# ── Preprocessor ──
target_compile_definitions(relimiter PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    IMGUI_DISABLE_DEMO_WINDOWS
    _CRT_SECURE_NO_WARNINGS
    VK_NO_PROTOTYPES
    $<$<NOT:$<BOOL:${RELIMITER_64BIT}>>:RELIMITER_32BIT>
)

if(RELIMITER_ENABLE_MCP)
    target_compile_definitions(relimiter PRIVATE RELIMITER_ENABLE_MCP=1)
endif()

# ImGui symbols resolved at runtime by ReShade — no force-include needed

# ── Compiler flags ──
if(MSVC)
    # Static CRT (matches working example — avoids CRT DLL dependency)
    set_property(TARGET relimiter PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # Common
    target_compile_options(relimiter PRIVATE /W4 /utf-8)

    # Suppress specific warnings that are noisy but harmless in this codebase
    target_compile_options(relimiter PRIVATE
        /wd4100  # unreferenced formal parameter (common in hook stubs)
        /wd4201  # nonstandard extension: nameless struct/union (NvAPI)
        /wd4244  # conversion warnings in MSVC STL internals
    )

    # Per-configuration
    target_compile_options(relimiter PRIVATE
        $<$<CONFIG:Debug>:/Zi /Od /RTC1>
        $<$<CONFIG:Release>:/O2 /GL /GS- /Zi>
    )

    target_link_options(relimiter PRIVATE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF /DEBUG>
    )
endif()

# ── Link libraries ──
if(RELIMITER_64BIT)
    target_link_libraries(relimiter PRIVATE
        minhook_static
        presentmon_presentdata
        advapi32.lib
        tdh.lib
        dxgi.lib
        d3d12.lib
        gdi32.lib
        winmm.lib
        ${CMAKE_SOURCE_DIR}/deps/nvapi/amd64/nvapi64.lib
    )
else()
    target_link_libraries(relimiter PRIVATE
        minhook_static
        presentmon_presentdata
        advapi32.lib
        tdh.lib
        dxgi.lib
        gdi32.lib
        winmm.lib
        ${CMAKE_SOURCE_DIR}/deps/nvapi/x86/nvapi.lib
    )
endif()

# ── Optional read-only MCP stdio bridge ──
if(RELIMITER_ENABLE_MCP)
    add_executable(relimiter_mcp_bridge
        tools/mcp_bridge/main.cpp
        tools/mcp_bridge/json.cpp
    )
    target_compile_features(relimiter_mcp_bridge PRIVATE cxx_std_20)
    target_compile_definitions(relimiter_mcp_bridge PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
    target_include_directories(relimiter_mcp_bridge PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tools/mcp_bridge
        ${RELIMITER_GENERATED_INCLUDE_DIR}
    )
    set_target_properties(relimiter_mcp_bridge PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    if(MSVC)
        set_property(TARGET relimiter_mcp_bridge PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(relimiter_mcp_bridge PRIVATE /W4 /utf-8 /permissive-)
    endif()
endif()

# ── MinHook C files: handled by minhook_static target ──

# ── Tests (standalone, no DLL dependency) ──
add_subdirectory(tests)
