#include "milp/dual_simplex_solver.h"

#include <chrono>

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <utility>

namespace milp {
namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr double kRatioTieTolerance = 1e-9;

bool isFinite(double value) { return std::isfinite(value); }

// Dense Gauss-Jordan inverse with partial pivoting. Returns std::nullopt if
// `matrix` (assumed square) is numerically singular.
std::optional<std::vector<std::vector<double>>> invert(
    std::vector<std::vector<double>> matrix,
    double pivotTolerance)
{
    const std::size_t m = matrix.size();
    std::vector<std::vector<double>> inverse(m, std::vector<double>(m, 0.0));
    for (std::size_t i = 0; i < m; ++i) {
        inverse[i][i] = 1.0;
    }

    for (std::size_t col = 0; col < m; ++col) {
        std::size_t pivotRow = col;
        double best = std::abs(matrix[col][col]);
        for (std::size_t row = col + 1; row < m; ++row) {
            const double candidate = std::abs(matrix[row][col]);
            if (candidate > best) {
                best = candidate;
                pivotRow = row;
            }
        }
        if (best <= pivotTolerance) {
            return std::nullopt;
        }
        if (pivotRow != col) {
            std::swap(matrix[pivotRow], matrix[col]);
            std::swap(inverse[pivotRow], inverse[col]);
        }

        const double pivot = matrix[col][col];
        for (std::size_t k = 0; k < m; ++k) {
            matrix[col][k] /= pivot;
            inverse[col][k] /= pivot;
        }

        for (std::size_t row = 0; row < m; ++row) {
            if (row == col) {
                continue;
            }
            const double factor = matrix[row][col];
            if (factor == 0.0) {
                continue;
            }
            for (std::size_t k = 0; k < m; ++k) {
                matrix[row][k] -= factor * matrix[col][k];
                inverse[row][k] -= factor * inverse[col][k];
            }
        }
    }
    return inverse;
}

// The unified bounded-variable system: constraints `rowLower <= A x <=
// rowUpper` are rewritten as the homogeneous equality system
//   [A | -I] [x; y] = 0
// where y is one logical (slack) variable per row, bounded by that row's own
// [rowLower, rowUpper]. This is the standard technique bounded revised
// simplex codes use to fold range constraints into a single equality system
// with two blocks of bounded variables -- structural columns 0..n-1 and
// logical columns n..n+m-1.
struct Problem {
    int n = 0; // structural variable count
    int m = 0; // row / logical variable count

    std::vector<double> cost;                 // size n+m; logicals are always 0
    std::vector<double> lower;                // size n+m
    std::vector<double> upper;                // size n+m
    std::vector<std::vector<double>> columns; // size n, each length m (columns of A)

    [[nodiscard]] int totalCount() const noexcept { return n + m; }
    [[nodiscard]] bool isLogical(int j) const noexcept { return j >= n; }

    // B^{-1} * column_j (the FTRAN of column j).
    [[nodiscard]] std::vector<double> ftran(
        int j,
        const std::vector<std::vector<double>> &inverse) const
    {
        std::vector<double> result(static_cast<std::size_t>(m), 0.0);
        if (isLogical(j)) {
            const std::size_t row = static_cast<std::size_t>(j - n);
            for (int r = 0; r < m; ++r) {
                result[static_cast<std::size_t>(r)] = -inverse[static_cast<std::size_t>(r)][row];
            }
            return result;
        }
        const auto &column = columns[static_cast<std::size_t>(j)];
        for (int r = 0; r < m; ++r) {
            double sum = 0.0;
            const auto &invRow = inverse[static_cast<std::size_t>(r)];
            for (int k = 0; k < m; ++k) {
                sum += invRow[static_cast<std::size_t>(k)] * column[static_cast<std::size_t>(k)];
            }
            result[static_cast<std::size_t>(r)] = sum;
        }
        return result;
    }

