#include "qp/qp_model.h"

#include <atomic>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace qp {

namespace {

struct Triplet {
    int row;
    int col;
    double val;
};

}  // namespace

SparseMatrix SparseMatrix::fromTriplets(
    int rows,
    int columns,
    const std::vector<double>& row,
    const std::vector<double>& col,
    const std::vector<double>& val
) {
    if (row.size() != col.size() || row.size() != val.size()) {
        throw std::invalid_argument(
            "SparseMatrix::fromTriplets: row/col/val length mismatch");
    }
    if (rows < 0 || columns < 0) {
        throw std::invalid_argument(
            "SparseMatrix::fromTriplets: negative row/column count");
    }

    // Convert to internal triplet vector.
    std::vector<Triplet> triplets;
    triplets.reserve(row.size());
    for (std::size_t k = 0; k < row.size(); ++k) {
        const int r = static_cast<int>(row[k]);
        const int c = static_cast<int>(col[k]);
        if (r < 0 || r >= rows || c < 0 || c >= columns) {
            throw std::invalid_argument(
                "SparseMatrix::fromTriplets: triplet index out of range");
        }
        if (!std::isfinite(val[k])) {
            throw std::invalid_argument(
                "SparseMatrix::fromTriplets: triplet value is not finite");
        }
        triplets.push_back({r, c, val[k]});
    }

    // Counting sort by row, then by column within each row. Stable, O(nnz).
    std::vector<Offset> counts(static_cast<std::size_t>(rows), 0);
    for (const auto& t : triplets) {
        counts[static_cast<std::size_t>(t.row)] += 1;
    }
    std::vector<Offset> starts(static_cast<std::size_t>(rows) + 1, 0);
    for (int r = 0; r < rows; ++r) {
        starts[static_cast<std::size_t>(r) + 1] =
            starts[static_cast<std::size_t>(r)] + counts[static_cast<std::size_t>(r)];
    }

    std::vector<Triplet> sorted(triplets.size());
    {
        std::vector<Offset> cursor = starts;
        for (const auto& t : triplets) {
            const std::size_t idx = static_cast<std::size_t>(cursor[static_cast<std::size_t>(t.row)]++);
            sorted[idx] = t;
        }
    }

    // Within each row, sort by column index (counting sort over n columns is
    // not memory-friendly for large n; std::sort over a row's slice is fine
    // because the rows are usually short).
    for (int r = 0; r < rows; ++r) {
        const auto begin = static_cast<std::size_t>(starts[static_cast<std::size_t>(r)]);
        const auto end   = static_cast<std::size_t>(starts[static_cast<std::size_t>(r) + 1]);
        std::sort(
            sorted.begin() + static_cast<std::ptrdiff_t>(begin),
            sorted.begin() + static_cast<std::ptrdiff_t>(end),
            [](const Triplet& a, const Triplet& b) { return a.col < b.col; }
        );
    }

    // Merge duplicates within each row; drop cancellations.
    std::vector<Offset> mergedStarts(static_cast<std::size_t>(rows) + 1, 0);
    std::vector<Index>  mergedCols;
    std::vector<double> mergedVals;
    mergedCols.reserve(sorted.size());
    mergedVals.reserve(sorted.size());

    for (int r = 0; r < rows; ++r) {
        const auto begin = static_cast<std::size_t>(starts[static_cast<std::size_t>(r)]);
        const auto end   = static_cast<std::size_t>(starts[static_cast<std::size_t>(r) + 1]);
        mergedStarts[static_cast<std::size_t>(r)] =
            static_cast<Offset>(mergedCols.size());

        // Where THIS row starts in the output. The merge test must be scoped to
        // it: mergedCols is global across rows, so testing only
        // `mergedCols.back() == c` compares the first entry of row r against the
        // last entry of row r-1 and merges two different rows whenever they share
        // a column index. A = ones(k,1) collapsed to a single entry of value k
        // that way, which made A^T*A come out as k^2 instead of k.
        const std::size_t rowStart = mergedCols.size();

        for (std::size_t k = begin; k < end; ++k) {
            const int c = sorted[k].col;
            const double v = sorted[k].val;
            if (mergedCols.size() > rowStart && mergedCols.back() == c) {
                // Entries are column-sorted within the row, so a repeat of the
                // previous column is a genuine duplicate.
                mergedVals.back() += v;
            } else {
                mergedCols.push_back(c);
                mergedVals.push_back(v);
            }
        }
    }
    mergedStarts[static_cast<std::size_t>(rows)] =
        static_cast<Offset>(mergedCols.size());

    // Drop entries whose value is exactly zero after merging.
    std::vector<Offset> finalStarts(static_cast<std::size_t>(rows) + 1, 0);
    std::vector<Index>  finalCols;
    std::vector<double> finalVals;
    finalCols.reserve(mergedCols.size());
    finalVals.reserve(mergedVals.size());

    for (int r = 0; r < rows; ++r) {
        finalStarts[static_cast<std::size_t>(r)] = static_cast<Offset>(finalCols.size());
        const auto begin = static_cast<std::size_t>(mergedStarts[static_cast<std::size_t>(r)]);
        const auto end   = static_cast<std::size_t>(mergedStarts[static_cast<std::size_t>(r) + 1]);
        for (std::size_t k = begin; k < end; ++k) {
            if (mergedVals[k] != 0.0) {
                finalCols.push_back(mergedCols[k]);
                finalVals.push_back(mergedVals[k]);
            }
        }
    }
    finalStarts[static_cast<std::size_t>(rows)] = static_cast<Offset>(finalCols.size());

    SparseMatrix out;
    out.rows_ = rows;
    out.columns_ = columns;
    out.csrRowStart_ = std::move(finalStarts);
    out.csrColumnIndex_ = std::move(finalCols);
    out.csrValues_ = std::move(finalVals);

    // Transpose by counting sort. Walking CSR in row order leaves each CSC
    // column sorted by row index, so the gather in A^T*y stays monotone.
    const std::size_t nnz = out.csrValues_.size();
    out.cscColumnStart_.assign(static_cast<std::size_t>(columns) + 1, 0);
    for (std::size_t k = 0; k < nnz; ++k) {
        ++out.cscColumnStart_[static_cast<std::size_t>(out.csrColumnIndex_[k]) + 1];
    }
    for (int c = 0; c < columns; ++c) {
        out.cscColumnStart_[static_cast<std::size_t>(c) + 1] +=
            out.cscColumnStart_[static_cast<std::size_t>(c)];
    }
    out.cscRowIndex_.resize(nnz);
    out.cscValues_.resize(nnz);
    {
        std::vector<Offset> cursor = out.cscColumnStart_;
        for (int r = 0; r < rows; ++r) {
            const auto begin = static_cast<std::size_t>(out.csrRowStart_[static_cast<std::size_t>(r)]);
            const auto end   = static_cast<std::size_t>(out.csrRowStart_[static_cast<std::size_t>(r) + 1]);
            for (std::size_t k = begin; k < end; ++k) {
                const Index c = out.csrColumnIndex_[k];
                const auto pos = static_cast<std::size_t>(cursor[static_cast<std::size_t>(c)]++);
                out.cscRowIndex_[pos] = r;
                out.cscValues_[pos] = out.csrValues_[k];
            }
        }
    }
    return out;
}

