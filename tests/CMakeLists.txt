cmake_minimum_required(VERSION 3.20)

# ── DMFG detection property tests ──
# Standalone console executables — no Windows API dependencies.

add_executable(test_dmfg_detection test_dmfg_detection.cpp)
target_compile_features(test_dmfg_detection PRIVATE cxx_std_17)

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

## ── DMFG output cap property tests ──
# Tests cadence formula, EMA convergence, config clamping, wake target.

add_executable(test_dmfg_output_cap test_dmfg_output_cap.cpp)
target_compile_features(test_dmfg_output_cap PRIVATE cxx_std_17)

if(MSVC)
    set_property(TARGET test_dmfg_output_cap PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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

enable_testing()
add_test(NAME dmfg_detection_properties COMMAND test_dmfg_detection)
add_test(NAME scheduler_passthrough COMMAND test_scheduler_passthrough)
add_test(NAME dmfg_output_cap_properties COMMAND test_dmfg_output_cap)
if(RELIMITER_ENABLE_MCP)
    add_test(NAME live_debug_protocol_and_analysis COMMAND test_live_debug)
    add_test(NAME mcp_bridge_dispatch COMMAND relimiter_mcp_bridge --self-test)
endif()
