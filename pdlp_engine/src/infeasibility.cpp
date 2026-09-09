#include "pdlp/infeasibility.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pdlp {
namespace {

constexpr double kTiny = 1e-300;

double euclideanNorm(const std::vector<double>& values) noexcept {
    double sum = 0.0;
    for (double value : values) {
        sum += value * value;
    }
    return std::sqrt(sum);
}

// Scales a direction to unit length. Returns false if it has no direction to
// speak of, which is the common case early in a solve.
bool normalizeInto(
    const std::vector<double>& source,
    std::vector<double>& target
) {
    const double norm = euclideanNorm(source);
    if (!(norm > kTiny) || !std::isfinite(norm)) {
        return false;
    }
    target.resize(source.size());
    const double inverse = 1.0 / norm;
    for (std::size_t i = 0; i < source.size(); ++i) {
        target[i] = source[i] * inverse;
    }
    return true;
}

}  // namespace

InfeasibilityDetector::InfeasibilityDetector(
    const CompiledLp& problem,
    PdlpOptions options,
    Executor* executor,
    const SpmvPlan* plan
)
    : problem_(problem), options_(options), executor_(executor), plan_(plan) {
    scratchColumns_.assign(static_cast<std::size_t>(problem.numColumns()), 0.0);
    scratchRows_.assign(static_cast<std::size_t>(problem.numRows()), 0.0);
}

InfeasibilityVerdict InfeasibilityDetector::testDualRay(
    const std::vector<double>& direction
) {
    InfeasibilityVerdict verdict;
    if (direction.size() != static_cast<std::size_t>(problem_.numRows())) {
        return verdict;
    }
    if (!normalizeInto(direction, normalized_)) {
        return verdict;
    }

    problem_.matrix.transposeMultiply(normalized_, scratchColumns_, executor_, plan_);

    // inf over the variable box of (A'y)'x. Finite only where the sign of the
    // reduced cost selects a finite bound; the distance to that cone is charged
    // as violation.
    double boxInfimum = 0.0;
    double violationSquared = 0.0;
    for (int column = 0; column < problem_.numColumns(); ++column) {
        const auto j = static_cast<std::size_t>(column);
        const double reduced = scratchColumns_[j];
        const double lower = problem_.variableLower[j];
        const double upper = problem_.variableUpper[j];

        double projected = reduced;
        if (!std::isfinite(lower)) {
            projected = std::min(projected, 0.0);
        }
        if (!std::isfinite(upper)) {
            projected = std::max(projected, 0.0);
        }
        const double drift = reduced - projected;
        violationSquared += drift * drift;

        if (projected > 0.0) {
            boxInfimum += projected * lower;
        } else if (projected < 0.0) {
            boxInfimum += projected * upper;
        }
    }

    // sup over the row bounds of y'z, with the same treatment.
    double support = 0.0;
    for (int row = 0; row < problem_.numRows(); ++row) {
        const auto i = static_cast<std::size_t>(row);
        const double value = normalized_[i];
        const double lower = problem_.rowLower[i];
        const double upper = problem_.rowUpper[i];

        double projected = value;
        if (!std::isfinite(upper)) {
            projected = std::min(projected, 0.0);
        }
        if (!std::isfinite(lower)) {
            projected = std::max(projected, 0.0);
        }
        const double drift = value - projected;
        violationSquared += drift * drift;

        if (projected > 0.0) {
            support += projected * upper;
        } else if (projected < 0.0) {
            support += projected * lower;
        }
    }

    verdict.certificateValue = boxInfimum - support;
    if (!std::isfinite(verdict.certificateValue)) {
        return verdict;
    }

    const double violation = std::sqrt(violationSquared);
    if (verdict.certificateValue > 0.0) {
        verdict.relativeViolation = violation / verdict.certificateValue;
        verdict.provesPrimalInfeasible =
            verdict.relativeViolation <= options_.infeasibilityTolerance;
    }
    return verdict;
}

InfeasibilityVerdict InfeasibilityDetector::testPrimalRay(
    const std::vector<double>& direction
) {
    InfeasibilityVerdict verdict;
    if (direction.size() != static_cast<std::size_t>(problem_.numColumns())) {
        return verdict;
    }
    if (!normalizeInto(direction, normalized_)) {
        return verdict;
    }

    // Project onto the recession cone of the variable box: a coordinate may
    // only grow where its bound is infinite in that direction.
    double violationSquared = 0.0;
    double objectiveRate = 0.0;
    for (int column = 0; column < problem_.numColumns(); ++column) {
        const auto j = static_cast<std::size_t>(column);
        const double value = normalized_[j];
        double projected = value;
        if (std::isfinite(problem_.variableUpper[j])) {
            projected = std::min(projected, 0.0);
        }
        if (std::isfinite(problem_.variableLower[j])) {
            projected = std::max(projected, 0.0);
        }
        const double drift = value - projected;
        violationSquared += drift * drift;

        scratchColumns_[j] = projected;
        objectiveRate += problem_.objective[j] * projected;
    }

    problem_.matrix.multiply(scratchColumns_, scratchRows_, executor_, plan_);

    // A*x must likewise stay inside the recession cone of the row bounds.
    for (int row = 0; row < problem_.numRows(); ++row) {
        const auto i = static_cast<std::size_t>(row);
        const double activity = scratchRows_[i];
        double projected = activity;
        if (std::isfinite(problem_.rowUpper[i])) {
            projected = std::min(projected, 0.0);
        }
        if (std::isfinite(problem_.rowLower[i])) {
            projected = std::max(projected, 0.0);
        }
        const double drift = activity - projected;
        violationSquared += drift * drift;
    }

    verdict.certificateValue = objectiveRate;
    if (!std::isfinite(objectiveRate)) {
        return verdict;
    }

    const double violation = std::sqrt(violationSquared);
    if (objectiveRate < 0.0) {
        verdict.relativeViolation = violation / (-objectiveRate);
        verdict.provesUnbounded =
            verdict.relativeViolation <= options_.infeasibilityTolerance;
    }
    return verdict;
}

}  // namespace pdlp
