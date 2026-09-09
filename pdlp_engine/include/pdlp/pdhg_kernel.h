#pragma once

#include "pdlp/compiled_lp.h"
#include "pdlp/parallel.h"
#include "pdlp/pdlp_state.h"

#include <vector>

namespace pdlp {

struct KernelTrialResult {
    // Movement measured in the inverse-preconditioner norm, which is the norm
    // the PDHG descent condition is stated in:
    //   sum_j dx_j^2 / T_j   and   sum_i dy_i^2 / Sigma_i.
    // Using the plain Euclidean norm here would make the linesearch bound wrong
    // whenever a preconditioner is active.
    double primalMovementWeighted = 0.0;
    double dualMovementWeighted = 0.0;

    // dy^T A dx, the interaction term the step size test is built on.
    double interaction = 0.0;

    bool finite = true;
};

// One trial PDHG iteration, fused.
//
// The textbook formulation materialises A^T*y and A*x_bar into temporaries and
// walks them again to apply the proximal steps. Column j of the primal prox
// depends only on entry j of A^T*y, and row i of the dual prox only on entry i
// of A*x_bar, so both temporaries are eliminated: each coordinate's sparse dot
// product feeds straight into its prox while still in a register. That leaves
// exactly one barrier per iteration, the minimum PDHG admits.
//
// The step is computed into trial buffers and not applied, so an adaptive
// linesearch can reject it without having to restore the previous iterate.
// commit() installs the accepted trial by swapping buffers, never by copying.
class CpuPdhgKernel {
public:
    // `executor` and `plan` may be null, selecting serial execution. Both must
    // outlive the kernel, and `plan` must have been built from problem.matrix.
    CpuPdhgKernel(
        const CompiledLp& problem,
        Executor* executor = nullptr,
        const SpmvPlan* plan = nullptr
    );

    // Sizes the state's buffers and recomputes rowActivity = A*x exactly. Must
    // be called before the first trial and again whenever primal is replaced
    // from outside the kernel, such as on a restart.
    void refreshActivity(PdlpState& state) const;

    [[nodiscard]] KernelTrialResult trial(
        const PdlpState& state,
        const DiagonalPreconditioner& preconditioner,
        const StepParameters& steps
    );

    // Installs the most recent trial by swapping buffers, never by copying.
    // Only valid immediately after trial().
    void commit(PdlpState& state);


private:
    struct alignas(64) PartialReduction {
        double primal = 0.0;
        double dual = 0.0;
        double interaction = 0.0;
    };

    double primalHalf(
        const PdlpState& state,
        const DiagonalPreconditioner& preconditioner,
        const StepParameters& steps,
        int begin,
        int end
    ) noexcept;

    void dualHalf(
        const PdlpState& state,
        const DiagonalPreconditioner& preconditioner,
        const StepParameters& steps,
        int begin,
        int end,
        double& movementWeighted,
        double& interaction
    ) noexcept;

    const CompiledLp& problem_;
    Executor* executor_ = nullptr;
    const SpmvPlan* plan_ = nullptr;
    int parts_ = 1;

    std::vector<double> primalTrial_;    // x'
    std::vector<double> dualTrial_;      // y'
    std::vector<double> activityTrial_;  // A*x'
    std::vector<PartialReduction> reductions_;
};

}  // namespace pdlp
