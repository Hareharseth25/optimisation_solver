#include "barrier/barrier_solver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace barrier {

const char* toString(Status status) noexcept {
    switch (status) {
        case Status::Optimal: return "Optimal";
        case Status::IterationLimit: return "IterationLimit";
        case Status::TimeLimit: return "TimeLimit";
        case Status::NumericalFailure: return "NumericalFailure";
        case Status::InvalidProblem: return "InvalidProblem";
        case Status::SuspectedInfeasibleOrUnbounded: return "SuspectedInfeasibleOrUnbounded";
    }
    return "Unknown";
}

namespace {

using Vec = std::vector<double>;

double infinityNorm(const Vec& v) {
    double n = 0.0;
    for (double a : v) n = std::max(n, std::abs(a));
    return n;
}

struct Direction {
    Vec x, y, lowerDual, upperDual;
};

bool diverging(const Vec& x, const Vec& y, const Vec& zLower, const Vec& zUpper,
               double threshold = 1e12) {
    return infinityNorm(x) > threshold || infinityNorm(y) > threshold ||
           infinityNorm(zLower) > threshold || infinityNorm(zUpper) > threshold;
}

// The augmented system
//
//   [ -(Q + D + Rp)   A' ] [dx]   [ -rd_hat ]
//   [      A          Rd ] [dy] = [   rp    ]
//
// assembled once and then refreshed in place. A and Q never change across
// iterations; only the two diagonal blocks do, so the pattern, the ordering and
// the symbolic analysis are computed a single time and every iteration pays
// only for the numeric factorisation. Rebuilding the matrix each iteration
// instead would make the ordering the dominant cost.
class AugmentedSystem {
public:
    bool build(const BarrierProblem& problem) {
        n_ = problem.variableCount();
        m_ = problem.constraintCount();
        total_ = n_ + m_;
        hessianDiagonal_.assign(static_cast<std::size_t>(n_), 0.0);

        TripletBuilder builder(total_, total_);
        for (Index j = 0; j < n_; ++j) builder.add(j, j, 0.0);  // reserve the diagonal
        const SparseCsc& q = problem.hessian;
        if (q.cols == n_) {
            for (Index j = 0; j < n_; ++j)
                for (Index p = q.columnStart[j]; p < q.columnStart[j + 1]; ++p) {
                    const Index i = q.rowIndex[p];
                    if (i > j) return false;  // must be the upper triangle
                    if (i == j) hessianDiagonal_[static_cast<std::size_t>(j)] = q.value[p];
                    else builder.add(i, j, -q.value[p]);
                }
        }
        const SparseCsc& a = problem.constraints;
        for (Index j = 0; j < n_; ++j)
            for (Index p = a.columnStart[j]; p < a.columnStart[j + 1]; ++p)
                builder.add(j, n_ + a.rowIndex[p], a.value[p]);
        for (Index i = 0; i < m_; ++i) builder.add(n_ + i, n_ + i, 0.0);

        const SparseCsc unordered = builder.build();
        if (!unordered.validate()) return false;

        std::vector<Index> permutation;
        approximateMinimumDegree(unordered, permutation);
        matrix_ = permuteSymmetric(unordered, permutation);
        if (!matrix_.validate()) return false;

        // Locate each original index's diagonal entry in the PERMUTED matrix.
        // A permutation maps (j,j) to (inverse[j], inverse[j]), so diagonals
        // stay diagonal and this mapping is well defined.
        diagonalSlot_.assign(static_cast<std::size_t>(total_), -1);
        for (Index k = 0; k < total_; ++k) {
            for (Index p = matrix_.columnStart[k]; p < matrix_.columnStart[k + 1]; ++p)
                if (matrix_.rowIndex[p] == k) {
                    diagonalSlot_[static_cast<std::size_t>(permutation[static_cast<std::size_t>(k)])] = p;
                    break;
                }
        }
        for (Index i = 0; i < total_; ++i)
            if (diagonalSlot_[static_cast<std::size_t>(i)] < 0) return false;

        inverse_.assign(static_cast<std::size_t>(total_), 0);
        for (Index k = 0; k < total_; ++k)
            inverse_[static_cast<std::size_t>(permutation[static_cast<std::size_t>(k)])] = k;
        return factorization_.analyse(matrix_);
    }

    // Refresh the diagonals and refactorise. Returns false when the factor is
    // unusable or its inertia contradicts quasidefiniteness.
    bool refactor(const Vec& scaling, double primalRegularization, double dualRegularization) {
        for (Index j = 0; j < n_; ++j)
            matrix_.value[static_cast<std::size_t>(diagonalSlot_[static_cast<std::size_t>(j)])] =
                -(hessianDiagonal_[static_cast<std::size_t>(j)] +
                  scaling[static_cast<std::size_t>(j)] + primalRegularization);
        for (Index i = 0; i < m_; ++i)
            matrix_.value[static_cast<std::size_t>(diagonalSlot_[static_cast<std::size_t>(n_ + i)])] =
                dualRegularization;
        if (!factorization_.factor(matrix_, 1e-14)) return false;
        // Inertia check. A quasidefinite matrix of this shape has exactly n
        // negative pivots; anything else means the regularisation was too weak
        // for this scaling and the factor cannot be trusted. Checking it is far
        // cheaper than discovering the bad direction downstream.
        return factorization_.negativePivots() == n_;
    }

