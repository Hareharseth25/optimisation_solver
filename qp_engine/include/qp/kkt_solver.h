#pragma once

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 7
#pragma GCC diagnostic ignored "-Wattributes"
#endif

#include "qp/qp_model.h"
#include "qp/qp_types.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace qp {

// KKT linear-system solver for the ADMM x-update step.
//
// At each ADMM iteration we solve
//
//   K * x_next = b   where   K = P + rho * A^T * A
//
// K is symmetric positive semidefinite (SPD when P is SPD and rho > 0).
// This solver computes a sparse Cholesky / LDLT factorisation of K and
// reuses it across iterations. When rho changes, the factor is rebuilt.
//
// Dense path (n <= 500): standard column-major dense Cholesky.
// Sparse path (n > 500): left-looking sparse Cholesky via column elimination.
//
class KktSolver {
public:
    explicit KktSolver(const QpModel& model, double rho);
    bool refactor(double rho);
    bool solve(std::vector<double>& x) const;

    // Proximal regularisation of the x-subproblem: K = P + sigma*I + rho*A^T*A.
    //
    // This exists so the factorisation stays definite when P is singular -- which
    // it always is for an LP, where P = 0. It is deliberately part of the LINEAR
    // SYSTEM and not of the model: the x-update carries a matching +sigma*x_prev
    // on the right-hand side, so at the fixed point x = x_prev the two sigma
    // terms cancel and the iteration converges to the solution of the ORIGINAL
    // problem. That cancellation is the whole point.
    //
    // Regularising the model instead -- folding an epsilon onto P's diagonal in
    // the adapter, which is what this engine used to do -- has no such
    // cancellation. It silently solves a different problem: with epsilon = 1e-12
    // the objective was wrong by 0.5*epsilon*||x||^2, which is 5e-8 relative at
    // ||x|| = 1e5 (already past the 1e-8 tolerance) and 50.0 absolute at
    // ||x|| = 1e7, and it turned unbounded LPs into "optimal" ones with the
    // spurious minimiser x = -q/epsilon = 1e12.
    //
    // 1e-6 follows OSQP. Because it cancels, it can be far larger -- and hence
    // far better conditioned -- than any value that has to be small enough to
    // hide in the answer.
    static constexpr double kSigma = 1e-6;

    [[nodiscard]] bool usesDense() const noexcept { return useDense_; }
    [[nodiscard]] double measuredDensity() const noexcept { return density_; }
    [[nodiscard]] bool isFactorValid() const noexcept { return factorValid_; }

    // Public for use by the implementation; clients should not depend on these.
    // Dense / sparse Cholesky selection.
    //
    // Selection is by the DENSITY of the KKT matrix, not by its dimension. The
    // dense path costs O(n^3) time and O(n^2) memory however sparse the problem
    // is, so dimension alone is the wrong question: measured on this codebase a
    // 400-variable problem at 1% density solves 77x faster on the sparse path
    // (0.0017 s against 0.1314 s), while the same dimension at 100% density
    // solves 1.6x faster on the dense one (0.1996 s against 0.3192 s). The old
    // rule -- dense whenever n <= 500 -- got both of those backwards.
    //
    // Density is measured from the symbolic pattern of K's lower triangle, not
    // estimated: an upper bound that ignores overlap read 3.2 on a matrix that
    // was 25% dense. 0.80 sits inside the measured gap -- the dense path only
    // won at 100% density, and lost by 33x at 25%.
    static double denseDensityThreshold;

    // Second condition: how dense A itself must be. Measured, a fully dense K is
    // not sufficient -- with a sparse A the per-iteration products stay cheap and
    // the sparse path wins overall even when the factor is dense.
    static double denseConstraintDensity;

    // Escape hatch for benchmarking: forces one path when set.
    enum class PathOverride { Auto, ForceDense, ForceSparse };
    static PathOverride pathOverride;
    static constexpr double kSingularityEpsilon = 1e-12;

private:
    void buildKkt(double rho);
    bool numericFactor();

    int n_ = 0;
    int m_ = 0;
    bool factorValid_ = false;
    double currentRho_ = 1.0;
    QpModel model_;
    bool useDense_ = false;
    double density_ = 1.0;

    // Dense path (n <= 500).
    struct DenseImpl;
    std::unique_ptr<DenseImpl> denseImpl_;

    // Sparse path (n > 500).
    struct SparseImpl;
    std::unique_ptr<SparseImpl> sparseImpl_;
};

}  // namespace qp

// The DenseImpl and SparseImpl struct definitions must be visible wherever
// KktSolver is instantiated. We place them in the header so they are always
// complete types.
namespace qp {

// Dense Cholesky implementation.
struct KktSolver::DenseImpl {
    DenseImpl(const QpModel& model, int n);
    bool factor(double rho);
    bool solve(std::vector<double>& rhs) const;

private:
    void buildDenseK(double rho);

    const QpModel* model_;
    int n_;
    std::vector<double> K_;  // column-major n×n, lower triangle is L after factor
};

// Sparse Cholesky implementation.
struct KktSolver::SparseImpl {
    SparseImpl(const QpModel& model, int n);
    bool factor(double rho);
    bool solve(std::vector<double>& rhs) const;

private:
    const QpModel* model_;
    int n_;

    // K's lower-triangle pattern and its numeric values. Kept SEPARATE from the
    // factor: factorising in place overwrote the pattern with L's, so the next
    // refactorisation refilled numbers against the wrong sparsity and produced a
    // different matrix.
    std::vector<std::vector<Index>> kPattern_;
    std::vector<std::vector<double>> kValues_;

    // The Cholesky factor, by column. lPattern_/lValues_ hold the STRICTLY lower
    // entries; lDiag_ holds L(j,j) itself, not L(j,j)^2.
    std::vector<std::vector<Index>> lPattern_;
    std::vector<std::vector<double>> lValues_;
    std::vector<double> lDiag_;

    // Factorisation scratch, kept across calls so refactorising allocates nothing.
    mutable std::vector<double> work_;
    mutable std::vector<Index> touched_;
    mutable std::vector<char> marked_;
};

}  // namespace qp