bool SparseMatrix::validate() const noexcept {
    if (rows_ < 0 || columns_ < 0) {
        return false;
    }
    if (csrRowStart_.size() != static_cast<std::size_t>(rows_) + 1) {
        return false;
    }
    if (csrColumnIndex_.size() != csrValues_.size()) {
        return false;
    }
    if (csrRowStart_.front() != 0 ||
        csrRowStart_.back() != static_cast<Offset>(csrValues_.size())) {
        return false;
    }
    for (int r = 0; r < rows_; ++r) {
        const auto begin = static_cast<std::size_t>(csrRowStart_[static_cast<std::size_t>(r)]);
        const auto end   = static_cast<std::size_t>(csrRowStart_[static_cast<std::size_t>(r) + 1]);
        if (begin > end || end > csrValues_.size()) {
            return false;
        }
        for (std::size_t k = begin; k < end; ++k) {
            if (csrColumnIndex_[k] < 0 || csrColumnIndex_[k] >= columns_) {
                return false;
            }
            if (k > begin && csrColumnIndex_[k] <= csrColumnIndex_[k - 1]) {
                return false;
            }
        }
    }
    return true;
}

void SparseMatrix::multiply(
    const std::vector<double>& x,
    std::vector<double>& result
) const {
    result.assign(static_cast<std::size_t>(rows_), 0.0);
    for (int r = 0; r < rows_; ++r) {
        const auto begin = static_cast<std::size_t>(csrRowStart_[static_cast<std::size_t>(r)]);
        const auto end   = static_cast<std::size_t>(csrRowStart_[static_cast<std::size_t>(r) + 1]);
        double sum = 0.0;
        for (std::size_t k = begin; k < end; ++k) {
            const Index c = csrColumnIndex_[k];
            const double v = csrValues_[k];
            sum += v * x[static_cast<std::size_t>(c)];
        }
        result[static_cast<std::size_t>(r)] = sum;
    }
}

