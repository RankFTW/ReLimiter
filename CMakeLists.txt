cmake_minimum_required(VERSION 3.20)
project(relimiter LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Output ──
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Sources ──
# Full source list with minimal dllmain to test static init freeze
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
    src/fg_divisor.cpp
    src/flip_metering.cpp
    src/flip_model.cpp
    src/flush.cpp
    src/frame_latency_controller.cpp
    src/frame_splitting.cpp
    src/health.cpp
    src/hooks.cpp
    src/hw_spin.cpp
    src/loadlib_hooks.cpp
    src/marker_log.cpp
    src/nvapi_hooks.cpp
    src/osd.cpp
    src/pll.cpp
    src/predictor.cpp
    src/regime_detector.cpp
    src/scheduler.cpp
    src/sleep.cpp
    src/sleep_mode.cpp
    src/streamline_hooks.cpp
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
    src/vk_enforce.cpp
    src/pcl_hooks.cpp
    src/presentation_gate.cpp
    src/reflex_inject.cpp
    src/vsync_control.cpp
)

# ── MinHook (compiled as part of the addon) ──
set(MINHOOK_SOURCES
    deps/reshade/deps/minhook/src/buffer.c
    deps/reshade/deps/minhook/src/hook.c
    deps/reshade/deps/minhook/src/trampoline.c
    deps/reshade/deps/minhook/src/hde/hde32.c
    deps/reshade/deps/minhook/src/hde/hde64.c
)

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
)

# ── Output as .addon ──
set_target_properties(relimiter PROPERTIES
    SUFFIX ".addon64"
    PREFIX ""
)

# ── Include paths ──
target_include_directories(relimiter PRIVATE
    ${CMAKE_SOURCE_DIR}/src
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
)

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
        $<$<CONFIG:Release>:/O2 /GL /GS->
    )

    target_link_options(relimiter PRIVATE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
    )
endif()

# ── Link libraries ──
target_link_libraries(relimiter PRIVATE
    minhook_static
    dxgi.lib
    d3d12.lib
    gdi32.lib
    winmm.lib
)

# ── MinHook C files: handled by minhook_static target ──
