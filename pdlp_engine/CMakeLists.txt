cmake_minimum_required(VERSION 3.20)
project(pdlp_engine LANGUAGES CXX)

# This module is built two ways: standalone during development, and as a
# subdirectory of the parent solver project. Anything that would reach outside
# this directory -- forcing a build type, enabling tests, adding extra
# executables, tuning for the local CPU -- is gated on which of the two is
# happening, so add_subdirectory() here has no side effects on the parent.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(PDLP_STANDALONE TRUE)
else()
    set(PDLP_STANDALONE FALSE)
endif()

option(PDLP_BUILD_TESTS "Build the PDLP test suite" ${PDLP_STANDALONE})
option(PDLP_BUILD_TOOLS "Build the benchmark, validation and ablation harnesses" ${PDLP_STANDALONE})
option(PDLP_ENABLE_LTO  "Enable link-time optimisation for Release builds" ON)

# -march=native produces binaries that fault on a different CPU, so it is only
# a default when this module is the whole build. In the parent build the team
# gets portable output unless it explicitly asks otherwise.
option(PDLP_NATIVE_ARCH "Tune for the building machine's CPU" ${PDLP_STANDALONE})

# Only meaningful standalone: setting this in a subdirectory would overwrite the
# parent project's cached build type.
if(PDLP_STANDALONE AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    message(STATUS "CMAKE_BUILD_TYPE not set; defaulting to Release")
endif()

add_library(pdlp_engine
    src/sparse_matrix.cpp
    src/parallel.cpp
    src/scaling.cpp
    src/infeasibility.cpp
    src/preconditioner.cpp
    src/step_controller.cpp
    src/iterate_average.cpp
    src/restart_controller.cpp
    src/termination.cpp
    src/pdhg_kernel.cpp
    src/feasibility_polishing.cpp
    src/pdlp_solver.cpp
)

target_include_directories(pdlp_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Consumers get C++17 without the parent having to set anything.
target_compile_features(pdlp_engine PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
target_link_libraries(pdlp_engine PUBLIC Threads::Threads)

include(CheckCXXCompilerFlag)

if(MSVC)
    target_compile_options(pdlp_engine PRIVATE /W4 /permissive- /fp:precise)
    target_compile_options(pdlp_engine PRIVATE $<$<CONFIG:Release>:/O2>)
else()
    target_compile_options(pdlp_engine PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(pdlp_engine PRIVATE $<$<CONFIG:Release>:-O3>)

    # Contraction into FMA is safe here and shortens the sparse dot products.
    # Full -ffast-math is deliberately NOT used: it permits reassociating the
    # residual reductions and would let the compiler assume no infinities, which
    # this code relies on for one-sided row bounds.
    check_cxx_compiler_flag("-ffp-contract=fast" PDLP_HAS_FP_CONTRACT)
    if(PDLP_HAS_FP_CONTRACT)
        target_compile_options(pdlp_engine PRIVATE -ffp-contract=fast)
    endif()

    # PRIVATE, not PUBLIC: this tunes how the engine itself is compiled and must
    # not be forced onto anything that links it.
    if(PDLP_NATIVE_ARCH)
        check_cxx_compiler_flag("-march=native" PDLP_HAS_MARCH_NATIVE)
        if(PDLP_HAS_MARCH_NATIVE)
            target_compile_options(pdlp_engine PRIVATE -march=native)
        else()
            check_cxx_compiler_flag("-mcpu=native" PDLP_HAS_MCPU_NATIVE)
            if(PDLP_HAS_MCPU_NATIVE)
                target_compile_options(pdlp_engine PRIVATE -mcpu=native)
            endif()
        endif()
    endif()
endif()

if(PDLP_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT PDLP_IPO_SUPPORTED OUTPUT PDLP_IPO_MESSAGE)
    if(PDLP_IPO_SUPPORTED)
        set_property(TARGET pdlp_engine
            PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    else()
        message(STATUS "LTO unavailable: ${PDLP_IPO_MESSAGE}")
    endif()
endif()

# Development harnesses. Off in the parent build: they are how the engine's
# performance and accuracy claims are reproduced, not part of the library.
#   pdlp_bench    time per iteration on generated sparse LPs
#   pdlp_validate emit instances plus answers for cross-checking against HiGHS
if(PDLP_BUILD_TOOLS)
    add_executable(pdlp_example examples/solve_example.cpp)
    target_link_libraries(pdlp_example PRIVATE pdlp_engine)

    add_executable(pdlp_bench bench/bench_pdlp.cpp)
    target_link_libraries(pdlp_bench PRIVATE pdlp_engine)

    add_executable(pdlp_validate tools/validate_against_reference.cpp)
    target_link_libraries(pdlp_validate PRIVATE pdlp_engine)
endif()

if(PDLP_BUILD_TESTS)
    if(PDLP_STANDALONE)
        enable_testing()
    endif()
    add_executable(pdlp_tests tests/test_pdlp.cpp)
    target_link_libraries(pdlp_tests PRIVATE pdlp_engine)
    add_test(NAME pdlp_tests COMMAND pdlp_tests)
endif()
