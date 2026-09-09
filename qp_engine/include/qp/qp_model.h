#pragma once

#include "qp/parallel.h"
#include "qp/qp_types.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

// GCC 6 issues a spurious "-Wattributes" warning for the [[nodiscard]]
// attribute on inline member functions. Silence it; the attribute is still
// applied to the function and the warning is a false positive on this
// compiler. Newer GCCs and Clang do not warn.
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 7
#pragma GCC diagnostic ignored "-Wattributes"
#endif

namespace qp {

// ---------------------------------------------------------------------------
// Sparse matrix in CSR format (read-only after construction)
// ---------------------------------------------------------------------------

class SparseMatrix {
public:
    SparseMatrix() = default;

    // Build from triplets (row, column, value). Duplicates are summed; zero
    // entries after summing are dropped.
    static SparseMatrix fromTriplets(
        int rows,
        int columns,
        const std::vector<double>& row,
        const std::vector<double>& col,
        const std::vector<double>& val
    );

    [[nodiscard]] int rows() const noexcept { return rows_; }
    [[nodiscard]] int columns() const noexcept { return columns_; }
    [[nodiscard]] std::size_t nonzeros() const noexcept { return csrValues_.size(); }

    // Returns true if the CSR structure is internally consistent.
    [[nodiscard]] bool validate() const noexcept;

    // Contiguous slices of rows / columns balanced by NONZERO count, so each
    // worker gets an equal share of the work rather than an equal share of the
    // lines. Several chunks per worker, claimed dynamically, so one slow core
    // cannot hold the barrier on a hybrid CPU.
    struct Plan {
        std::vector<int> rowChunk;
        std::vector<int> columnChunk;
        int parts = 1;
        [[nodiscard]] int rowChunkCount() const noexcept {
            return static_cast<int>(rowChunk.size()) - 1;
        }
        [[nodiscard]] int columnChunkCount() const noexcept {
            return static_cast<int>(columnChunk.size()) - 1;
        }
    };
    [[nodiscard]] Plan buildPlan(int parts, int chunksPerPart = 8) const;

    // result <- A * x.  result is resized as needed.
    void multiply(const std::vector<double>& x, std::vector<double>& result) const;

    // result <- A^T * y.  result is resized as needed.
    void transposeMultiply(const std::vector<double>& y, std::vector<double>& result) const;

    // Parallel forms. A null executor or plan selects serial execution.
    void multiply(const std::vector<double>& x, std::vector<double>& result,
                  Executor* executor, const Plan* plan) const;
    void transposeMultiply(const std::vector<double>& y, std::vector<double>& result,
                           Executor* executor, const Plan* plan) const;

    void multiplyRange(const double* x, double* result, int begin, int end) const noexcept;
    void transposeMultiplyRange(const double* y, double* result, int begin, int end) const noexcept;

    [[nodiscard]] const std::vector<Offset>& cscColumnStart() const noexcept {
        return cscColumnStart_;
    }
    [[nodiscard]] const std::vector<Index>& cscRowIndex() const noexcept {
        return cscRowIndex_;
    }
    [[nodiscard]] const std::vector<double>& cscValues() const noexcept {
        return cscValues_;
    }

    [[nodiscard]] const std::vector<Offset>& csrRowStart() const noexcept {
        return csrRowStart_;
    }
    [[nodiscard]] const std::vector<Index>& csrColumnIndex() const noexcept {
        return csrColumnIndex_;
    }
    [[nodiscard]] const std::vector<double>& csrValues() const noexcept {
        return csrValues_;
    }

    // Returns a logically equivalent matrix with each entry multiplied by
    // rowScale[i] * colScale[j].  The result keeps the same sparsity
    // pattern; zero entries stay zero.  Used by Ruiz equilibration.
    [[nodiscard]] SparseMatrix scaled(
        const std::vector<double>& rowScale,
        const std::vector<double>& colScale
    ) const;

private:
    int rows_ = 0;
    int columns_ = 0;

    // A^T*y is a scatter over CSR (write conflicts, poor locality) but a gather
    // over CSC. Storing both is what makes the transpose product both fast and
    // safely parallel, and is what pdlp_engine does for the same reason.
    std::vector<Offset> csrRowStart_;
    std::vector<Index> csrColumnIndex_;
    std::vector<double> csrValues_;

    std::vector<Offset> cscColumnStart_;
    std::vector<Index>  cscRowIndex_;
    std::vector<double> cscValues_;
};

// ---------------------------------------------------------------------------
// QP in standard form
//
//   minimize  0.5 * x^T * P * x  +  q^T * x
//   subject to  l <= A * x <= u
//
// P  : SPD Hessian (n x n). Stored as the FULL symmetric matrix -- both (i,j)
//      and (j,i) must be present. Not triangular: admm_solver computes P*x with
//      a plain CSR product, which would silently drop half the Hessian.
// q  : linear objective (n)
// A  : constraint matrix (m x n), may have m == 0
// l  : lower bounds on A*x (m), may contain -inf
// u  : upper bounds on A*x (m), may contain +inf
// ---------------------------------------------------------------------------

struct QpModel {
    SparseMatrix P;   // Hessian, size n x n
    SparseMatrix A;   // constraints, size m x n (m may be 0)

    std::vector<double> q;   // size n
    std::vector<double> l;   // size m
    std::vector<double> u;   // size m

    [[nodiscard]] int numVariables() const noexcept { return P.columns(); }
    [[nodiscard]] int numConstraints() const noexcept { return A.rows(); }
    [[nodiscard]] bool hasConstraints() const noexcept { return numConstraints() > 0; }

    // Throws std::invalid_argument if dimensions are inconsistent or bounds
    // are malformed.
    void validate() const {
        const int n = numVariables();
        const int m = numConstraints();

        if (P.rows() != n || P.columns() != n) {
            throw std::invalid_argument(
                "QpModel: P must be n x n, got " + std::to_string(P.rows()) +
                " x " + std::to_string(P.columns()));
        }
        if (A.columns() != n) {
            throw std::invalid_argument(
                "QpModel: A must have n columns, got " + std::to_string(A.columns()));
        }
        if (static_cast<int>(q.size()) != n) {
            throw std::invalid_argument(
                "QpModel: q must have length n, got " + std::to_string(q.size()));
        }
        if (static_cast<int>(l.size()) != m) {
            throw std::invalid_argument(
                "QpModel: l must have length m, got " + std::to_string(l.size()));
        }
        if (static_cast<int>(u.size()) != m) {
            throw std::invalid_argument(
                "QpModel: u must have length m, got " + std::to_string(u.size()));
        }

        for (int j = 0; j < n; ++j) {
            if (!std::isfinite(q[j])) {
                throw std::invalid_argument(
                    "QpModel: q[" + std::to_string(j) + "] is not finite");
            }
        }
        for (int i = 0; i < m; ++i) {
            if (std::isnan(l[i]) || std::isnan(u[i])) {
                throw std::invalid_argument(
                    "QpModel: bound at constraint " + std::to_string(i) + " is NaN");
            }
            if (l[i] > u[i]) {
                throw std::invalid_argument(
                    "QpModel: l[" + std::to_string(i) + "] > u[" +
                    std::to_string(i) + "]");
            }
        }
    }
};

}  // namespace qp
