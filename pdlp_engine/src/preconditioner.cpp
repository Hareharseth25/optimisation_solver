#include "pdlp/preconditioner.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace pdlp {
namespace {

constexpr double kMinimumNorm = 1e-12;
constexpr double kPowerTolerance = 1e-8;

double normalize(std::vector<double>& vector) noexcept {
    double squaredNorm = 0.0;
    for (double value : vector) {
        squaredNorm += value * value;
    }
    const double norm = std::sqrt(squaredNorm);
    if (norm > 0.0) {
        const double inverse = 1.0 / norm;
        for (double& value : vector) {
            value *= inverse;
        }
    }
    return norm;
}

}  // namespace

DiagonalPreconditioner Preconditioner::compute(
    const CompiledLp& problem,
    bool enabled
) {
    DiagonalPreconditioner result;
    result.primalScale.assign(static_cast<std::size_t>(problem.numColumns()), 1.0);
    result.dualScale.assign(static_cast<std::size_t>(problem.numRows()), 1.0);
    result.primalScaleInverse.assign(static_cast<std::size_t>(problem.numColumns()), 1.0);
    result.dualScaleInverse.assign(static_cast<std::size_t>(problem.numRows()), 1.0);
    if (!enabled) {
        return result;
    }

    std::vector<double> columnSum(static_cast<std::size_t>(problem.numColumns()), 0.0);
    std::vector<double> rowSum(static_cast<std::size_t>(problem.numRows()), 0.0);

    const auto& rowStart = problem.matrix.csrRowStart();
    const auto& columnIndex = problem.matrix.csrColumnIndex();
    const auto& values = problem.matrix.csrValues();

    for (int row = 0; row < problem.numRows(); ++row) {
        double accumulated = 0.0;
        const auto begin = rowStart[static_cast<std::size_t>(row)];
        const auto end = rowStart[static_cast<std::size_t>(row) + 1];
        for (auto k = begin; k < end; ++k) {
            const double magnitude = std::abs(values[static_cast<std::size_t>(k)]);
            accumulated += magnitude;
            columnSum[static_cast<std::size_t>(columnIndex[static_cast<std::size_t>(k)])] +=
                magnitude;
        }
        rowSum[static_cast<std::size_t>(row)] = accumulated;
    }

    for (int column = 0; column < problem.numColumns(); ++column) {
        const auto j = static_cast<std::size_t>(column);
        const double sum = columnSum[j];
        const double scale = sum > kMinimumNorm ? 1.0 / sum : 1.0;
        result.primalScale[j] = scale;
        result.primalScaleInverse[j] = 1.0 / scale;
    }
    for (int row = 0; row < problem.numRows(); ++row) {
        const auto i = static_cast<std::size_t>(row);
        const double sum = rowSum[i];
        const double scale = sum > kMinimumNorm ? 1.0 / sum : 1.0;
        result.dualScale[i] = scale;
        result.dualScaleInverse[i] = 1.0 / scale;
    }

    return result;
}

double Preconditioner::estimateSpectralNorm(
    const SparseMatrix& matrix,
    int iterations,
    bool* converged
) {
    if (converged != nullptr) {
        *converged = true;
    }
    if (matrix.rows() == 0 || matrix.columns() == 0 || matrix.nonzeros() == 0) {
        return 0.0;
    }

    std::mt19937 generator(1);
    std::uniform_real_distribution<double> distribution(-1.0, 1.0);
    std::vector<double> x(static_cast<std::size_t>(matrix.columns()));
    for (double& value : x) {
        value = distribution(generator);
    }
    if (normalize(x) == 0.0) {
        x.assign(x.size(), 1.0 / std::sqrt(static_cast<double>(x.size())));
    }

    std::vector<double> y;
    std::vector<double> z;
    const int budget = std::max(iterations, 1);
    double previous = 0.0;
    double estimate = 0.0;
    bool reachedTolerance = false;

    for (int iteration = 0; iteration < budget; ++iteration) {
        matrix.multiply(x, y);
        matrix.transposeMultiply(y, z);

        // Rayleigh quotient x^T A^T A x with ||x|| = 1, so the estimate is
        // sigma_max^2 before the square root.
        double rayleigh = 0.0;
        for (std::size_t j = 0; j < x.size(); ++j) {
            rayleigh += x[j] * z[j];
        }
        estimate = std::sqrt(std::max(rayleigh, 0.0));

        if (normalize(z) == 0.0) {
            return estimate;
        }
        x.swap(z);

        if (iteration > 0 && estimate > 0.0 &&
            std::abs(estimate - previous) <= kPowerTolerance * estimate) {
            reachedTolerance = true;
            break;
        }
        previous = estimate;
    }

    if (converged != nullptr) {
        *converged = reachedTolerance;
    }
    return estimate;
}

}  // namespace pdlp
