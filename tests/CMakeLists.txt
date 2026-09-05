cmake_minimum_required(VERSION 3.20)

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

## ── DMFG output cap property tests ──
# Tests cadence formula, EMA convergence, config clamping, wake target.

add_executable(test_dmfg_output_cap test_dmfg_output_cap.cpp)
target_compile_features(test_dmfg_output_cap PRIVATE cxx_std_17)

if(MSVC)
    set_property(TARGET test_dmfg_output_cap PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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

## ── Delayed Present association tests ──
# Directly tests the platform-independent production matcher and retention.
# This does not validate PresentMon event production or live hook timestamps.

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

## ── Arrival-lead Estimator A tests ──
# Directly tests the platform-independent production estimator, bounded
# observation handoff, and exact-key deadline registry. Runtime hook ordering
# and pacing quality still require live capture validation.

add_executable(test_arrival_lead_estimator
    test_arrival_lead_estimator.cpp
    ${CMAKE_SOURCE_DIR}/src/arrival_lead_estimator.cpp
)
set_target_properties(test_arrival_lead_estimator PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(test_arrival_lead_estimator PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
    set_property(TARGET test_arrival_lead_estimator PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_arrival_lead_estimator PRIVATE /W4 /utf-8 /permissive-)
endif()

## ── Completed frame-time adaptation tests ──
# Directly tests the production Reflex-independent interval controller. Runtime
# marker ordering and pacing quality still require live capture validation.

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

## ── Adaptive deadline-reserve tests ──
# Directly tests exact-key release/gate joins, reserve reconstruction, bounded
# p99 tracking, warm-up, generation rejection, and reset behavior. The model is
# authority policy; live pacing still requires capture validation.

add_executable(test_adaptive_deadline_reserve
    test_adaptive_deadline_reserve.cpp
    ${CMAKE_SOURCE_DIR}/src/adaptive_deadline_reserve.cpp
    ${CMAKE_SOURCE_DIR}/src/frame_timeline.cpp
    ${CMAKE_SOURCE_DIR}/src/arrival_lead_estimator.cpp
)
set_target_properties(test_adaptive_deadline_reserve PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(test_adaptive_deadline_reserve PRIVATE
    ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
    set_property(TARGET test_adaptive_deadline_reserve PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_adaptive_deadline_reserve PRIVATE
        /W4 /utf-8 /permissive-)
endif()

add_executable(test_addon_exports test_addon_exports.cpp)
target_compile_features(test_addon_exports PRIVATE cxx_std_17)
add_dependencies(test_addon_exports relimiter)
if(MSVC)
    set_property(TARGET test_addon_exports PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_addon_exports PRIVATE /W4 /utf-8 /permissive-)
endif()

if(RELIMITER_ENABLE_MCP)
    add_executable(test_live_debug
        test_live_debug.cpp
        ${CMAKE_SOURCE_DIR}/tools/mcp_bridge/json.cpp
    )
    set_target_properties(test_live_debug PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
    target_include_directories(test_live_debug PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tools/mcp_bridge
    )
    if(MSVC)
        set_property(TARGET test_live_debug PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(test_live_debug PRIVATE /W4 /utf-8 /permissive-)
    endif()
endif()

add_executable(test_hook_regressions
    test_hook_regressions.cpp
    ${CMAKE_SOURCE_DIR}/src/hooks.cpp)
target_include_directories(test_hook_regressions PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/deps/reshade/deps/minhook/include)
target_compile_definitions(test_hook_regressions PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(test_shutdown_lifecycle test_shutdown_lifecycle.cpp)
add_executable(test_oled_care test_oled_care.cpp)
add_executable(test_osd_position test_osd_position.cpp)
add_executable(test_swapchain_selection_policy test_swapchain_selection_policy.cpp)
add_executable(test_pacing_sleep
    test_pacing_sleep.cpp
    ${CMAKE_SOURCE_DIR}/src/sleep.cpp
    ${CMAKE_SOURCE_DIR}/src/wake_guard.cpp)
target_compile_definitions(test_pacing_sleep PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN)
foreach(regression_target IN ITEMS test_hook_regressions
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
add_test(NAME oled_care COMMAND test_oled_care)
add_test(NAME osd_position COMMAND test_osd_position)
add_test(NAME hook_regressions COMMAND test_hook_regressions)
add_test(NAME shutdown_lifecycle COMMAND test_shutdown_lifecycle)
add_test(NAME swapchain_selection_policy COMMAND test_swapchain_selection_policy)
add_test(NAME pacing_sleep COMMAND test_pacing_sleep)
add_test(NAME addon_exports
    COMMAND test_addon_exports $<TARGET_FILE:relimiter>)
add_test(NAME dmfg_detection_properties COMMAND test_dmfg_detection)
add_test(NAME scheduler_passthrough COMMAND test_scheduler_passthrough)
add_test(NAME scheduler_safety COMMAND test_scheduler_safety)
add_test(NAME nvapi_marker_policy COMMAND test_nvapi_marker_policy)
add_test(NAME dmfg_output_cap_properties COMMAND test_dmfg_output_cap)
add_test(NAME frame_timeline_foundation COMMAND test_frame_timeline)
add_test(NAME present_association COMMAND test_present_association)
add_test(NAME arrival_lead_estimator COMMAND test_arrival_lead_estimator)
add_test(NAME frame_time_adaptation COMMAND test_frame_time_adaptation)
add_test(NAME adaptive_deadline_reserve COMMAND test_adaptive_deadline_reserve)
if(RELIMITER_ENABLE_MCP)
    add_test(NAME live_debug_protocol_and_analysis COMMAND test_live_debug)
    add_test(NAME mcp_bridge_dispatch COMMAND relimiter_mcp_bridge --self-test)
endif()
