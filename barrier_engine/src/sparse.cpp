#include "barrier/sparse.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace barrier {

bool SparseCsc::validate() const noexcept {
    if (rows < 0 || cols < 0) return false;
    if (columnStart.size() != static_cast<std::size_t>(cols) + 1) return false;
    if (columnStart.front() != 0) return false;
    if (rowIndex.size() != value.size()) return false;
    if (static_cast<std::size_t>(columnStart.back()) != rowIndex.size()) return false;
    for (Index j = 0; j < cols; ++j) {
        if (columnStart[j] > columnStart[j + 1]) return false;
        for (Index p = columnStart[j]; p < columnStart[j + 1]; ++p) {
            if (rowIndex[p] < 0 || rowIndex[p] >= rows) return false;
            // Ascending and unique within the column.
            if (p > columnStart[j] && rowIndex[p] <= rowIndex[p - 1]) return false;
        }
    }
    return true;
}

void SparseCsc::multiply(const std::vector<double>& x, std::vector<double>& y) const {
    y.assign(static_cast<std::size_t>(rows), 0.0);
    for (Index j = 0; j < cols; ++j) {
        const double xj = x[static_cast<std::size_t>(j)];
        if (xj == 0.0) continue;
        for (Index p = columnStart[j]; p < columnStart[j + 1]; ++p)
            y[static_cast<std::size_t>(rowIndex[p])] += value[p] * xj;
    }
}

void SparseCsc::transposeMultiply(const std::vector<double>& x, std::vector<double>& y) const {
    y.assign(static_cast<std::size_t>(cols), 0.0);
    for (Index j = 0; j < cols; ++j) {
        double sum = 0.0;
        for (Index p = columnStart[j]; p < columnStart[j + 1]; ++p)
            sum += value[p] * x[static_cast<std::size_t>(rowIndex[p])];
        y[static_cast<std::size_t>(j)] = sum;
    }
}

SparseCsc TripletBuilder::build() const {
    SparseCsc out;
    out.rows = rows_;
    out.cols = cols_;
    out.columnStart.assign(static_cast<std::size_t>(cols_) + 1, 0);
    // Counting sort by column, then order each column's rows.
    for (Index c : col_) ++out.columnStart[static_cast<std::size_t>(c) + 1];
    for (Index j = 0; j < cols_; ++j) out.columnStart[j + 1] += out.columnStart[j];

    std::vector<Index> cursor(out.columnStart.begin(), out.columnStart.end() - 1);
    std::vector<Index> scatterRow(value_.size());
    std::vector<double> scatterValue(value_.size());
    for (std::size_t k = 0; k < value_.size(); ++k) {
        const Index slot = cursor[static_cast<std::size_t>(col_[k])]++;
        scatterRow[static_cast<std::size_t>(slot)] = row_[k];
        scatterValue[static_cast<std::size_t>(slot)] = value_[k];
    }

    out.rowIndex.reserve(value_.size());
    out.value.reserve(value_.size());
    std::vector<Index> order;
    std::vector<Index> newStart(static_cast<std::size_t>(cols_) + 1, 0);
    for (Index j = 0; j < cols_; ++j) {
        const Index begin = out.columnStart[j], end = out.columnStart[j + 1];
        order.resize(static_cast<std::size_t>(end - begin));
        std::iota(order.begin(), order.end(), begin);
        std::sort(order.begin(), order.end(), [&](Index a, Index b) {
            return scatterRow[static_cast<std::size_t>(a)] < scatterRow[static_cast<std::size_t>(b)];
        });
        // Sum duplicates so a repeated triplet cannot create two entries for
        // one position, which would silently corrupt the symbolic pattern.
        for (Index k : order) {
            const Index r = scatterRow[static_cast<std::size_t>(k)];
            const double v = scatterValue[static_cast<std::size_t>(k)];
            if (!out.rowIndex.empty() &&
                static_cast<Index>(out.rowIndex.size()) > newStart[j] &&
                out.rowIndex.back() == r) {
                out.value.back() += v;
            } else {
                out.rowIndex.push_back(r);
                out.value.push_back(v);
            }
        }
        newStart[j + 1] = static_cast<Index>(out.rowIndex.size());
    }
    out.columnStart = newStart;
    return out;
}

void multiplySymmetric(const SparseCsc& upper, const std::vector<double>& x,
                       std::vector<double>& y) {
    y.assign(static_cast<std::size_t>(upper.cols), 0.0);
    for (Index j = 0; j < upper.cols; ++j) {
        const double xj = x[static_cast<std::size_t>(j)];
        for (Index p = upper.columnStart[j]; p < upper.columnStart[j + 1]; ++p) {
            const Index i = upper.rowIndex[p];
            const double v = upper.value[p];
            y[static_cast<std::size_t>(i)] += v * xj;
            // The mirrored entry, except on the diagonal where there is none.
            if (i != j) y[static_cast<std::size_t>(j)] += v * x[static_cast<std::size_t>(i)];
        }
    }
}