void SparseMatrix::transposeMultiply(
    const std::vector<double>& y,
    std::vector<double>& result
) const {
    result.assign(static_cast<std::size_t>(columns_), 0.0);
    for (int r = 0; r < rows_; ++r) {
        const double yi = y[static_cast<std::size_t>(r)];
        if (yi == 0.0) continue;
        const auto begin = static_cast<std::size_t>(csrRowStart_[static_cast<std::size_t>(r)]);
        const auto end   = static_cast<std::size_t>(csrRowStart_[static_cast<std::size_t>(r) + 1]);
        for (std::size_t k = begin; k < end; ++k) {
            const Index c = csrColumnIndex_[k];
            result[static_cast<std::size_t>(c)] += csrValues_[k] * yi;
        }
    }
}

SparseMatrix SparseMatrix::scaled(
    const std::vector<double>& rowScale,
    const std::vector<double>& colScale
) const {
    SparseMatrix out;
    out.rows_ = rows_;
    out.columns_ = columns_;
    out.csrRowStart_ = csrRowStart_;
    out.csrColumnIndex_ = csrColumnIndex_;
    out.csrValues_.resize(csrValues_.size());
    for (int r = 0; r < rows_; ++r) {
        const double rs = rowScale[static_cast<std::size_t>(r)];
        const auto begin = static_cast<std::size_t>(csrRowStart_[static_cast<std::size_t>(r)]);
        const auto end   = static_cast<std::size_t>(csrRowStart_[static_cast<std::size_t>(r) + 1]);
        for (std::size_t k = begin; k < end; ++k) {
            const Index c = csrColumnIndex_[k];
            out.csrValues_[k] = csrValues_[k] * rs * colScale[static_cast<std::size_t>(c)];
        }
    }
    return out;
}



namespace {

// Splits `count` lines into `parts` slices carrying roughly equal nonzeros.
std::vector<int> balancedSplit(const std::vector<Offset>& start,
                               int count, int parts) {
    std::vector<int> split(static_cast<std::size_t>(parts) + 1, 0);
    split.back() = count;
    if (parts <= 1 || count <= 0) {
        return split;
    }
    const Offset total = start[static_cast<std::size_t>(count)];
    int previous = 0;
    for (int part = 1; part < parts; ++part) {
        const Offset target = total * part / parts;
        const auto it = std::lower_bound(start.begin() + previous,
                                         start.begin() + count, target);
        int index = static_cast<int>(it - start.begin());
        index = std::max(previous, std::min(index, count));
        split[static_cast<std::size_t>(part)] = index;
        previous = index;
    }
    return split;
}

}  // namespace

SparseMatrix::Plan SparseMatrix::buildPlan(int parts, int chunksPerPart) const {
    Plan plan;
    plan.parts = std::max(parts, 1);
    // Several chunks per worker, claimed dynamically, so a slow core cannot
    // hold the barrier on a hybrid CPU.
    const int chunks = std::max(plan.parts, plan.parts * std::max(chunksPerPart, 1));
    plan.rowChunk = balancedSplit(csrRowStart_, rows_, chunks);
    plan.columnChunk = balancedSplit(cscColumnStart_, columns_, chunks);
    return plan;
}

