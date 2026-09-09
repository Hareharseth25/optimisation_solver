cmake_minimum_required(VERSION 3.20)

project(Optimisation_solver)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(bench
    src/main.cpp
    src/model/model.cpp
    src/benchmark/benchmarkresult.cpp
    src/presolve/presolver.cpp
)

target_include_directories(bench PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

enable_testing()

add_executable(test_batch2
    tests/presolve/test_batch2.cpp
    src/model/model.cpp
    src/presolve/presolver.cpp
)
target_include_directories(test_batch2 PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME PresolveBatch2 COMMAND test_batch2)

add_executable(test_presolve_correctness
    tests/presolve/test_presolve_correctness.cpp
    src/model/model.cpp
    src/presolve/presolver.cpp
)
target_include_directories(test_presolve_correctness PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME PresolveCorrectness COMMAND test_presolve_correctness)

add_executable(test_transformation_metadata
    tests/presolve/test_transformation_metadata.cpp
    src/model/model.cpp
    src/presolve/presolver.cpp
)
target_include_directories(test_transformation_metadata PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME PresolveTransformationMetadata COMMAND test_transformation_metadata)

add_executable(test_pipeline_cascades
    tests/presolve/test_pipeline_cascades.cpp
    src/model/model.cpp
    src/presolve/presolver.cpp
)
target_include_directories(test_pipeline_cascades PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME PresolvePipelineCascades COMMAND test_pipeline_cascades)

add_executable(test_adversarial
    tests/presolve/test_adversarial.cpp
    src/model/model.cpp
    src/presolve/presolver.cpp
)
target_include_directories(test_adversarial PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME PresolveAdversarial COMMAND test_adversarial)

# Linear programming engine. Self-contained and C++17; builds the library only.
# Its tests and benchmark harnesses stay off here and are enabled by configuring
# pdlp_engine/ standalone, or with -DPDLP_BUILD_TESTS=ON -DPDLP_BUILD_TOOLS=ON.
add_subdirectory(pdlp_engine)

# Branch-and-cut over a dual simplex, and ADMM for convex QP. Both build their
# own tests, so `ctest` from the root covers all three engines.
add_subdirectory(milp_engine)
add_subdirectory(qp_engine)

# Classifier and dispatcher. Classification runs before presolve (its reductions
# differ by problem class); engine selection runs after (presolve changes size,
# can eliminate all integrality, and can prove infeasibility outright).
add_library(solver_dispatch
    src/solver/classifier.cpp
    src/solver/dispatcher.cpp
    src/model/model.cpp
    src/presolve/presolver.cpp
)
target_include_directories(solver_dispatch PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(solver_dispatch PUBLIC cxx_std_17)
if(NOT MSVC)
    target_compile_options(solver_dispatch PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_executable(test_classifier_dispatcher tests/solver/test_classifier_dispatcher.cpp)
target_link_libraries(test_classifier_dispatcher PRIVATE solver_dispatch)
add_test(NAME test_classifier_dispatcher COMMAND test_classifier_dispatcher)

# The only place model::Model and pdlp::CompiledLp meet. Links pdlp_engine for
# CompiledLp and PdlpResult; everything else in the pipeline speaks model::Model.
add_library(pdlp_adapter
    src/adapter/pdlp_adapter.cpp
    src/model/model.cpp
)
target_include_directories(pdlp_adapter PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(pdlp_adapter PUBLIC pdlp_engine)
target_compile_features(pdlp_adapter PUBLIC cxx_std_17)
if(NOT MSVC)
    target_compile_options(pdlp_adapter PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(test_pdlp_adapter tests/adapter/test_pdlp_adapter.cpp)
target_link_libraries(test_pdlp_adapter PRIVATE pdlp_adapter)
add_test(NAME test_pdlp_adapter COMMAND test_pdlp_adapter)

# The orchestrator is the one entry point a caller needs: it classifies, runs
# presolve, dispatches to whichever engine the decision names, and normalises
# every engine's result into a single SolveResult. It is therefore the only
# target that links all three engines at once.
add_library(solver_orchestrator
    src/solver/orchestrator.cpp
)
target_include_directories(solver_orchestrator PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(solver_orchestrator PUBLIC
    solver_dispatch
    pdlp_adapter
    milp_engine
    qp_engine
)
target_compile_features(solver_orchestrator PUBLIC cxx_std_17)
if(NOT MSVC)
    target_compile_options(solver_orchestrator PRIVATE -Wall -Wextra -Wpedantic)
endif()

# End-to-end: model in, solution out, through every layer in the real order.
add_executable(test_end_to_end tests/pipeline/test_end_to_end.cpp)
target_link_libraries(test_end_to_end PRIVATE solver_orchestrator)
add_test(NAME test_end_to_end COMMAND test_end_to_end)

# Pins the Model quadratic convention through the public solve API: a
# diagonal q_ii must reach the engine as P_ii = 2*q_ii and an off-diagonal
# q_ij as P_ij = P_ji = q_ij, with maximization negating both P and q and the
# objective offset restored exactly once. Every expected value is derived by
# hand in the test, independently of the code under test.
add_executable(test_qp_convention tests/qp/test_qp_convention.cpp)
target_link_libraries(test_qp_convention PRIVATE solver_orchestrator)
add_test(NAME test_qp_convention COMMAND test_qp_convention)

# Ground truth by exhaustive enumeration: every small binary MILP is solved
# by brute force over all 2^n subsets and compared against the pipeline. No
# reference solver required, so this stays runnable anywhere.
add_executable(test_milp_enumeration tests/milp/test_milp_enumeration.cpp)
target_link_libraries(test_milp_enumeration PRIVATE solver_orchestrator)
add_test(NAME test_milp_enumeration COMMAND test_milp_enumeration)
