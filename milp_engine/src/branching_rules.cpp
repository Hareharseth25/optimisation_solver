#include "milp/branching_rules.h"

#include <algorithm>
#include <cmath>

namespace milp {
namespace {

constexpr double kScoreMixingFactor = 0.16;

bool validIndex(int variableIndex, std::size_t count) {
    return variableIndex >= 0 && static_cast<std::size_t>(variableIndex) < count;
}

} // namespace

PseudoCostTracker::PseudoCostTracker(std::size_t variableCount) : stats_(variableCount) {}

void PseudoCostTracker::update(int variableIndex, double changeDown, double changeUp, double frac) {
    if (!validIndex(variableIndex, stats_.size())) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    PseudoCostStats &s = stats_[static_cast<std::size_t>(variableIndex)];

    if (changeDown >= 0.0 && frac > 0.0) {
        s.psiDownSum += changeDown / frac;
        ++s.psiDownCount;
    }

    const double upSpan = 1.0 - frac;
    if (changeUp >= 0.0 && upSpan > 0.0) {
        s.psiUpSum += changeUp / upSpan;
        ++s.psiUpCount;
    }
}

bool PseudoCostTracker::hasHistory(int variableIndex) const {
    if (!validIndex(variableIndex, stats_.size())) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const PseudoCostStats &s = stats_[static_cast<std::size_t>(variableIndex)];
    return s.psiDownCount >= 1 && s.psiUpCount >= 1;
}

PseudoCostStats PseudoCostTracker::stats(int variableIndex) const {
    if (!validIndex(variableIndex, stats_.size())) {
        return {};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_[static_cast<std::size_t>(variableIndex)];
}

double PseudoCostTracker::score(int variableIndex, double value) const {
    if (!validIndex(variableIndex, stats_.size())) {
        return 0.0;
    }

    PseudoCostStats s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        s = stats_[static_cast<std::size_t>(variableIndex)];
    }

    if (s.psiDownCount < 1 || s.psiUpCount < 1) {
        return 0.0;
    }

    const double frac = value - std::floor(value);
    const double psiDown = s.psiDownSum / static_cast<double>(s.psiDownCount);
    const double psiUp = s.psiUpSum / static_cast<double>(s.psiUpCount);

    const double dDown = psiDown * frac;
    const double dUp = psiUp * (1.0 - frac);

    return std::min(dDown, dUp) + kScoreMixingFactor * std::max(dDown, dUp);
}

int PseudoCostTracker::selectBranchingVariable(
    const model::Model &model,
    const std::vector<double> &primal,
    double integralityTolerance) const
{
    int bestPseudoCostVar = -1;
    double bestPseudoCostScore = -1.0;

    int bestFractionalVar = -1;
    double bestFractionalDistance = -1.0;

    for (std::size_t j = 0; j < model.variables.size() && j < primal.size(); ++j) {
        if (model.variables[j].type == model::VariableType::Continuous) {
            continue;
        }

        const double value = primal[j];
        const double nearest = std::round(value);
        if (std::abs(value - nearest) <= integralityTolerance) {
            continue;
        }

        const double frac = value - std::floor(value);
        const double distance = std::min(frac, 1.0 - frac);
        if (distance > bestFractionalDistance) {
            bestFractionalDistance = distance;
            bestFractionalVar = static_cast<int>(j);
        }

        const int idx = static_cast<int>(j);
        if (hasHistory(idx)) {
            const double variableScore = score(idx, value);
            if (variableScore > bestPseudoCostScore) {
                bestPseudoCostScore = variableScore;
                bestPseudoCostVar = idx;
            }
        }
    }

    return (bestPseudoCostVar >= 0) ? bestPseudoCostVar : bestFractionalVar;
}

} // namespace milp
