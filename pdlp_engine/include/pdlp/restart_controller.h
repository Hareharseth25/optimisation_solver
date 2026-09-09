#pragma once

#include "pdlp/pdlp_options.h"
#include "pdlp/termination.h"

#include <cstdint>
#include <limits>

namespace pdlp {

enum class RestartChoice {
    None,
    Current,
    Average
};

struct RestartDecision {
    RestartChoice choice = RestartChoice::None;
    double candidateScore = std::numeric_limits<double>::infinity();
};

// Adaptive restart policy.
//
// Restarts are what turn PDHG's sublinear tail into the linear convergence PDLP
// relies on, so the trigger has to be aggressive. Three conditions fire it:
//
//   sufficient decay  the candidate improved on the last restart point by a
//                     large factor, so restarting locks the progress in;
//   necessary decay   the candidate improved by a smaller factor and has now
//                     started getting worse, meaning the current cycle has
//                     passed its best point and further iterations are wasted;
//   artificial        the cycle has run for a fixed fraction of the whole solve
//                     without either condition firing, which bounds the damage
//                     a badly conditioned cycle can do.
//
// The candidate is whichever of the current and averaged iterates scores lower.
class RestartController {
public:
    explicit RestartController(PdlpOptions options) : options_(options) {}

    [[nodiscard]] RestartDecision choose(
        const CandidateMetrics& current,
        const CandidateMetrics& average,
        double baselineScore,
        std::int64_t iterationsSinceRestart,
        std::int64_t totalIterations
    ) noexcept;

    void reset() noexcept {
        previousCandidateScore_ = std::numeric_limits<double>::infinity();
    }

private:
    PdlpOptions options_;
    double previousCandidateScore_ = std::numeric_limits<double>::infinity();
};

}  // namespace pdlp
