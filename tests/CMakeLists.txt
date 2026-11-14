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

## ── Unified pacer tests ──
# Directly tests the production pure law (src/unified_pacer.cpp): interval
# composition, start target and no catch-up, pairs and staleness, hard limit,
# start resolution and the Sleep-mode tuple. Hooks and pacing need live validation.

add_executable(test_unified_pacer
    test_unified_pacer.cpp
    ${CMAKE_SOURCE_DIR}/src/unified_pacer.cpp
    ${CMAKE_SOURCE_DIR}/src/unified_queue_law.cpp
)
set_target_properties(test_unified_pacer PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(test_unified_pacer PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
    set_property(TARGET test_unified_pacer PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_unified_pacer PRIVATE /W4 /utf-8 /permissive-)
endif()

## ── Unified queue law tests ──
# Directly tests the production pure queue law (src/unified_queue_law.cpp) and the probe feed
# ring (src/gpu_queue_law_feed.cpp), and the law through the production unified pacer: samples
# from records, the window statistics, the fixed points of P, the fallback rule, the credit's
# rule switching and the key off identity. The probe's records on a live queue need live validation.

add_executable(test_unified_queue_law
    test_unified_queue_law.cpp
    ${CMAKE_SOURCE_DIR}/src/unified_pacer.cpp
    ${CMAKE_SOURCE_DIR}/src/unified_queue_law.cpp
    ${CMAKE_SOURCE_DIR}/src/gpu_queue_law_feed.cpp
)
set_target_properties(test_unified_queue_law PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

target_include_directories(test_unified_queue_law PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
    set_property(TARGET test_unified_queue_law PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_unified_queue_law PRIVATE /W4 /utf-8 /permissive-)
endif()

## ── Pacing test mode tests ──
# The developer key pacing_test_mode against the old-key arms it replaces: the resolver's pipeline
# decision and the Sleep-mode tuple over an enumeration of capabilities and runtime states, the named
# differences, and the pinned decision table. The scheduler's dispatch, the Sleep hook and the driver
# need live validation.

add_executable(test_pacing_test_mode
    test_pacing_test_mode.cpp
    ${CMAKE_SOURCE_DIR}/src/pacing_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/unified_pacer.cpp
    ${CMAKE_SOURCE_DIR}/src/unified_queue_law.cpp
)
set_target_properties(test_pacing_test_mode PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(test_pacing_test_mode PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
    set_property(TARGET test_pacing_test_mode PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_pacing_test_mode PRIVATE /W4 /utf-8 /permissive-)
endif()

## ── Enforcement route tests ──
# The dispatcher's route selection with the marker state scripted: marker routes are a 64-bit route, so a 32-bit
# process takes the present route whatever the marker state says. The hooks and the scheduler need live validation.

add_executable(test_enforcement_route
    test_enforcement_route.cpp
    ${CMAKE_SOURCE_DIR}/src/enforcement_dispatcher.cpp
)
set_target_properties(test_enforcement_route PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(test_enforcement_route PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/deps/reshade/include)
target_compile_definitions(test_enforcement_route PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

if(MSVC)
    set_property(TARGET test_enforcement_route PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_enforcement_route PRIVATE /W4 /utf-8 /permissive-)
endif()

## ── GPU queue probe core tests ──
# Directly tests the production accounting core of the developer probe
# gpu_queue_probe (src/gpu_queue_probe_core.cpp): empty / first-gap split, start stamps,
# the done chain, parked and out-of-order stamps, boundaries, incomplete frames, lateness
# bounds and the bounded wait-free ring. The D3D12 hook, fences and completion thread need
# live validation.

add_executable(test_gpu_queue_probe_core
    test_gpu_queue_probe_core.cpp
    ${CMAKE_SOURCE_DIR}/src/gpu_queue_probe_core.cpp
)
set_target_properties(test_gpu_queue_probe_core PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(test_gpu_queue_probe_core PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
    set_property(TARGET test_gpu_queue_probe_core PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_gpu_queue_probe_core PRIVATE /W4 /utf-8 /permissive-)
endif()

# The callback rundown (src/runtime_lifecycle.cpp, the production unit, no stubs): per-thread slots, the Dekker handshake
# with s_stopping, the exact examination of all counters, overflow, slot reuse and a guard left on another thread.
# `test_runtime_lifecycle --bench` prints the cost of a guard pair (not a test).
add_executable(test_runtime_lifecycle
    test_runtime_lifecycle.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime_lifecycle.cpp
)
set_target_properties(test_runtime_lifecycle PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(test_runtime_lifecycle PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_runtime_lifecycle PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    set_property(TARGET test_runtime_lifecycle PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_runtime_lifecycle PRIVATE /W4 /utf-8 /permissive-)
endif()
add_test(NAME runtime_lifecycle COMMAND test_runtime_lifecycle)
set_tests_properties(runtime_lifecycle PROPERTIES TIMEOUT 300)

# The pure dependency rule of the probe's source key (src/gpu_queue_dependency.cpp): tables, matching by
# fence and value, hop limit, CPU-signalled branch end, window selection by command lists, tie to the
# presenting queue, aged-out fences. The hooks that feed it need live validation.
add_executable(test_gpu_queue_dependency
    test_gpu_queue_dependency.cpp
    ${CMAKE_SOURCE_DIR}/src/gpu_queue_dependency.cpp
)
set_target_properties(test_gpu_queue_dependency PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(test_gpu_queue_dependency PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(MSVC)
    set_property(TARGET test_gpu_queue_dependency PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_gpu_queue_dependency PRIVATE /W4 /utf-8 /permissive-)
endif()

# The probe's borrowing rules (src/gpu_queue_borrow.cpp: pending word, frame pattern, gate, late classification,
# in-order merge, the one reference on the game's fence with a fake fence) and the core's two additions for them
# (borrowed submissions per frame, InvalidateSubmission). The hooks and the completion thread need live validation.
add_executable(test_gpu_queue_borrow
    test_gpu_queue_borrow.cpp
    ${CMAKE_SOURCE_DIR}/src/gpu_queue_borrow.cpp
    ${CMAKE_SOURCE_DIR}/src/gpu_queue_probe_core.cpp
)
set_target_properties(test_gpu_queue_borrow PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(test_gpu_queue_borrow PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(MSVC)
    set_property(TARGET test_gpu_queue_borrow PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_gpu_queue_borrow PRIVATE /W4 /utf-8 /permissive-)
endif()

# Replay of the tail-stamp policy (tools/testbed/stamp_policy_replay): an executable, NOT a ctest. It drives the
# production core and the production worker-step planner with the events recorded submission streams would produce and
# writes one CSV row per frame record (the judge is tools/testbed/stamp_policy_sim.py). Built with the tests, both architectures.
add_executable(stamp_policy_replay
    ${CMAKE_SOURCE_DIR}/tools/testbed/stamp_policy_replay/replay.cpp
    ${CMAKE_SOURCE_DIR}/src/gpu_queue_borrow.cpp
    ${CMAKE_SOURCE_DIR}/src/gpu_queue_probe_core.cpp
)
set_target_properties(stamp_policy_replay PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(stamp_policy_replay PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(MSVC)
    set_property(TARGET stamp_policy_replay PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(stamp_policy_replay PRIVATE /W4 /utf-8 /permissive-)
endif()

# The REAL gpu_queue_probe glue (src/gpu_queue_probe_d3d12.cpp, src/streamline_queue_hooks.cpp with the pure core, borrow and
# dependency units) against FAKE D3D12 queue / fence / device objects (gpu_queue_glue_support.h/.cpp: hand-written COM vtables, a
# fake GPU thread, stubs of the glue's externals; no GPU). x64 only: the probe does not exist on Win32.
#   test_gpu_queue_glue    ctest, functional: adoption of a render queue (role 3), borrowed markers, the Wait path, the worker's records,
#                          a short interleaved three-thread run, a foreign Signal, the end of the session (see its header).
#                          gpu_queue_glue adopts through the test seam (RELIMITER_GLUE_TEST_SEAM, src/gpu_queue_probe_test_seam.h);
#                          gpu_queue_glue_rule lets the dependency rule decide and is SKIPPED (77) when it does not pick the render queue.
#   bench_gpu_queue_hooks  NOT a ctest: nanoseconds per Execute + borrowed Signal pair and per unsatisfied Wait through the real detours.
# The two translation units that read the clock are compiled with a counting force-include (MSVC): clock reads per call are reported.
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(GPU_QUEUE_GLUE_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/gpu_queue_glue_support.cpp
        ${CMAKE_SOURCE_DIR}/src/gpu_queue_probe_d3d12.cpp
        ${CMAKE_SOURCE_DIR}/src/streamline_queue_hooks.cpp
        ${CMAKE_SOURCE_DIR}/src/gpu_queue_probe_core.cpp
        ${CMAKE_SOURCE_DIR}/src/gpu_queue_borrow.cpp
        ${CMAKE_SOURCE_DIR}/src/gpu_queue_dependency.cpp
    )
    if(MSVC)
        set_source_files_properties(
            ${CMAKE_SOURCE_DIR}/src/gpu_queue_probe_d3d12.cpp
            ${CMAKE_SOURCE_DIR}/src/streamline_queue_hooks.cpp
            PROPERTIES COMPILE_OPTIONS "/FI${CMAKE_CURRENT_SOURCE_DIR}/gpu_queue_glue_qpc_counter.h")
    endif()
    foreach(glue_target test_gpu_queue_glue bench_gpu_queue_hooks)
        add_executable(${glue_target} ${glue_target}.cpp ${GPU_QUEUE_GLUE_SOURCES})
        set_target_properties(${glue_target} PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )
        target_include_directories(${glue_target} PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/deps/reshade/deps/minhook/include
            ${CMAKE_SOURCE_DIR}/deps/reshade/include)
        target_compile_definitions(${glue_target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN RELIMITER_GLUE_TEST_SEAM)
        target_link_libraries(${glue_target} PRIVATE winmm)
        if(MSVC)
            set_property(TARGET ${glue_target} PROPERTY
                MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
            target_compile_options(${glue_target} PRIVATE /W4 /utf-8 /permissive-)
        endif()
    endforeach()
    add_test(NAME gpu_queue_glue COMMAND test_gpu_queue_glue)
    set_tests_properties(gpu_queue_glue PROPERTIES TIMEOUT 120)
    add_test(NAME gpu_queue_glue_rule COMMAND test_gpu_queue_glue --rule)
    set_tests_properties(gpu_queue_glue_rule PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    # Each ends the session in its own way, so each runs in its own process: gpu_queue_glue_rundown (a rundown that times
    # out retains the bound game fence, the session goes dead, no bind opens) and gpu_queue_glue_covered_bind (a marker whose
    # value the game fence had already reached is not borrowed and borrowing ends).
    add_test(NAME gpu_queue_glue_rundown COMMAND test_gpu_queue_glue --rundown-timeout)
    set_tests_properties(gpu_queue_glue_rundown PROPERTIES TIMEOUT 120)
    add_test(NAME gpu_queue_glue_covered_bind COMMAND test_gpu_queue_glue --covered-bind)
    set_tests_properties(gpu_queue_glue_covered_bind PROPERTIES TIMEOUT 120)
    # The dependency consumers never record a zero time (production consumers called with now == 0), and the clock-read count
    # per Signal / Wait through the detours per key state (1 with both keys on, 0 with either off). No session: it toggles the keys.
    add_test(NAME gpu_queue_glue_dependency_clock COMMAND test_gpu_queue_glue --dependency-clock)
    set_tests_properties(gpu_queue_glue_dependency_clock PROPERTIES TIMEOUT 60)
    # A binder of the Signal detour held at a pause point of BindMarker (GpuQueueProbeTest_SetBindPause) while the
    # slot of its submission is repaired and claimed again (k + 256): the lost take (point 0) must touch nothing; a successful
    # covered binder held after its value is published (point 2) or before its Submit is pushed (point 3) must still push its own
    # original Submit. Each in its own process.
    add_test(NAME gpu_queue_glue_bind_race_lost_take COMMAND test_gpu_queue_glue --bind-race-lost-take)
    set_tests_properties(gpu_queue_glue_bind_race_lost_take PROPERTIES TIMEOUT 120)
    add_test(NAME gpu_queue_glue_bind_race_lost_take_covered COMMAND test_gpu_queue_glue --bind-race-lost-take-covered)
    set_tests_properties(gpu_queue_glue_bind_race_lost_take_covered PROPERTIES TIMEOUT 120)
    add_test(NAME gpu_queue_glue_bind_race_covered_2 COMMAND test_gpu_queue_glue --bind-race-covered-2)
    set_tests_properties(gpu_queue_glue_bind_race_covered_2 PROPERTIES TIMEOUT 120)
    add_test(NAME gpu_queue_glue_bind_race_covered_3 COMMAND test_gpu_queue_glue --bind-race-covered-3)
    set_tests_properties(gpu_queue_glue_bind_race_covered_3 PROPERTIES TIMEOUT 120)
    # The lock-timeout taker's Lost word after a slot lap, and the worker's tail read of a consumed marker, each in its own process.
    add_test(NAME gpu_queue_glue_lose_word_lap COMMAND test_gpu_queue_glue --lose-word-lap)
    set_tests_properties(gpu_queue_glue_lose_word_lap PROPERTIES TIMEOUT 120)
    add_test(NAME gpu_queue_glue_tail_consumed COMMAND test_gpu_queue_glue --tail-consumed)
    set_tests_properties(gpu_queue_glue_tail_consumed PROPERTIES TIMEOUT 120)
endif()

## ── Marker stamps tests ──
# Directly tests the production lock-free stamp ring of the game's own SIMULATION_START,
# SIMULATION_END, RENDERSUBMIT_START and RENDERSUBMIT_END markers (src/marker_stamps.cpp) and the
# hooks' gate (MarkerStamps_Capture / MarkerStamps_Publish): consistent reads against concurrent writers, the complete
# frame key (domain, producer epoch, frame id), replacement by a later frame, the reset and a hook
# resuming after it, the duplicate policy (latest sample, one whole), the key-off identity (no clock
# read, nothing stamped), the CPU time of a span (same thread only, known rate only) and the cycle
# rate's decision logic (src/marker_rate.cpp: paired reads, invariant TSC, agreement of the thread
# cycle counter). On Windows it also runs the thread sample's micro-benchmark and the calibration
# (src/marker_cycles.cpp; prints, no assertion on speed). The marker hooks, their threads and the
# real marker streams of a game need live validation.

add_executable(test_marker_stamps
    test_marker_stamps.cpp
    ${CMAKE_SOURCE_DIR}/src/marker_stamps.cpp
    ${CMAKE_SOURCE_DIR}/src/marker_rate.cpp
)
if(WIN32)
    target_sources(test_marker_stamps PRIVATE ${CMAKE_SOURCE_DIR}/src/marker_cycles.cpp)
endif()
set_target_properties(test_marker_stamps PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(test_marker_stamps PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
    set_property(TARGET test_marker_stamps PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_marker_stamps PRIVATE /W4 /utf-8 /permissive-)
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
)
# The rung_feasibility event reasons (FeasibilityCause wire values and text).
add_executable(test_rung_feasibility
    test_rung_feasibility.cpp
    ${CMAKE_SOURCE_DIR}/src/rung_feasibility.cpp
)
foreach(controller_target IN ITEMS test_period_governor test_rung_feasibility)
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
    ${CMAKE_SOURCE_DIR}/src/unified_pacer.cpp
    ${CMAKE_SOURCE_DIR}/src/unified_queue_law.cpp
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
    ${CMAKE_SOURCE_DIR}/src/vtable_copy.cpp
    ${CMAKE_SOURCE_DIR}/src/startup_breadcrumb.cpp
    ${CMAKE_SOURCE_DIR}/src/module_presence.cpp
    ${CMAKE_SOURCE_DIR}/src/hooks.cpp)
target_include_directories(test_hook_regressions PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/deps/reshade/include
    ${CMAKE_SOURCE_DIR}/deps/reshade/deps/minhook/include)
target_compile_definitions(test_hook_regressions PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN)

# The hook batch (src/hooks.cpp alone; MinHook, the logger and the breadcrumb are fakes in the test): one apply per
# batch, publication before the queue, the empty and the nested batch, the exclusive install lock, refused
# admission, the failing-apply path (pending hooks enabled one by one, none left queued) and a retained hook set
# enabled again. The real MinHook freeze and the site wiring need live validation.
add_executable(test_hook_batch
    test_hook_batch.cpp
    ${CMAKE_SOURCE_DIR}/src/hooks.cpp)
target_include_directories(test_hook_batch PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/deps/reshade/deps/minhook/include)
target_compile_definitions(test_hook_batch PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN)

# The hook installer thread (src/hooks.cpp alone; MinHook, the logger and the breadcrumb are fakes; the thread, its
# descriptor, self-check and death are real): the kill switch, the unprotected fallback, dispatch and results, the
# single job slot, the inline entry, stop, death before a job and in flight. Skipped (77) where the debug
# privilege cannot be removed from the token.
add_executable(test_hook_installer
    test_hook_installer.cpp
    ${CMAKE_SOURCE_DIR}/src/hooks.cpp)
target_include_directories(test_hook_installer PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/deps/reshade/deps/minhook/include)
target_compile_definitions(test_hook_installer PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN)

# The installer thread against the REAL MinHook (minhook_static): a requester enabling and disabling a real hook
# through hooks.cpp, another engine suspending every thread it can open in a tight loop, and a caller of the hooked
# function. Every thread must keep making progress and every call must land in the original or the detour. Has its
# own watchdog; skipped (77) where the installer thread is not protected.
add_executable(test_hook_installer_stress
    test_hook_installer_stress.cpp
    ${CMAKE_SOURCE_DIR}/src/hooks.cpp)
target_include_directories(test_hook_installer_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/deps/reshade/deps/minhook/include)
target_compile_definitions(test_hook_installer_stress PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(test_hook_installer_stress PRIVATE minhook_static)

# The start-up breadcrumb (src/startup_breadcrumb.cpp): the record seqlock, the marks (first-wins, nothing after
# Running except Closed, the 32nd completed present), the judgement of a previous launch's record (valid and
# before Running only) and the reset/attach protocol on real temporary files, including a dropped mapping as the
# kill equivalent, a record torn in every way, a missing, short or unwritable file, and a file held by another
# handle. The logger's warning line and the hook sites need live validation.
add_executable(test_startup_breadcrumb
    test_startup_breadcrumb.cpp
    ${CMAKE_SOURCE_DIR}/src/startup_breadcrumb.cpp
)
set_target_properties(test_startup_breadcrumb PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(test_startup_breadcrumb PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_startup_breadcrumb PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    set_property(TARGET test_startup_breadcrumb PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_startup_breadcrumb PRIVATE /W4 /utf-8 /permissive-)
endif()

# The pure rules of the native route's private vtable copy (src/vtable_copy.cpp): the copy extent on a synthetic
# vtable, the forwarding records (immutable, the chain after a replacement), the verdict on a changed vptr,
# the thread-local dedup record, the teardown rules. Builds for both architectures. The install, the detours and
# DXGI itself need live validation.
add_executable(test_vtable_copy
    test_vtable_copy.cpp
    ${CMAKE_SOURCE_DIR}/src/vtable_copy.cpp
)
set_target_properties(test_vtable_copy PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(test_vtable_copy PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(MSVC)
    set_property(TARGET test_vtable_copy PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_vtable_copy PRIVATE /W4 /utf-8 /permissive-)
endif()

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
    ${CMAKE_SOURCE_DIR}/src/startup_breadcrumb.cpp
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
    ${CMAKE_SOURCE_DIR}/src/unified_pacer.cpp
    ${CMAKE_SOURCE_DIR}/src/marker_stamps.cpp
    ${CMAKE_SOURCE_DIR}/src/unified_queue_law.cpp
    ${CMAKE_SOURCE_DIR}/src/frame_timeline.cpp
    ${CMAKE_SOURCE_DIR}/src/frame_plan.cpp
    ${CMAKE_SOURCE_DIR}/src/cadence_grid.cpp
    ${CMAKE_SOURCE_DIR}/src/cadence_grid_publication.cpp
    ${CMAKE_SOURCE_DIR}/src/pacing_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/render_handoff.cpp
    ${CMAKE_SOURCE_DIR}/src/module_presence.cpp
    ${CMAKE_SOURCE_DIR}/src/unified_pacer.cpp)
target_link_libraries(test_feedback_device_identity PRIVATE Synchronization)
target_include_directories(test_feedback_device_identity PRIVATE
    ${CMAKE_SOURCE_DIR}/deps/reshade/include
    ${CMAKE_SOURCE_DIR}/deps/reshade/deps/minhook/include)
target_compile_definitions(test_feedback_device_identity PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN)
add_executable(test_oled_care test_oled_care.cpp)
add_executable(test_osd_position test_osd_position.cpp)
add_executable(test_module_presence test_module_presence.cpp
    ${CMAKE_SOURCE_DIR}/src/module_presence.cpp)
target_compile_definitions(test_module_presence PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_executable(test_hotkey_edge test_hotkey_edge.cpp
    ${CMAKE_SOURCE_DIR}/src/hotkey_poller.cpp)
target_compile_definitions(test_hotkey_edge PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
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
# The pacing schema fixture (data/pacing_schema.txt) against the production CSV writer and name functions.
add_executable(test_pacing_schema test_pacing_schema.cpp
    ${CMAKE_SOURCE_DIR}/src/csv_writer.cpp
    ${CMAKE_SOURCE_DIR}/src/frame_timeline.cpp
    ${CMAKE_SOURCE_DIR}/src/pacing_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/period_governor.cpp
    ${CMAKE_SOURCE_DIR}/src/rung_feasibility.cpp)
set_target_properties(test_pacing_schema PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_include_directories(test_pacing_schema PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_pacing_schema PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    PACING_SCHEMA_FIXTURE="${CMAKE_SOURCE_DIR}/tests/data/pacing_schema.txt")
if(MSVC)
    set_property(TARGET test_pacing_schema PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(test_pacing_schema PRIVATE /W4 /utf-8 /permissive-)
endif()
foreach(regression_target IN ITEMS test_hook_regressions test_hook_batch test_hook_installer test_hook_installer_stress
    test_pacing_metrics test_pacing_join test_reflex_clock_map test_native_pcl_loss_window
    test_native_present_gate test_cross_thread_slot
    test_gpu_readiness
    test_gpu_present_gate
    test_native_pcl_snapshot
    test_config_async
    test_scheduler_cadence test_osd_cadence
    test_feedback_device_identity
    test_oled_care test_osd_position test_module_presence test_hotkey_edge
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
# The fake ReShade device lock must fail AT ONCE (not hang) on a proxy call made while the thread holds it.
add_test(NAME feedback_device_lock_fixture COMMAND test_feedback_device_identity --proxy-call-under-lock)
set_tests_properties(feedback_device_lock_fixture PROPERTIES TIMEOUT 30
    PASS_REGULAR_EXPRESSION "FAIL line [0-9]+: owner.load\\(\\) != self")
add_test(NAME oled_care COMMAND test_oled_care)
add_test(NAME osd_position COMMAND test_osd_position)
add_test(NAME module_presence COMMAND test_module_presence)
add_test(NAME hotkey_edge COMMAND test_hotkey_edge)
add_test(NAME hook_regressions COMMAND test_hook_regressions)
add_test(NAME hook_batch COMMAND test_hook_batch)
add_test(NAME hook_installer COMMAND test_hook_installer)
set_tests_properties(hook_installer PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
add_test(NAME hook_installer_stress COMMAND test_hook_installer_stress)
set_tests_properties(hook_installer_stress PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
add_test(NAME vtable_copy COMMAND test_vtable_copy)
add_test(NAME startup_breadcrumb COMMAND test_startup_breadcrumb)
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
add_test(NAME pacing_schema COMMAND test_pacing_schema)
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
add_test(NAME unified_pacer COMMAND test_unified_pacer)
set_tests_properties(unified_pacer PROPERTIES TIMEOUT 120)
add_test(NAME pacing_test_mode COMMAND test_pacing_test_mode)
add_test(NAME enforcement_route COMMAND test_enforcement_route)
add_test(NAME unified_queue_law COMMAND test_unified_queue_law)
add_test(NAME gpu_queue_probe_core COMMAND test_gpu_queue_probe_core)
add_test(NAME gpu_queue_dependency COMMAND test_gpu_queue_dependency)
add_test(NAME gpu_queue_borrow COMMAND test_gpu_queue_borrow)
add_test(NAME marker_stamps COMMAND test_marker_stamps)
add_test(NAME lead_controller COMMAND test_lead_controller)
add_test(NAME period_governor COMMAND test_period_governor)
add_test(NAME rung_feasibility COMMAND test_rung_feasibility)
add_test(NAME source_stage COMMAND test_source_stage)
add_test(NAME frame_plan COMMAND test_frame_plan)
set_tests_properties(frame_plan PROPERTIES TIMEOUT 60)
add_test(NAME control_file COMMAND test_control_file)
add_test(NAME dxgi_display_source COMMAND test_dxgi_display_source)
add_test(NAME pqi COMMAND test_pqi)
