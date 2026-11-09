cmake_minimum_required(VERSION 3.20)

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(test_streamline_gpu_handoff test_streamline_gpu_handoff.cpp ${CMAKE_SOURCE_DIR}/src/streamline_proxy_search.cpp)
    set_target_properties(test_streamline_gpu_handoff PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
    target_include_directories(test_streamline_gpu_handoff PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/deps/reshade/deps/minhook/include ${CMAKE_SOURCE_DIR}/deps/reshade/include)
    target_compile_definitions(test_streamline_gpu_handoff PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_property(TARGET test_streamline_gpu_handoff PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_test(NAME streamline_gpu_handoff COMMAND test_streamline_gpu_handoff)
    add_executable(test_streamline_proxy_search test_streamline_proxy_search.cpp)
    set_target_properties(test_streamline_proxy_search PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
    target_include_directories(test_streamline_proxy_search PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(test_streamline_proxy_search PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_property(TARGET test_streamline_proxy_search PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_test(NAME streamline_proxy_search COMMAND test_streamline_proxy_search)
    add_executable(test_streamline_discovery test_streamline_discovery.cpp)
    set_target_properties(test_streamline_discovery PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
    target_include_directories(test_streamline_discovery PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/deps/reshade/deps/minhook/include)
    target_compile_definitions(test_streamline_discovery PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_property(TARGET test_streamline_discovery PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_test(NAME streamline_discovery COMMAND test_streamline_discovery)
    add_executable(test_fg_input_observer test_fg_input_observer.cpp)
    target_compile_features(test_fg_input_observer PRIVATE cxx_std_17)
    target_include_directories(test_fg_input_observer PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/deps/reshade/deps/minhook/include)
    target_compile_definitions(test_fg_input_observer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(test_fg_input_observer PRIVATE advapi32)
    set_property(TARGET test_fg_input_observer PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_test(NAME fg_input_observer COMMAND test_fg_input_observer)
endif()


# ── DMFG detection property tests ──
# Standalone console executables — no Windows API dependencies.

add_executable(test_dmfg_detection test_dmfg_detection.cpp)
target_compile_features(test_dmfg_detection PRIVATE cxx_std_17)
target_include_directories(test_dmfg_detection PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
    set_property(TARGET test_dmfg_detection PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

## ── Scheduler passthrough unit tests ──
# Tests the passthrough gating logic as pure functions.

add_executable(test_scheduler_passthrough test_scheduler_passthrough.cpp)
target_compile_features(test_scheduler_passthrough PRIVATE cxx_std_17)

if(MSVC)
    set_property(TARGET test_scheduler_passthrough PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

## ── Scheduler ownership and fixed MarkerLog safety tests ──
# Directly tests the production allocation-free marker table and header-only
# nonblocking owner guard. Hook placement still requires live verification.

add_executable(test_scheduler_safety
    test_scheduler_safety.cpp
    ${CMAKE_SOURCE_DIR}/src/marker_log.cpp
)
target_compile_features(test_scheduler_safety PRIVATE cxx_std_17)
target_include_directories(test_scheduler_safety PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
    set_property(TARGET test_scheduler_safety PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_scheduler_safety PRIVATE /W4 /utf-8 /permissive-)
endif()

## ── NvAPI marker-source policy tests ──
# Directly tests the production RTSS/direct marker admission policy. Runtime
# caller attribution and hook ordering still require live verification.

add_executable(test_nvapi_marker_policy test_nvapi_marker_policy.cpp)
target_compile_features(test_nvapi_marker_policy PRIVATE cxx_std_17)
target_include_directories(test_nvapi_marker_policy PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
    set_property(TARGET test_nvapi_marker_policy PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_nvapi_marker_policy PRIVATE /W4 /utf-8 /permissive-)
endif()

## ── Canonical frame timeline foundation tests ──
# Directly tests platform-independent production identity and queue code.
# This does not validate hook capture points or runtime instrumentation cost.

add_executable(test_frame_timeline
    test_frame_timeline.cpp
    ${CMAKE_SOURCE_DIR}/src/frame_timeline.cpp
)
target_compile_features(test_frame_timeline PRIVATE cxx_std_17)
target_include_directories(test_frame_timeline PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
    set_property(TARGET test_frame_timeline PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_frame_timeline PRIVATE /W4 /utf-8 /permissive-)
endif()

## ── Present classification tests ──
# Directly tests the production real/generated classification of PresentMon
# observations. This does not validate PresentMon event production.

add_executable(test_present_association
    test_present_association.cpp
    ${CMAKE_SOURCE_DIR}/src/present_association.cpp
)
target_compile_features(test_present_association PRIVATE cxx_std_17)
target_include_directories(test_present_association PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
    set_property(TARGET test_present_association PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_present_association PRIVATE /W4 /utf-8 /permissive-)
endif()

## ── Stage-demand tests (historical target name frame_time_adaptation) ──
# Directly tests the production stage-demand computation, Reflex report
# admission and the delayed-report queue that feed the period governor.
# Runtime marker ordering and pacing quality still require live capture validation.

add_executable(test_frame_time_adaptation
    test_frame_time_adaptation.cpp
    ${CMAKE_SOURCE_DIR}/src/frame_time_adaptation.cpp
)
set_target_properties(test_frame_time_adaptation PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(test_frame_time_adaptation PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
    set_property(TARGET test_frame_time_adaptation PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_frame_time_adaptation PRIVATE /W4 /utf-8 /permissive-)
endif()

## ── Frame-plan registry and stage-event queue tests ──
# Directly tests the production exact-key plan registry, output claims and the
# bounded stage-event queue. Executor hook placement needs live validation.

add_executable(test_frame_plan
    test_frame_plan.cpp
    ${CMAKE_SOURCE_DIR}/src/frame_plan.cpp
)
set_target_properties(test_frame_plan PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(test_frame_plan PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
    set_property(TARGET test_frame_plan PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_frame_plan PRIVATE /W4 /utf-8 /permissive-)
endif()

## ── Developer control file parser tests ──
# The command-file parser only (header); the poller and the present-callback
# execution need a live game.

add_executable(test_control_file test_control_file.cpp)
set_target_properties(test_control_file PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(test_control_file PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
    set_property(TARGET test_control_file PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_control_file PRIVATE /W4 /utf-8 /permissive-)
endif()

## ── Pacing pipeline resolver tests ──
# Directly tests the production resolver rules, generation publication and
# capability tags. Owner and executor wiring needs live validation.

add_executable(test_pacing_resolver
    test_pacing_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/pacing_resolver.cpp
)
set_target_properties(test_pacing_resolver PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(test_pacing_resolver PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
    set_property(TARGET test_pacing_resolver PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_pacing_resolver PRIVATE /W4 /utf-8 /permissive-)
endif()

## ── Cadence grid tests ──
# Directly tests the production grid segments, epochs, fixed-refresh snapping
# and seqlock publication. Executor wiring and pacing need live validation.

add_executable(test_cadence_grid
    test_cadence_grid.cpp
    ${CMAKE_SOURCE_DIR}/src/cadence_grid.cpp
    ${CMAKE_SOURCE_DIR}/src/cadence_grid_publication.cpp
)
set_target_properties(test_cadence_grid PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(test_cadence_grid PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
    set_property(TARGET test_cadence_grid PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_cadence_grid PRIVATE /W4 /utf-8 /permissive-)
endif()

## ── Lead controller C1 tests ──
# Directly tests the production sample formulas, pending-frame assembly,
# histogram quantile and lead slew. Event producers and pacing need live validation.

add_executable(test_lead_controller
    test_lead_controller.cpp
    ${CMAKE_SOURCE_DIR}/src/lead_controller.cpp
    ${CMAKE_SOURCE_DIR}/src/pacing_metrics.cpp
)
set_target_properties(test_lead_controller PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(test_lead_controller PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
    set_property(TARGET test_lead_controller PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_lead_controller PRIVATE /W4 /utf-8 /permissive-)
endif()

## ── Period governor C2 and rung feasibility C3 tests ──
# Directly test the production period tracker (rise, miss bump, hold, fall,
# probe backoff, fixed-refresh quantization; C2) and demotion, dwell doubling
# and override immunity (C3).
# Scheduler wiring and pacing need live validation.

add_executable(test_period_governor
    test_period_governor.cpp
    ${CMAKE_SOURCE_DIR}/src/period_governor.cpp
    ${CMAKE_SOURCE_DIR}/src/pacing_metrics.cpp
)
# The retry test drives the production lead controller's slack window.
add_executable(test_rung_feasibility
    test_rung_feasibility.cpp
    ${CMAKE_SOURCE_DIR}/src/rung_feasibility.cpp
    ${CMAKE_SOURCE_DIR}/src/lead_controller.cpp
    ${CMAKE_SOURCE_DIR}/src/pacing_metrics.cpp
)
# Governor-step and rung-feasibility event derivation of the telemetry worker.
# The worker thread and CSV rows need live capture validation.
add_executable(test_pacing_controller_events
    test_pacing_controller_events.cpp
    ${CMAKE_SOURCE_DIR}/src/pacing_controller_events.cpp
)
# Pre-Sleep placement: margin, measured
# selection and guards. The Sleep hook, the owner guard and the driver's Sleep
# behavior need live validation.
add_executable(test_source_placement
    test_source_placement.cpp
    ${CMAKE_SOURCE_DIR}/src/source_placement.cpp
    ${CMAKE_SOURCE_DIR}/src/pacing_metrics.cpp
)
foreach(controller_target IN ITEMS test_period_governor test_rung_feasibility
    test_pacing_controller_events test_source_placement)
    set_target_properties(${controller_target} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
    target_include_directories(${controller_target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    if(MSVC)
        set_property(TARGET ${controller_target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(${controller_target} PRIVATE /W4 /utf-8 /permissive-)
    endif()
endforeach()

## ── Source stage tests ──
# Directly tests the production planning core (plan before the wait, lead
# feed, record fields, epochs, stall injector) with a scripted clock and the
# production grid, plans and lead. OnMarker wiring and pacing need live validation.

add_executable(test_source_stage
    test_source_stage.cpp
    ${CMAKE_SOURCE_DIR}/src/source_stage.cpp
    ${CMAKE_SOURCE_DIR}/src/cadence_grid.cpp
    ${CMAKE_SOURCE_DIR}/src/cadence_grid_publication.cpp
    ${CMAKE_SOURCE_DIR}/src/frame_plan.cpp
    ${CMAKE_SOURCE_DIR}/src/lead_controller.cpp
    ${CMAKE_SOURCE_DIR}/src/pacing_metrics.cpp
    ${CMAKE_SOURCE_DIR}/src/pacing_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/pll.cpp
)
set_target_properties(test_source_stage PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(test_source_stage PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
    set_property(TARGET test_source_stage PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_source_stage PRIVATE /W4 /utf-8 /permissive-)
endif()

## ── DXGI display source and display-metric PQI tests ──
# Directly test the production present-count matcher with its join and chain
# contract, and PQI_FromSnapshot golden vectors. The Present hook, correlator
# polling and live DXGI statistics need live validation.

add_executable(test_dxgi_display_source test_dxgi_display_source.cpp
    ${CMAKE_SOURCE_DIR}/src/dxgi_display_source.cpp
    ${CMAKE_SOURCE_DIR}/src/pacing_join.cpp
    ${CMAKE_SOURCE_DIR}/src/pacing_metrics.cpp)
add_executable(test_pqi test_pqi.cpp ${CMAKE_SOURCE_DIR}/src/pqi.cpp)
foreach(display_target IN ITEMS test_dxgi_display_source test_pqi)
    set_target_properties(${display_target} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_include_directories(${display_target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    if(MSVC)
        set_property(TARGET ${display_target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(${display_target} PRIVATE /W4 /utf-8 /permissive-)
    endif()
endforeach()

add_executable(test_addon_exports test_addon_exports.cpp)
target_compile_features(test_addon_exports PRIVATE cxx_std_17)
add_dependencies(test_addon_exports relimiter)
if(MSVC)
    set_property(TARGET test_addon_exports PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_addon_exports PRIVATE /W4 /utf-8 /permissive-)
endif()

add_executable(test_hook_regressions
    test_hook_regressions.cpp
    ${CMAKE_SOURCE_DIR}/src/gpu_present_gate.cpp
    ${CMAKE_SOURCE_DIR}/src/gpu_present_types.cpp
    ${CMAKE_SOURCE_DIR}/src/native_present_gate.cpp
    ${CMAKE_SOURCE_DIR}/src/pacing_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/hooks.cpp)
target_include_directories(test_hook_regressions PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/deps/reshade/include
    ${CMAKE_SOURCE_DIR}/deps/reshade/deps/minhook/include)
target_compile_definitions(test_hook_regressions PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(test_shutdown_lifecycle test_shutdown_lifecycle.cpp)
add_executable(test_native_pcl_snapshot test_native_pcl_snapshot.cpp)
add_executable(test_config_async test_config_async.cpp
    ${CMAKE_SOURCE_DIR}/src/pacing_resolver.cpp)
add_executable(test_cross_thread_slot test_cross_thread_slot.cpp)
add_executable(test_native_present_gate test_native_present_gate.cpp
    ${CMAKE_SOURCE_DIR}/src/native_present_gate.cpp
    ${CMAKE_SOURCE_DIR}/src/frame_timeline.cpp)
add_executable(test_gpu_present_gate test_gpu_present_gate.cpp
    ${CMAKE_SOURCE_DIR}/src/gpu_present_gate.cpp ${CMAKE_SOURCE_DIR}/src/gpu_present_types.cpp
    ${CMAKE_SOURCE_DIR}/src/gpu_readiness.cpp
    ${CMAKE_SOURCE_DIR}/src/frame_timeline.cpp ${CMAKE_SOURCE_DIR}/src/wake_guard.cpp
    ${CMAKE_SOURCE_DIR}/src/pacing_resolver.cpp ${CMAKE_SOURCE_DIR}/src/frame_plan.cpp
    ${CMAKE_SOURCE_DIR}/src/cadence_grid.cpp ${CMAKE_SOURCE_DIR}/src/cadence_grid_publication.cpp)
target_include_directories(test_gpu_present_gate PRIVATE ${CMAKE_SOURCE_DIR}/deps/reshade/include)
target_compile_definitions(test_gpu_present_gate PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_sources(test_gpu_present_gate PRIVATE ${CMAKE_SOURCE_DIR}/src/gpu_readiness_d3d12_queue.cpp
        ${CMAKE_SOURCE_DIR}/src/feedback_device_identity.cpp ${CMAKE_SOURCE_DIR}/src/render_handoff.cpp)
    target_link_libraries(test_gpu_present_gate PRIVATE Synchronization)
endif()
add_executable(test_gpu_readiness test_gpu_readiness.cpp
    ${CMAKE_SOURCE_DIR}/src/gpu_present_types.cpp
    ${CMAKE_SOURCE_DIR}/src/gpu_readiness.cpp
    ${CMAKE_SOURCE_DIR}/src/frame_timeline.cpp)
target_include_directories(test_gpu_readiness PRIVATE ${CMAKE_SOURCE_DIR}/deps/reshade/include)
target_compile_definitions(test_gpu_readiness PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(RELIMITER_64BIT)
    target_sources(test_gpu_readiness PRIVATE ${CMAKE_SOURCE_DIR}/src/gpu_readiness_d3d12_queue.cpp
        ${CMAKE_SOURCE_DIR}/src/frame_plan.cpp)
    add_executable(test_gpu_readiness_warp test_gpu_readiness_warp.cpp
        ${CMAKE_SOURCE_DIR}/src/gpu_readiness.cpp
        ${CMAKE_SOURCE_DIR}/src/gpu_readiness_d3d12_queue.cpp)
    set_target_properties(test_gpu_readiness_warp PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_include_directories(test_gpu_readiness_warp PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(test_gpu_readiness_warp PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(test_gpu_readiness_warp PRIVATE d3d12 dxgi)
    if(MSVC)
        set_property(TARGET test_gpu_readiness_warp PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(test_gpu_readiness_warp PRIVATE /W4 /utf-8 /permissive-)
    endif()
endif()
target_compile_definitions(test_config_async PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_executable(test_scheduler_cadence test_scheduler_cadence.cpp)
add_executable(test_osd_cadence test_osd_cadence.cpp)
add_executable(test_feedback_device_identity
    test_feedback_device_identity.cpp
    ${CMAKE_SOURCE_DIR}/src/native_present_gate.cpp
    ${CMAKE_SOURCE_DIR}/src/feedback_device_identity.cpp
    ${CMAKE_SOURCE_DIR}/src/frame_timeline.cpp
    ${CMAKE_SOURCE_DIR}/src/frame_plan.cpp
    ${CMAKE_SOURCE_DIR}/src/cadence_grid.cpp
    ${CMAKE_SOURCE_DIR}/src/cadence_grid_publication.cpp
    ${CMAKE_SOURCE_DIR}/src/pacing_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/render_handoff.cpp)
target_link_libraries(test_feedback_device_identity PRIVATE Synchronization)
target_include_directories(test_feedback_device_identity PRIVATE
    ${CMAKE_SOURCE_DIR}/deps/reshade/include
    ${CMAKE_SOURCE_DIR}/deps/reshade/deps/minhook/include)
target_compile_definitions(test_feedback_device_identity PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN)
add_executable(test_oled_care test_oled_care.cpp)
add_executable(test_osd_position test_osd_position.cpp)
add_executable(test_swapchain_selection_policy test_swapchain_selection_policy.cpp)
add_executable(test_pacing_sleep
    test_pacing_sleep.cpp
    ${CMAKE_SOURCE_DIR}/src/sleep.cpp
    ${CMAKE_SOURCE_DIR}/src/wake_guard.cpp)
target_compile_definitions(test_pacing_sleep PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN)
# Pacing telemetry
add_executable(test_pacing_metrics test_pacing_metrics.cpp
    ${CMAKE_SOURCE_DIR}/src/pacing_metrics.cpp)
add_executable(test_pacing_join test_pacing_join.cpp
    ${CMAKE_SOURCE_DIR}/src/pacing_join.cpp
    ${CMAKE_SOURCE_DIR}/src/pacing_metrics.cpp)
add_executable(test_reflex_clock_map test_reflex_clock_map.cpp
    ${CMAKE_SOURCE_DIR}/src/reflex_clock_map.cpp)
add_executable(test_native_pcl_loss_window test_native_pcl_loss_window.cpp)
add_executable(test_native_pcl_display_latency test_native_pcl_display_latency.cpp)
set_target_properties(test_native_pcl_display_latency PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_include_directories(test_native_pcl_display_latency PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_native_pcl_display_latency PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    set_property(TARGET test_native_pcl_display_latency PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_native_pcl_display_latency PRIVATE /W4 /utf-8 /permissive-)
endif()
# Production CSV writer with stub logger and native PCL snapshot.
add_executable(test_pacing_csv test_pacing_csv.cpp
    ${CMAKE_SOURCE_DIR}/src/csv_writer.cpp
    ${CMAKE_SOURCE_DIR}/src/frame_timeline.cpp)
set_target_properties(test_pacing_csv PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_include_directories(test_pacing_csv PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_pacing_csv PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    set_property(TARGET test_pacing_csv PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_pacing_csv PRIVATE /W4 /utf-8 /permissive-)
endif()
foreach(regression_target IN ITEMS test_hook_regressions
    test_pacing_metrics test_pacing_join test_reflex_clock_map test_native_pcl_loss_window
    test_native_present_gate test_cross_thread_slot
    test_gpu_readiness
    test_gpu_present_gate
    test_native_pcl_snapshot
    test_config_async
    test_scheduler_cadence test_osd_cadence
    test_feedback_device_identity
    test_oled_care test_osd_position
        test_shutdown_lifecycle test_swapchain_selection_policy test_pacing_sleep)
    set_target_properties(${regression_target} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_include_directories(${regression_target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    if(MSVC)
        set_property(TARGET ${regression_target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(${regression_target} PRIVATE /W4 /utf-8 /permissive-)
    endif()
endforeach()

enable_testing()
add_test(NAME native_present_gate COMMAND test_native_present_gate)
add_test(NAME cross_thread_slot COMMAND test_cross_thread_slot)
add_test(NAME gpu_readiness COMMAND test_gpu_readiness)
add_test(NAME gpu_present_gate COMMAND test_gpu_present_gate)
set_tests_properties(gpu_present_gate PROPERTIES TIMEOUT 30)
if(RELIMITER_64BIT)
    add_test(NAME gpu_readiness_warp COMMAND test_gpu_readiness_warp)
    set_tests_properties(gpu_readiness_warp PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
endif()
add_test(NAME native_pcl_snapshot COMMAND test_native_pcl_snapshot)
add_test(NAME config_async COMMAND test_config_async)
set_tests_properties(config_async PROPERTIES TIMEOUT 30)
add_test(NAME scheduler_cadence COMMAND test_scheduler_cadence)
add_test(NAME osd_cadence COMMAND test_osd_cadence)
add_test(NAME feedback_device_identity COMMAND test_feedback_device_identity)
set_tests_properties(feedback_device_identity PROPERTIES TIMEOUT 30)
add_test(NAME oled_care COMMAND test_oled_care)
add_test(NAME osd_position COMMAND test_osd_position)
add_test(NAME hook_regressions COMMAND test_hook_regressions)
add_test(NAME shutdown_lifecycle COMMAND test_shutdown_lifecycle)
add_test(NAME swapchain_selection_policy COMMAND test_swapchain_selection_policy)
add_test(NAME pacing_sleep COMMAND test_pacing_sleep)
add_test(NAME pacing_metrics COMMAND test_pacing_metrics
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME pacing_join COMMAND test_pacing_join)
add_test(NAME reflex_clock_map COMMAND test_reflex_clock_map)
add_test(NAME native_pcl_loss_window COMMAND test_native_pcl_loss_window)
add_test(NAME native_pcl_display_latency COMMAND test_native_pcl_display_latency)
add_test(NAME pacing_csv COMMAND test_pacing_csv)
add_test(NAME addon_exports
    COMMAND test_addon_exports $<TARGET_FILE:relimiter>)
add_test(NAME dmfg_detection_properties COMMAND test_dmfg_detection)
add_test(NAME scheduler_passthrough COMMAND test_scheduler_passthrough)
add_test(NAME scheduler_safety COMMAND test_scheduler_safety)
add_test(NAME nvapi_marker_policy COMMAND test_nvapi_marker_policy)
add_test(NAME frame_timeline_foundation COMMAND test_frame_timeline)
add_test(NAME present_association COMMAND test_present_association)
add_test(NAME frame_time_adaptation COMMAND test_frame_time_adaptation)
add_test(NAME pacing_resolver COMMAND test_pacing_resolver)
add_test(NAME cadence_grid COMMAND test_cadence_grid)
set_tests_properties(cadence_grid PROPERTIES TIMEOUT 60)
add_test(NAME lead_controller COMMAND test_lead_controller)
add_test(NAME period_governor COMMAND test_period_governor)
add_test(NAME source_placement COMMAND test_source_placement)
add_test(NAME rung_feasibility COMMAND test_rung_feasibility)
add_test(NAME pacing_controller_events COMMAND test_pacing_controller_events)
add_test(NAME source_stage COMMAND test_source_stage)
add_test(NAME frame_plan COMMAND test_frame_plan)
set_tests_properties(frame_plan PROPERTIES TIMEOUT 60)
add_test(NAME control_file COMMAND test_control_file)
add_test(NAME dxgi_display_source COMMAND test_dxgi_display_source)
add_test(NAME pqi COMMAND test_pqi)