    // rho . column_j, for a single given row vector rho of length m -- used
    // during the ratio-test scan and for reduced-cost evaluation without
    // materialising the full FTRAN of every candidate column.
    [[nodiscard]] double dotColumn(const std::vector<double> &rho, int j) const noexcept {
        if (isLogical(j)) {
            return -rho[static_cast<std::size_t>(j - n)];
        }
        const auto &column = columns[static_cast<std::size_t>(j)];
        double sum = 0.0;
        for (int k = 0; k < m; ++k) {
            sum += rho[static_cast<std::size_t>(k)] * column[static_cast<std::size_t>(k)];
        }
        return sum;
    }
};

Problem buildProblem(const model::Model &model) {
    if (!model.validate()) {
        throw std::invalid_argument("DualSimplexSolver: model failed structural validation");
    }
    if (!model.objective.quadraticTerms.empty()) {
        throw std::invalid_argument(
            "DualSimplexSolver: quadratic objectives are not supported (LP relaxations only)");
    }

    Problem problem;
    problem.n = static_cast<int>(model.variables.size());
    problem.m = static_cast<int>(model.constraints.size());

    const bool maximize = model.objective.sense == model::ObjectiveSense::Maximize;
    const double sign = maximize ? -1.0 : 1.0;

    const std::size_t total = static_cast<std::size_t>(problem.n + problem.m);
    problem.cost.assign(total, 0.0);
    for (const auto &term : model.objective.linearTerms) {
        problem.cost[static_cast<std::size_t>(term.variableIndex)] += sign * term.value;
    }

    problem.lower.resize(total);
    problem.upper.resize(total);
    for (int j = 0; j < problem.n; ++j) {
        problem.lower[static_cast<std::size_t>(j)] = model.variables[static_cast<std::size_t>(j)].lowerBound;
        problem.upper[static_cast<std::size_t>(j)] = model.variables[static_cast<std::size_t>(j)].upperBound;
    }
    for (int i = 0; i < problem.m; ++i) {
        const auto &constraint = model.constraints[static_cast<std::size_t>(i)];
        problem.lower[static_cast<std::size_t>(problem.n + i)] = constraint.lowerBound;
        problem.upper[static_cast<std::size_t>(problem.n + i)] = constraint.upperBound;
    }

    problem.columns.assign(
        static_cast<std::size_t>(problem.n),
        std::vector<double>(static_cast<std::size_t>(problem.m), 0.0));
    for (int i = 0; i < problem.m; ++i) {
        for (const auto &term : model.constraints[static_cast<std::size_t>(i)].linearTerms) {
            problem.columns[static_cast<std::size_t>(term.variableIndex)][static_cast<std::size_t>(i)] +=
                term.value;
        }
    }

    return problem;
}

double boundedNonbasicValue(BasisStatus status, double lower, double upper) {
    switch (status) {
        case BasisStatus::AtLower:
        case BasisStatus::Fixed:
            return lower;
        case BasisStatus::AtUpper:
            return upper;
        case BasisStatus::Free:
        case BasisStatus::Basic:
            return 0.0;
    }
    return 0.0;
}

enum class CrashOutcome { Ok, Unbounded };

// Builds the classic all-slack starting basis, then crashes into the basis
// any structural variable whose only available bound cannot be made dual
// feasible against a zero-cost basis (a free variable with nonzero cost, or
// a one-sided-bound variable whose cost sign disagrees with that bound).
//
// If such a variable's column is entirely zero, the model is unbounded: an
// unconstrained direction with a nonzero cost gradient improves forever.
// That is the one certain, cheap-to-detect unboundedness case this crash
// checks for -- it does not attempt to prove unboundedness reachable only
// mid-algorithm.
//
// If a variable needs to be crashed in but every row where it has a nonzero
// coefficient is already claimed by an earlier crash, or has a logical
// variable with no finite bound on either side to send the displaced row to,
// it is left at its (possibly dual-infeasible) assigned bound. This is a
// documented, deliberately narrow gap in an otherwise-general crash: making
// it fully general requires a proper dual Phase 1, which is out of scope for
// this foundational engine.
CrashOutcome buildColdStartBasis(
    const Problem &problem,
    const DualSimplexOptions &options,
    std::vector<int> &basisVar,
    std::vector<BasisStatus> &status,
    std::vector<double> &nonbasicValue)
{
    const int n = problem.n;
    const int m = problem.m;
    const std::size_t total = static_cast<std::size_t>(n + m);

    basisVar.assign(static_cast<std::size_t>(m), -1);
    status.assign(total, BasisStatus::AtLower);
    nonbasicValue.assign(total, 0.0);

    for (int i = 0; i < m; ++i) {
        basisVar[static_cast<std::size_t>(i)] = n + i;
        status[static_cast<std::size_t>(n + i)] = BasisStatus::Basic;
    }

    std::vector<bool> rowClaimed(static_cast<std::size_t>(m), false);

    for (int j = 0; j < n; ++j) {
        const double lo = problem.lower[static_cast<std::size_t>(j)];
        const double hi = problem.upper[static_cast<std::size_t>(j)];
        const double c = problem.cost[static_cast<std::size_t>(j)];

        BasisStatus st;
        if (std::abs(hi - lo) <= options.primalFeasibilityTolerance) {
            st = BasisStatus::Fixed;
        } else if (isFinite(lo) && isFinite(hi)) {
            st = (c >= 0.0) ? BasisStatus::AtLower : BasisStatus::AtUpper;
        } else if (isFinite(lo)) {
            st = BasisStatus::AtLower;
        } else if (isFinite(hi)) {
            st = BasisStatus::AtUpper;
        } else {
            st = BasisStatus::Free;
        }

        status[static_cast<std::size_t>(j)] = st;
        nonbasicValue[static_cast<std::size_t>(j)] = boundedNonbasicValue(st, lo, hi);

        const bool needsCrash =
            (st == BasisStatus::AtLower && !isFinite(hi) && c < -options.dualFeasibilityTolerance) ||
            (st == BasisStatus::AtUpper && !isFinite(lo) && c > options.dualFeasibilityTolerance) ||
            (st == BasisStatus::Free && std::abs(c) > options.dualFeasibilityTolerance);

        if (!needsCrash) {
            continue;
        }

        const auto &column = problem.columns[static_cast<std::size_t>(j)];

        bool hasNonzero = false;
        for (int i = 0; i < m; ++i) {
            if (std::abs(column[static_cast<std::size_t>(i)]) > options.pivotTolerance) {
                hasNonzero = true;
                break;
            }
        }
        if (!hasNonzero) {
            return CrashOutcome::Unbounded;
        }

        for (int i = 0; i < m; ++i) {
            if (rowClaimed[static_cast<std::size_t>(i)]) {
                continue;
            }
            if (std::abs(column[static_cast<std::size_t>(i)]) <= options.pivotTolerance) {
                continue;
            }

            const int displaced = basisVar[static_cast<std::size_t>(i)];
            const double dLo = problem.lower[static_cast<std::size_t>(displaced)];
            const double dHi = problem.upper[static_cast<std::size_t>(displaced)];

            BasisStatus dStatus;
            if (std::abs(dHi - dLo) <= options.primalFeasibilityTolerance) {
                dStatus = BasisStatus::Fixed;
            } else if (isFinite(dLo)) {
                dStatus = BasisStatus::AtLower;
            } else if (isFinite(dHi)) {
                dStatus = BasisStatus::AtUpper;
            } else {
                continue; // Unconstrained row: skip, keep scanning (see comment above).
            }

            status[static_cast<std::size_t>(displaced)] = dStatus;
            nonbasicValue[static_cast<std::size_t>(displaced)] = boundedNonbasicValue(dStatus, dLo, dHi);

            basisVar[static_cast<std::size_t>(i)] = j;
            status[static_cast<std::size_t>(j)] = BasisStatus::Basic;
            rowClaimed[static_cast<std::size_t>(i)] = true;
            break;
        }
    }

    return CrashOutcome::Ok;
}

// Holds all per-solve mutable state and runs the Phase 2 dual simplex loop.
class Workspace {
public:
    Workspace(
        const Problem &problem,
        const DualSimplexOptions &options,
        std::vector<int> basisVar,
        std::vector<BasisStatus> status,
        std::vector<double> nonbasicValue,
        std::vector<std::vector<double>> inverse)
        : problem_(problem),
          options_(options),
          basisVar_(std::move(basisVar)),
          status_(std::move(status)),
          nonbasicValue_(std::move(nonbasicValue)),
          inverse_(std::move(inverse)),
          rowOfColumn_(static_cast<std::size_t>(problem.totalCount()), -1)
    {
        for (int r = 0; r < problem_.m; ++r) {
            rowOfColumn_[static_cast<std::size_t>(basisVar_[static_cast<std::size_t>(r)])] = r;
        }
        recomputeBasicValues();
    }

