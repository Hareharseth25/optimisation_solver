#include "qp/scaling.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace qp {
namespace {

constexpr double kMinimumMagnitude = 1e-30;

// Rescale a single entry; infinities are preserved exactly.
inline double scaleBound(double bound, double factor) noexcept {
    if (!std::isfinite(bound)) {
        return bound;
    }
    return bound * factor;
}

// Build a copy of `source` with values multiplied by `rowFactor * columnFactor`
// (applied row by row, using `source`'s CSR layout). Empty source passes
// through.
SparseMatrix rescaleMatrix(
    const SparseMatrix& source,
    const std::vector<double>& rowFactor,
    const std::vector<double>& columnFactor
) {
    const int rows = source.rows();
    const int columns = source.columns();
    std::vector<Offset> newStarts(static_cast<std::size_t>(rows) + 1, 0);
    std::vector<Index>  newCols;
    std::vector<double> newVals;

    newStarts.reserve(static_cast<std::size_t>(rows) + 1);
    newCols.reserve(source.csrValues().size());
    newVals.reserve(source.csrValues().size());

    for (int r = 0; r < rows; ++r) {
        newStarts[static_cast<std::size_t>(r)] = static_cast<Offset>(newCols.size());
        const double rowMul =
            r < static_cast<int>(rowFactor.size()) ? rowFactor[static_cast<std::size_t>(r)] : 1.0;
        const auto begin = static_cast<std::size_t>(source.csrRowStart()[static_cast<std::size_t>(r)]);
        const auto end   = static_cast<std::size_t>(source.csrRowStart()[static_cast<std::size_t>(r) + 1]);
        for (std::size_t k = begin; k < end; ++k) {
            const Index c = source.csrColumnIndex()[k];
            const double colMul =
                c < static_cast<int>(columnFactor.size())
                    ? columnFactor[static_cast<std::size_t>(c)]
                    : 1.0;
            const double v = source.csrValues()[k] * rowMul * colMul;
            if (v != 0.0) {
                newCols.push_back(c);
                newVals.push_back(v);
            }
        }
    }
    newStarts[static_cast<std::size_t>(rows)] = static_cast<Offset>(newCols.size());

    SparseMatrix out;
    // We can't set private members from here, so we round-trip through
    // fromTriplets. For a 0x0 matrix this is the empty path.
    if (rows == 0 || columns == 0) {
        return SparseMatrix::fromTriplets(rows, columns, {}, {}, {});
    }
    // Reuse fromTriplets which does its own dedup / sort, so this is exact.
    // We rebuild row/col/val vectors of identical content (no dedup needed
    // since the source has no duplicates).
    std::vector<double> rr; rr.reserve(newCols.size());
    std::vector<double> cc; cc.reserve(newCols.size());
    std::vector<double> vv; vv.reserve(newCols.size());
    for (int r = 0; r < rows; ++r) {
        const auto begin = static_cast<std::size_t>(newStarts[static_cast<std::size_t>(r)]);
        const auto end   = static_cast<std::size_t>(newStarts[static_cast<std::size_t>(r) + 1]);
        for (std::size_t k = begin; k < end; ++k) {
            rr.push_back(static_cast<double>(r));
            cc.push_back(static_cast<double>(newCols[k]));
            vv.push_back(newVals[k]);
        }
    }
    return SparseMatrix::fromTriplets(rows, columns, rr, cc, vv);
}

}  // namespace

void QpScaling::toOriginal(
    const std::vector<double>& scaledPrimal,
    std::vector<double>& primal
) const {
    primal.resize(scaledPrimal.size());
    const std::size_t n = scaledPrimal.size();
    for (std::size_t j = 0; j < n; ++j) {
        const double d = j < columnScale.size() ? columnScale[j] : 1.0;
        primal[j] = d * scaledPrimal[j];
    }
}

void QpScaling::toOriginalDual(
    const std::vector<double>& scaledDual,
    std::vector<double>& dual
) const {
    dual.resize(scaledDual.size());
    const std::size_t m = scaledDual.size();
    for (std::size_t i = 0; i < m; ++i) {
        const double d = i < rowScale.size() ? rowScale[i] : 1.0;
        dual[i] = d * scaledDual[i];
    }
}

