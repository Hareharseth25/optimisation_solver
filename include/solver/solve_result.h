#pragma once

#include "solver/dispatcher.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace solver {

// One status for every engine.
//
// PDLP reports PdlpStatus, branch-and-cut reports MilpStatus, and the two
// disagree on both spelling and meaning -- MilpStatus::NodeLimit and
// PdlpStatus::IterationLimit are the same idea under different names, and only
// one of the enums has Unbounded. Postsolve should not have to know which
// engine ran, so both are normalised here.
enum class SolveStatus {
    Optimal,
    Infeasible,
    Unbounded,

    // Ran out of budget with no proof either way. `variableValues` holds the
    // best point found, if any.
    LimitReached,

    NumericalFailure,
    InvalidModel,

    // No engine in this build can solve the problem as classified.
    Unsupported
};

[[nodiscard]] const char* toString(SolveStatus value) noexcept;

// The single shape postsolve consumes, whichever engine produced it.
//
// COORDINATES. Sense flips and any engine-internal rescaling are already undone,
// so the values are in the model's own units and sense. They are NOT yet mapped
// back through presolve: the vectors are indexed by the REDUCED model's
// variables and constraints, because postsolve is the layer that expands them
// and it does not exist yet. `reducedVariableCount` and `reducedConstraintCount`
// say what the indices refer to, so a caller cannot mistake reduced indices for
// original ones.
struct SolveResult {
    SolveStatus status = SolveStatus::InvalidModel;
    std::string message;

    // Which engine the DISPATCHER chose, and why. Kept because "which engine
    // ran" is the first question asked when a solve looks wrong.
    Engine engine = Engine::Unsupported;
    std::string engineReason;

    // Which engine ACTUALLY ran, recorded by the code path that invokes it
    // rather than by the dispatcher that selected it.
    //
    // These two are separate fields on purpose. They disagreed once already:
    // the dispatcher named Engine::DualSimplex for requireVertexSolution
    // while the orchestrator's switch routed that case into PDLP, so the
    // reported engine was a decision nobody had executed and the vertex
    // promise was silently broken. A decision label cannot detect that; only
    // a value written by the path that did the work can.
    //
    // Engine::Unsupported here means no engine was invoked at all -- the
    // solve was answered by presolve (Infeasible), by the bound-walk
    // (Trivial), or refused before dispatch.
    Engine executedEngine = Engine::Unsupported;

    // Length model.variables.size(). Empty when no point was found.
    std::vector<double> variableValues;

    // Shadow prices, d(objective)/d(right-hand side), length
    // model.constraints.size(). EMPTY when the engine provides none:
    // branch-and-cut has no meaningful dual for the integer problem. Check
    // hasDuals rather than assuming.
    std::vector<double> constraintDuals;
    bool hasDuals = false;

    double objectiveValue = 0.0;

    // For a model with integer variables, how far the returned point is from
    // integral. Zero for a pure LP. A non-zero value on a MILP means the caller
    // was handed a relaxation, not a solution.
    double maxIntegralityViolation = 0.0;
    bool integralityRespected = true;

    // Work done. Whichever is meaningful for the engine that ran; the other
    // stays zero.
    std::int64_t iterations = 0;
    std::int64_t nodeCount = 0;
    double solveSeconds = 0.0;

    // Dimensions the vectors above are indexed by. Equal to the original
    // model's counts only when presolve removed nothing.
    std::size_t reducedVariableCount = 0;
    std::size_t reducedConstraintCount = 0;
};

}  // namespace solver
