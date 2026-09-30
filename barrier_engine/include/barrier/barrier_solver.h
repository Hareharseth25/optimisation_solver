#pragma once
// Infeasible primal-dual interior-point (barrier) method.
//
//   min  c'x + 0.5 x'Qx        s.t.  A x = b,   l <= x <= u
//
// Mehrotra's predictor-corrector with Gondzio's multiple centrality
// correctors, over a regularised quasidefinite augmented system.
//
// References
//   Mehrotra (1992), On the implementation of a primal-dual interior point
//     method, SIAM J. Optimization 2(4), 575-601. Predictor-corrector and the
//     adaptive centering parameter mu = (g_a/g)^2 * (g_a/n).
//   Lustig, Marsten & Shanno (1992), On implementing Mehrotra's
//     predictor-corrector interior-point method. Starting point and step-length
//     heuristics.
//   Gondzio (1996), Multiple centrality corrections in a primal-dual method for
//     linear programming. Reuses one factorisation for extra corrections;
//     reported 25-40% fewer iterations.
//   Altman & Gondzio (1999), Regularized symmetric indefinite systems in
//     interior point methods. Primal/dual regularisation into quasidefiniteness.
//   Vanderbei (1995), Symmetric quasi-definite matrices. Any symmetric
//     permutation of a quasidefinite matrix admits a stable LDL', which is what
//     lets a fill-reducing order be used without pivoting.
//   Amestoy, Davis & Duff (1996), An approximate minimum degree ordering
//     algorithm.
#include "barrier/sparse.h"

#include <limits>
#include <string>
#include <vector>

namespace barrier {

constexpr double infinity = std::numeric_limits<double>::infinity();

// min c'x + 0.5 x'Qx s.t. A x = b, l <= x <= u.
//
// Q holds the UPPER triangle by column of a symmetric positive semidefinite
// Hessian, and must be empty for an LP. Infinite bounds mean absent. A variable
// with lower == upper must be eliminated before it gets here: a fixed variable
// has no interior, so the barrier is undefined for it. The adapter does that
// substitution; the solver rejects it rather than dividing by zero.
struct BarrierProblem {
    SparseCsc constraints;  // A, m x n
    SparseCsc hessian;      // Q, n x n upper triangle, or empty
    std::vector<double> objective, rightHandSide, lower, upper;
    double objectiveOffset = 0.0;

    [[nodiscard]] Index variableCount() const noexcept { return constraints.cols; }
    [[nodiscard]] Index constraintCount() const noexcept { return constraints.rows; }
};

enum class Status {
    // All three relative measures met their tolerances.
    Optimal,
    IterationLimit,
    TimeLimit,
    // Factorisation or step computation broke down irrecoverably.
    NumericalFailure,
    InvalidProblem,
    // The iterate diverged in a way consistent with primal infeasibility or
    // dual unboundedness. Deliberately NOT called Infeasible or Unbounded:
    // this method computes no certificate for either, and a divergence
    // heuristic is not a proof. See the README.
    SuspectedInfeasibleOrUnbounded
};
[[nodiscard]] const char* toString(Status status) noexcept;

struct Options {
    int iterationLimit = 200;
    double primalTolerance = 1e-8, dualTolerance = 1e-8, gapTolerance = 1e-8;
    double timeLimitSeconds = 0.0;

    // Gondzio correctors attempted per iteration, reusing the factorisation.
    // 0 gives plain Mehrotra predictor-corrector.
    int centralityCorrectors = 2;
    // Corrector targets are pulled into [betaMin, betaMax] * mu.
    double correctorBetaMin = 0.1, correctorBetaMax = 10.0;
    // A corrector is kept only if it lengthens the step by at least this much.
    double correctorMinimumGain = 0.01;

    // Fraction-to-boundary: how much of the distance to the nearest bound a
    // step may consume. 1.0 would land exactly on the boundary and destroy the
    // interior the method depends on.
    double fractionToBoundary = 0.995;

    // Altman-Gondzio regularisation. Escalated automatically when the factor's
    // inertia disagrees with the quasidefinite prediction.
    double primalRegularization = 1e-8, dualRegularization = 1e-8;
    int refinementRounds = 2;

    // Active-set polishing after convergence; see polishToActiveSet. The
    // polished point replaces the interior one only when it verifies.
    bool polish = true;

    bool verbose = false;
};

struct Result {
    Status status = Status::InvalidProblem;
    std::string message;

    // Primal, equality multipliers, and the bound multipliers. zLower and
    // zUpper are nonnegative and supported only where the matching bound is
    // finite; elsewhere they are zero.
    std::vector<double> primal, equalityDual, lowerDual, upperDual;
    bool hasSolution = false;

    double primalObjective = 0.0, dualObjective = 0.0;
    // Relative measures, matching the convergence test.
    double primalResidual = infinity, dualResidual = infinity, relativeGap = infinity;
    double complementarity = infinity;

    int iterations = 0, factorizations = 0, correctorsAccepted = 0;
    int regularizationEscalations = 0;
    std::size_t factorNonzeros = 0;

    // Whether the returned point is the polished one, and why or why not.
    bool polished = false;
    std::string polishDetail;
    double solveSeconds = 0.0;
};

// Active-set polishing: the quadratic-programming counterpart of crossover.
//
// An interior point stops at distance ~mu/z from every active bound, never ON
// it, and leaves small nonzero multipliers on inactive ones. A QP optimum need
// not be a vertex, so crossover does not apply; instead, fix the variables in
// `activeSet` at their bounds (-1 lower, +1 upper, 0 free), solve the resulting
// equality-constrained KKT system, and recover the bound multipliers from
// stationarity. The result has exactly zero slack on active bounds and exactly
// zero multipliers on inactive ones.
//
// The KKT system is factorised with a tiny quasidefinite regularisation and
// then iteratively refined against the UNREGULARISED matrix, so the answer
// carries no regularisation bias (the approach OSQP's polishing takes).
//
// Returns false, with `detail` saying why, unless every check passes: free
// variables inside their bounds, active multipliers of the right sign, and the
// relative primal and dual residuals within tolerance. A wrong active set fails
// one of those checks rather than producing a plausible wrong answer.
struct PolishedPoint {
    std::vector<double> primal, equalityDual, lowerDual, upperDual;
    double primalObjective = 0.0, dualObjective = 0.0;
    double primalResidual = infinity, dualResidual = infinity;
    std::string detail;
    // On a failure caused by a wrong active set: the set corrected by what
    // this solve revealed (a free variable that left its box becomes active
    // at that bound; an active one with a wrong-sign multiplier is released).
    // Empty when the failure was of any other kind.
    std::vector<int> correctedActiveSet;
};
[[nodiscard]] bool polishToActiveSet(const BarrierProblem& problem,
                                     const std::vector<int>& activeSet,
                                     const Options& options, PolishedPoint& out);

class BarrierSolver {
public:
    [[nodiscard]] Result solve(const BarrierProblem& problem, const Options& options = {}) const;
};

}  // namespace barrier
