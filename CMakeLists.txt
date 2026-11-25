cmake_minimum_required(VERSION 3.20)
project(relimiter LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
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
    src/native_pcl_snapshot_policy.h
    src/scheduler_cadence.h
    src/osd_cadence_policy.h
    src/oled_care_policy.h
    src/osd_position_policy.h
    src/capture_lifecycle.h
    src/shutdown_admission.h
    src/reflex_adaptation_queue.h
    src/pacing_sleep.h
    src/swapchain_selection_policy.h
    src/background_tasks.cpp
    src/frame_time_adaptation.cpp
    src/dllmain.cpp
    src/config.cpp
    src/correlator.cpp
    src/display_poll_thread.cpp
    src/display_resolver.cpp
    src/display_state.cpp
    src/display_worker.cpp
    src/enforcement_dispatcher.cpp
    src/feedback.cpp
    src/feedback_device_identity.cpp
    src/flip_model.cpp
    src/flush.cpp
    src/frame_latency_controller.cpp
    src/frame_timeline.cpp
    src/health.cpp
    src/hooks.cpp
    src/hw_monitor.cpp
    src/hw_spin.cpp
    src/loadlib_hooks.cpp
    src/module_presence.h
    src/module_presence.cpp
    src/marker_log.cpp
    src/marker_stamps.h
    src/marker_stamps.cpp
    src/marker_cycles.h
    src/marker_cycles.cpp
    src/marker_rate.h
    src/marker_rate.cpp
    src/native_pcl_measurement.cpp
    src/nvapi_hooks.cpp
    src/osd.cpp
    src/pll.cpp
    src/runtime_lifecycle.cpp
    src/scheduler.cpp
    src/sleep.cpp
    src/sleep_mode.cpp
    src/swapchain_manager.cpp
    src/system_hardening.cpp
    src/tsc_cal.cpp
    src/vblank_thread.cpp
    src/wake_guard.cpp
    src/logger.cpp
    src/startup_breadcrumb.h
    src/startup_breadcrumb.cpp
    src/hang_progress.h
    src/lock_owner_diag.h
    src/hang_report.h
    src/hang_report.cpp
    src/hang_report_policy.h
    src/hang_report_policy.cpp
    src/csv_writer.cpp
    src/pacing_sample_sources.h
    src/presentation_gate.cpp
    src/native_present_gate.cpp
    src/native_present_gate_runtime.cpp
    src/gpu_readiness.cpp
    src/gpu_present_types.cpp
    src/gpu_present_gate.cpp
    src/control_file.cpp
    src/control_file.h
    src/nvapi_marker_census.h
    src/present_association.cpp
    src/vsync_control.cpp
    src/streamline_base_present.h
    src/streamline_base_present.cpp
    src/vtable_copy.h
    src/vtable_copy.cpp
    src/vk_enforce.cpp
    src/blackout.cpp
    src/hotkey_edge.h
    src/hotkey_poller.h
    src/hotkey_poller.cpp
    src/focus_lock.cpp
    src/pacing_telemetry_types.h
    src/pacing_core_types.h
    src/cadence_grid.h
    src/cadence_grid.cpp
    src/unified_pacer.h
    src/unified_pacer.cpp
    src/unified_queue_law.h
    src/unified_queue_law.cpp
    src/gpu_queue_law_feed.h
    src/gpu_queue_law_feed.cpp
    src/cadence_grid_publication.cpp
    src/lead_controller.h
    src/lead_controller.cpp
    src/gpu_queue_probe_core.h
    src/gpu_queue_probe_core.cpp
    src/gpu_queue_dependency.h
    src/gpu_queue_dependency.cpp
    src/gpu_queue_borrow.h
    src/gpu_queue_borrow.cpp
    src/period_governor.h
    src/period_governor.cpp
    src/source_placement.h
    src/render_handoff.h
    src/render_handoff.cpp
    src/rung_feasibility.h
    src/rung_feasibility.cpp
    src/pacing_resolver.h
    src/pacing_resolver.cpp
    src/frame_plan.h
    src/frame_plan.cpp
    src/source_stage.h
    src/source_stage.cpp
    src/pacing_telemetry.h
    src/pacing_telemetry.cpp
    src/pacing_metrics.h
    src/pacing_metrics.cpp
    src/pacing_join.h
    src/pacing_join.cpp
    src/dxgi_display_source.h
    src/dxgi_display_source.cpp
    src/reflex_clock_map.h
    src/reflex_clock_map.cpp
    src/native_pcl_loss_window.h
)

# ── 64-bit only sources ──
if(RELIMITER_64BIT)
    list(APPEND ADDON_SOURCES
        src/fg_divisor.cpp
        src/frame_splitting.cpp
        src/ngx_hooks.cpp
        src/streamline_hooks.cpp
        src/streamline_discovery.cpp
        src/streamline_discovery.h
        src/streamline_com_evidence.h
        src/streamline_factory_probe.cpp
        src/streamline_factory_probe.h
        src/streamline_proxy_search.cpp
        src/streamline_proxy_search.h
        src/streamline_proxy_probe.cpp
        src/streamline_proxy_probe.h
        src/streamline_queue_hooks.cpp
        src/streamline_queue_hooks.h
        src/streamline_plugin.h
        src/streamline_gpu_handoff.cpp
        src/streamline_gpu_handoff.h
        src/fg_input_observer.cpp
        src/fg_queue_observer.cpp
        src/pcl_hooks.cpp
        src/reflex_inject.cpp
        src/dlss_presets.cpp
        src/gpu_readiness_d3d12.cpp
        src/gpu_readiness_d3d12_queue.cpp
        src/gpu_present_gate_d3d12.cpp
        src/gpu_queue_probe_d3d12.cpp
        src/fg_present_order.cpp
        src/fg_present_order.h
        src/gpu_hold_order.cpp
        src/gpu_hold_order.h
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
        $<$<CONFIG:Release>:/O2 /GL /GS>
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
    src/fg_state_policy.h
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
    $<$<BOOL:${RELIMITER_64BIT}>:RELIMITER_FG_PRESENT_ORDER>
    $<$<BOOL:${RELIMITER_64BIT}>:RELIMITER_GPU_HOLD_ORDER>
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
        $<$<CONFIG:Release>:/O2 /GL /GS /Zi>
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
        Synchronization.lib
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
        Synchronization.lib
        ${CMAKE_SOURCE_DIR}/deps/nvapi/x86/nvapi.lib
    )
endif()

# ── MinHook C files: handled by minhook_static target ──

# ── Tests (standalone, no DLL dependency) ──
add_subdirectory(tests)
