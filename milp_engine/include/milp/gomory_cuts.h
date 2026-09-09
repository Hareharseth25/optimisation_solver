#pragma once

#include "milp/dual_simplex_solver.h"
#include "milp/milp_types.h"
#include "model/model.h"

#include <vector>

namespace milp {

enum class CutSense {
    GreaterEqual // the only sense GomoryCutGenerator produces today; kept as
                 // an explicit field (rather than assumed) so a future cut
                 // family can share this struct without redesigning it.
};

// A single valid inequality over the ORIGINAL model's structural variables:
//   coeffs . x >= rhs
// `coeffs` is dense, size = model.variables.size(); entries for variables
// the cut doesn't touch are 0.
struct Cut {
    std::vector<double> coeffs;
    double rhs = 0.0;
    CutSense sense = CutSense::GreaterEqual;
};

struct GomoryCutOptions {
    // A basic variable's value must be at least this far from both floor
    // and ceiling to be treated as genuinely fractional; also guards the
    // GMI formula's f0/(1-f0) denominators from blowing up (documented in
    // the task as the "near-zero pivot element" filter).
    double integralityTolerance = kDefaultFeasibilityTolerance * 1e4; // 1e-5

    // A generated cut is dropped if the current fractional point violates
    // it by less than this.
    double minimumViolation = 1e-4;

    // A generated cut is dropped if, among its nonzero coefficients,
    // max(|coeff|) / min(|coeff|) exceeds this -- an extreme coefficient
    // spread is a numerical-stability hazard for the LP re-solve that
    // follows.
    double maxCoefficientDynamism = 1e8;
};

// Derives Gomory Mixed-Integer (GMI) cuts from an optimal DualSimplexResult's
// terminal tableau.
//
// For every row whose basic variable is a fractional-valued structural
// Integer/Binary variable, this applies the standard GMI derivation (Wolsey,
// "Integer Programming", ch. 8; Marchand & Wolsey 2001) to the tableau row
// expressed over nonbasic columns of the solver's internal unified [A | -I]
// system, then substitutes each nonbasic logical (row/slack) column's
// contribution back out via that row's own coefficients -- since a Cut must
// be expressible purely over the original model's structural variables, not
// the solver's internal slack columns. Logical columns are always treated as
// continuous in the GMI split (safe default: their coefficients aren't
// guaranteed integer even when every structural coefficient is).
//
// A row is skipped entirely -- no cut attempted -- if any nonbasic column
// feeding into it has BasisStatus::Free: the derivation assumes every
// nonbasic sits at a finite bound, and a genuinely free nonbasic column
// breaks that premise. This is expected to be rare in practice, since
// DualSimplexSolver's own cold-start crash already tries to keep nonzero-cost
// free variables out of nonbasic status.
class GomoryCutGenerator {
public:
    [[nodiscard]] std::vector<Cut> generate(
        const model::Model &model,
        const DualSimplexResult &solved,
        const GomoryCutOptions &options = {}) const;
};

} // namespace milp
