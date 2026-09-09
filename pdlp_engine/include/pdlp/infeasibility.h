#pragma once

#include "pdlp/compiled_lp.h"
#include "pdlp/parallel.h"
#include "pdlp/pdlp_options.h"
#include "pdlp/pdlp_state.h"

#include <vector>

namespace pdlp {

// Outcome of testing one candidate direction.
struct InfeasibilityVerdict {
    bool provesPrimalInfeasible = false;
    bool provesUnbounded = false;

    // Homogeneous objective of the ray. Positive for a valid Farkas
    // certificate; negative for a valid improving ray.
    double certificateValue = 0.0;

    // How far the direction is from the cone the certificate requires,
    // relative to certificateValue. Zero for an exact certificate.
    double relativeViolation = 1.0;
};

// Detects primal infeasibility and unboundedness from the iterate sequence.
//
// PDHG has no basis to read a Farkas certificate off, so the certificate comes
// from where the iterates go rather than from where they stop. On an infeasible
// primal the dual iterates diverge, and the direction they diverge along
// converges to a Farkas ray; on an unbounded primal the same holds for the
// primal iterates and an improving ray. Applegate et al. (2021) show the
// difference of iterates converges to that ray faster than the iterates
// themselves, so both are tested.
//
// Every test is performed on the ORIGINAL problem, in the caller's coordinates,
// for the same reason termination is: a certificate for a rescaled problem is
// not a certificate for the one the caller handed in.
class InfeasibilityDetector {
public:
    InfeasibilityDetector(
        const CompiledLp& problem,
        PdlpOptions options,
        Executor* executor = nullptr,
        const SpmvPlan* plan = nullptr
    );

    // Tests a dual direction as a Farkas certificate of primal infeasibility.
    //
    // For a ranged row LP with box variables the primal is infeasible when some
    // y makes the homogeneous dual objective strictly positive:
    //
    //     inf_{xl <= x <= xu} (A'y)'x  -  sup_{rl <= z <= ru} y'z  >  0
    //
    // Both terms must be finite, which constrains the sign of A'y wherever a
    // variable bound is infinite and the sign of y wherever a row bound is.
    // The distance to that cone is reported as the violation.
    [[nodiscard]] InfeasibilityVerdict testDualRay(const std::vector<double>& direction);

    // Tests a primal direction as an improving ray proving unboundedness:
    // c'x < 0, with x in the recession cone of the variable box and A*x in the
    // recession cone of the row bounds.
    [[nodiscard]] InfeasibilityVerdict testPrimalRay(const std::vector<double>& direction);

private:
    const CompiledLp& problem_;
    PdlpOptions options_;
    Executor* executor_ = nullptr;
    const SpmvPlan* plan_ = nullptr;

    std::vector<double> scratchColumns_;
    std::vector<double> scratchRows_;
    std::vector<double> normalized_;
};

}  // namespace pdlp
