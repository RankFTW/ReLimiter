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

enable_testing()
add_test(NAME dmfg_detection_properties COMMAND test_dmfg_detection)
add_test(NAME scheduler_passthrough COMMAND test_scheduler_passthrough)
add_test(NAME dmfg_output_cap_properties COMMAND test_dmfg_output_cap)

## ── Adaptive DLSS Scaling config property tests ──
# Tests config round-trip integrity and validation clamping.

add_executable(test_config_dlss test_config_dlss.cpp)
target_compile_features(test_config_dlss PRIVATE cxx_std_17)

if(MSVC)
    set_property(TARGET test_config_dlss PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_test(NAME config_dlss_properties COMMAND test_config_dlss)

## ── K_Controller property tests ──
# Tests tier generation, hysteresis, RR floor, FG awareness, safety lock, scale invariance.

add_executable(test_k_controller test_k_controller.cpp)
target_compile_features(test_k_controller PRIVATE cxx_std_17)

if(MSVC)
    set_property(TARGET test_k_controller PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_test(NAME k_controller_properties COMMAND test_k_controller)

## ── Resolution math property tests ──
# Tests fake dimension reporting, proxy resize, NGX override, clamping, pre-allocation viewport.

add_executable(test_resolution_math test_resolution_math.cpp)
target_compile_features(test_resolution_math PRIVATE cxx_std_17)

if(MSVC)
    set_property(TARGET test_resolution_math PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_test(NAME resolution_math_properties COMMAND test_resolution_math)

## ── Lanczos-3 kernel property tests ──
# Tests symmetry, compact support, unit peak, and normalization.

add_executable(test_lanczos_kernel test_lanczos_kernel.cpp)
target_compile_features(test_lanczos_kernel PRIVATE cxx_std_17)

if(MSVC)
    set_property(TARGET test_lanczos_kernel PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_test(NAME lanczos_kernel_properties COMMAND test_lanczos_kernel)

## ── Mip Corrector property tests ──
# Tests mip bias calculation and selective application.

add_executable(test_mip_corrector test_mip_corrector.cpp)
target_compile_features(test_mip_corrector PRIVATE cxx_std_17)

if(MSVC)
    set_property(TARGET test_mip_corrector PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_test(NAME mip_corrector_properties COMMAND test_mip_corrector)
