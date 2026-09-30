#pragma once
// Crossover: from an optimal interior point to an optimal vertex.
//
// Why it exists. The barrier method converges to the analytic centre of the
// optimal face, strictly inside every bound. Its multipliers on inactive
// constraints are small but never exactly zero, and it never lands exactly ON
// an active bound. Everything downstream in this pipeline is vertex-shaped:
// postsolve hands a reduced cost back to an eliminated singleton row only when
// the variable sits exactly on the bound that row produced, and the benchmark
// checker demands exactly-zero duals on inactive rows. Measured on Netlib
// adlittle, the raw interior point left 1.83 of reduced cost stranded on a bound
// that is not active in the original model, so postsolve rightly withheld the
// duals.
//
// Production barrier codes (CPLEX, Gurobi, MOSEK) close this gap the same way:
// barrier, then crossover, then simplex cleanup. Here that is
//
//   1. identify a basis from the interior point, ranking every column by the
//      ratio of its distance to the nearest bound to its multiplier magnitude
//      (large -> basic, small -> nonbasic at that bound); complementarity makes
//      the two groups separate by many orders of magnitude near optimality;
//   2. pick exactly m linearly independent basic columns greedily in that
//      order, completing with row logicals, which always succeeds because
//      [A | I] has full row rank;
//   3. hand the basis to the dual simplex's warm start for the few cleanup
//      pivots needed to reach an exact vertex with exact duals.
//
// Safe by construction: the vertex is accepted ONLY if the simplex reports
// Optimal and its objective agrees with the interior point's. Otherwise the
// caller keeps the barrier result unchanged. Crossover can add a vertex; it can
// never make a result worse.
//
// Scope: linear objectives only (the dual simplex is an LP method), and only
// within the dense dual simplex's own size limits.
#include "model/model.h"
#include "solver/dispatcher.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace solver {

struct CrossoverResult {
    bool applied = false;  // true only when a verified vertex replaced the point
    std::string detail;    // what happened, or why crossover was not applied
    std::vector<double> primal, duals;  // model's own sense, when applied
    double objective = 0.0;
    std::int64_t pivots = 0;
    std::size_t basicStructural = 0, basicLogical = 0;
};

[[nodiscard]] CrossoverResult crossoverToVertex(const model::Model& model,
                                                const std::vector<double>& interiorPrimal,
                                                const std::vector<double>& interiorDuals,
                                                double interiorObjective,
                                                const SolverOptions& options);

}  // namespace solver
