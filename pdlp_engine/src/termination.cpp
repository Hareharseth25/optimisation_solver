#include "pdlp/termination.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace pdlp {
namespace {

inline double clipped(double value, double lower, double upper) noexcept {
    return std::max(lower, std::min(value, upper));
}

// L2 norm of the finite row bounds, i.e. the right-hand side vector.
double finiteBoundsNorm(
    const std::vector<double>& lower,
    const std::vector<double>& upper
) {
    double sum = 0.0;
    for (std::size_t i = 0; i < lower.size(); ++i) {
        if (std::isfinite(lower[i])) {
            sum += lower[i] * lower[i];
        }
        if (std::isfinite(upper[i]) && upper[i] != lower[i]) {
            sum += upper[i] * upper[i];
        }
    }
    return std::sqrt(sum);
}

double vectorNorm(const std::vector<double>& vector) {
    double sum = 0.0;
    for (double value : vector) {
        sum += value * value;
    }
    return std::sqrt(sum);
}

}  // namespace

TerminationChecker::TerminationChecker(
    PdlpOptions options,
    const CompiledLp& problem,
    Executor* executor,
    const SpmvPlan* plan
)
    : options_(options), problem_(problem), executor_(executor), plan_(plan) {
    if (executor_ != nullptr && plan_ != nullptr && plan_->parts > 1) {
        parts_ = plan_->parts;
    } else {
        executor_ = nullptr;
        plan_ = nullptr;
        parts_ = 1;
    }
    rowReductions_.assign(static_cast<std::size_t>(parts_), RowReduction{});
    columnReductions_.assign(static_cast<std::size_t>(parts_), ColumnReduction{});

    // These depend only on the problem, but the previous implementation rebuilt
    // both with hypot chains on every termination check.
    primalNormaliser_ = 1.0 + finiteBoundsNorm(problem.rowLower, problem.rowUpper);
    dualNormaliser_ = 1.0 + vectorNorm(problem.objective);
}

// Fused row pass: A*x is consumed coordinate-wise, so no activity vector is
// materialised. Accumulates primal infeasibility and the row support function.
void TerminationChecker::rowPass(
    const double* primalValues,
    const double* dualValues,
    int begin,
    int end,
    RowReduction& out
) const noexcept {
    using Offset = SparseMatrix::Offset;
    const SparseMatrix& matrix = problem_.matrix;
    const Offset* const __restrict rowStart = matrix.csrRowStart().data();
    const int* const __restrict columnIndex = matrix.csrColumnIndex().data();
    const double* const __restrict entries = matrix.csrValues().data();

    const double* const __restrict lower = problem_.rowLower.data();
    const double* const __restrict upper = problem_.rowUpper.data();
    const double* const __restrict primal = primalValues;
    const double* const __restrict dual = dualValues;

    double violationSquared = 0.0;
    double support = 0.0;
    double infeasibilitySquared = 0.0;

    for (int row = begin; row < end; ++row) {
        Offset k = rowStart[row];
        const Offset last = rowStart[row + 1];
        double a0 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
        double a3 = 0.0;
        for (; k + 4 <= last; k += 4) {
            a0 += entries[k] * primal[columnIndex[k]];
            a1 += entries[k + 1] * primal[columnIndex[k + 1]];
            a2 += entries[k + 2] * primal[columnIndex[k + 2]];
            a3 += entries[k + 3] * primal[columnIndex[k + 3]];
        }
        for (; k < last; ++k) {
            a0 += entries[k] * primal[columnIndex[k]];
        }
        const double activity = (a0 + a1) + (a2 + a3);

        const double rowLower = lower[row];
        const double rowUpper = upper[row];
        const double excess = activity - clipped(activity, rowLower, rowUpper);
        violationSquared += excess * excess;

        // Project y onto the domain of the support function of [l, u]: an
        // unbounded side forces the corresponding sign. Iterates produced by the
        // PDHG prox already satisfy this exactly, so the projection distance is
        // zero; it matters only for user-supplied warm starts.
        double projected = dual[row];
        if (!std::isfinite(rowUpper)) {
            projected = std::min(projected, 0.0);
        }
        if (!std::isfinite(rowLower)) {
            projected = std::max(projected, 0.0);
        }
        const double drift = dual[row] - projected;
        infeasibilitySquared += drift * drift;

        if (projected > 0.0) {
            support += projected * rowUpper;
        } else if (projected < 0.0) {
            support += projected * rowLower;
        }
    }

    out.violationSquared = violationSquared;
    out.support = support;
    out.dualInfeasibilitySquared = infeasibilitySquared;
}