SparseCsc permuteSymmetric(const SparseCsc& upper, const std::vector<Index>& permutation) {
    const Index n = upper.cols;
    std::vector<Index> inverse(static_cast<std::size_t>(n));
    for (Index k = 0; k < n; ++k) inverse[static_cast<std::size_t>(permutation[k])] = k;

    TripletBuilder builder(n, n);
    for (Index j = 0; j < n; ++j) {
        for (Index p = upper.columnStart[j]; p < upper.columnStart[j + 1]; ++p) {
            const Index i = upper.rowIndex[p];
            Index pi = inverse[static_cast<std::size_t>(i)];
            Index pj = inverse[static_cast<std::size_t>(j)];
            // Keep the result in the same upper-triangle-by-column convention.
            if (pi > pj) std::swap(pi, pj);
            builder.add(pi, pj, upper.value[p]);
        }
    }
    return builder.build();
}

// ---------------------------------------------------------------------------
// LDL'
//
// Input convention is the UPPER triangle in column form, matching Davis's LDL:
// column k holds the entries A(i,k) with i <= k. For a symmetric matrix that is
// the same storage as the lower triangle by rows.
// ---------------------------------------------------------------------------

bool LdlFactorization::analyse(const SparseCsc& upper) {
    if (upper.rows != upper.cols) return false;
    n_ = upper.cols;
    parent_.assign(static_cast<std::size_t>(n_), -1);
    columnCount_.assign(static_cast<std::size_t>(n_), 0);
    flag_.assign(static_cast<std::size_t>(n_), 0);

    for (Index k = 0; k < n_; ++k) {
        parent_[static_cast<std::size_t>(k)] = -1;
        flag_[static_cast<std::size_t>(k)] = k;
        columnCount_[static_cast<std::size_t>(k)] = 0;
        for (Index p = upper.columnStart[k]; p < upper.columnStart[k + 1]; ++p) {
            Index i = upper.rowIndex[p];
            if (i >= k) continue;
            // Walk i to the root of the elimination tree, stopping where this
            // k has already been. Every node on that path gains an entry in
            // row k of L.
            for (; flag_[static_cast<std::size_t>(i)] != k;
                   i = parent_[static_cast<std::size_t>(i)]) {
                if (parent_[static_cast<std::size_t>(i)] == -1)
                    parent_[static_cast<std::size_t>(i)] = k;
                ++columnCount_[static_cast<std::size_t>(i)];
                flag_[static_cast<std::size_t>(i)] = k;
            }
        }
    }

    lColumnStart_.assign(static_cast<std::size_t>(n_) + 1, 0);
    for (Index k = 0; k < n_; ++k)
        lColumnStart_[static_cast<std::size_t>(k) + 1] =
            lColumnStart_[static_cast<std::size_t>(k)] + columnCount_[static_cast<std::size_t>(k)];

    const std::size_t total = static_cast<std::size_t>(lColumnStart_[static_cast<std::size_t>(n_)]);
    lRowIndex_.assign(total, 0);
    lValue_.assign(total, 0.0);
    diagonal_.assign(static_cast<std::size_t>(n_), 0.0);
    y_.assign(static_cast<std::size_t>(n_), 0.0);
    pattern_.assign(static_cast<std::size_t>(n_), 0);
    valid_ = false;
    return true;
}

