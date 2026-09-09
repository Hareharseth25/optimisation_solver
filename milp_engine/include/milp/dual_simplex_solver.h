#pragma once

#include "milp/basis_state.h"
#include "milp/milp_types.h"
#include "model/model.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace milp {

struct DualSimplexOptions {
    double primalFeasibilityTolerance = kDefaultFeasibilityTolerance;
    double dualFeasibilityTolerance = kDefaultFeasibilityTolerance;

    // Minimum |pivot element| accepted during basis inversion and the ratio
    // test; anything smaller is treated as structurally zero.
    double pivotTolerance = 1e-9;

    std::int64_t iterationLimit = 100000;

    // Wall-clock budget for the PIVOT LOOP. 0 (the default) means unlimited,
    // which is what branch-and-cut wants: it calls this solver once per node
    // and budgets the SEARCH, not the individual relaxation.
    //
    // Without this the orchestrator had no way to honour
    // SolverOptions::timeLimitSeconds on the bare-LP path -- measured, a
    // 2500-row LP under requireVertexSolution ran 17.4s against a 1.0s budget
    // because the option was dropped at the engine boundary.
    //
    // It bounds iterations, NOT setup. Problem construction, the basis matrix
    // and its inversion all run before the first deadline check and are not
    // preemptible, so a solve can still overrun this by the setup cost:
    // measured on a denser 1200-row LP, a 0.05s budget returned TimeLimit
    // after 19 pivots but 0.598s wall clock, essentially all of it setup.
    // Treat this as "stop pivoting after roughly this long", not as a hard
    // real-time guarantee.
    double timeLimitSeconds = 0.0;

    // When true, the leaving row is chosen by (infeasibility^2 / dual
    // steepest-edge weight) instead of plain largest-infeasibility (Dantzig)
    // pricing. Weights are recomputed exactly from the maintained dense basis
    // inverse each iteration (this engine keeps a dense inverse throughout,
    // so an exact recompute costs the same order as the rest of the
    // iteration) rather than updated incrementally/approximately as
    // industrial solvers do for speed.
    bool useDualSteepestEdge = true;
};

enum class DualSimplexStatus {
    Optimal,
    Infeasible,
    Unbounded,
    IterationLimit,

    // Stopped on timeLimitSeconds. Distinct from IterationLimit so a caller
    // can tell "needs more time" from "needs more pivots"; both mean the
    // solve ended without a proof either way, so every existing consumer --
    // all of which test `!= Optimal` -- keeps working unchanged.
    TimeLimit
};

// The terminal basis inverse and enough bookkeeping to reconstruct any
// tableau row on demand, over the solver's internal unified column space:
// structural columns are [0, numStructural), logical (one per row, the slack
// implied by that row's own bounds) columns are
// [numStructural, numStructural + numRows) -- see dual_simplex_solver.cpp's
// header comment on `Problem` for the full [A | -I] formulation this indexes
// into. Populated from whatever basis the solver ended on, regardless of
// status, so a cut generator (e.g. GomoryCutGenerator) can derive cuts from
// an Optimal result without re-solving or re-factorizing the basis.
struct SimplexTableau {
    int numStructural = 0; // n
    int numRows = 0;       // m

    std::vector<int> basisColumns;                 // row -> column index, size numRows
    std::vector<std::vector<double>> basisInverse;  // numRows x numRows

    // Per-column bookkeeping over the full unified column space (size
    // numStructural + numRows), matching BasisState's status convention:
    // a nonbasic column sits at columnLower or columnUpper depending on
    // columnStatus.
    std::vector<BasisStatus> columnStatus;
    std::vector<double> columnLower;
    std::vector<double> columnUpper;
};

struct DualSimplexResult {
    DualSimplexStatus status = DualSimplexStatus::IterationLimit;

    double objectiveValue = std::numeric_limits<double>::quiet_NaN();

    std::vector<double> primal; // structural variable values, size = model.variables.size()
    std::vector<double> dual;   // one multiplier per constraint row; empty when Infeasible/Unbounded

    // Populated only when status == Infeasible: a row-weight vector proving
    // no point can satisfy every row/variable bound simultaneously (see
    // dual_simplex_solver.cpp for its derivation from the basis inverse and
    // its sign convention).
    std::vector<double> dualFarkasRay;

    // Valid for Optimal and IterationLimit (a usable, if not yet optimal,
    // warm-start point); reflects the basis at the point of detection for
    // Infeasible; default-constructed (empty) for Unbounded, which is
    // detected before any basis is built.
    BasisState basis;

    // See SimplexTableau's own comment for what this carries and when.
    SimplexTableau tableau;

    std::int64_t iterations = 0;
};

// A dense, bounded-variable revised dual simplex solver over model::Model.
//
// This is deliberately a dense reference implementation: the basis inverse is
// maintained as a full m x m matrix, updated in O(m^2) per pivot via the
// standard product-form-of-inverse formula, rather than a sparse LU
// factorization. That is the right tradeoff for what this solver exists to
// do -- resolve branch-and-cut node relaxations after a handful of bound
// changes from a parent's basis -- not for solving very large sparse LPs
// from scratch (pdlp_engine already covers that case).
//
// VariableType (Integer/Binary) is ignored: this solves the LP relaxation
// only. Integrality enforcement belongs to the branch-and-cut driver built on
// top of this engine, not to the relaxation solver itself.
class DualSimplexSolver {
public:
    // Cold start: builds the classic all-slack starting basis, then crashes
    // any structural variable whose available bound cannot be made dual
    // feasible against it (a free variable, or a one-sided-bound variable
    // whose cost sign disagrees with that bound) directly into the basis
    // before running Phase 2.
    [[nodiscard]] DualSimplexResult solve(
        const model::Model &model,
        const DualSimplexOptions &options = {}) const;

    // Warm start: reconstructs and refactorizes the basis matrix from
    // `initialBasis`'s status arrays alone (no primal/dual values are
    // stored in a BasisState) and runs Phase 2 directly from it.
    //
    // This assumes `initialBasis` is dual feasible for `model` -- true by
    // construction for the intended use (a parent branch-and-cut node's
    // optimal basis, re-solved after tightening a variable bound, since
    // bound changes alone cannot change reduced-cost signs). It is not
    // re-verified here; an inconsistent basis produces best-effort results,
    // not a diagnostic. Throws std::invalid_argument if `initialBasis`'s
    // sizes don't match `model`, or if exactly model.constraints.size()
    // columns aren't marked Basic, or if the resulting basis matrix is
    // numerically singular.
    [[nodiscard]] DualSimplexResult solveFromBasis(
        const model::Model &model,
        const BasisState &initialBasis,
        const DualSimplexOptions &options = {}) const;
};

} // namespace milp
