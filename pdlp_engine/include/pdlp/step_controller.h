#pragma once

#include "pdlp/pdlp_options.h"
#include "pdlp/pdlp_state.h"

#include <cstdint>

namespace pdlp {

// Step size and primal weight policy.
//
// The step size is chosen by the PDLP adaptive linesearch rather than pinned to
// the Pock-Chambolle bound eta <= 1/||A||. That bound is worst case over all
// directions; the iterate moves in one direction, where the effective curvature
// is routinely one to two orders of magnitude smaller. The linesearch measures
// that curvature directly from the step it just computed, which is why it can
// and does exceed the static bound.
class StepController {
public:
    StepController(const PdlpOptions& options, double maximumSafeGlobalStep);

    [[nodiscard]] const StepParameters& parameters() const noexcept {
        return parameters_;
    }

    // Tests a trial step against the local curvature and updates the step size.
    // Returns true if the trial should be committed; false means the caller must
    // recompute the trial with the new, smaller step.
    //
    //   eta_bar = ||dz||^2_omega / (2 |dy^T A dx|)
    //   accept if eta <= eta_bar
    //   eta <- min{ (1 - (k+1)^-0.3) eta_bar, (1 + (k+1)^-0.6) eta }
    //
    // where ||dz||^2_omega = omega * sum dx_j^2/T_j + (1/omega) * sum dy_i^2/Sigma_i,
    // the inverse-preconditioner norm the descent condition is stated in.
    [[nodiscard]] bool evaluateTrial(
        double primalMovementWeighted,
        double dualMovementWeighted,
        double interaction,
        std::int64_t iteration
    ) noexcept;


    void setPrimalWeight(double weight) noexcept;

    // Moves omega toward dualDistance / primalDistance, smoothed in log space.
    // Distances are straight-line displacements since the previous restart, not
    // accumulated path lengths: the path length of an oscillating cycle can
    // exceed its displacement by orders of magnitude, which drives the weight in
    // the wrong direction.
    void updatePrimalWeight(double primalDistance, double dualDistance);

private:
    PdlpOptions options_;
    StepParameters parameters_;
};

}  // namespace pdlp
