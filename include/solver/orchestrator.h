#pragma once

#include "model/model.h"
#include "solver/classifier.h"
#include "solver/dispatcher.h"
#include "solver/solve_result.h"

namespace solver {

// The whole pipeline behind one call.
//
//   classify (before presolve)  ->  presolve  ->  dispatch (after presolve)
//   ->  the chosen engine  ->  one normalised SolveResult
//
// Classification happens before presolve because presolve's reductions depend
// on the problem class; dispatch happens after because presolve changes the
// size, can eliminate every integer variable, and can settle infeasibility
// outright.
//
// The returned values are in the coordinates of `model` as handed in. Mapping
// back through presolve's own reductions is postsolve's job and is not done
// here; solveReduced() exposes the reduced-model result for that layer.
[[nodiscard]] SolveResult solve(
    const model::Model& model,
    const SolverOptions& options = {}
);

}  // namespace solver