void SparseMatrix::multiplyRange(const double* x, double* result,
                                 int begin, int end) const noexcept {
    for (int r = begin; r < end; ++r) {
        const auto b = static_cast<std::size_t>(csrRowStart_[static_cast<std::size_t>(r)]);
        const auto e = static_cast<std::size_t>(csrRowStart_[static_cast<std::size_t>(r) + 1]);
        double a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
        std::size_t k = b;
        for (; k + 4 <= e; k += 4) {
            a0 += csrValues_[k]     * x[csrColumnIndex_[k]];
            a1 += csrValues_[k + 1] * x[csrColumnIndex_[k + 1]];
            a2 += csrValues_[k + 2] * x[csrColumnIndex_[k + 2]];
            a3 += csrValues_[k + 3] * x[csrColumnIndex_[k + 3]];
        }
        for (; k < e; ++k) a0 += csrValues_[k] * x[csrColumnIndex_[k]];
        result[r] = (a0 + a1) + (a2 + a3);
    }
}

void SparseMatrix::transposeMultiplyRange(const double* y, double* result,
                                          int begin, int end) const noexcept {
    for (int c = begin; c < end; ++c) {
        const auto b = static_cast<std::size_t>(cscColumnStart_[static_cast<std::size_t>(c)]);
        const auto e = static_cast<std::size_t>(cscColumnStart_[static_cast<std::size_t>(c) + 1]);
        double a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
        std::size_t k = b;
        for (; k + 4 <= e; k += 4) {
            a0 += cscValues_[k]     * y[cscRowIndex_[k]];
            a1 += cscValues_[k + 1] * y[cscRowIndex_[k + 1]];
            a2 += cscValues_[k + 2] * y[cscRowIndex_[k + 2]];
            a3 += cscValues_[k + 3] * y[cscRowIndex_[k + 3]];
        }
        for (; k < e; ++k) a0 += cscValues_[k] * y[cscRowIndex_[k]];
        result[c] = (a0 + a1) + (a2 + a3);
    }
}

void SparseMatrix::multiply(const std::vector<double>& x, std::vector<double>& result,
                            Executor* executor, const Plan* plan) const {
    result.resize(static_cast<std::size_t>(rows_));
    const double* const src = x.data();
    double* const dst = result.data();
    if (executor == nullptr || plan == nullptr || plan->parts <= 1) {
        multiplyRange(src, dst, 0, rows_);
        return;
    }
    const std::vector<int>& chunk = plan->rowChunk;
    const int chunks = plan->rowChunkCount();
    std::atomic<int> cursor{0};
    executor->run([&](int) {
        while (true) {
            const int i = cursor.fetch_add(1, std::memory_order_relaxed);
            if (i >= chunks) break;
            multiplyRange(src, dst, chunk[static_cast<std::size_t>(i)],
                          chunk[static_cast<std::size_t>(i) + 1]);
        }
    });
}

void SparseMatrix::transposeMultiply(const std::vector<double>& y,
                                     std::vector<double>& result,
                                     Executor* executor, const Plan* plan) const {
    result.resize(static_cast<std::size_t>(columns_));
    const double* const src = y.data();
    double* const dst = result.data();
    if (executor == nullptr || plan == nullptr || plan->parts <= 1) {
        transposeMultiplyRange(src, dst, 0, columns_);
        return;
    }
    const std::vector<int>& chunk = plan->columnChunk;
    const int chunks = plan->columnChunkCount();
    std::atomic<int> cursor{0};
    executor->run([&](int) {
        while (true) {
            const int i = cursor.fetch_add(1, std::memory_order_relaxed);
            if (i >= chunks) break;
            transposeMultiplyRange(src, dst, chunk[static_cast<std::size_t>(i)],
                                   chunk[static_cast<std::size_t>(i) + 1]);
        }
    });
}

}  // namespace qp