    void run() {
        // Wall-clock deadline, resolved once. A clock read per iteration is
        // negligible against the O(m^2) work each pivot does on the dense
        // basis inverse, so this is checked every iteration rather than
        // sampled.
        const bool timed = options_.timeLimitSeconds > 0.0;
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::duration<double>(timed ? options_.timeLimitSeconds : 0.0);

        for (;;) {
            if (iterations_ >= options_.iterationLimit) {
                finalStatus_ = DualSimplexStatus::IterationLimit;
                return;
            }
            if (timed && std::chrono::steady_clock::now() >= deadline) {
                finalStatus_ = DualSimplexStatus::TimeLimit;
                return;
            }

            int leaveRow = -1;
            bool leaveTooHigh = false;
            if (!selectLeavingRow(leaveRow, leaveTooHigh)) {
                // Primal feasible. That is NOT sufficient for optimality: the
                // dual simplex maintains dual feasibility as an invariant, but
                // buildColdStartBasis has a documented gap -- when two
                // crash-needing variables compete for the same row, the loser is
                // left at a dual-infeasible bound -- so the invariant may never
                // have held. Declaring Optimal here on trust returned
                // obj = -1 at (1, 0) for "min -x-y s.t. x-y <= 1, x,y >= 0",
                // which is unbounded. Verify before claiming.
                finalStatus_ = classifyPrimalFeasibleBasis();
                return;
            }

            const std::vector<double> rho = inverse_[static_cast<std::size_t>(leaveRow)];
            const std::vector<double> y = computeMultipliers();

            int enterCol = -1;
            double enterAlpha = 0.0;
            if (!selectEnteringColumn(rho, y, leaveTooHigh, enterCol, enterAlpha)) {
                finalStatus_ = DualSimplexStatus::Infeasible;
                farkasRay_ = rho;
                if (!leaveTooHigh) {
                    for (auto &v : farkasRay_) {
                        v = -v;
                    }
                }
                return;
            }

            pivot(leaveRow, leaveTooHigh, enterCol, enterAlpha);
            ++iterations_;
        }
    }

