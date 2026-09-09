#pragma once

#include "pdlp/compiled_lp.h"
#include "pdlp/pdlp_state.h"

namespace pdlp {

class Preconditioner {
public:
    // Pock-Chambolle diagonal preconditioner with alpha = 1:
    //   tau_j = 1 / sum_i |a_ij|,  sigma_i = 1 / sum_j |a_ij|.
    // Under this choice the PDHG convergence condition holds for any global
    // step <= 1, so no spectral norm estimate is needed.
    [[nodiscard]] static DiagonalPreconditioner compute(
        const CompiledLp& problem,
        bool enabled
    );

    // Power iteration on A^T*A. Converges to sigma_max from below, so callers
    // must apply a safety margin before using it to bound a step size.
    // `converged` reports whether the relative change fell below the internal
    // tolerance within the iteration budget.
    [[nodiscard]] static double estimateSpectralNorm(
        const SparseMatrix& matrix,
        int iterations,
        bool* converged = nullptr
    );
};

}  // namespace pdlp
