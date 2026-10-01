#pragma once
// Compressed-sparse-column matrix, plus the symmetric LDL' machinery the
// interior-point method needs.
//
// This is deliberately separate from qp::SparseMatrix. That class is CSR and is
// built around K = P + sigma*I + rho*A'A for ADMM, whose factor is Cholesky of
// a positive definite matrix with no fill-reducing permutation. The barrier
// method factorises a symmetric QUASIDEFINITE matrix -- negative definite in
// its leading block, positive definite in its trailing block -- which needs
// signed pivots, and it refactorises every iteration, which makes a
// fill-reducing ordering the difference between usable and not.
#include <cstddef>
#include <string>
#include <vector>

namespace barrier {

using Index = int;

// Column-major sparse matrix. Row indices within a column are ascending and
// unique after build(); duplicates are summed.
struct SparseCsc {
    Index rows = 0, cols = 0;
    std::vector<Index> columnStart;  // size cols+1
    std::vector<Index> rowIndex;
    std::vector<double> value;

    [[nodiscard]] std::size_t nonzeros() const noexcept { return value.size(); }
    [[nodiscard]] bool validate() const noexcept;

    // y = A*x (y is resized and overwritten).
    void multiply(const std::vector<double>& x, std::vector<double>& y) const;
    // y = A'*x (y is resized and overwritten).
    void transposeMultiply(const std::vector<double>& x, std::vector<double>& y) const;
};

// Triplet accumulator. Zero values are kept: a structural zero that becomes
// nonzero on a later refactorisation must already be in the pattern, because
// the symbolic analysis is computed once and reused.
class TripletBuilder {
public:
    TripletBuilder(Index rows, Index cols) : rows_(rows), cols_(cols) {}
    void add(Index row, Index col, double value) {
        row_.push_back(row); col_.push_back(col); value_.push_back(value);
    }
    [[nodiscard]] SparseCsc build() const;
    [[nodiscard]] std::size_t size() const noexcept { return value_.size(); }

private:
    Index rows_, cols_;
    std::vector<Index> row_, col_;
    std::vector<double> value_;
};

// Approximate minimum degree ordering.
//
// Computes a fill-reducing symmetric permutation of a symmetric matrix given
// the pattern of its lower triangle. Follows Amestoy, Davis & Duff: a quotient
// graph of supervariables and eliminated elements, degrees bounded by AMD's
// external-degree approximation rather than computed exactly, with element
// absorption and indistinguishable-supervariable detection.
//
// The approximation is the point. An exact minimum-degree update costs more
// than the factorisation it is trying to cheapen; AMD's bound is within a small
// factor of the true degree and is what every production code uses.
//
// `pattern` needs only the lower triangle; the upper is implied. Diagonal
// entries are ignored. Returns `permutation` with permutation[k] = the original
// index eliminated k-th.
void approximateMinimumDegree(const SparseCsc& pattern,
                              std::vector<Index>& permutation);

// y = A*x for a symmetric A stored as its upper triangle by column.
void multiplySymmetric(const SparseCsc& upper, const std::vector<double>& x,
                       std::vector<double>& y);

// Symmetric permutation: C = P*A*P'. Both input and output hold the UPPER
// triangle by column. `permutation[k] = i` means original row/column i becomes
// index k.
[[nodiscard]] SparseCsc permuteSymmetric(const SparseCsc& lower,
                                         const std::vector<Index>& permutation);

// Sparse LDL' of a symmetric matrix, up-looking (Davis).
//
// A = L*D*L' with L unit lower triangular and D diagonal. D is allowed to be
// NEGATIVE: that is what makes this usable for the quasidefinite augmented
// system, where the leading block contributes negative pivots. No pivoting is
// performed, which is sound here and only here -- a symmetric quasidefinite
// matrix factorises stably under ANY symmetric permutation (Vanderbei 1995), so
// the fill-reducing order chosen by AMD is also a numerically valid order. That
// is exactly why the augmented system is regularised into quasidefiniteness
// before it reaches this code rather than being factorised as it stands.
class LdlFactorization {
public:
    // Symbolic analysis of the lower triangle: elimination tree and column
    // counts. Done once; the pattern is fixed across refactorisations because
    // only the numeric values of the diagonal change between iterations.
    [[nodiscard]] bool analyse(const SparseCsc& lower);

    // Numeric factorisation against the analysed pattern. `lower` must have the
    // identical pattern passed to analyse(). Returns false when a pivot is too
    // small to divide by, leaving the factor invalid.
    [[nodiscard]] bool factor(const SparseCsc& lower, double pivotTolerance);

    // Solves A*x = b in place. Requires a valid factor.
    void solve(std::vector<double>& x) const;

    // Solves with fixed-point iterative refinement against the original matrix.
    //
    // Necessary, not optional. An unpivoted LDL' of a quasidefinite matrix is
    // stable in the sense that it completes, but its accuracy degrades as the
    // definite blocks shrink -- and the barrier method drives the dual
    // regularisation Rd down to ~1e-8 on purpose, which is precisely that
    // regime. Measured on a 70+40 augmented system with Rd = 1e-8: the raw
    // solve leaves a residual of 1.1e-8 where dense partial pivoting achieves
    // 2.4e-15. Two rounds of refinement cost two matrix-vector products and two
    // triangular solves and recover most of that gap, which is why every
    // production interior-point code does this rather than pivoting.
    void solveRefined(const SparseCsc& upper, std::vector<double>& b, int rounds) const;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] std::size_t factorNonzeros() const noexcept { return lRowIndex_.size(); }
    // Count of pivots that had to be pushed to pivotTolerance's magnitude.
    [[nodiscard]] int repairedPivots() const noexcept { return repairedPivots_; }
    // Signature of D, used to confirm the quasidefinite inertia is what the
    // augmented system predicts: negativePivots() should equal the leading
    // block's dimension.
    [[nodiscard]] int negativePivots() const noexcept { return negativePivots_; }

private:
    Index n_ = 0;
    bool valid_ = false;
    int repairedPivots_ = 0, negativePivots_ = 0;
    std::vector<Index> parent_, columnCount_, lColumnStart_, lRowIndex_;
    std::vector<double> lValue_, diagonal_;
    // Factorisation scratch, retained so a refactorisation allocates nothing.
    mutable std::vector<double> y_;
    mutable std::vector<Index> pattern_, flag_;
};

}  // namespace barrier