QpScaling RuizScaler::equilibrate(const QpModel& model, int iterations) {
    const int n = model.numVariables();
    const int m = model.numConstraints();

    QpScaling scaling;
    scaling.rowScale.assign(static_cast<std::size_t>(m), 1.0);
    scaling.columnScale.assign(static_cast<std::size_t>(n), 1.0);

    // Working diagonal of P (square), and the column view of A.
    std::vector<double> pDiag(n, 0.0);
    {
        const auto& pStart = model.P.csrRowStart();
        const auto& pCols  = model.P.csrColumnIndex();
        const auto& pVals  = model.P.csrValues();
        for (int r = 0; r < n; ++r) {
            const auto begin = static_cast<std::size_t>(pStart[static_cast<std::size_t>(r)]);
            const auto end   = static_cast<std::size_t>(pStart[static_cast<std::size_t>(r) + 1]);
            for (std::size_t k = begin; k < end; ++k) {
                if (pCols[k] == r) {
                    pDiag[static_cast<std::size_t>(r)] = pVals[k];
                }
            }
        }
    }

    std::vector<double> rowMax(m, 0.0);
    std::vector<double> colMax(n, 0.0);
    std::vector<double> pMax(n, 0.0);

    const int passes = std::max(iterations, 0);
    for (int pass = 0; pass < passes; ++pass) {
        std::fill(rowMax.begin(), rowMax.end(), 0.0);
        std::fill(colMax.begin(), colMax.end(), 0.0);
        std::fill(pMax.begin(),  pMax.end(),  0.0);

        if (m > 0) {
            const auto& aStart = model.A.csrRowStart();
            const auto& aCols  = model.A.csrColumnIndex();
            const auto& aVals  = model.A.csrValues();
            for (int r = 0; r < m; ++r) {
                const double rowFactor = scaling.rowScale[static_cast<std::size_t>(r)];
                double localMax = 0.0;
                const auto begin = static_cast<std::size_t>(aStart[static_cast<std::size_t>(r)]);
                const auto end   = static_cast<std::size_t>(aStart[static_cast<std::size_t>(r) + 1]);
                for (std::size_t k = begin; k < end; ++k) {
                    const Index c = aCols[k];
                    const double mag =
                        std::abs(aVals[k]) * rowFactor *
                        scaling.columnScale[static_cast<std::size_t>(c)];
                    localMax = std::max(localMax, mag);
                    colMax[static_cast<std::size_t>(c)] =
                        std::max(colMax[static_cast<std::size_t>(c)], mag);
                }
                rowMax[static_cast<std::size_t>(r)] = localMax;
            }
        }

        // P is symmetric; we use only its diagonal for the diagonal-only
        // equilibration path. The off-diagonals of P get rescaled implicitly
        // through columnScale, but we do not iterate on P's row/column norms
        // because the off-diagonal-block structure of the KKT is already
        // handled through A.
        for (int j = 0; j < n; ++j) {
            const double d = std::abs(pDiag[static_cast<std::size_t>(j)]) *
                             scaling.columnScale[static_cast<std::size_t>(j)] *
                             scaling.columnScale[static_cast<std::size_t>(j)];
            pMax[static_cast<std::size_t>(j)] = d;
        }

        if (m > 0) {
            for (int r = 0; r < m; ++r) {
                const double mag = rowMax[static_cast<std::size_t>(r)];
                if (mag > kMinimumMagnitude) {
                    scaling.rowScale[static_cast<std::size_t>(r)] /= std::sqrt(mag);
                }
            }
        }
        for (int c = 0; c < n; ++c) {
            // Combine A's column norm with the diagonal P element. Both go
            // through the same column scale, and we take the max of the two so
            // neither dominates.
            const double aMag = colMax[static_cast<std::size_t>(c)];
            const double pMag = pMax[static_cast<std::size_t>(c)];
            const double mag  = std::max(aMag, pMag);
            if (mag > kMinimumMagnitude) {
                scaling.columnScale[static_cast<std::size_t>(c)] /= std::sqrt(mag);
            }
        }
    }

    // Materialise the scaled problem.
    QpModel scaled;
    scaled.P = rescaleMatrix(
        model.P,
        scaling.columnScale,   // P row index == column index
        scaling.columnScale
    );
    if (m > 0) {
        scaled.A = rescaleMatrix(model.A, scaling.rowScale, scaling.columnScale);
    } else {
        scaled.A = SparseMatrix::fromTriplets(0, n, {}, {}, {});
    }

    scaled.q.resize(static_cast<std::size_t>(n));
    for (int j = 0; j < n; ++j) {
        const double d = scaling.columnScale[static_cast<std::size_t>(j)];
        scaled.q[static_cast<std::size_t>(j)] =
            model.q[static_cast<std::size_t>(j)] * d;
    }

    scaled.l.resize(static_cast<std::size_t>(m));
    scaled.u.resize(static_cast<std::size_t>(m));
    for (int i = 0; i < m; ++i) {
        const double d = scaling.rowScale[static_cast<std::size_t>(i)];
        scaled.l[static_cast<std::size_t>(i)] = scaleBound(model.l[static_cast<std::size_t>(i)], d);
        scaled.u[static_cast<std::size_t>(i)] = scaleBound(model.u[static_cast<std::size_t>(i)], d);
    }

    scaling.scaled = std::move(scaled);
    return scaling;
}

double RuizScaler::conditionSpread(const SparseMatrix& matrix) {
    const int rows = matrix.rows();
    if (rows == 0) return 1.0;

    const auto& rowStart = matrix.csrRowStart();
    const auto& values   = matrix.csrValues();

    double smallest = std::numeric_limits<double>::infinity();
    double largest  = 0.0;
    for (int r = 0; r < rows; ++r) {
        double localMax = 0.0;
        const auto begin = static_cast<std::size_t>(rowStart[static_cast<std::size_t>(r)]);
        const auto end   = static_cast<std::size_t>(rowStart[static_cast<std::size_t>(r) + 1]);
        for (std::size_t k = begin; k < end; ++k) {
            localMax = std::max(localMax, std::abs(values[k]));
        }
        if (localMax > 0.0) {
            smallest = std::min(smallest, localMax);
            largest  = std::max(largest,  localMax);
        }
    }
    if (!std::isfinite(smallest) || smallest <= 0.0) {
        return 1.0;
    }
    return largest / smallest;
}

}  // namespace qp