    [[nodiscard]] DualSimplexResult buildResult() const {
        DualSimplexResult result;
        result.status = finalStatus_;
        result.iterations = iterations_;

        result.primal.resize(static_cast<std::size_t>(problem_.n));
        for (int j = 0; j < problem_.n; ++j) {
            const int row = rowOfColumn_[static_cast<std::size_t>(j)];
            result.primal[static_cast<std::size_t>(j)] = (row >= 0)
                ? xB_[static_cast<std::size_t>(row)]
                : nonbasicValue_[static_cast<std::size_t>(j)];
        }

        if (finalStatus_ == DualSimplexStatus::Infeasible) {
            result.dualFarkasRay = farkasRay_;
        } else {
            result.dual = computeMultipliers();

            double objective = 0.0;
            for (int j = 0; j < problem_.n; ++j) {
                objective += problem_.cost[static_cast<std::size_t>(j)] * result.primal[static_cast<std::size_t>(j)];
            }
            result.objectiveValue = objective;
        }

        result.basis.variableStatus.resize(static_cast<std::size_t>(problem_.n));
        for (int j = 0; j < problem_.n; ++j) {
            result.basis.variableStatus[static_cast<std::size_t>(j)] = status_[static_cast<std::size_t>(j)];
        }
        result.basis.constraintStatus.resize(static_cast<std::size_t>(problem_.m));
        for (int i = 0; i < problem_.m; ++i) {
            result.basis.constraintStatus[static_cast<std::size_t>(i)] =
                status_[static_cast<std::size_t>(problem_.n + i)];
        }

        result.tableau.numStructural = problem_.n;
        result.tableau.numRows = problem_.m;
        result.tableau.basisColumns = basisVar_;
        result.tableau.basisInverse = inverse_;
        result.tableau.columnStatus = status_;
        result.tableau.columnLower = problem_.lower;
        result.tableau.columnUpper = problem_.upper;

        return result;
    }

private:
    // x_B = -B^{-1} * N * x_N, for the homogeneous system [A | -I][x; y] = 0.
    //
    // Accumulate the weighted nonbasic column sum FIRST, then apply B^{-1}
    // once:
    //     x_B = -sum_j B^{-1} a_j v_j = -B^{-1} (sum_j a_j v_j)
    // The two are identical in exact arithmetic -- the inverse is linear --
    // but the cost is not. Applying B^{-1} per column is one dense O(m^2)
    // FTRAN for every nonbasic variable, so O(n*m^2); hoisting it out leaves
    // O(n*m) to accumulate plus a single O(m^2) mat-vec.
    //
    // This is called from the Workspace constructor, so it runs on every
    // solve -- including every branch-and-cut node. Measured on a 1500-row
    // tridiagonal LP that needs 2 pivots, it was 3.23s of the 3.26s total
    // solve time, which is also why a wall-clock budget could not be honoured:
    // the whole budget was gone before run() executed its first check.
    void recomputeBasicValues() {
        const int m = problem_.m;
        const int n = problem_.n;

        std::vector<double> weighted(static_cast<std::size_t>(m), 0.0);
        for (int j = 0; j < problem_.totalCount(); ++j) {
            if (status_[static_cast<std::size_t>(j)] == BasisStatus::Basic) {
                continue;
            }
            const double value = nonbasicValue_[static_cast<std::size_t>(j)];
            if (value == 0.0) {
                continue;
            }
            if (problem_.isLogical(j)) {
                // Logical column j is -e_{j-n}.
                weighted[static_cast<std::size_t>(j - n)] -= value;
            } else {
                const auto &column = problem_.columns[static_cast<std::size_t>(j)];
                for (int r = 0; r < m; ++r) {
                    weighted[static_cast<std::size_t>(r)] +=
                        column[static_cast<std::size_t>(r)] * value;
                }
            }
        }

        xB_.assign(static_cast<std::size_t>(m), 0.0);
        for (int r = 0; r < m; ++r) {
            const auto &invRow = inverse_[static_cast<std::size_t>(r)];
            double sum = 0.0;
            for (int k = 0; k < m; ++k) {
                sum += invRow[static_cast<std::size_t>(k)] * weighted[static_cast<std::size_t>(k)];
            }
            xB_[static_cast<std::size_t>(r)] = -sum;
        }
    }