    // Solves for (dx, dy) given the two right-hand-side blocks.
    void solve(const Vec& top, const Vec& bottom, Vec& dx, Vec& dy, int refinementRounds) const {
        scratch_.assign(static_cast<std::size_t>(total_), 0.0);
        for (Index j = 0; j < n_; ++j)
            scratch_[static_cast<std::size_t>(inverse_[static_cast<std::size_t>(j)])] =
                top[static_cast<std::size_t>(j)];
        for (Index i = 0; i < m_; ++i)
            scratch_[static_cast<std::size_t>(inverse_[static_cast<std::size_t>(n_ + i)])] =
                bottom[static_cast<std::size_t>(i)];
        factorization_.solveRefined(matrix_, scratch_, refinementRounds);
        dx.resize(static_cast<std::size_t>(n_));
        dy.resize(static_cast<std::size_t>(m_));
        for (Index j = 0; j < n_; ++j)
            dx[static_cast<std::size_t>(j)] =
                scratch_[static_cast<std::size_t>(inverse_[static_cast<std::size_t>(j)])];
        for (Index i = 0; i < m_; ++i)
            dy[static_cast<std::size_t>(i)] =
                scratch_[static_cast<std::size_t>(inverse_[static_cast<std::size_t>(n_ + i)])];
    }

    [[nodiscard]] std::size_t factorNonzeros() const noexcept { return factorization_.factorNonzeros(); }

private:
    Index n_ = 0, m_ = 0, total_ = 0;
    SparseCsc matrix_;
    LdlFactorization factorization_;
    std::vector<Index> diagonalSlot_, inverse_;
    Vec hessianDiagonal_;
    mutable Vec scratch_;
};

}  // namespace