// Fused column pass: A^T*y is consumed coordinate-wise. Accumulates the primal
// objective, the reduced-cost infimum over the variable box, and dual
// infeasibility.
void TerminationChecker::columnPass(
    const double* primalValues,
    const double* dualValues,
    int begin,
    int end,
    ColumnReduction& out
) const noexcept {
    using Offset = SparseMatrix::Offset;
    const SparseMatrix& matrix = problem_.matrix;
    const Offset* const __restrict columnStart = matrix.cscColumnStart().data();
    const int* const __restrict rowIndex = matrix.cscRowIndex().data();
    const double* const __restrict entries = matrix.cscValues().data();

    const double* const __restrict cost = problem_.objective.data();
    const double* const __restrict lower = problem_.variableLower.data();
    const double* const __restrict upper = problem_.variableUpper.data();
    const double* const __restrict primal = primalValues;
    const double* const __restrict dual = dualValues;

    double objective = 0.0;
    double infimum = 0.0;
    double infeasibilitySquared = 0.0;

    for (int column = begin; column < end; ++column) {
        Offset k = columnStart[column];
        const Offset last = columnStart[column + 1];
        double a0 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
        double a3 = 0.0;
        for (; k + 4 <= last; k += 4) {
            a0 += entries[k] * dual[rowIndex[k]];
            a1 += entries[k + 1] * dual[rowIndex[k + 1]];
            a2 += entries[k + 2] * dual[rowIndex[k + 2]];
            a3 += entries[k + 3] * dual[rowIndex[k + 3]];
        }
        for (; k < last; ++k) {
            a0 += entries[k] * dual[rowIndex[k]];
        }

        const double reduced = cost[column] + ((a0 + a1) + (a2 + a3));
        const double variableLower = lower[column];
        const double variableUpper = upper[column];

        objective += cost[column] * primal[column];

        // min over [l, u] of reduced * x is finite only if the bound that the
        // sign of `reduced` selects is finite. Project onto the cone where it
        // is, and charge the distance to dual infeasibility.
        double projected = reduced;
        if (!std::isfinite(variableLower)) {
            projected = std::min(projected, 0.0);
        }
        if (!std::isfinite(variableUpper)) {
            projected = std::max(projected, 0.0);
        }
        const double drift = reduced - projected;
        infeasibilitySquared += drift * drift;

        if (projected > 0.0) {
            infimum += projected * variableLower;
        } else if (projected < 0.0) {
            infimum += projected * variableUpper;
        }
    }

    out.objective = objective;
    out.infimum = infimum;
    out.infeasibilitySquared = infeasibilitySquared;
}

