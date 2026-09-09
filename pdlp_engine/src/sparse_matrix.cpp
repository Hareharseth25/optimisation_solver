#include "pdlp/sparse_matrix.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>

namespace pdlp {
namespace {

// Splits `count` items into `parts` contiguous slices carrying roughly equal
// nonzero counts, using the CSR/CSC offset array as the prefix sum of work.
std::vector<int> balancedSplit(
    const std::vector<SparseMatrix::Offset>& start,
    int count,
    int parts
) {
    std::vector<int> split(static_cast<std::size_t>(parts) + 1, 0);
    split.back() = count;
    if (parts <= 1 || count <= 0) {
        return split;
    }

    const SparseMatrix::Offset total = start[static_cast<std::size_t>(count)];
    int previous = 0;
    for (int part = 1; part < parts; ++part) {
        const SparseMatrix::Offset target = total * part / parts;
        const auto position = std::lower_bound(
            start.begin() + previous,
            start.begin() + count,
            target
        );
        int index = static_cast<int>(position - start.begin());
        index = std::clamp(index, previous, count);
        split[static_cast<std::size_t>(part)] = index;
        previous = index;
    }
    return split;
}

}  // namespace

namespace {

// Chunk count for one index space: enough chunks to smooth over uneven core
// speeds, but never so many that a chunk carries less work than the atomic
// claim costs.
int chunkCountFor(SparseMatrix::Offset nonzeros, int parts, int chunksPerPart) {
    constexpr SparseMatrix::Offset kMinimumChunkNonzeros = 4096;
    if (parts <= 1) {
        return 1;
    }
    const int requested = parts * std::max(chunksPerPart, 1);
    const auto affordable = static_cast<int>(
        std::max<SparseMatrix::Offset>(nonzeros / kMinimumChunkNonzeros, 1));
    return std::clamp(std::min(requested, affordable), parts, requested);
}

}  // namespace

SpmvPlan SpmvPlan::build(
    const SparseMatrix& matrix,
    int parts,
    int chunksPerPart
) {
    SpmvPlan plan;
    plan.parts = std::max(parts, 1);
    const auto nonzeros = static_cast<SparseMatrix::Offset>(matrix.nonzeros());
    const int chunks = chunkCountFor(nonzeros, plan.parts, chunksPerPart);

    plan.rowChunk = balancedSplit(matrix.csrRowStart(), matrix.rows(), chunks);
    plan.columnChunk = balancedSplit(matrix.cscColumnStart(), matrix.columns(), chunks);
    return plan;
}

SparseMatrix SparseMatrix::fromTriplets(
    int rows,
    int columns,
    std::vector<MatrixTriplet> triplets
) {
    if (rows < 0 || columns < 0) {
        throw std::invalid_argument("Sparse matrix dimensions must be nonnegative");
    }

    for (const auto& entry : triplets) {
        if (entry.row < 0 || entry.row >= rows ||
            entry.column < 0 || entry.column >= columns) {
            throw std::invalid_argument("Sparse matrix triplet index is out of range");
        }
        if (!std::isfinite(entry.value)) {
            throw std::invalid_argument("Sparse matrix coefficient must be finite");
        }
    }

    SparseMatrix matrix;
    matrix.rows_ = rows;
    matrix.columns_ = columns;

    // Counting sort by row: O(nnz) instead of a comparison sort over 16-byte
    // triplets, which dominates load time on multi-million-nonzero models.
    std::vector<Offset> rowStart(static_cast<std::size_t>(rows) + 1, 0);
    for (const auto& entry : triplets) {
        ++rowStart[static_cast<std::size_t>(entry.row) + 1];
    }
    for (int row = 0; row < rows; ++row) {
        rowStart[static_cast<std::size_t>(row) + 1] +=
            rowStart[static_cast<std::size_t>(row)];
    }

    const std::size_t count = triplets.size();
    std::vector<int> scatterColumn(count);
    std::vector<double> scatterValue(count);
    {
        std::vector<Offset> cursor = rowStart;
        for (const auto& entry : triplets) {
            const Offset position = cursor[static_cast<std::size_t>(entry.row)]++;
            scatterColumn[static_cast<std::size_t>(position)] = entry.column;
            scatterValue[static_cast<std::size_t>(position)] = entry.value;
        }
    }
    triplets.clear();
    triplets.shrink_to_fit();

    // Order each row by column index and merge duplicates in place. Rows are
    // short, so the per-row sort is far cheaper than a global sort.
    matrix.csrRowStart_.assign(static_cast<std::size_t>(rows) + 1, 0);
    matrix.csrColumnIndex_.resize(count);
    matrix.csrValues_.resize(count);

    std::vector<std::pair<int, double>> rowBuffer;
    Offset written = 0;
    for (int row = 0; row < rows; ++row) {
        const Offset begin = rowStart[static_cast<std::size_t>(row)];
        const Offset end = rowStart[static_cast<std::size_t>(row) + 1];

        rowBuffer.clear();
        rowBuffer.reserve(static_cast<std::size_t>(end - begin));
        for (Offset k = begin; k < end; ++k) {
            rowBuffer.emplace_back(
                scatterColumn[static_cast<std::size_t>(k)],
                scatterValue[static_cast<std::size_t>(k)]
            );
        }
        std::sort(
            rowBuffer.begin(),
            rowBuffer.end(),
            [](const auto& left, const auto& right) { return left.first < right.first; }
        );

        for (std::size_t k = 0; k < rowBuffer.size();) {
            const int column = rowBuffer[k].first;
            double accumulated = rowBuffer[k].second;
            ++k;
            while (k < rowBuffer.size() && rowBuffer[k].first == column) {
                accumulated += rowBuffer[k].second;
                ++k;
            }
            if (accumulated != 0.0) {
                matrix.csrColumnIndex_[static_cast<std::size_t>(written)] = column;
                matrix.csrValues_[static_cast<std::size_t>(written)] = accumulated;
                ++written;
            }
        }
        matrix.csrRowStart_[static_cast<std::size_t>(row) + 1] = written;
    }
    matrix.csrColumnIndex_.resize(static_cast<std::size_t>(written));
    matrix.csrValues_.resize(static_cast<std::size_t>(written));
    matrix.csrColumnIndex_.shrink_to_fit();
    matrix.csrValues_.shrink_to_fit();

    // Transpose by counting sort. Scanning CSR in row order leaves each CSC
    // column sorted by row index, which keeps A^T*y gathers monotone.
    matrix.cscColumnStart_.assign(static_cast<std::size_t>(columns) + 1, 0);
    for (Offset k = 0; k < written; ++k) {
        ++matrix.cscColumnStart_[
            static_cast<std::size_t>(matrix.csrColumnIndex_[static_cast<std::size_t>(k)]) + 1];
    }
    for (int column = 0; column < columns; ++column) {
        matrix.cscColumnStart_[static_cast<std::size_t>(column) + 1] +=
            matrix.cscColumnStart_[static_cast<std::size_t>(column)];
    }

    matrix.cscRowIndex_.resize(static_cast<std::size_t>(written));
    matrix.cscValues_.resize(static_cast<std::size_t>(written));
    {
        std::vector<Offset> cursor = matrix.cscColumnStart_;
        for (int row = 0; row < rows; ++row) {
            const Offset begin = matrix.csrRowStart_[static_cast<std::size_t>(row)];
            const Offset end = matrix.csrRowStart_[static_cast<std::size_t>(row) + 1];
            for (Offset k = begin; k < end; ++k) {
                const int column = matrix.csrColumnIndex_[static_cast<std::size_t>(k)];
                const Offset position = cursor[static_cast<std::size_t>(column)]++;
                matrix.cscRowIndex_[static_cast<std::size_t>(position)] = row;
                matrix.cscValues_[static_cast<std::size_t>(position)] =
                    matrix.csrValues_[static_cast<std::size_t>(k)];
            }
        }
    }

    return matrix;
}

bool SparseMatrix::validate() const noexcept {
    const Offset nnz = static_cast<Offset>(csrValues_.size());
    if (rows_ < 0 || columns_ < 0 ||
        csrRowStart_.size() != static_cast<std::size_t>(rows_) + 1 ||
        cscColumnStart_.size() != static_cast<std::size_t>(columns_) + 1 ||
        csrColumnIndex_.size() != csrValues_.size() ||
        cscRowIndex_.size() != cscValues_.size() ||
        csrValues_.size() != cscValues_.size()) {
        return false;
    }
    if (csrRowStart_.front() != 0 || cscColumnStart_.front() != 0 ||
        csrRowStart_.back() != nnz || cscColumnStart_.back() != nnz) {
        return false;
    }

    for (std::size_t i = 1; i < csrRowStart_.size(); ++i) {
        if (csrRowStart_[i] < csrRowStart_[i - 1]) {
            return false;
        }
    }
    for (std::size_t j = 1; j < cscColumnStart_.size(); ++j) {
        if (cscColumnStart_[j] < cscColumnStart_[j - 1]) {
            return false;
        }
    }
    for (std::size_t k = 0; k < csrValues_.size(); ++k) {
        if (csrColumnIndex_[k] < 0 || csrColumnIndex_[k] >= columns_ ||
            !std::isfinite(csrValues_[k])) {
            return false;
        }
    }
    for (std::size_t k = 0; k < cscValues_.size(); ++k) {
        if (cscRowIndex_[k] < 0 || cscRowIndex_[k] >= rows_ ||
            !std::isfinite(cscValues_[k])) {
            return false;
        }
    }
    return true;
}

SparseMatrix SparseMatrix::scaled(
    const std::vector<double>& rowScale,
    const std::vector<double>& columnScale
) const {
    if (rowScale.size() != static_cast<std::size_t>(rows_) ||
        columnScale.size() != static_cast<std::size_t>(columns_)) {
        throw std::invalid_argument("Scaling vectors do not match the matrix dimensions");
    }

    SparseMatrix result = *this;
    for (int row = 0; row < rows_; ++row) {
        const double factor = rowScale[static_cast<std::size_t>(row)];
        const Offset begin = csrRowStart_[static_cast<std::size_t>(row)];
        const Offset end = csrRowStart_[static_cast<std::size_t>(row) + 1];
        for (Offset k = begin; k < end; ++k) {
            const std::size_t index = static_cast<std::size_t>(k);
            result.csrValues_[index] *= factor *
                columnScale[static_cast<std::size_t>(csrColumnIndex_[index])];
        }
    }
    for (int column = 0; column < columns_; ++column) {
        const double factor = columnScale[static_cast<std::size_t>(column)];
        const Offset begin = cscColumnStart_[static_cast<std::size_t>(column)];
        const Offset end = cscColumnStart_[static_cast<std::size_t>(column) + 1];
        for (Offset k = begin; k < end; ++k) {
            const std::size_t index = static_cast<std::size_t>(k);
            result.cscValues_[index] *= factor *
                rowScale[static_cast<std::size_t>(cscRowIndex_[index])];
        }
    }
    return result;
}

void SparseMatrix::multiplyRange(
    const double* x,
    double* result,
    int rowBegin,
    int rowEnd
) const noexcept {
    const Offset* const __restrict rowStart = csrRowStart_.data();
    const int* const __restrict columnIndex = csrColumnIndex_.data();
    const double* const __restrict values = csrValues_.data();

    for (int row = rowBegin; row < rowEnd; ++row) {
        const Offset begin = rowStart[row];
        const Offset end = rowStart[row + 1];

        // Four independent accumulators: the reduction is otherwise a serial
        // dependency chain of ~4-cycle FMAs, which throttles a unit that can
        // retire several per cycle.
        double a0 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
        double a3 = 0.0;
        Offset k = begin;
        for (; k + 4 <= end; k += 4) {
            a0 += values[k] * x[columnIndex[k]];
            a1 += values[k + 1] * x[columnIndex[k + 1]];
            a2 += values[k + 2] * x[columnIndex[k + 2]];
            a3 += values[k + 3] * x[columnIndex[k + 3]];
        }
        for (; k < end; ++k) {
            a0 += values[k] * x[columnIndex[k]];
        }
        result[row] = (a0 + a1) + (a2 + a3);
    }
}

void SparseMatrix::transposeMultiplyRange(
    const double* y,
    double* result,
    int columnBegin,
    int columnEnd
) const noexcept {
    const Offset* const __restrict columnStart = cscColumnStart_.data();
    const int* const __restrict rowIndex = cscRowIndex_.data();
    const double* const __restrict values = cscValues_.data();

    for (int column = columnBegin; column < columnEnd; ++column) {
        const Offset begin = columnStart[column];
        const Offset end = columnStart[column + 1];

        double a0 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
        double a3 = 0.0;
        Offset k = begin;
        for (; k + 4 <= end; k += 4) {
            a0 += values[k] * y[rowIndex[k]];
            a1 += values[k + 1] * y[rowIndex[k + 1]];
            a2 += values[k + 2] * y[rowIndex[k + 2]];
            a3 += values[k + 3] * y[rowIndex[k + 3]];
        }
        for (; k < end; ++k) {
            a0 += values[k] * y[rowIndex[k]];
        }
        result[column] = (a0 + a1) + (a2 + a3);
    }
}

void SparseMatrix::multiply(
    const std::vector<double>& x,
    std::vector<double>& result,
    Executor* executor,
    const SpmvPlan* plan
) const {
    if (x.size() != static_cast<std::size_t>(columns_)) {
        throw std::invalid_argument("A*x received a vector with the wrong dimension");
    }
    // Every entry is written unconditionally, so resize (which only touches new
    // elements) replaces the previous full zero-fill of the output.
    result.resize(static_cast<std::size_t>(rows_));

    const double* const source = x.data();
    double* const destination = result.data();

    if (executor == nullptr || plan == nullptr || plan->parts <= 1) {
        multiplyRange(source, destination, 0, rows_);
        return;
    }

    const std::vector<int>& chunk = plan->rowChunk;
    const int chunks = plan->rowChunkCount();
    std::atomic<int> cursor{0};
    executor->run([&](int) {
        while (true) {
            const int index = cursor.fetch_add(1, std::memory_order_relaxed);
            if (index >= chunks) {
                break;
            }
            multiplyRange(
                source,
                destination,
                chunk[static_cast<std::size_t>(index)],
                chunk[static_cast<std::size_t>(index) + 1]
            );
        }
    });
}

void SparseMatrix::transposeMultiply(
    const std::vector<double>& y,
    std::vector<double>& result,
    Executor* executor,
    const SpmvPlan* plan
) const {
    if (y.size() != static_cast<std::size_t>(rows_)) {
        throw std::invalid_argument("A^T*y received a vector with the wrong dimension");
    }
    result.resize(static_cast<std::size_t>(columns_));

    const double* const source = y.data();
    double* const destination = result.data();

    if (executor == nullptr || plan == nullptr || plan->parts <= 1) {
        transposeMultiplyRange(source, destination, 0, columns_);
        return;
    }

    const std::vector<int>& chunk = plan->columnChunk;
    const int chunks = plan->columnChunkCount();
    std::atomic<int> cursor{0};
    executor->run([&](int) {
        while (true) {
            const int index = cursor.fetch_add(1, std::memory_order_relaxed);
            if (index >= chunks) {
                break;
            }
            transposeMultiplyRange(
                source,
                destination,
                chunk[static_cast<std::size_t>(index)],
                chunk[static_cast<std::size_t>(index) + 1]
            );
        }
    });
}

}  // namespace pdlp