    // Decides what a primal-feasible basis actually proves.
    //
    // Optimality needs primal feasibility AND dual feasibility. This checks the
    // second: every nonbasic variable must have a reduced cost whose sign says
    // moving it off its bound cannot improve the objective. If one can improve,
    // the basis is not optimal, and the question becomes whether the improving
    // move is blocked by anything:
    //
    //   * nothing blocks it            -> the model is unbounded;
    //   * something blocks it          -> the model may well have an optimum,
    //                                     but this basis is not it and the dual
    //                                     simplex cannot get there from a
    //                                     dual-infeasible start. Report
    //                                     IterationLimit -- "not proven" -- as
    //                                     the honest answer. Reaching it needs
    //                                     a dual Phase 1, which this engine does
    //                                     not have.
    [[nodiscard]] DualSimplexStatus classifyPrimalFeasibleBasis() const {
        const std::vector<double> y = computeMultipliers();
        const int total = problem_.n + problem_.m;

        for (int j = 0; j < total; ++j) {
            const BasisStatus st = status_[static_cast<std::size_t>(j)];
            if (st == BasisStatus::Basic || st == BasisStatus::Fixed) {
                continue;
            }

            const double d = problem_.cost[static_cast<std::size_t>(j)] -
                             problem_.dotColumn(y, j);
            const double tol = options_.dualFeasibilityTolerance;

            // Direction that would improve: +1 raises x_j, -1 lowers it.
            int dir = 0;
            if (st == BasisStatus::AtLower && d < -tol)      dir = +1;
            else if (st == BasisStatus::AtUpper && d > tol)  dir = -1;
            else if (st == BasisStatus::Free && std::abs(d) > tol) dir = (d < 0.0) ? +1 : -1;
            if (dir == 0) {
                continue;
            }

            // The entering variable's own opposite bound blocks it first.
            const double ownBound = (dir > 0) ? problem_.upper[static_cast<std::size_t>(j)]
                                              : problem_.lower[static_cast<std::size_t>(j)];
            if (isFinite(ownBound)) {
                return DualSimplexStatus::IterationLimit;
            }

            // Primal ratio test: moving x_j by t changes basic r by
            // -dir * alpha[r] * t. Unbounded iff no basic variable reaches a
            // finite bound.
            const std::vector<double> alpha = problem_.ftran(j, inverse_);
            for (int r = 0; r < problem_.m; ++r) {
                const double a = alpha[static_cast<std::size_t>(r)];
                if (std::abs(a) <= options_.pivotTolerance) {
                    continue;
                }
                const int basic = basisVar_[static_cast<std::size_t>(r)];
                const double rate = -static_cast<double>(dir) * a;
                const double blockingBound =
                    (rate > 0.0) ? problem_.upper[static_cast<std::size_t>(basic)]
                                 : problem_.lower[static_cast<std::size_t>(basic)];
                if (isFinite(blockingBound)) {
                    return DualSimplexStatus::IterationLimit;
                }
            }
            return DualSimplexStatus::Unbounded;
        }

        return DualSimplexStatus::Optimal;
    }

    // y = c_B^T B^{-1}, the simplex multipliers implied by the current basis.
    [[nodiscard]] std::vector<double> computeMultipliers() const {
        std::vector<double> y(static_cast<std::size_t>(problem_.m), 0.0);
        for (int col = 0; col < problem_.m; ++col) {
            const int basicVariable = basisVar_[static_cast<std::size_t>(col)];
            const double cB = problem_.cost[static_cast<std::size_t>(basicVariable)];
            if (cB == 0.0) {
                continue;
            }
            const auto &invRow = inverse_[static_cast<std::size_t>(col)];
            for (int r = 0; r < problem_.m; ++r) {
                y[static_cast<std::size_t>(r)] += cB * invRow[static_cast<std::size_t>(r)];
            }
        }
        return y;
    }

