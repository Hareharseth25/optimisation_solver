#include "pdlp/scaling.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pdlp {
namespace {

constexpr double kMinimumMagnitude = 1e-30;

// Rescales a bound, preserving infinities exactly: an infinite bound must stay
// infinite rather than becoming a very large finite number.
inline double scaleBound(double bound, double factor) noexcept {
    if (!std::isfinite(bound)) {
        return bound;
    }
    return bound * factor;
}

}  // namespace

void ProblemScaling::toOriginal(
    const std::vector<double>& scaledPrimal,
    const std::vector<double>& scaledDual,
    std::vector<double>& primal,
    std::vector<double>& dual
) const {
    primal.resize(scaledPrimal.size());
    for (std::size_t j = 0; j < scaledPrimal.size(); ++j) {
        primal[j] = columnScale[j] * scaledPrimal[j];
    }
    dual.resize(scaledDual.size());
    for (std::size_t i = 0; i < scaledDual.size(); ++i) {
        dual[i] = rowScale[i] * scaledDual[i];
    }
}

void ProblemScaling::toScaled(
    const std::vector<double>& primal,
    const std::vector<double>& dual,
    std::vector<double>& scaledPrimal,
    std::vector<double>& scaledDual
) const {
    scaledPrimal.resize(primal.size());
    for (std::size_t j = 0; j < primal.size(); ++j) {
        scaledPrimal[j] = primal[j] / columnScale[j];
    }
    scaledDual.resize(dual.size());
    for (std::size_t i = 0; i < dual.size(); ++i) {
        scaledDual[i] = dual[i] / rowScale[i];
    }
}

ProblemScaling RuizScaler::equilibrate(const CompiledLp& problem, int iterations) {
    const int rows = problem.numRows();
    const int columns = problem.numColumns();

    ProblemScaling scaling;
    scaling.rowScale.assign(static_cast<std::size_t>(rows), 1.0);
    scaling.columnScale.assign(static_cast<std::size_t>(columns), 1.0);

    const auto& rowStart = problem.matrix.csrRowStart();
    const auto& columnIndex = problem.matrix.csrColumnIndex();
    const auto& values = problem.matrix.csrValues();

    std::vector<double> rowMax(static_cast<std::size_t>(rows), 0.0);
    std::vector<double> columnMax(static_cast<std::size_t>(columns), 0.0);

    const int passes = std::max(iterations, 0);
    for (int pass = 0; pass < passes; ++pass) {
        std::fill(rowMax.begin(), rowMax.end(), 0.0);
        std::fill(columnMax.begin(), columnMax.end(), 0.0);

        // One sweep produces both norm vectors; the current scaling is applied
        // on the fly so no scaled copy of A is materialised per pass.
        for (int row = 0; row < rows; ++row) {
            const double rowFactor = scaling.rowScale[static_cast<std::size_t>(row)];
            double localMax = 0.0;
            const auto begin = rowStart[static_cast<std::size_t>(row)];
            const auto end = rowStart[static_cast<std::size_t>(row) + 1];
            for (auto k = begin; k < end; ++k) {
                const std::size_t index = static_cast<std::size_t>(k);
                const auto column = static_cast<std::size_t>(columnIndex[index]);
                const double magnitude =
                    std::abs(values[index]) * rowFactor * scaling.columnScale[column];
                localMax = std::max(localMax, magnitude);
                columnMax[column] = std::max(columnMax[column], magnitude);
            }
            rowMax[static_cast<std::size_t>(row)] = localMax;
        }

        // The square root splits each correction between the row and the column
        // side, which is what makes the alternating iteration converge.
        for (int row = 0; row < rows; ++row) {
            const double magnitude = rowMax[static_cast<std::size_t>(row)];
            if (magnitude > kMinimumMagnitude) {
                scaling.rowScale[static_cast<std::size_t>(row)] /= std::sqrt(magnitude);
            }
        }
        for (int column = 0; column < columns; ++column) {
            const double magnitude = columnMax[static_cast<std::size_t>(column)];
            if (magnitude > kMinimumMagnitude) {
                scaling.columnScale[static_cast<std::size_t>(column)] /= std::sqrt(magnitude);
            }
        }
    }

    CompiledLp& scaled = scaling.problem;
    scaled.matrix = problem.matrix.scaled(scaling.rowScale, scaling.columnScale);
    scaled.objectiveOffset = problem.objectiveOffset;

    // xhat = x / columnScale, so the objective and the variable bounds pick up
    // columnScale while the row bounds pick up rowScale.
    scaled.objective.resize(static_cast<std::size_t>(columns));
    scaled.variableLower.resize(static_cast<std::size_t>(columns));
    scaled.variableUpper.resize(static_cast<std::size_t>(columns));
    for (int column = 0; column < columns; ++column) {
        const auto j = static_cast<std::size_t>(column);
        const double factor = scaling.columnScale[j];
        scaled.objective[j] = problem.objective[j] * factor;
        scaled.variableLower[j] = scaleBound(problem.variableLower[j], 1.0 / factor);
        scaled.variableUpper[j] = scaleBound(problem.variableUpper[j], 1.0 / factor);
    }

    scaled.rowLower.resize(static_cast<std::size_t>(rows));
    scaled.rowUpper.resize(static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row) {
        const auto i = static_cast<std::size_t>(row);
        const double factor = scaling.rowScale[i];
        scaled.rowLower[i] = scaleBound(problem.rowLower[i], factor);
        scaled.rowUpper[i] = scaleBound(problem.rowUpper[i], factor);
    }

    return scaling;
}

double RuizScaler::conditionSpread(const SparseMatrix& matrix) {
    const auto& rowStart = matrix.csrRowStart();
    const auto& values = matrix.csrValues();

    double smallest = std::numeric_limits<double>::infinity();
    double largest = 0.0;
    for (int row = 0; row < matrix.rows(); ++row) {
        double localMax = 0.0;
        const auto begin = rowStart[static_cast<std::size_t>(row)];
        const auto end = rowStart[static_cast<std::size_t>(row) + 1];
        for (auto k = begin; k < end; ++k) {
            localMax = std::max(localMax, std::abs(values[static_cast<std::size_t>(k)]));
        }
        if (localMax > 0.0) {
            smallest = std::min(smallest, localMax);
            largest = std::max(largest, localMax);
        }
    }
    if (!std::isfinite(smallest) || smallest <= 0.0) {
        return 1.0;
    }
    return largest / smallest;
}

}  // namespace pdlp