bool polishToActiveSet(const BarrierProblem& problem, const std::vector<int>& activeSet,
                       const Options& options, PolishedPoint& out) {
    out = PolishedPoint{};
    const Index n = problem.variableCount(), m = problem.constraintCount();
    const auto un = static_cast<std::size_t>(n), um = static_cast<std::size_t>(m);
    if (activeSet.size() != un) { out.detail = "active set has the wrong length"; return false; }

    // Fix the active variables; number the free ones.
    std::vector<Index> freeIndex(un, -1);
    Vec fixedPart(un, 0.0);
    Index freeCount = 0;
    for (Index j = 0; j < n; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (activeSet[uj] < 0) {
            if (!std::isfinite(problem.lower[uj])) { out.detail = "active at a missing lower bound"; return false; }
            fixedPart[uj] = problem.lower[uj];
        } else if (activeSet[uj] > 0) {
            if (!std::isfinite(problem.upper[uj])) { out.detail = "active at a missing upper bound"; return false; }
            fixedPart[uj] = problem.upper[uj];
        } else {
            freeIndex[uj] = freeCount++;
        }
    }
    if (freeCount == 0) {
        out.detail = "no free variables, so the equality multipliers are not determined";
        return false;
    }

    // Stationarity on the free variables and feasibility, with x_A fixed:
    //   [ -Q_FF   A_F' ] [x_F]   [ c_F + Q_FA x_A ]
    //   [  A_F     0   ] [ y ] = [ b - A_A x_A    ]
    const bool curved = problem.hessian.cols == n;
    Vec hessianFixed(un, 0.0), constraintFixed;
    if (curved) multiplySymmetric(problem.hessian, fixedPart, hessianFixed);
    problem.constraints.multiply(fixedPart, constraintFixed);
    const Index total = freeCount + m;
    Vec rhs(static_cast<std::size_t>(total), 0.0);
    for (Index j = 0; j < n; ++j) {
        const Index f = freeIndex[static_cast<std::size_t>(j)];
        if (f >= 0) rhs[static_cast<std::size_t>(f)] =
            problem.objective[static_cast<std::size_t>(j)] + hessianFixed[static_cast<std::size_t>(j)];
    }
    for (Index i = 0; i < m; ++i)
        rhs[static_cast<std::size_t>(freeCount + i)] =
            problem.rightHandSide[static_cast<std::size_t>(i)] - constraintFixed[static_cast<std::size_t>(i)];

    TripletBuilder builder(total, total);
    for (Index k = 0; k < total; ++k) builder.add(k, k, 0.0);  // diagonal always in the pattern
    if (curved) {
        const SparseCsc& q = problem.hessian;
        for (Index j = 0; j < n; ++j)
            for (Index p = q.columnStart[j]; p < q.columnStart[j + 1]; ++p) {
                Index fi = freeIndex[static_cast<std::size_t>(q.rowIndex[p])];
                Index fj = freeIndex[static_cast<std::size_t>(j)];
                if (fi < 0 || fj < 0) continue;
                if (fi > fj) std::swap(fi, fj);
                builder.add(fi, fj, -q.value[p]);
            }
    }
    const SparseCsc& a = problem.constraints;
    for (Index j = 0; j < n; ++j) {
        const Index f = freeIndex[static_cast<std::size_t>(j)];
        if (f < 0) continue;
        for (Index p = a.columnStart[j]; p < a.columnStart[j + 1]; ++p)
            builder.add(f, freeCount + a.rowIndex[p], a.value[p]);
    }
    const SparseCsc unordered = builder.build();
    std::vector<Index> permutation;
    approximateMinimumDegree(unordered, permutation);
    const SparseCsc exact = permuteSymmetric(unordered, permutation);
    std::vector<Index> inverse(static_cast<std::size_t>(total));
    for (Index k = 0; k < total; ++k) inverse[static_cast<std::size_t>(permutation[static_cast<std::size_t>(k)])] = k;

    // Factor a quasidefinite perturbation; refine against the exact matrix so
    // the perturbation leaves no bias in the answer. With a very small delta,
    // roundoff can flip a pivot's sign; because refinement is against the
    // EXACT matrix, a larger delta changes only how fast refinement converges,
    // not the answer it converges to, so escalate on an inertia failure.
    LdlFactorization factorization;
    SparseCsc regularised;
    bool factored = false;
    for (const double delta : {1e-10, 1e-8, 1e-6}) {
        regularised = exact;
        for (Index k = 0; k < total; ++k) {
            const Index column = inverse[static_cast<std::size_t>(k)];
            for (Index p = regularised.columnStart[column]; p < regularised.columnStart[column + 1]; ++p)
                if (regularised.rowIndex[p] == column) {
                    regularised.value[static_cast<std::size_t>(p)] += k < freeCount ? -delta : delta;
                    break;
                }
        }
        if (!factorization.analyse(regularised) || !factorization.factor(regularised, 1e-14)) continue;
        if (factorization.negativePivots() != freeCount) continue;
        factored = true;
        break;
    }
    if (!factored) {
        out.detail = "polishing system could not be factorised with the predicted inertia";
        return false;
    }
    Vec solution(static_cast<std::size_t>(total));
    for (Index k = 0; k < total; ++k)
        solution[static_cast<std::size_t>(inverse[static_cast<std::size_t>(k)])] = rhs[static_cast<std::size_t>(k)];
    factorization.solveRefined(exact, solution, 20);

    Vec x = fixedPart, y(um, 0.0);
    for (Index j = 0; j < n; ++j) {
        const Index f = freeIndex[static_cast<std::size_t>(j)];
        if (f >= 0) x[static_cast<std::size_t>(j)] =
            solution[static_cast<std::size_t>(inverse[static_cast<std::size_t>(f)])];
    }
    for (Index i = 0; i < m; ++i)
        y[static_cast<std::size_t>(i)] =
            solution[static_cast<std::size_t>(inverse[static_cast<std::size_t>(freeCount + i)])];
    for (double v : x) if (!std::isfinite(v)) { out.detail = "polished point is not finite"; return false; }
    for (double v : y) if (!std::isfinite(v)) { out.detail = "polished multipliers are not finite"; return false; }

    // ---- verification --------------------------------------------------
    const double objectiveScale = std::max(1.0, infinityNorm(problem.objective));
    const double rhsScale = std::max(1.0, infinityNorm(problem.rightHandSide));
    // A free variable outside its box means a bound that should have been
    // active was not: the active set is wrong. The correction activates only
    // the MOST violated bound. Activating every violator at once overshoots:
    // on  min x^2 - 4x, x + y = 1.5, 0 <= x <= 1, y >= 0  with both free, x = 2
    // and y = -0.5 both leave their boxes, but only x belongs at a bound --
    // fixing both makes the row infeasible and the correction dead-ends. That
    // is why active-set methods add one constraint at a time.
    std::vector<int> corrected = activeSet;
    bool leftBox = false;
    double worst = 0.0;
    Index worstIndex = -1;
    int worstSide = 0;
    for (Index j = 0; j < n; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (activeSet[uj] != 0) continue;
        const double l = problem.lower[uj], u = problem.upper[uj];
        double violation = 0.0;
        int side = 0;
        if (std::isfinite(l) && x[uj] < l - 1e-9 * (1.0 + std::abs(l))) { violation = l - x[uj]; side = -1; }
        else if (std::isfinite(u) && x[uj] > u + 1e-9 * (1.0 + std::abs(u))) { violation = x[uj] - u; side = 1; }
        if (side != 0 && violation > worst) { worst = violation; worstIndex = j; worstSide = side; leftBox = true; }
    }
    if (leftBox) {
        corrected[static_cast<std::size_t>(worstIndex)] = worstSide;
        out.detail = "a free variable left its bounds, so the active set was misidentified";
        out.correctedActiveSet = std::move(corrected);
        return false;
    }
    for (Index j = 0; j < n; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (activeSet[uj] != 0) continue;
        if (std::isfinite(problem.lower[uj])) x[uj] = std::max(x[uj], problem.lower[uj]);
        if (std::isfinite(problem.upper[uj])) x[uj] = std::min(x[uj], problem.upper[uj]);
    }
    Vec qx(un, 0.0), aty;
    if (curved) multiplySymmetric(problem.hessian, x, qx);
    problem.constraints.transposeMultiply(y, aty);
    // Bound multipliers come from stationarity. A wrong sign means the
    // variable was held at a bound it wants to leave: the active set is wrong
    // and this is not a KKT point. Every such variable is released in the
    // corrected set.
    Vec zLower(un, 0.0), zUpper(un, 0.0);
    double dualResidual = 0.0;
    bool wrongSign = false;
    const double signTolerance = options.dualTolerance * (1.0 + objectiveScale);
    for (Index j = 0; j < n; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        const double gradient = problem.objective[uj] + qx[uj] - aty[uj];
        if (activeSet[uj] < 0) {
            if (gradient < -signTolerance) { corrected[uj] = 0; wrongSign = true; }
            zLower[uj] = std::max(gradient, 0.0);
        } else if (activeSet[uj] > 0) {
            if (-gradient < -signTolerance) { corrected[uj] = 0; wrongSign = true; }
            zUpper[uj] = std::max(-gradient, 0.0);
        }
        dualResidual = std::max(dualResidual, std::abs(gradient - zLower[uj] + zUpper[uj]));
    }
    if (wrongSign) {
        out.detail = "an active bound has a negative multiplier, so the active set was misidentified";
        out.correctedActiveSet = std::move(corrected);
        return false;
    }
    Vec ax;
    problem.constraints.multiply(x, ax);
    double primalResidual = 0.0;
    for (Index i = 0; i < m; ++i)
        primalResidual = std::max(primalResidual,
            std::abs(problem.rightHandSide[static_cast<std::size_t>(i)] - ax[static_cast<std::size_t>(i)]));
    out.primalResidual = primalResidual / (1.0 + rhsScale);
    out.dualResidual = dualResidual / (1.0 + objectiveScale);
    if (!(out.primalResidual <= options.primalTolerance) || !(out.dualResidual <= options.dualTolerance)) {
        out.detail = "polished point failed the residual tolerances";
        return false;
    }

    double quadratic = 0.0, linear = 0.0;
    for (Index j = 0; j < n; ++j) {
        quadratic += x[static_cast<std::size_t>(j)] * qx[static_cast<std::size_t>(j)];
        linear += problem.objective[static_cast<std::size_t>(j)] * x[static_cast<std::size_t>(j)];
    }
    out.primalObjective = linear + 0.5 * quadratic + problem.objectiveOffset;
    double dual = problem.objectiveOffset - 0.5 * quadratic;
    for (Index i = 0; i < m; ++i)
        dual += problem.rightHandSide[static_cast<std::size_t>(i)] * y[static_cast<std::size_t>(i)];
    for (Index j = 0; j < n; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (zLower[uj] > 0.0) dual += problem.lower[uj] * zLower[uj];
        if (zUpper[uj] > 0.0) dual -= problem.upper[uj] * zUpper[uj];
    }
    out.dualObjective = dual;
    out.primal = std::move(x);
    out.equalityDual = std::move(y);
    out.lowerDual = std::move(zLower);
    out.upperDual = std::move(zUpper);
    out.detail = "polished to the identified active set (" +
                 std::to_string(n - freeCount) + " active bounds)";
    return true;
}

