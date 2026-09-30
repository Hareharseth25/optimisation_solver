# Primal-dual interior-point (barrier) engine. Self-contained linear algebra:
# CSC storage, approximate minimum degree ordering, and a quasidefinite LDL'.
# No third-party solver or linear-algebra library.
add_library(barrier_engine
    src/sparse.cpp
    src/ordering.cpp
    src/barrier_solver.cpp
    src/barrier_adapter.cpp
)
target_include_directories(barrier_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    # model/model.h, for the adapter.
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_features(barrier_engine PUBLIC cxx_std_17)
if(NOT MSVC)
    target_compile_options(barrier_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Throwing checks, not assert(): NDEBUG in Release cannot disable them.
add_executable(barrier_linalg_tests tests/test_linalg.cpp)
target_link_libraries(barrier_linalg_tests PRIVATE barrier_engine)
add_test(NAME barrier_linalg_tests COMMAND barrier_linalg_tests)

add_executable(barrier_solver_tests tests/test_barrier.cpp)
target_link_libraries(barrier_solver_tests PRIVATE barrier_engine)
add_test(NAME barrier_solver_tests COMMAND barrier_solver_tests)
