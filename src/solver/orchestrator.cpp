#include "solver/orchestrator.h"

#include "adapter/pdlp_adapter.h"
#include "milp/branch_and_bound.h"
#include "milp/dual_simplex_solver.h"
#include "pdlp/pdlp_solver.h"
#include "qp/qp_adapter.h"
#include "qp/qp_solver.h"
#include "presolve/presolver.h"

#include <chrono>
#include <stdexcept>
#include <cmath>
#include <vector>

namespace solver {
namespace {

using Clock = std::chrono::steady_clock;

double secondsSince(const Clock::time_point& start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

SolveStatus normalise(pdlp::PdlpStatus status) noexcept {
    switch (status) {
        case pdlp::PdlpStatus::Optimal:          return SolveStatus::Optimal;
        case pdlp::PdlpStatus::Infeasible:       return SolveStatus::Infeasible;
        case pdlp::PdlpStatus::Unbounded:        return SolveStatus::Unbounded;
        case pdlp::PdlpStatus::IterationLimit:   return SolveStatus::LimitReached;
        case pdlp::PdlpStatus::TimeLimit:        return SolveStatus::LimitReached;
        case pdlp::PdlpStatus::NumericalFailure: return SolveStatus::NumericalFailure;
        case pdlp::PdlpStatus::InvalidProblem:   return SolveStatus::InvalidModel;
    }
    return SolveStatus::NumericalFailure;
}

SolveStatus normalise(qp::QpStatus status) noexcept {
    switch (status) {
        case qp::QpStatus::Optimal:          return SolveStatus::Optimal;
        case qp::QpStatus::Infeasible:       return SolveStatus::Infeasible;
        case qp::QpStatus::Unbounded:        return SolveStatus::Unbounded;
        case qp::QpStatus::IterationLimit:   return SolveStatus::LimitReached;
        case qp::QpStatus::TimeLimit:        return SolveStatus::LimitReached;
        case qp::QpStatus::NumericalFailure: return SolveStatus::NumericalFailure;
        case qp::QpStatus::InvalidProblem:   return SolveStatus::InvalidModel;
    }
    return SolveStatus::NumericalFailure;
}

SolveStatus normalise(milp::MilpStatus status) noexcept {
    switch (status) {
        case milp::MilpStatus::Optimal:    return SolveStatus::Optimal;
        case milp::MilpStatus::Infeasible: return SolveStatus::Infeasible;
        case milp::MilpStatus::Unbounded:  return SolveStatus::Unbounded;
        case milp::MilpStatus::NodeLimit:  return SolveStatus::LimitReached;
        case milp::MilpStatus::TimeLimit:  return SolveStatus::LimitReached;
    }
    return SolveStatus::NumericalFailure;
}

SolveStatus normalise(milp::DualSimplexStatus status) noexcept {
    switch (status) {
        case milp::DualSimplexStatus::Optimal:        return SolveStatus::Optimal;
        case milp::DualSimplexStatus::Infeasible:     return SolveStatus::Infeasible;
        case milp::DualSimplexStatus::Unbounded:      return SolveStatus::Unbounded;
        case milp::DualSimplexStatus::IterationLimit: return SolveStatus::LimitReached;
        case milp::DualSimplexStatus::TimeLimit:      return SolveStatus::LimitReached;
    }
    return SolveStatus::NumericalFailure;
}

// Measures how far a point is from satisfying the model's integrality.
double integralityViolation(const model::Model& model,
                            const std::vector<double>& values) {
    double worst = 0.0;
    for (std::size_t j = 0; j < model.variables.size() && j < values.size(); ++j) {
        if (model.variables[j].type == model::VariableType::Continuous) {
            continue;
        }
        worst = std::max(worst, std::abs(values[j] - std::round(values[j])));
    }
    return worst;
}

// No constraints remain, so the variables no longer interact: each one moves to
// whichever of its own bounds improves the objective. Returning just the
// objective offset here -- as an earlier version did -- silently reported the
// wrong optimum and an empty solution vector for every model presolve stripped
// down to bounds.
SolveResult solveTrivially(const model::Model& reduced, SolveResult result) {
    const std::size_t n = reduced.variables.size();
    result.variableValues.assign(n, 0.0);

    std::vector<double> cost(n, 0.0);
    for (const auto& term : reduced.objective.linearTerms) {
        const auto j = static_cast<std::size_t>(term.variableIndex);
        if (j < n) {
            cost[j] += term.value;
        }
    }

    const bool maximise =
        reduced.objective.sense == model::ObjectiveSense::Maximize;
    double objective = reduced.objective.offset;

    for (std::size_t j = 0; j < n; ++j) {
        const model::Variable& variable = reduced.variables[j];
        // Which bound helps depends on the sign of the cost and the sense.
        const bool wantUpper = maximise ? (cost[j] > 0.0) : (cost[j] < 0.0);
        double value = 0.0;

        if (cost[j] == 0.0) {
            // Indifferent: settle on any feasible point inside the bounds.
            value = std::isfinite(variable.lowerBound) ? variable.lowerBound
                  : (std::isfinite(variable.upperBound) ? variable.upperBound : 0.0);
        } else if (wantUpper) {
            if (!std::isfinite(variable.upperBound)) {
                result.status = SolveStatus::Unbounded;
                result.message = "variable " + variable.name +
                    " improves the objective without an upper bound";
                return result;
            }
            value = variable.upperBound;
        } else {
            if (!std::isfinite(variable.lowerBound)) {
                result.status = SolveStatus::Unbounded;
                result.message = "variable " + variable.name +
                    " improves the objective without a lower bound";
                return result;
            }
            value = variable.lowerBound;
        }

        result.variableValues[j] = value;
        objective += cost[j] * value;
    }

    result.status = SolveStatus::Optimal;
    result.objectiveValue = objective;
    result.executedEngine = Engine::Trivial;
    // Every remaining constraint was removed, so there are no prices to report.
    result.hasDuals = false;
    return result;
}

SolveResult runPdlp(const model::Model& reduced, const SolverOptions& options,
                    SolveResult result) {
    pdlp::CompiledLp compiled;
    adapter::AdapterOptions adapterOptions;
    // The dispatcher only routes here when no integrality survives presolve, so
    // relaxation should never trigger. Left false deliberately: if it ever does
    // trigger, the adapter refuses rather than silently solving a relaxation.
    const adapter::PdlpTranslation translation =
        adapter::toCompiledLp(reduced, compiled, adapterOptions);
    if (!translation.ok) {
        result.status = SolveStatus::InvalidModel;
        result.message = translation.error;
        return result;
    }

    pdlp::PdlpOptions engineOptions;
    engineOptions.primalTolerance = options.tolerance;
    engineOptions.dualTolerance = options.tolerance;
    engineOptions.gapTolerance = options.tolerance;
    engineOptions.timeLimitSeconds = options.timeLimitSeconds;
    result.executedEngine = Engine::Pdlp;
    const pdlp::PdlpResult raw = pdlp::PdlpSolver{}.solve(compiled, engineOptions);
    const adapter::ModelSolution solution =
        adapter::toModelSolution(reduced, translation, raw);

    result.status = normalise(raw.status);
    result.message = raw.statusMessage;
    result.variableValues = solution.variableValues;
    result.constraintDuals = solution.constraintDuals;
    result.hasDuals = !result.constraintDuals.empty();
    result.objectiveValue = solution.objectiveValue;
    result.iterations = raw.iterations;
    return result;
}

// The dual simplex, run directly on a bare LP.
//
// This is a different entry point from branch-and-cut's per-node use of the
// same solver: there is no tree, no bound tightening, just one cold-start
// solve of the model as given. It is what makes SolverOptions'
// requireVertexSolution a real promise -- the simplex terminates AT a vertex,
// with a basis, which PDLP's iterate does not.
//
// Conventions were verified against a known LP before wiring rather than
// assumed: DualSimplexResult::objectiveValue already includes
// model.objective.offset (finalizeSign adds it) and ::dual is already in the
// model's own sign convention -- on "max 3x+5y s.t. x<=4, 2y<=12, 3x+2y<=18"
// it returns duals [0, 1.5, 1], matching HiGHS's marginals for that LP
// directly, with no flip. So nothing is adjusted here.
SolveResult runDualSimplex(const model::Model& reduced, const SolverOptions& options,
                           SolveResult result) {
    milp::DualSimplexOptions engineOptions;
    engineOptions.primalFeasibilityTolerance = options.tolerance;
    engineOptions.dualFeasibilityTolerance = options.tolerance;
    // Forwarded, not dropped. Measured before this line existed: a 2500-row
    // LP under requireVertexSolution ran 17.4s against a 1.0s budget.
    engineOptions.timeLimitSeconds = options.timeLimitSeconds;

    // The dual simplex signals two very different things by throwing: input it
    // cannot represent, and a numerical breakdown on input that was perfectly
    // valid. Collapsing both to InvalidModel blames the caller for the
    // solver's own limits -- measured on a valid LP with duplicate rows, the
    // crash basis came out singular and the pipeline reported invalid_model
    // for a model that is not invalid.
    //
    // The two representable-input conditions are checked up front, so anything
    // thrown by the solve itself is a numerical failure and is reported as one.
    if (!reduced.validate()) {
        result.status = SolveStatus::InvalidModel;
        result.message = "model failed structural validation";
        return result;
    }
    if (!reduced.objective.quadraticTerms.empty()) {
        result.status = SolveStatus::InvalidModel;
        result.message = "the dual simplex solves linear objectives only";
        return result;
    }

    milp::DualSimplexResult raw;
    try {
        result.executedEngine = Engine::DualSimplex;
        raw = milp::DualSimplexSolver{}.solve(reduced, engineOptions);
    } catch (const std::exception& error) {
        // Valid input, solver could not proceed -- e.g. a singular crash
        // basis, which a proper dual Phase 1 would avoid. Report it as the
        // numerical failure it is, and keep the orchestrator's contract that
        // every engine path returns a status rather than propagating a throw.
        result.status = SolveStatus::NumericalFailure;
        result.message = error.what();
        return result;
    }

    result.status = normalise(raw.status);
    result.message = "dual simplex";
    result.variableValues = raw.primal;
    result.objectiveValue = raw.objectiveValue;
    result.iterations = raw.iterations;

    // Empty for Infeasible/Unbounded, where there is no basis to price from.
    if (raw.dual.size() == reduced.constraints.size()) {
        result.constraintDuals = raw.dual;
        result.hasDuals = !raw.dual.empty();
    }
    return result;
}

SolveResult runQp(const model::Model& reduced, const SolverOptions& options,
                  SolveResult result) {
    qp::QpModel problem;
    qp::QpTranslation translation;
    try {
        problem = qp::fromModel(reduced, translation);
    } catch (const std::exception& error) {
        result.status = SolveStatus::InvalidModel;
        result.message = error.what();
        return result;
    }

    qp::AdmmOptions engineOptions;
    engineOptions.primalTolerance = options.tolerance;
    engineOptions.dualTolerance = options.tolerance;
    engineOptions.timeLimitSeconds = options.timeLimitSeconds;
    result.executedEngine = Engine::Qp;
    const qp::AdmmResult raw = qp::QpSolver{}.solve(problem, engineOptions);

    result.status = normalise(raw.status);
    result.message = raw.statusMessage;
    result.variableValues = raw.primal;
    // fromModel negated P and q for Maximize so the ADMM engine (which only
    // ever minimises) solves an equivalent problem; undo that negation before
    // adding the offset, which is sign-independent of Maximize/Minimize. Doing
    // this in the other order -- offset then negate -- would flip the
    // offset's sign too, which is wrong: "maximize x + 100" and
    // "minimize -x - 100" are the same problem, not "minimize -x + 100".
    result.objectiveValue =
        (translation.objectiveNegated ? -raw.primalObjective : raw.primalObjective) +
        translation.objectiveOffset;
    result.iterations = raw.iterations;

    // The QP adapter appends one identity row per bounded variable, so the dual
    // vector is longer than the model's constraint list. Report only the
    // multipliers that correspond to real constraints; the trailing entries are
    // reduced costs on the variable bounds, which SolveResult has no field for.
    const std::size_t rows = reduced.constraints.size();
    if (raw.constraintDual.size() >= rows) {
        result.constraintDuals.assign(raw.constraintDual.begin(),
                                      raw.constraintDual.begin() +
                                          static_cast<std::ptrdiff_t>(rows));
        result.hasDuals = rows > 0;
    }
    return result;
}

SolveResult runBranchAndCut(const model::Model& reduced, const SolverOptions& options,
                            SolveResult result) {
    milp::MilpOptions engineOptions;
    engineOptions.timeLimitSeconds = options.timeLimitSeconds;
    result.executedEngine = Engine::BranchAndCut;
    const milp::MilpResult raw =
        milp::BranchAndBoundSolver{}.solve(reduced, engineOptions);

    result.status = normalise(raw.status);
    result.message = "branch-and-cut";
    result.variableValues = raw.primal;
    result.objectiveValue = raw.objectiveValue;
    result.nodeCount = raw.nodeCount;

    // Branch-and-cut has no meaningful dual for the integer problem. Say so by
    // leaving the vector empty rather than filling it with zeros, which a
    // caller could mistake for "every constraint is slack".
    result.hasDuals = false;
    return result;
}

}  // namespace

const char* toString(SolveStatus value) noexcept {
    switch (value) {
        case SolveStatus::Optimal:          return "optimal";
        case SolveStatus::Infeasible:       return "infeasible";
        case SolveStatus::Unbounded:        return "unbounded";
        case SolveStatus::LimitReached:     return "limit_reached";
        case SolveStatus::NumericalFailure: return "numerical_failure";
        case SolveStatus::InvalidModel:     return "invalid_model";
        case SolveStatus::Unsupported:      return "unsupported";
    }
    return "unknown";
}

SolveResult solve(const model::Model& model, const SolverOptions& options) {
    const Clock::time_point start = Clock::now();
    SolveResult result;

    if (!model.validate()) {
        result.status = SolveStatus::InvalidModel;
        result.message = "model failed structural validation";
        result.solveSeconds = secondsSince(start);
        return result;
    }

    // 1. Classify the ORIGINAL model. Presolve's reductions depend on the class.
    const Classification classification = classify(model);

    // 2. Presolve.
    presolve::Presolver presolver;
    const presolve::PresolveResult presolved = presolver.run(model);

    // 3. Dispatch on the REDUCED model.
    const DispatchDecision decision =
        dispatch(presolved.model, classification, presolved, options);
    result.engine = decision.engine;
    result.engineReason = decision.reason;

    switch (decision.engine) {
        case Engine::Infeasible:
            result.status = SolveStatus::Infeasible;
            result.message = decision.reason;
            break;

        case Engine::Trivial:
            result = solveTrivially(presolved.model, std::move(result));
            break;

        case Engine::Unsupported:
            result.status = SolveStatus::Unsupported;
            result.message = decision.reason;
            break;

        case Engine::BranchAndCut:
            result = runBranchAndCut(presolved.model, options, std::move(result));
            break;

        case Engine::Qp:
            result = runQp(presolved.model, options, std::move(result));
            break;

        case Engine::DualSimplex:
            result = runDualSimplex(presolved.model, options, std::move(result));
            break;

        case Engine::Pdlp:
            result = runPdlp(presolved.model, options, std::move(result));
            break;
    }

    result.reducedVariableCount = presolved.model.variables.size();
    result.reducedConstraintCount = presolved.model.constraints.size();
    result.maxIntegralityViolation =
        integralityViolation(presolved.model, result.variableValues);
    result.integralityRespected = result.maxIntegralityViolation <= 1e-5;
    result.solveSeconds = secondsSince(start);
    return result;
}

}  // namespace solver