bool LdlFactorization::factor(const SparseCsc& upper, double pivotTolerance) {
    if (upper.cols != n_ || n_ == 0) {
        valid_ = n_ == 0;
        return valid_;
    }
    std::vector<Index> running(static_cast<std::size_t>(n_), 0);
    std::fill(y_.begin(), y_.end(), 0.0);
    repairedPivots_ = 0;
    negativePivots_ = 0;

    for (Index k = 0; k < n_; ++k) {
        y_[static_cast<std::size_t>(k)] = 0.0;
        Index top = n_;
        flag_[static_cast<std::size_t>(k)] = k;
        running[static_cast<std::size_t>(k)] = 0;
        for (Index p = upper.columnStart[k]; p < upper.columnStart[k + 1]; ++p) {
            Index i = upper.rowIndex[p];
            if (i > k) continue;
            y_[static_cast<std::size_t>(i)] += upper.value[p];
            Index len = 0;
            for (; flag_[static_cast<std::size_t>(i)] != k;
                   i = parent_[static_cast<std::size_t>(i)]) {
                pattern_[static_cast<std::size_t>(len++)] = i;
                flag_[static_cast<std::size_t>(i)] = k;
            }
            while (len > 0) pattern_[static_cast<std::size_t>(--top)] = pattern_[static_cast<std::size_t>(--len)];
        }

        double dk = y_[static_cast<std::size_t>(k)];
        y_[static_cast<std::size_t>(k)] = 0.0;
        // Sparse triangular solve down the reach of row k, in topological order.
        for (; top < n_; ++top) {
            const Index i = pattern_[static_cast<std::size_t>(top)];
            const double yi = y_[static_cast<std::size_t>(i)];
            y_[static_cast<std::size_t>(i)] = 0.0;
            const Index end = lColumnStart_[static_cast<std::size_t>(i)] + running[static_cast<std::size_t>(i)];
            for (Index p = lColumnStart_[static_cast<std::size_t>(i)]; p < end; ++p)
                y_[static_cast<std::size_t>(lRowIndex_[static_cast<std::size_t>(p)])] -=
                    lValue_[static_cast<std::size_t>(p)] * yi;
            const double lki = yi / diagonal_[static_cast<std::size_t>(i)];
            dk -= lki * yi;
            lRowIndex_[static_cast<std::size_t>(end)] = k;
            lValue_[static_cast<std::size_t>(end)] = lki;
            ++running[static_cast<std::size_t>(i)];
        }

        if (!std::isfinite(dk)) { valid_ = false; return false; }
        // A quasidefinite matrix has no zero pivots in exact arithmetic, so a
        // pivot at or below the tolerance means the regularisation upstream was
        // too weak. Nudge it along its OWN sign and count it: silently flipping
        // a sign here would change the inertia and hide the real problem.
        if (std::abs(dk) < pivotTolerance) {
            dk = (dk < 0.0 ? -pivotTolerance : pivotTolerance);
            ++repairedPivots_;
        }
        if (dk < 0.0) ++negativePivots_;
        diagonal_[static_cast<std::size_t>(k)] = dk;
    }
    valid_ = true;
    return true;
}

void LdlFactorization::solveRefined(const SparseCsc& upper, std::vector<double>& b,
                                    int rounds) const {
    if (!valid_ || n_ == 0) { solve(b); return; }
    const std::vector<double> rhs = b;
    solve(b);
    std::vector<double> product, correction;
    for (int round = 0; round < rounds; ++round) {
        multiplySymmetric(upper, b, product);
        correction.resize(static_cast<std::size_t>(n_));
        double worst = 0.0;
        for (Index i = 0; i < n_; ++i) {
            const double residual = rhs[static_cast<std::size_t>(i)] - product[static_cast<std::size_t>(i)];
            correction[static_cast<std::size_t>(i)] = residual;
            worst = std::max(worst, std::abs(residual));
        }
        if (!std::isfinite(worst) || worst == 0.0) break;
        solve(correction);
        bool usable = true;
        for (Index i = 0; i < n_; ++i)
            if (!std::isfinite(correction[static_cast<std::size_t>(i)])) { usable = false; break; }
        // A non-finite correction means the factor cannot improve this solve;
        // keep the last finite iterate rather than poisoning it.
        if (!usable) break;
        for (Index i = 0; i < n_; ++i) b[static_cast<std::size_t>(i)] += correction[static_cast<std::size_t>(i)];
    }
}

void LdlFactorization::solve(std::vector<double>& x) const {
    if (!valid_) return;
    for (Index j = 0; j < n_; ++j) {
        const double xj = x[static_cast<std::size_t>(j)];
        for (Index p = lColumnStart_[static_cast<std::size_t>(j)];
             p < lColumnStart_[static_cast<std::size_t>(j) + 1]; ++p)
            x[static_cast<std::size_t>(lRowIndex_[static_cast<std::size_t>(p)])] -=
                lValue_[static_cast<std::size_t>(p)] * xj;
    }
    for (Index j = 0; j < n_; ++j) x[static_cast<std::size_t>(j)] /= diagonal_[static_cast<std::size_t>(j)];
    for (Index j = n_ - 1; j >= 0; --j) {
        double sum = x[static_cast<std::size_t>(j)];
        for (Index p = lColumnStart_[static_cast<std::size_t>(j)];
             p < lColumnStart_[static_cast<std::size_t>(j) + 1]; ++p)
            sum -= lValue_[static_cast<std::size_t>(p)] *
                   x[static_cast<std::size_t>(lRowIndex_[static_cast<std::size_t>(p)])];
        x[static_cast<std::size_t>(j)] = sum;
    }
}

}  // namespace barrier
