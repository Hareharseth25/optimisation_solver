#pragma once

#include "milp/dual_simplex_solver.h"
#include "milp/milp_types.h"
#include "model/model.h"

#include <limits>
#include <vector>

namespace milp {

struct HeuristicResult {
    bool foundFeasible = false;
    double objective = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> solution; // size = model.variables.size() when foundFeasible
};

struct SimpleRoundingOptions {
    // A variable within this of an integer is treated as already integral,
    // not a rounding candidate.
    double integralityTolerance = kDefaultFeasibilityTolerance * 1e4; // 1e-5

    // How much row/own-bound violation the final rounded point may carry
    // and still be reported feasible.
    double constraintFeasibilityTolerance = kDefaultFeasibilityTolerance * 1e4; // 1e-5
};

// A fast, non-iterative primal heuristic: rounds every fractional
// Integer/Binary variable in `relaxationSolution` to whichever of its two
// neighboring integers minimizes total violation (its own bounds plus every
// row it appears in, evaluated against the running point as each variable
// is committed in turn), then checks the resulting point against every row
// and variable bound exactly once. There is no repair pass -- if the
// rounded point still isn't feasible, this reports failure rather than
// iterating further; FractionalDivingHeuristic is the fallback for that.
class SimpleRoundingHeuristic {
public:
    [[nodiscard]] HeuristicResult run(
        const model::Model &model,
        const std::vector<double> &relaxationSolution,
        const SimpleRoundingOptions &options = {}) const;
};

struct FractionalDivingOptions {
    int maxDepth = 20;
    double integralityTolerance = kDefaultFeasibilityTolerance * 1e4; // 1e-5
};

// A quick single-path dive: repeatedly fixes the most-fractional
// Integer/Binary variable (the same most-fractional rule
// BranchAndBoundSolver's own branching uses) to its nearest integer and
// re-solves via DualSimplexSolver::solveFromBasis, stopping at the first
// integer-feasible point, the first non-Optimal re-solve, or
// `options.maxDepth` steps.
//
// Unlike SimpleRoundingHeuristic, every step only ever tightens a variable's
// bounds (never relaxes them), so each re-solve stays a valid restriction of
// the relaxation it started from -- the same warm-start mechanism
// BranchAndBoundSolver's own node processing uses, just followed down one
// single path instead of branched into a tree.
class FractionalDivingHeuristic {
public:
    [[nodiscard]] HeuristicResult run(
        const model::Model &model,
        const DualSimplexResult &relaxation,
        const FractionalDivingOptions &options = {}) const;
};

} // namespace milp