CandidateMetrics TerminationChecker::evaluate(
    const std::vector<double>& primal,
    const std::vector<double>& dual
) {
    CandidateMetrics metrics;
    if (primal.size() != static_cast<std::size_t>(problem_.numColumns()) ||
        dual.size() != static_cast<std::size_t>(problem_.numRows())) {
        return metrics;
    }

    const double* const primalValues = primal.data();
    const double* const dualValues = dual.data();

    const int rows = problem_.numRows();
    const int columns = problem_.numColumns();

    RowReduction rowTotal;
    ColumnReduction columnTotal;

    if (parts_ <= 1) {
        rowPass(primalValues, dualValues, 0, rows, rowTotal);
        columnPass(primalValues, dualValues, 0, columns, columnTotal);
    } else {
        const std::vector<int>& rowChunk = plan_->rowChunk;
        const int rowChunks = plan_->rowChunkCount();
        std::atomic<int> rowCursor{0};
        executor_->run([&](int worker) {
            RowReduction total;
            while (true) {
                const int index = rowCursor.fetch_add(1, std::memory_order_relaxed);
                if (index >= rowChunks) {
                    break;
                }
                RowReduction partial;
                rowPass(
                    primalValues,
                    dualValues,
                    rowChunk[static_cast<std::size_t>(index)],
                    rowChunk[static_cast<std::size_t>(index) + 1],
                    partial
                );
                total.violationSquared += partial.violationSquared;
                total.support += partial.support;
                total.dualInfeasibilitySquared += partial.dualInfeasibilitySquared;
            }
            rowReductions_[static_cast<std::size_t>(worker)] = total;
        });

        const std::vector<int>& columnChunk = plan_->columnChunk;
        const int columnChunks = plan_->columnChunkCount();
        std::atomic<int> columnCursor{0};
        executor_->run([&](int worker) {
            ColumnReduction total;
            while (true) {
                const int index = columnCursor.fetch_add(1, std::memory_order_relaxed);
                if (index >= columnChunks) {
                    break;
                }
                ColumnReduction partial;
                columnPass(
                    primalValues,
                    dualValues,
                    columnChunk[static_cast<std::size_t>(index)],
                    columnChunk[static_cast<std::size_t>(index) + 1],
                    partial
                );
                total.objective += partial.objective;
                total.infimum += partial.infimum;
                total.infeasibilitySquared += partial.infeasibilitySquared;
            }
            columnReductions_[static_cast<std::size_t>(worker)] = total;
        });

        for (const RowReduction& partial : rowReductions_) {
            rowTotal.violationSquared += partial.violationSquared;
            rowTotal.support += partial.support;
            rowTotal.dualInfeasibilitySquared += partial.dualInfeasibilitySquared;
        }
        for (const ColumnReduction& partial : columnReductions_) {
            columnTotal.objective += partial.objective;
            columnTotal.infimum += partial.infimum;
            columnTotal.infeasibilitySquared += partial.infeasibilitySquared;
        }
    }

    metrics.primalResidual =
        std::sqrt(rowTotal.violationSquared) / primalNormaliser_;
    metrics.dualResidual =
        std::sqrt(columnTotal.infeasibilitySquared + rowTotal.dualInfeasibilitySquared) /
        dualNormaliser_;

    metrics.primalObjective = problem_.objectiveOffset + columnTotal.objective;
    metrics.dualObjective =
        problem_.objectiveOffset + columnTotal.infimum - rowTotal.support;

    metrics.finite = std::isfinite(metrics.primalObjective) &&
        std::isfinite(metrics.dualObjective) &&
        std::isfinite(metrics.primalResidual) &&
        std::isfinite(metrics.dualResidual);

    if (!metrics.finite) {
        metrics.dualObjectiveValid = false;
        metrics.kktScore = std::numeric_limits<double>::infinity();
        return metrics;
    }

    metrics.dualObjectiveValid = true;
    metrics.relativeGap = std::abs(metrics.primalObjective - metrics.dualObjective) /
        (1.0 + std::abs(metrics.primalObjective) + std::abs(metrics.dualObjective));
    metrics.kktScore = std::max(
        std::max(metrics.primalResidual, metrics.dualResidual),
        metrics.relativeGap
    );
    return metrics;
}

bool TerminationChecker::isOptimal(const CandidateMetrics& metrics) const noexcept {
    return metrics.finite &&
        metrics.dualObjectiveValid &&
        metrics.primalResidual <= options_.primalTolerance &&
        metrics.dualResidual <= options_.dualTolerance &&
        metrics.relativeGap <= options_.gapTolerance;
}

}  // namespace pdlp
