#pragma once

#include "postsolve/postsolver.h"
#include "solver/classifier.h"
#include "solver/dispatcher.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace solver {

// How a solve happened, as opposed to what it produced.
//
//   SolveResult  the final outcome: status, point, duals, engine provenance.
//   SolveReport  observability: what classification, presolve, dispatch,
//                the engine and the validation stages saw and how long each
//                took.
//
// The report is filled IN PLACE by the one pipeline run that produces the
// SolveResult. It is never recomputed: nothing here may call classify(),
// Presolver::run(), dispatch() or an engine a second time, because a report
// rebuilt that way could describe a different run than the one whose answer
// the caller holds (and its timings would be of the wrong work).
//
// A section that is std::nullopt means that stage did not run for this solve
// -- for example presolve proving infeasibility leaves no engine, no
// reduced-space validation and no postsolve. Absent is never reported as zero.

// Stable summary of presolve::PresolveResult. The transformation log itself
// is an internal postsolve contract, so it is summarised by type rather than
// exposed record by record.
struct PresolveSummary {
    bool infeasible = false;
    bool converged = true;

    std::size_t originalVariables = 0;
    std::size_t originalConstraints = 0;
    std::size_t reducedVariables = 0;
    std::size_t reducedConstraints = 0;

    // Constraint-matrix nonzeros counted by solver::countNonzeros(), the rule
    // the dispatcher's size threshold uses.
    std::int64_t originalNonzeros = 0;
    std::int64_t reducedNonzeros = 0;

    std::size_t transformationCount = 0;
    struct TransformationCounts {
        std::size_t removeVariable = 0;
        std::size_t removeConstraint = 0;
        std::size_t fixVariable = 0;
        std::size_t substituteVariable = 0;
        std::size_t tightenLowerBound = 0;
        std::size_t tightenUpperBound = 0;
    } transformationsByType;
};

// Copied from the same values the pipeline wrote into SolveResult, so the two
// always agree; kept here so a report can be read on its own.
struct DispatchSummary {
    // False when the answer was settled before dispatch() was called (invalid
    // model, presolve-proved infeasibility, an NLP refusal). `engine` and
    // `reason` then describe that early exit, exactly as SolveResult does.
    bool dispatcherInvoked = false;

    Engine engine = Engine::Unsupported;          // == SolveResult::engine
    std::string reason;                           // == SolveResult::engineReason
    Engine executedEngine = Engine::Unsupported;  // == SolveResult::executedEngine
};

// One primal validation pass: Postsolver::validateSolution in reduced space,
// or Postsolver::process (reconstruction + validation) in original space.
struct ValidationSummary {
    bool passed = false;

    // The Postsolver's verdict on the point, and why the pipeline rejected it
    // (empty when passed). A point the Postsolver accepts can still be
    // rejected for a non-finite objective: passed=false, status=Success.
    postsolve::PostsolveStatus status = postsolve::PostsolveStatus::InternalError;
    std::string failure;

    // Worst absolute and magnitude-scaled violations over every bound and row.
    // Populated only when the check passed: a failing check stops early, so
    // its partial maxima are not a statement about the whole point. NaN
    // otherwise.
    double maxBoundResidual = std::numeric_limits<double>::quiet_NaN();
    double maxConstraintResidual = std::numeric_limits<double>::quiet_NaN();
    double maxBoundResidualScaled = std::numeric_limits<double>::quiet_NaN();
    double maxConstraintResidualScaled = std::numeric_limits<double>::quiet_NaN();

    // Objective recomputed from the validated point in this check's model.
    // NaN when the check failed.
    double objectiveValue = std::numeric_limits<double>::quiet_NaN();
};

struct ReducedValidationSummary : ValidationSummary {
    // The objective the engine itself reported for its point, before the
    // pipeline replaced it with `objectiveValue` recomputed from the point.
    double engineReportedObjective = std::numeric_limits<double>::quiet_NaN();
};

struct PostsolveSummary : ValidationSummary {
    // Whether original-space duals were requested from postsolve (optimal,
    // continuous, engine supplied duals). Whether they were PUBLISHED, and the
    // residual they achieved, are SolveResult::hasDuals / maxDualResidual.
    bool dualsRequested = false;
};

// Wall-clock seconds per stage, steady_clock. Negative means "did not run".
//
//   validation         Model::validate() on the input model, plus the reduced
//                      model's structural check at the start of solveReduced.
//   classification     classify() on the input model.
//   presolve           Presolver::run().
//   dispatch           dispatch().
//   engine             The engine path, end to end: adapter translation,
//                      CUDA availability check, the engine's own solve and,
//                      for the barrier, crossover. Not broken down further;
//                      the engines do not expose finer stage timings here.
//                      Set whenever an engine path (including the bound walk
//                      for Engine::Trivial) was entered, even if it refused
//                      before iterating.
//   reducedValidation  Engine-result validation in the reduced model.
//   postsolve          Reconstruction + original-space validation.
//   total              Exactly SolveResult::solveSeconds.
//
// Stages are disjoint, so their sum is <= total; the remainder is
// bookkeeping between stages.
struct StageTimings {
    double validation = -1.0;
    double classification = -1.0;
    double presolve = -1.0;
    double dispatch = -1.0;
    double engine = -1.0;
    double reducedValidation = -1.0;
    double postsolve = -1.0;
    double total = -1.0;
};

struct SolveReport {
    // The Classification the pipeline dispatched with: computed by solve(), or
    // the one the caller supplied to solveReduced().
    std::optional<Classification> classification;
    std::optional<PresolveSummary> presolve;
    DispatchSummary dispatch;
    std::optional<ReducedValidationSummary> reducedValidation;
    std::optional<PostsolveSummary> postsolve;
    StageTimings stageSeconds;
};

}  // namespace solver
