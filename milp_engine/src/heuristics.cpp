#include "milp/heuristics.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace milp {
namespace {

double boundViolation(double value, double lower, double upper) {
    double violation = 0.0;
    if (value < lower) {
        violation += lower - value;
    }
    if (value > upper) {
        violation += value - upper;
    }
    return violation;
}

} // namespace

HeuristicResult SimpleRoundingHeuristic::run(
    const model::Model &model,
    const std::vector<double> &relaxationSolution,
    const SimpleRoundingOptions &options) const
{
    HeuristicResult result;

    const std::size_t n = model.variables.size();
    const std::size_t m = model.constraints.size();
    if (relaxationSolution.size() != n) {
        return result;
    }

    // Reverse index: for each variable, the (row, coefficient) pairs it
    // appears in, so evaluating a candidate rounding only touches the rows
    // that variable actually affects.
    std::vector<std::vector<std::pair<int, double>>> columnEntries(n);
    for (std::size_t i = 0; i < m; ++i) {
        for (const auto &term : model.constraints[i].linearTerms) {
            columnEntries[static_cast<std::size_t>(term.variableIndex)].push_back(
                {static_cast<int>(i), term.value});
        }
    }

    std::vector<double> point = relaxationSolution;

    std::vector<double> activity(m, 0.0);
    for (std::size_t i = 0; i < m; ++i) {
        double sum = 0.0;
        for (const auto &term : model.constraints[i].linearTerms) {
            sum += term.value * point[static_cast<std::size_t>(term.variableIndex)];
        }
        activity[i] = sum;
    }

    const auto rowViolation = [&model](std::size_t row, double value) {
        return boundViolation(value, model.constraints[row].lowerBound, model.constraints[row].upperBound);
    };

    for (std::size_t j = 0; j < n; ++j) {
        const model::Variable &variable = model.variables[j];
        if (variable.type == model::VariableType::Continuous) {
            continue;
        }

        const double value = point[j];
        const double nearest = std::round(value);
        if (std::abs(value - nearest) <= options.integralityTolerance) {
            continue; // already (near) integral -- nothing to decide.
        }

        const double floorVal = std::floor(value);
        const double ceilVal = std::ceil(value);

        double bestViolation = std::numeric_limits<double>::infinity();
        double bestValue = floorVal;

        for (double candidate : {floorVal, ceilVal}) {
            double violation = boundViolation(candidate, variable.lowerBound, variable.upperBound);

            const double delta = candidate - value;
            for (const auto &entry : columnEntries[j]) {
                const auto row = static_cast<std::size_t>(entry.first);
                const double candidateActivity = activity[row] + entry.second * delta;
                violation += rowViolation(row, candidateActivity);
            }

            if (violation < bestViolation) {
                bestViolation = violation;
                bestValue = candidate;
            }
        }

        const double delta = bestValue - point[j];
        for (const auto &entry : columnEntries[j]) {
            activity[static_cast<std::size_t>(entry.first)] += entry.second * delta;
        }
        point[j] = bestValue;
    }

    for (std::size_t j = 0; j < n; ++j) {
        if (boundViolation(point[j], model.variables[j].lowerBound, model.variables[j].upperBound) >
            options.constraintFeasibilityTolerance) {
            return result;
        }
    }
    for (std::size_t i = 0; i < m; ++i) {
        if (rowViolation(i, activity[i]) > options.constraintFeasibilityTolerance) {
            return result;
        }
    }

    double objective = model.objective.offset;
    for (const auto &term : model.objective.linearTerms) {
        objective += term.value * point[static_cast<std::size_t>(term.variableIndex)];
    }

    result.foundFeasible = true;
    result.objective = objective;
    result.solution = std::move(point);
    return result;
}

HeuristicResult FractionalDivingHeuristic::run(
    const model::Model &model,
    const DualSimplexResult &relaxation,
    const FractionalDivingOptions &options) const
{
    HeuristicResult result;

    if (relaxation.status != DualSimplexStatus::Optimal) {
        return result;
    }

    const std::size_t n = model.variables.size();
    if (relaxation.primal.size() != n) {
        return result;
    }

    DualSimplexSolver solver;
    model::Model workingModel = model;
    DualSimplexResult current = relaxation;

    for (int step = 0; step < options.maxDepth; ++step) {
        int branchVariable = -1;
        double bestDistance = -1.0;

        for (std::size_t j = 0; j < n; ++j) {
            if (model.variables[j].type == model::VariableType::Continuous) {
                continue;
            }
            const double value = current.primal[j];
            const double nearestInteger = std::round(value);
            if (std::abs(value - nearestInteger) <= options.integralityTolerance) {
                continue;
            }
            const double fractional = value - std::floor(value);
            const double distance = std::min(fractional, 1.0 - fractional);
            if (distance > bestDistance) {
                bestDistance = distance;
                branchVariable = static_cast<int>(j);
            }
        }

        if (branchVariable < 0) {
            result.foundFeasible = true;
            result.objective = current.objectiveValue;
            result.solution = current.primal;
            return result;
        }

        const auto idx = static_cast<std::size_t>(branchVariable);
        const double nearestValue = std::round(current.primal[idx]);

        // A dive step must only ever tighten bounds, never relax them. If
        // the rounding target falls outside the variable's own current
        // bounds -- possible only if those bounds are themselves fractional,
        // an unusual input for an integer variable -- fixing it there would
        // silently widen the feasible region instead. Bail out rather than
        // risk producing a point infeasible for the original problem.
        if (nearestValue < workingModel.variables[idx].lowerBound - options.integralityTolerance ||
            nearestValue > workingModel.variables[idx].upperBound + options.integralityTolerance) {
            return result;
        }

        workingModel.variables[idx].lowerBound = nearestValue;
        workingModel.variables[idx].upperBound = nearestValue;

        const DualSimplexResult next = solver.solveFromBasis(workingModel, current.basis, DualSimplexOptions{});
        if (next.status != DualSimplexStatus::Optimal) {
            return result; // dive hit infeasibility (or otherwise failed to converge)
        }

        current = next;
    }

    return result; // exhausted maxDepth without reaching an integer point
}

} // namespace milp