    [[nodiscard]] bool selectLeavingRow(int &leaveRow, bool &leaveTooHigh) const {
        std::vector<double> dseWeight;
        if (options_.useDualSteepestEdge) {
            dseWeight.assign(static_cast<std::size_t>(problem_.m), 0.0);
            for (int r = 0; r < problem_.m; ++r) {
                double sumSquares = 1.0; // reference-framework weight includes the leaving row's own unit component
                const auto &invRow = inverse_[static_cast<std::size_t>(r)];
                for (int k = 0; k < problem_.m; ++k) {
                    const double v = invRow[static_cast<std::size_t>(k)];
                    sumSquares += v * v;
                }
                dseWeight[static_cast<std::size_t>(r)] = sumSquares;
            }
        }

        double bestScore = -1.0;
        leaveRow = -1;
        leaveTooHigh = false;

        for (int r = 0; r < problem_.m; ++r) {
            const int basicVariable = basisVar_[static_cast<std::size_t>(r)];
            const double lo = problem_.lower[static_cast<std::size_t>(basicVariable)];
            const double hi = problem_.upper[static_cast<std::size_t>(basicVariable)];
            const double value = xB_[static_cast<std::size_t>(r)];

            double infeasibility = 0.0;
            bool tooHigh = false;
            if (value < lo - options_.primalFeasibilityTolerance) {
                infeasibility = lo - value;
            } else if (value > hi + options_.primalFeasibilityTolerance) {
                infeasibility = value - hi;
                tooHigh = true;
            } else {
                continue;
            }

            const double score = options_.useDualSteepestEdge
                ? (infeasibility * infeasibility) / dseWeight[static_cast<std::size_t>(r)]
                : infeasibility;

            if (score > bestScore) {
                bestScore = score;
                leaveRow = r;
                leaveTooHigh = tooHigh;
            }
        }

        return leaveRow >= 0;
    }

    [[nodiscard]] bool selectEnteringColumn(
        const std::vector<double> &rho,
        const std::vector<double> &y,
        bool leaveTooHigh,
        int &enterCol,
        double &enterAlpha) const
    {
        enterCol = -1;
        enterAlpha = 0.0;
        double bestRatio = kInfinity;

        for (int j = 0; j < problem_.totalCount(); ++j) {
            const BasisStatus st = status_[static_cast<std::size_t>(j)];
            if (st == BasisStatus::Basic || st == BasisStatus::Fixed) {
                continue;
            }

            const double alpha = problem_.dotColumn(rho, j);
            if (std::abs(alpha) <= options_.pivotTolerance) {
                continue;
            }

            bool eligible;
            if (st == BasisStatus::Free) {
                eligible = true;
            } else if (!leaveTooHigh) {
                eligible = (st == BasisStatus::AtLower && alpha < 0.0) ||
                           (st == BasisStatus::AtUpper && alpha > 0.0);
            } else {
                eligible = (st == BasisStatus::AtLower && alpha > 0.0) ||
                           (st == BasisStatus::AtUpper && alpha < 0.0);
            }
            if (!eligible) {
                continue;
            }

            const double reducedCost = problem_.cost[static_cast<std::size_t>(j)] - problem_.dotColumn(y, j);
            const double ratio = std::abs(reducedCost) / std::abs(alpha);

            const bool strictlyBetter = ratio < bestRatio - kRatioTieTolerance;
            const bool tiedButMoreStable =
                ratio < bestRatio + kRatioTieTolerance && std::abs(alpha) > std::abs(enterAlpha);

            if (strictlyBetter || tiedButMoreStable) {
                bestRatio = ratio;
                enterCol = j;
                enterAlpha = alpha;
            }
        }

        return enterCol >= 0;
    }

