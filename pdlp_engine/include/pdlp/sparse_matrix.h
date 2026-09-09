#pragma once

#include "pdlp/parallel.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pdlp {

class SparseMatrix;

// Execution plan for sparse products: the row chunks (for A*x) and column
// chunks (for A^T*y) that workers claim.
//
// Chunks are balanced by nonzero count rather than by row count, because
// industrial constraint matrices routinely contain a handful of near-dense rows
// (material balances, pooling constraints) and an even row split would leave one
// worker holding most of the work.
//
// There are several times more chunks than workers, and workers claim them
// dynamically. A static one-chunk-per-worker split is only optimal when all
// cores are equally fast, which is false on every hybrid CPU -- Apple Silicon
// and Intel P/E designs both pair fast and slow cores, and a barrier waits on
// the slowest. Dynamic claiming lets a fast core absorb several chunks while a
// slow one finishes its first.
struct SpmvPlan {
    std::vector<int> rowChunk;      // size rowChunkCount + 1
    std::vector<int> columnChunk;   // size columnChunkCount + 1
    int parts = 1;

    [[nodiscard]] int rowChunkCount() const noexcept {
        return static_cast<int>(rowChunk.size()) - 1;
    }
    [[nodiscard]] int columnChunkCount() const noexcept {
        return static_cast<int>(columnChunk.size()) - 1;
    }

    [[nodiscard]] static SpmvPlan build(
        const SparseMatrix& matrix,
        int parts,
        int chunksPerPart = 8
    );
};

struct MatrixTriplet {
    int row = 0;
    int column = 0;
    double value = 0.0;
};

class SparseMatrix {
public:
    using Offset = std::int64_t;

    SparseMatrix() = default;

    static SparseMatrix fromTriplets(
        int rows,
        int columns,
        std::vector<MatrixTriplet> triplets
    );

    [[nodiscard]] int rows() const noexcept { return rows_; }
    [[nodiscard]] int columns() const noexcept { return columns_; }
    [[nodiscard]] std::size_t nonzeros() const noexcept { return csrValues_.size(); }

    [[nodiscard]] bool validate() const noexcept;

    // Returns diag(rowScale) * A * diag(columnScale). The sparsity structure is
    // unchanged, so this copies the pattern and rescales values in place rather
    // than rebuilding the matrix from triplets.
    [[nodiscard]] SparseMatrix scaled(
        const std::vector<double>& rowScale,
        const std::vector<double>& columnScale
    ) const;

    // result <- A * x. `result` is resized as needed and fully overwritten.
    // Passing an executor and a matching plan runs the product in parallel.
    void multiply(
        const std::vector<double>& x,
        std::vector<double>& result,
        Executor* executor = nullptr,
        const SpmvPlan* plan = nullptr
    ) const;

    // result <- A^T * y, same conventions.
    void transposeMultiply(
        const std::vector<double>& y,
        std::vector<double>& result,
        Executor* executor = nullptr,
        const SpmvPlan* plan = nullptr
    ) const;

    // Single-slice kernels; used by multiply()/transposeMultiply() and directly
    // by fused parallel regions that already own a worker partition.
    void multiplyRange(const double* x, double* result, int rowBegin, int rowEnd) const noexcept;
    void transposeMultiplyRange(const double* y, double* result, int columnBegin, int columnEnd) const noexcept;

    [[nodiscard]] const std::vector<Offset>& csrRowStart() const noexcept {
        return csrRowStart_;
    }

    [[nodiscard]] const std::vector<int>& csrColumnIndex() const noexcept {
        return csrColumnIndex_;
    }

    [[nodiscard]] const std::vector<double>& csrValues() const noexcept {
        return csrValues_;
    }

    [[nodiscard]] const std::vector<Offset>& cscColumnStart() const noexcept {
        return cscColumnStart_;
    }

    [[nodiscard]] const std::vector<int>& cscRowIndex() const noexcept {
        return cscRowIndex_;
    }

    [[nodiscard]] const std::vector<double>& cscValues() const noexcept {
        return cscValues_;
    }

private:
    int rows_ = 0;
    int columns_ = 0;

    std::vector<Offset> csrRowStart_;
    std::vector<int> csrColumnIndex_;
    std::vector<double> csrValues_;

    std::vector<Offset> cscColumnStart_;
    std::vector<int> cscRowIndex_;
    std::vector<double> cscValues_;
};

}  // namespace pdlp