Result BarrierSolver::solve(const BarrierProblem& problem, const Options& options) const {
    Result result;
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };
    const auto finish = [&](Status status, std::string message) {
        result.status = status;
        result.message = std::move(message);
        result.solveSeconds = elapsed();
        return result;
    };

    const Index n = problem.variableCount(), m = problem.constraintCount();
    const auto un = static_cast<std::size_t>(n), um = static_cast<std::size_t>(m);
    if (!problem.constraints.validate() ||
        problem.objective.size() != un || problem.rightHandSide.size() != um ||
        problem.lower.size() != un || problem.upper.size() != un)
        return finish(Status::InvalidProblem, "barrier problem dimensions are inconsistent");
    if (options.iterationLimit < 0 || !(options.primalTolerance > 0) ||
        !(options.dualTolerance > 0) || !(options.gapTolerance > 0) ||
        !(options.fractionToBoundary > 0 && options.fractionToBoundary < 1) ||
        !(options.primalRegularization > 0) || !(options.dualRegularization > 0) ||
        options.refinementRounds < 0 || options.centralityCorrectors < 0)
        return finish(Status::InvalidProblem, "invalid barrier options");
    for (Index j = 0; j < n; ++j) {
        const double l = problem.lower[static_cast<std::size_t>(j)];
        const double u = problem.upper[static_cast<std::size_t>(j)];
        if (std::isnan(l) || std::isnan(u) || l > u)
            return finish(Status::InvalidProblem, "variable bounds are invalid");
        if (l == u)
            return finish(Status::InvalidProblem,
                          "a fixed variable has no interior; eliminate it before the barrier method");
    }

    std::vector<char> hasLower(un, 0), hasUpper(un, 0);
    int boundCount = 0;
    for (Index j = 0; j < n; ++j) {
        hasLower[static_cast<std::size_t>(j)] = std::isfinite(problem.lower[static_cast<std::size_t>(j)]);
        hasUpper[static_cast<std::size_t>(j)] = std::isfinite(problem.upper[static_cast<std::size_t>(j)]);
        boundCount += hasLower[static_cast<std::size_t>(j)] + hasUpper[static_cast<std::size_t>(j)];
    }

    AugmentedSystem system;
    if (!system.build(problem))
        return finish(Status::NumericalFailure,
                      "could not assemble or analyse the augmented system");
    result.factorNonzeros = system.factorNonzeros();

    // ---- starting point --------------------------------------------------
    //
    // Lustig-Marsten-Shanno style: put x strictly inside its box with a margin
    // that scales with the bound magnitudes, take y = 0, and set the bound
    // multipliers from the objective scale. No attempt is made to start near
    // feasibility: this is an INFEASIBLE-start method, and the residuals rp and
    // rd are driven to zero alongside complementarity rather than beforehand.
    const double objectiveScale = std::max(1.0, infinityNorm(problem.objective));
    const double rhsScale = std::max(1.0, infinityNorm(problem.rightHandSide));
    Vec x(un, 0.0), y(um, 0.0), zLower(un, 0.0), zUpper(un, 0.0);
    for (Index j = 0; j < n; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        const double l = problem.lower[uj], u = problem.upper[uj];
        const double margin = std::max(1.0, 0.1 * rhsScale);
        if (hasLower[uj] && hasUpper[uj]) x[uj] = 0.5 * (l + u);
        else if (hasLower[uj]) x[uj] = l + margin;
        else if (hasUpper[uj]) x[uj] = u - margin;
        else x[uj] = 0.0;
        if (hasLower[uj]) zLower[uj] = objectiveScale;
        if (hasUpper[uj]) zUpper[uj] = objectiveScale;
    }

    Vec lowerSlack(un, 0.0), upperSlack(un, 0.0), scaling(un, 0.0);
    Vec primalResidual(um, 0.0), dualResidual(un, 0.0), topRhs(un, 0.0);
    Vec ax, aty, qx;
    Direction affine, corrector, candidate, correction;

    double primalRegularization = options.primalRegularization;
    double dualRegularization = options.dualRegularization;

    const auto refreshSlacks = [&] {
        for (Index j = 0; j < n; ++j) {
            const auto uj = static_cast<std::size_t>(j);
            lowerSlack[uj] = hasLower[uj] ? x[uj] - problem.lower[uj] : 0.0;
            upperSlack[uj] = hasUpper[uj] ? problem.upper[uj] - x[uj] : 0.0;
        }
    };
    const auto complementarityTotal = [&] {
        double total = 0.0;
        for (Index j = 0; j < n; ++j) {
            const auto uj = static_cast<std::size_t>(j);
            if (hasLower[uj]) total += lowerSlack[uj] * zLower[uj];
            if (hasUpper[uj]) total += upperSlack[uj] * zUpper[uj];
        }
        return total;
    };

    if (options.verbose)
        std::printf("  it  %11s %11s %11s %11s %7s\n",
                    "primal", "dual", "gap", "mu", "step");

    for (int iteration = 0;; ++iteration) {
        result.iterations = iteration;
        refreshSlacks();

        problem.constraints.multiply(x, ax);
        problem.constraints.transposeMultiply(y, aty);
        if (problem.hessian.cols == n) multiplySymmetric(problem.hessian, x, qx);
        else qx.assign(un, 0.0);

        for (Index i = 0; i < m; ++i)
            primalResidual[static_cast<std::size_t>(i)] =
                problem.rightHandSide[static_cast<std::size_t>(i)] - ax[static_cast<std::size_t>(i)];
        for (Index j = 0; j < n; ++j) {
            const auto uj = static_cast<std::size_t>(j);
            dualResidual[uj] = aty[uj] + zLower[uj] - zUpper[uj] - qx[uj] - problem.objective[uj];
        }

        double quadratic = 0.0;
        for (Index j = 0; j < n; ++j)
            quadratic += x[static_cast<std::size_t>(j)] * qx[static_cast<std::size_t>(j)];
        double linear = 0.0;
        for (Index j = 0; j < n; ++j)
            linear += problem.objective[static_cast<std::size_t>(j)] * x[static_cast<std::size_t>(j)];
        result.primalObjective = linear + 0.5 * quadratic + problem.objectiveOffset;

        double dual = problem.objectiveOffset - 0.5 * quadratic;
        for (Index i = 0; i < m; ++i)
            dual += problem.rightHandSide[static_cast<std::size_t>(i)] * y[static_cast<std::size_t>(i)];
        for (Index j = 0; j < n; ++j) {
            const auto uj = static_cast<std::size_t>(j);
            if (hasLower[uj]) dual += problem.lower[uj] * zLower[uj];
            if (hasUpper[uj]) dual -= problem.upper[uj] * zUpper[uj];
        }
        result.dualObjective = dual;

        const double gap = complementarityTotal();
        const double mu = boundCount > 0 ? gap / boundCount : 0.0;
        result.primalResidual = infinityNorm(primalResidual) / (1.0 + rhsScale);
        result.dualResidual = infinityNorm(dualResidual) / (1.0 + objectiveScale);
        result.relativeGap = std::abs(result.primalObjective - result.dualObjective) /
                             (1.0 + std::abs(result.primalObjective));
        result.complementarity = mu;
        result.primal = x;
        result.equalityDual = y;
        result.lowerDual = zLower;
        result.upperDual = zUpper;
        result.hasSolution = true;

        if (result.primalResidual <= options.primalTolerance &&
            result.dualResidual <= options.dualTolerance &&
            result.relativeGap <= options.gapTolerance)
        {
            std::string message = "converged: relative primal, dual and gap tolerances met";
            // Polishing runs after convergence, before the loop's time check,
            // and can cost up to 20 factorisations. It is charged to the same
            // budget: skipped when none is left, and stopped between rounds
            // once it runs out. The converged interior point is returned
            // either way, so running out of time here costs accuracy of the
            // multipliers, never correctness of the status.
            const bool outOfTime = options.timeLimitSeconds > 0 && elapsed() >= options.timeLimitSeconds;
            if (options.polish && boundCount > 0 && outOfTime) {
                message += "; polishing skipped: time limit reached";
            } else if (options.polish && boundCount > 0) {
                // Tapia-style identification: near optimality complementarity
                // leaves slack < multiplier on active bounds and slack >
                // multiplier on inactive ones.
                std::vector<int> active(un, 0);
                for (Index j = 0; j < n; ++j) {
                    const auto uj = static_cast<std::size_t>(j);
                    if (hasLower[uj] && lowerSlack[uj] < zLower[uj]) active[uj] = -1;
                    else if (hasUpper[uj] && upperSlack[uj] < zUpper[uj]) active[uj] = 1;
                }
                // A bounded number of active-set corrections: each failed
                // attempt that reveals a wrong active set proposes a corrected
                // one. Every attempt is verified in full, so a retry can only
                // succeed on a genuine KKT point.
                PolishedPoint polished;
                bool verified = false;
                int attempts = 0;
                for (; attempts < 20; ++attempts) {
                    if (attempts > 0 && options.timeLimitSeconds > 0 && elapsed() >= options.timeLimitSeconds) {
                        polished.detail = "time limit reached during active-set correction";
                        break;
                    }
                    if (polishToActiveSet(problem, active, options, polished)) { verified = true; break; }
                    if (polished.correctedActiveSet.empty() || polished.correctedActiveSet == active) break;
                    active = polished.correctedActiveSet;
                }
                if (verified && attempts > 0)
                    polished.detail += " after " + std::to_string(attempts) + " active-set correction" +
                                       (attempts == 1 ? "" : "s");
                if (verified) {
                    // A KKT point of a convex problem is optimal, so its
                    // objective cannot be materially worse; require that anyway.
                    const double slack = options.gapTolerance * (1.0 + std::abs(result.primalObjective));
                    if (polished.primalObjective <= result.primalObjective + slack) {
                        result.primal = std::move(polished.primal);
                        result.equalityDual = std::move(polished.equalityDual);
                        result.lowerDual = std::move(polished.lowerDual);
                        result.upperDual = std::move(polished.upperDual);
                        result.primalObjective = polished.primalObjective;
                        result.dualObjective = polished.dualObjective;
                        result.primalResidual = polished.primalResidual;
                        result.dualResidual = polished.dualResidual;
                        result.relativeGap = std::abs(result.primalObjective - result.dualObjective) /
                                             (1.0 + std::abs(result.primalObjective));
                        result.complementarity = 0.0;
                        result.polished = true;
                        message += "; " + polished.detail;
                    } else {
                        polished.detail = "polished objective was worse than the interior one";
                    }
                }
                result.polishDetail = polished.detail;
                if (!result.polished) message += "; polishing not applied: " + polished.detail;
            }
            return finish(Status::Optimal, message);
        }

        // Two divergence signatures, both consistent with primal infeasibility
        // or dual unboundedness and neither a proof of it -- so neither is ever
        // reported as Infeasible or Unbounded.
        //
        //  * Norm blow-up: a Farkas-like direction makes y or the bound
        //    multipliers grow without limit.
        //  * Complementarity converged but feasibility did not: mu has gone to
        //    zero while a residual is still orders of magnitude above its
        //    tolerance. On a feasible bounded problem the three measures fall
        //    together; this split is the signature of a problem whose
        //    complementarity can be satisfied but whose constraints cannot.
        if (diverging(x, y, zLower, zUpper))
            return finish(Status::SuspectedInfeasibleOrUnbounded,
                          "iterate diverged (an iterate norm exceeded 1e12); this method computes no "
                          "infeasibility or unboundedness certificate");
        if (iteration > 5 && mu < 1e-3 * options.gapTolerance &&
            (result.primalResidual > 1e3 * options.primalTolerance ||
             result.dualResidual > 1e3 * options.dualTolerance))
            return finish(Status::SuspectedInfeasibleOrUnbounded,
                          "complementarity converged while a residual did not; consistent with "
                          "infeasibility or unboundedness, but not a certificate of either");
        if (options.timeLimitSeconds > 0 && elapsed() >= options.timeLimitSeconds)
            return finish(Status::TimeLimit, "barrier time limit");
        if (iteration >= options.iterationLimit)
            return finish(Status::IterationLimit, "barrier iteration limit");
        // No early exit when there are no finite bounds. That case -- an
        // equality-constrained QP over free variables -- has no barrier term
        // at all, and the Newton step on the augmented system is then the
        // exact KKT solve, so the loop below finishes it in one or two steps.
        // It is 3 of the 14 Maros-Meszaros smoke instances (hs51, hs52,
        // genhs28), which this used to refuse outright.

        // ---- factorise ---------------------------------------------------
        for (Index j = 0; j < n; ++j) {
            const auto uj = static_cast<std::size_t>(j);
            double d = 0.0;
            if (hasLower[uj]) d += zLower[uj] / std::max(lowerSlack[uj], 1e-300);
            if (hasUpper[uj]) d += zUpper[uj] / std::max(upperSlack[uj], 1e-300);
            scaling[uj] = d;
        }
        // Relax any escalation from earlier iterations back toward the
        // requested regularisation. Altman-Gondzio regularisation is chosen per
        // iteration, not ratcheted: one transient breakdown must not leave the
        // rest of the solve factorising a heavily perturbed system, which slows
        // the method to a crawl near the optimum where accuracy matters most.
        primalRegularization = std::max(options.primalRegularization, 0.1 * primalRegularization);
        dualRegularization = std::max(options.dualRegularization, 0.1 * dualRegularization);
        bool factored = false;
        for (int attempt = 0; attempt < 8; ++attempt) {
            if (system.refactor(scaling, primalRegularization, dualRegularization)) {
                factored = true;
                break;
            }
            // Inertia or pivot failure: the regularisation was too weak for
            // this scaling. Escalate both and retry, which is Altman-Gondzio's
            // remedy and costs one extra factorisation rather than a restart.
            primalRegularization *= 100.0;
            dualRegularization *= 100.0;
            ++result.regularizationEscalations;
        }
        ++result.factorizations;
        if (!factored) {
            // On a diverging iterate the scaling D spans so many orders of
            // magnitude that no regularisation restores the inertia. Report
            // what the iterate shows rather than blaming the linear algebra.
            if (diverging(x, y, zLower, zUpper, 1e8))
                return finish(Status::SuspectedInfeasibleOrUnbounded,
                              "factorisation broke down on a diverging iterate; consistent with "
                              "infeasibility or unboundedness, but not a certificate of either");
            return finish(Status::NumericalFailure,
                          "augmented system could not be factorised into the predicted inertia even "
                          "after escalating regularisation");
        }

        // ---- predictor (affine scaling, sigma = 0) ------------------------
        const auto buildTopRhs = [&](const Vec& lowerTarget, const Vec& upperTarget) {
            for (Index j = 0; j < n; ++j) {
                const auto uj = static_cast<std::size_t>(j);
                double value = dualResidual[uj];
                if (hasLower[uj]) value += lowerTarget[uj] / std::max(lowerSlack[uj], 1e-300);
                if (hasUpper[uj]) value -= upperTarget[uj] / std::max(upperSlack[uj], 1e-300);
                topRhs[uj] = -value;
            }
        };
        const auto recoverBoundDuals = [&](const Vec& lowerTarget, const Vec& upperTarget,
                                           Direction& direction) {
            direction.lowerDual.assign(un, 0.0);
            direction.upperDual.assign(un, 0.0);
            for (Index j = 0; j < n; ++j) {
                const auto uj = static_cast<std::size_t>(j);
                if (hasLower[uj])
                    direction.lowerDual[uj] =
                        (lowerTarget[uj] - zLower[uj] * direction.x[uj]) / std::max(lowerSlack[uj], 1e-300);
                if (hasUpper[uj])
                    direction.upperDual[uj] =
                        (upperTarget[uj] + zUpper[uj] * direction.x[uj]) / std::max(upperSlack[uj], 1e-300);
            }
        };
        // Largest step keeping every slack and bound multiplier strictly
        // positive, before the fraction-to-boundary discount.
        const auto maximumStep = [&](const Direction& direction, double& primalStep, double& dualStep) {
            // UNCAPPED: the largest step to the boundary, which may exceed 1
            // or be infinite when nothing blocks. Callers cap it. Capping here
            // made every step at most 0.995 even when no bound was near.
            primalStep = dualStep = infinity;
            for (Index j = 0; j < n; ++j) {
                const auto uj = static_cast<std::size_t>(j);
                if (hasLower[uj]) {
                    if (direction.x[uj] < 0.0)
                        primalStep = std::min(primalStep, -lowerSlack[uj] / direction.x[uj]);
                    if (direction.lowerDual[uj] < 0.0)
                        dualStep = std::min(dualStep, -zLower[uj] / direction.lowerDual[uj]);
                }
                if (hasUpper[uj]) {
                    if (direction.x[uj] > 0.0)
                        primalStep = std::min(primalStep, upperSlack[uj] / direction.x[uj]);
                    if (direction.upperDual[uj] < 0.0)
                        dualStep = std::min(dualStep, -zUpper[uj] / direction.upperDual[uj]);
                }
            }
        };

        Vec lowerTarget(un, 0.0), upperTarget(un, 0.0);
        for (Index j = 0; j < n; ++j) {
            const auto uj = static_cast<std::size_t>(j);
            if (hasLower[uj]) lowerTarget[uj] = -lowerSlack[uj] * zLower[uj];
            if (hasUpper[uj]) upperTarget[uj] = -upperSlack[uj] * zUpper[uj];
        }
        buildTopRhs(lowerTarget, upperTarget);
        system.solve(topRhs, primalResidual, affine.x, affine.y, options.refinementRounds);
        recoverBoundDuals(lowerTarget, upperTarget, affine);

        double affinePrimalStep = 1.0, affineDualStep = 1.0;
        maximumStep(affine, affinePrimalStep, affineDualStep);
        // Mehrotra's affine step is at most a full Newton step.
        affinePrimalStep = std::min(1.0, affinePrimalStep);
        affineDualStep = std::min(1.0, affineDualStep);

        // Mehrotra's adaptive centering parameter:
        //   mu_target = (g_a / g)^2 * (g_a / n_c)
        // where g_a is the complementarity that the affine step would leave.
        // The cube makes the target aggressive when the affine step is
        // productive and conservative when it is not.
        double affineGap = 0.0;
        {
            const double ap = affinePrimalStep, ad = affineDualStep;
            for (Index j = 0; j < n; ++j) {
                const auto uj = static_cast<std::size_t>(j);
                if (hasLower[uj])
                    affineGap += (lowerSlack[uj] + ap * affine.x[uj]) *
                                 (zLower[uj] + ad * affine.lowerDual[uj]);
                if (hasUpper[uj])
                    affineGap += (upperSlack[uj] - ap * affine.x[uj]) *
                                 (zUpper[uj] + ad * affine.upperDual[uj]);
            }
        }
        double target = 0.0;
        if (gap > 0.0 && boundCount > 0) {
            const double ratio = std::max(affineGap, 0.0) / gap;
            target = ratio * ratio * (std::max(affineGap, 0.0) / boundCount);
            target = std::min(target, mu);
        }

        // ---- corrector ----------------------------------------------------
        for (Index j = 0; j < n; ++j) {
            const auto uj = static_cast<std::size_t>(j);
            if (hasLower[uj])
                lowerTarget[uj] = target - lowerSlack[uj] * zLower[uj] -
                                  affine.x[uj] * affine.lowerDual[uj];
            if (hasUpper[uj])
                upperTarget[uj] = target - upperSlack[uj] * zUpper[uj] +
                                  affine.x[uj] * affine.upperDual[uj];
        }
        buildTopRhs(lowerTarget, upperTarget);
        system.solve(topRhs, primalResidual, corrector.x, corrector.y, options.refinementRounds);
        recoverBoundDuals(lowerTarget, upperTarget, corrector);

        double primalStep = 1.0, dualStep = 1.0;
        maximumStep(corrector, primalStep, dualStep);

        // ---- Gondzio's multiple centrality correctors ---------------------
        //
        // Each extra correction reuses the factorisation already computed, so
        // it costs two triangular solves rather than a refactorisation. The
        // correction targets any complementarity product that the trial step
        // would push outside [betaMin, betaMax] * mu, and is kept only if the
        // step it permits is materially longer.
        for (int pass = 0; pass < options.centralityCorrectors; ++pass) {
            const double trialPrimal = std::min(1.0, options.fractionToBoundary * primalStep + 0.1);
            const double trialDual = std::min(1.0, options.fractionToBoundary * dualStep + 0.1);
            const double reference = target > 0.0 ? target : mu;
            for (Index j = 0; j < n; ++j) {
                const auto uj = static_cast<std::size_t>(j);
                if (hasLower[uj]) {
                    const double slack = lowerSlack[uj] + trialPrimal * corrector.x[uj];
                    const double multiplier = zLower[uj] + trialDual * corrector.lowerDual[uj];
                    const double product = slack * multiplier;
                    const double wanted = std::clamp(product, options.correctorBetaMin * reference,
                                                     options.correctorBetaMax * reference);
                    lowerTarget[uj] = wanted - product;
                }
                if (hasUpper[uj]) {
                    const double slack = upperSlack[uj] - trialPrimal * corrector.x[uj];
                    const double multiplier = zUpper[uj] + trialDual * corrector.upperDual[uj];
                    const double product = slack * multiplier;
                    const double wanted = std::clamp(product, options.correctorBetaMin * reference,
                                                     options.correctorBetaMax * reference);
                    upperTarget[uj] = wanted - product;
                }
            }
            // The correction solves the same system with only the
            // complementarity targets on the right-hand side: the primal and
            // dual residuals are already accounted for by `corrector`.
            for (Index j = 0; j < n; ++j) {
                const auto uj = static_cast<std::size_t>(j);
                double value = 0.0;
                if (hasLower[uj]) value += lowerTarget[uj] / std::max(lowerSlack[uj], 1e-300);
                if (hasUpper[uj]) value -= upperTarget[uj] / std::max(upperSlack[uj], 1e-300);
                topRhs[uj] = -value;
            }
            const Vec zeroRhs(um, 0.0);
            system.solve(topRhs, zeroRhs, correction.x, correction.y, options.refinementRounds);
            recoverBoundDuals(lowerTarget, upperTarget, correction);

            candidate.x.resize(un); candidate.y.resize(um);
            candidate.lowerDual.resize(un); candidate.upperDual.resize(un);
            for (Index j = 0; j < n; ++j) {
                const auto uj = static_cast<std::size_t>(j);
                candidate.x[uj] = corrector.x[uj] + correction.x[uj];
                candidate.lowerDual[uj] = corrector.lowerDual[uj] + correction.lowerDual[uj];
                candidate.upperDual[uj] = corrector.upperDual[uj] + correction.upperDual[uj];
            }
            for (Index i = 0; i < m; ++i)
                candidate.y[static_cast<std::size_t>(i)] =
                    corrector.y[static_cast<std::size_t>(i)] + correction.y[static_cast<std::size_t>(i)];

            double candidatePrimal = 1.0, candidateDual = 1.0;
            maximumStep(candidate, candidatePrimal, candidateDual);
            // Gondzio's acceptance test is on the SHORTER of the two steps: that
            // is the one limiting progress. Requiring both to improve would
            // reject every correction once either step is already capped at 1,
            // which is exactly when the other one most needs help.
            // Compared as usable step lengths, i.e. capped at 1: a correction
            // that lengthens an already-unblocked step buys nothing.
            const bool improved =
                std::min({1.0, candidatePrimal, candidateDual}) >=
                std::min({1.0, primalStep, dualStep}) + options.correctorMinimumGain;
            if (!improved) break;
            corrector.x = candidate.x;
            corrector.y = candidate.y;
            corrector.lowerDual = candidate.lowerDual;
            corrector.upperDual = candidate.upperDual;
            primalStep = candidatePrimal;
            dualStep = candidateDual;
            ++result.correctorsAccepted;
        }

        // ---- take the step ------------------------------------------------
        const double alphaPrimal = std::min(1.0, options.fractionToBoundary * primalStep);
        const double alphaDual = std::min(1.0, options.fractionToBoundary * dualStep);
        if (!(alphaPrimal > 0.0) || !(alphaDual > 0.0) ||
            !std::isfinite(alphaPrimal) || !std::isfinite(alphaDual))
            return finish(Status::NumericalFailure,
                          "step length collapsed to zero; the iterate is on the boundary of its box");

        for (Index j = 0; j < n; ++j) {
            const auto uj = static_cast<std::size_t>(j);
            x[uj] += alphaPrimal * corrector.x[uj];
            if (hasLower[uj]) zLower[uj] = std::max(zLower[uj] + alphaDual * corrector.lowerDual[uj], 0.0);
            if (hasUpper[uj]) zUpper[uj] = std::max(zUpper[uj] + alphaDual * corrector.upperDual[uj], 0.0);
        }
        for (Index i = 0; i < m; ++i)
            y[static_cast<std::size_t>(i)] += alphaDual * corrector.y[static_cast<std::size_t>(i)];

        bool finite = true;
        for (double v : x) if (!std::isfinite(v)) { finite = false; break; }
        for (double v : y) if (finite && !std::isfinite(v)) { finite = false; break; }
        if (!finite)
            return finish(Status::NumericalFailure, "iterate became non-finite");

        if (options.verbose)
            std::printf("  %3d  %11.3e %11.3e %11.3e %11.3e %7.4f\n",
                        iteration, result.primalResidual, result.dualResidual,
                        result.relativeGap, mu, alphaPrimal);
    }
}

}  // namespace barrier
