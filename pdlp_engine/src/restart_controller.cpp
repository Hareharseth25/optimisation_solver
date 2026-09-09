#include "pdlp/restart_controller.h"

#include <algorithm>
#include <cmath>

namespace pdlp {

RestartDecision RestartController::choose(
    const CandidateMetrics& current,
    const CandidateMetrics& average,
    double baselineScore,
    std::int64_t iterationsSinceRestart,
    std::int64_t totalIterations
) noexcept {
    RestartDecision decision;
    if (!options_.useRestarts ||
        iterationsSinceRestart < options_.minimumRestartIterations) {
        return decision;
    }

    const bool averageFinite = std::isfinite(average.kktScore);
    const bool currentFinite = std::isfinite(current.kktScore);
    if (!averageFinite && !currentFinite) {
        return decision;
    }

    // Restart from whichever candidate is better; the averaged iterate usually
    // is, which is the whole reason averaging is maintained.
    RestartChoice candidate = RestartChoice::Average;
    double score = average.kktScore;
    if (!averageFinite || (currentFinite && current.kktScore < average.kktScore)) {
        candidate = RestartChoice::Current;
        score = current.kktScore;
    }
    decision.candidateScore = score;

    const double previous = previousCandidateScore_;
    previousCandidateScore_ = score;

    const bool baselineFinite = std::isfinite(baselineScore);

    if (baselineFinite && score <= options_.restartSufficientDecay * baselineScore) {
        decision.choice = candidate;
        return decision;
    }

    if (baselineFinite &&
        score <= options_.restartNecessaryDecay * baselineScore &&
        std::isfinite(previous) && score > previous) {
        decision.choice = candidate;
        return decision;
    }

    const auto artificialLimit = static_cast<std::int64_t>(
        options_.restartArtificialFraction * static_cast<double>(totalIterations));
    if (iterationsSinceRestart >= std::max<std::int64_t>(artificialLimit, 1) ||
        iterationsSinceRestart >= options_.maximumRestartIterations) {
        decision.choice = candidate;
        return decision;
    }

    return decision;
}

}  // namespace pdlp