    void pivot(int leaveRow, bool leaveTooHigh, int enterCol, double /*enterAlpha*/) {
        const std::vector<double> colVec = problem_.ftran(enterCol, inverse_);
        const double pivotElement = colVec[static_cast<std::size_t>(leaveRow)];

        const int leaveVar = basisVar_[static_cast<std::size_t>(leaveRow)];
        const double loLeave = problem_.lower[static_cast<std::size_t>(leaveVar)];
        const double hiLeave = problem_.upper[static_cast<std::size_t>(leaveVar)];
        const double target = leaveTooHigh ? hiLeave : loLeave;

        const double delta = (xB_[static_cast<std::size_t>(leaveRow)] - target) / pivotElement;
        const double enterOldValue = nonbasicValue_[static_cast<std::size_t>(enterCol)];

        // Every basic variable shifts by -colVec[r]*delta as the entering
        // variable moves; row leaveRow's *identity* is about to change from
        // leaveVar to enterCol, so it is overwritten with enterCol's new
        // value afterwards rather than kept as the generic update (which
        // would equal `target` -- leaveVar's value, not enterCol's).
        for (int r = 0; r < problem_.m; ++r) {
            xB_[static_cast<std::size_t>(r)] -= colVec[static_cast<std::size_t>(r)] * delta;
        }
        xB_[static_cast<std::size_t>(leaveRow)] = enterOldValue + delta;

        basisVar_[static_cast<std::size_t>(leaveRow)] = enterCol;
        status_[static_cast<std::size_t>(enterCol)] = BasisStatus::Basic;
        rowOfColumn_[static_cast<std::size_t>(enterCol)] = leaveRow;
        rowOfColumn_[static_cast<std::size_t>(leaveVar)] = -1;

        if (std::abs(hiLeave - loLeave) <= options_.primalFeasibilityTolerance) {
            status_[static_cast<std::size_t>(leaveVar)] = BasisStatus::Fixed;
        } else {
            status_[static_cast<std::size_t>(leaveVar)] =
                leaveTooHigh ? BasisStatus::AtUpper : BasisStatus::AtLower;
        }
        nonbasicValue_[static_cast<std::size_t>(leaveVar)] = target;

        // Product-form-of-inverse update: subtract the scaled pivot row from
        // every other row *before* the pivot row itself is rescaled.
        for (int r = 0; r < problem_.m; ++r) {
            if (r == leaveRow) {
                continue;
            }
            const double factor = colVec[static_cast<std::size_t>(r)] / pivotElement;
            if (factor == 0.0) {
                continue;
            }
            auto &row = inverse_[static_cast<std::size_t>(r)];
            const auto &pivotRow = inverse_[static_cast<std::size_t>(leaveRow)];
            for (int k = 0; k < problem_.m; ++k) {
                row[static_cast<std::size_t>(k)] -= factor * pivotRow[static_cast<std::size_t>(k)];
            }
        }
        auto &pivotRow = inverse_[static_cast<std::size_t>(leaveRow)];
        for (int k = 0; k < problem_.m; ++k) {
            pivotRow[static_cast<std::size_t>(k)] /= pivotElement;
        }
    }

    const Problem &problem_;
    DualSimplexOptions options_;

    std::vector<int> basisVar_;             // row -> column index
    std::vector<BasisStatus> status_;        // column -> status
    std::vector<double> nonbasicValue_;      // column -> current value (meaningful only when nonbasic)
    std::vector<std::vector<double>> inverse_; // m x m basis inverse
    std::vector<int> rowOfColumn_;           // column -> row (-1 if nonbasic)
    std::vector<double> xB_;                 // row -> value of basisVar_[row]

    DualSimplexStatus finalStatus_ = DualSimplexStatus::IterationLimit;
    std::vector<double> farkasRay_;
    std::int64_t iterations_ = 0;
};

DualSimplexResult runDualSimplex(
    const Problem &problem,
    std::vector<int> basisVar,
    std::vector<BasisStatus> status,
    std::vector<double> nonbasicValue,
    const DualSimplexOptions &options)
{
    const int n = problem.n;
    const int m = problem.m;

    std::vector<std::vector<double>> basisMatrix(
        static_cast<std::size_t>(m), std::vector<double>(static_cast<std::size_t>(m)));
    for (int col = 0; col < m; ++col) {
        const int variable = basisVar[static_cast<std::size_t>(col)];
        for (int row = 0; row < m; ++row) {
            double value;
            if (variable >= n) {
                value = ((variable - n) == row) ? -1.0 : 0.0;
            } else {
                value = problem.columns[static_cast<std::size_t>(variable)][static_cast<std::size_t>(row)];
            }
            basisMatrix[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)] = value;
        }
    }

    // Fast path for the untouched all-slack basis.
    //
    // buildColdStartBasis seeds basisVar[col] = n + col, i.e. every basis
    // column is that row's own logical, whose column in [A | -I] is -e_col.
    // The basis matrix is then exactly -I, whose inverse is exactly -I -- no
    // arithmetic required, and no rounding introduced. Running the general
    // O(m^3) Gauss-Jordan on it instead is pure waste, and it dominated the
    // solve: measured on a 1500-row tridiagonal LP that needs only 2 pivots,
    // the whole solve took 3.4s at ANY iteration budget including zero,
    // because all of it was this inversion.
    //
    // The guard is deliberately exact (logical AND in its own row). A basis
    // with any structural crashed in, or with logicals permuted across rows,
    // falls through to the general path unchanged.
    bool allSlackIdentity = true;
    for (int col = 0; col < m && allSlackIdentity; ++col) {
        const int variable = basisVar[static_cast<std::size_t>(col)];
        allSlackIdentity = (variable >= n) && ((variable - n) == col);
    }

