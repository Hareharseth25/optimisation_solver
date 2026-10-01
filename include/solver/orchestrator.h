#pragma once

#include "model/model.h"
#include "solver/nlp.h"
#include "solver/classifier.h"
#include "solver/dispatcher.h"
#include "solver/solve_report.h"
#include "solver/solve_result.h"

namespace solver {

// The whole pipeline behind one call.
//
//   classify (before presolve)  ->  presolve  ->  dispatch (after presolve)
//   ->  the chosen engine  ->  postsolve  ->  original-model SolveResult
//
// Classification happens before presolve because presolve's reductions depend
// on the problem class; dispatch happens after because presolve changes the
// size, can eliminate every integer variable, and can settle infeasibility
// outright.
//
// Primal values, shadow prices and reduced costs are reconstructed and validated
// in the original model's coordinates. Presolve runs once, and its same metadata
// is used for reconstruction. Check hasPrimal/hasDuals before consuming vectors.
//
// `report`, when non-null, is overwritten with how this same run went (see
// solver/solve_report.h). Requesting it changes nothing about the solve: no
// stage runs twice, and the returned SolveResult is identical either way.
[[nodiscard]] SolveResult solve(
    const model::Model& model,
    const SolverOptions& options = {},
    SolveReport* report = nullptr
);

// Solves a model that has ALREADY been reduced/presolved.
//
// Dispatches directly to the selected engine on `presolvedModel` using the
// `classification` of the original model, and normalises the result into a
// SolveResult in the coordinates of `presolvedModel`.
// Unlike solve(), this does NOT run presolve or expand into another model's
// coordinates or invoke postsolve reconstruction. It validates primal/dual
// results directly against presolvedModel itself.
//
// `report`, when non-null, is overwritten as for solve(). Its presolve and
// postsolve sections stay empty because neither stage runs here, and its
// classification is the one supplied.
[[nodiscard]] SolveResult solveReduced(
    const model::Model& presolvedModel,
    const Classification& classification,
    const SolverOptions& options = {},
    SolveReport* report = nullptr
);

}  // namespace solver
