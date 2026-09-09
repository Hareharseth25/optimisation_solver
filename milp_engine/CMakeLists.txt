# MILP engine: basis-state / dual simplex foundation for Phase 2's
# branch-and-cut driver. Unlike pdlp_engine (deliberately decoupled from
# model::Model, see pdlp_engine/README.md), this module solves LP relaxations
# directly over model::Model, since branch-and-cut re-solves node relaxations
# after variable-bound changes and needs the same IR the rest of the solver
# uses -- so it links the model translation unit directly (matching the root
# project's existing flat, no-internal-libraries convention) rather than
# building as a standalone project the way pdlp_engine does.

add_library(milp_engine
    src/basis_state.cpp
    src/dual_simplex_solver.cpp
    src/gomory_cuts.cpp
    src/heuristics.cpp
    src/branching_rules.cpp
    src/branch_and_bound.cpp
    ${CMAKE_SOURCE_DIR}/src/model/model.cpp
    ${CMAKE_SOURCE_DIR}/src/util/parallel.cpp
)

target_include_directories(milp_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_features(milp_engine PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(milp_engine PRIVATE /W4 /permissive-)
else()
    target_compile_options(milp_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Called independently rather than relying on the parent's find_package (this
# module is add_subdirectory'd before the root CMakeLists.txt's own call),
# matching pdlp_engine/CMakeLists.txt's same self-contained pattern.
find_package(Threads REQUIRED)
target_link_libraries(milp_engine PUBLIC Threads::Threads)

enable_testing()

add_executable(test_dual_simplex tests/test_dual_simplex.cpp)
target_link_libraries(test_dual_simplex PRIVATE milp_engine)
add_test(NAME test_dual_simplex COMMAND test_dual_simplex)

add_executable(test_branch_and_bound tests/test_branch_and_bound.cpp)
target_link_libraries(test_branch_and_bound PRIVATE milp_engine)
add_test(NAME test_branch_and_bound COMMAND test_branch_and_bound)

add_executable(test_gomory_cuts tests/test_gomory_cuts.cpp)
target_link_libraries(test_gomory_cuts PRIVATE milp_engine)
add_test(NAME test_gomory_cuts COMMAND test_gomory_cuts)

add_executable(test_heuristics tests/test_heuristics.cpp)
target_link_libraries(test_heuristics PRIVATE milp_engine)
add_test(NAME test_heuristics COMMAND test_heuristics)

add_executable(test_branching tests/test_branching.cpp)
target_link_libraries(test_branching PRIVATE milp_engine)
add_test(NAME test_branching COMMAND test_branching)