    std::optional<std::vector<std::vector<double>>> inverted;
    if (allSlackIdentity) {
        std::vector<std::vector<double>> identity(
            static_cast<std::size_t>(m), std::vector<double>(static_cast<std::size_t>(m), 0.0));
        for (int i = 0; i < m; ++i) {
            identity[static_cast<std::size_t>(i)][static_cast<std::size_t>(i)] = -1.0;
        }
        inverted = std::move(identity);
    } else {
        inverted = invert(std::move(basisMatrix), options.pivotTolerance);
    }
    if (!inverted) {
        throw std::invalid_argument("DualSimplexSolver: the constructed basis matrix is singular");
    }

    Workspace workspace(
        problem, options, std::move(basisVar), std::move(status), std::move(nonbasicValue),
        std::move(*inverted));
    workspace.run();
    return workspace.buildResult();
}

DualSimplexResult finalizeSign(const model::Model &model, DualSimplexResult result) {
    if (model.objective.sense == model::ObjectiveSense::Maximize) {
        result.objectiveValue = -result.objectiveValue;
        for (double &d : result.dual) {
            d = -d;
        }
    }
    // The constant term belongs to the model's objective, so it is added here,
    // after the sense flip, where the value is already in the model's own sense.
    //
    // Leaving it out reported c'x instead of c'x + offset. That is not only a
    // wrong number: the branch-and-cut driver stores both LP objectives and
    // heuristic objectives in the same incumbent, and the heuristics compute
    // theirs from model.objective directly, offset included. The two scales
    // then differ by the offset, so incumbent comparisons and node pruning are
    // made between values that are not comparable, and a negative offset can
    // prune away the true optimum rather than merely misreport it.
    if (result.status != DualSimplexStatus::Infeasible &&
        result.status != DualSimplexStatus::Unbounded) {
        result.objectiveValue += model.objective.offset;
    }
    return result;
}

} // namespace

DualSimplexResult DualSimplexSolver::solve(
    const model::Model &model,
    const DualSimplexOptions &options) const
{
    const Problem problem = buildProblem(model);

    std::vector<int> basisVar;
    std::vector<BasisStatus> status;
    std::vector<double> nonbasicValue;

    const CrashOutcome outcome = buildColdStartBasis(problem, options, basisVar, status, nonbasicValue);
    if (outcome == CrashOutcome::Unbounded) {
        DualSimplexResult result;
        result.status = DualSimplexStatus::Unbounded;
        return result;
    }

    DualSimplexResult result = runDualSimplex(
        problem, std::move(basisVar), std::move(status), std::move(nonbasicValue), options);
    return finalizeSign(model, std::move(result));
}

DualSimplexResult DualSimplexSolver::solveFromBasis(
    const model::Model &model,
    const BasisState &initialBasis,
    const DualSimplexOptions &options) const
{
    const Problem problem = buildProblem(model);

    if (initialBasis.variableStatus.size() != static_cast<std::size_t>(problem.n) ||
        initialBasis.constraintStatus.size() != static_cast<std::size_t>(problem.m)) {
        throw std::invalid_argument(
            "DualSimplexSolver::solveFromBasis: basis size does not match model");
    }

    const std::size_t total = static_cast<std::size_t>(problem.n + problem.m);
    std::vector<BasisStatus> status(total);
    std::vector<double> nonbasicValue(total, 0.0);

    for (int j = 0; j < problem.n; ++j) {
        status[static_cast<std::size_t>(j)] = initialBasis.variableStatus[static_cast<std::size_t>(j)];
    }
    for (int i = 0; i < problem.m; ++i) {
        status[static_cast<std::size_t>(problem.n + i)] =
            initialBasis.constraintStatus[static_cast<std::size_t>(i)];
    }

    std::vector<int> basisVar;
    basisVar.reserve(static_cast<std::size_t>(problem.m));
    for (int j = 0; j < problem.n + problem.m; ++j) {
        const BasisStatus st = status[static_cast<std::size_t>(j)];
        if (st == BasisStatus::Basic) {
            basisVar.push_back(j);
        } else {
            nonbasicValue[static_cast<std::size_t>(j)] = boundedNonbasicValue(
                st, problem.lower[static_cast<std::size_t>(j)], problem.upper[static_cast<std::size_t>(j)]);
        }
    }

    if (static_cast<int>(basisVar.size()) != problem.m) {
        throw std::invalid_argument(
            "DualSimplexSolver::solveFromBasis: basis does not mark exactly one Basic "
            "column per constraint row");
    }

    DualSimplexResult result = runDualSimplex(
        problem, std::move(basisVar), std::move(status), std::move(nonbasicValue), options);
    return finalizeSign(model, std::move(result));
}

} // namespace milp
