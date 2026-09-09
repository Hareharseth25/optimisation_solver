#pragma once

#include "pdlp/compiled_lp.h"
#include "pdlp/pdlp_state.h"

#include <vector>

namespace pdlp {

// Ruiz equilibration of the constraint matrix.
//
// PDHG converges at a rate governed by the conditioning of A, and its residuals
// are measured against global norms. On a matrix whose entries span several
// orders of magnitude, both properties fail together: the badly scaled
// coordinates converge slowly, and the large rows inflate the residual
// normaliser until the violations of the small rows disappear below tolerance.
// The engine then reports a converged solve for a point that is materially
// infeasible.
//
// Equilibration removes both failure modes by rescaling rows and columns until
// every row and column of A has infinity norm near one. The solver iterates on
// the scaled problem but always evaluates termination on the original, so the
// tolerances the caller asks for are the tolerances they get.
//
//   scaled matrix   Ahat = diag(rowScale) * A * diag(columnScale)
//   variables       x    = diag(columnScale) * xhat
//   row duals       y    = diag(rowScale)    * yhat
struct ProblemScaling {
    std::vector<double> rowScale;
    std::vector<double> columnScale;
    CompiledLp problem;

    // Maps a scaled iterate back into the original problem's coordinates.
    void toOriginal(
        const std::vector<double>& scaledPrimal,
        const std::vector<double>& scaledDual,
        std::vector<double>& primal,
        std::vector<double>& dual
    ) const;

    // Maps an original-space iterate into scaled coordinates.
    void toScaled(
        const std::vector<double>& primal,
        const std::vector<double>& dual,
        std::vector<double>& scaledPrimal,
        std::vector<double>& scaledDual
    ) const;
};

class RuizScaler {
public:
    // `iterations` passes of alternating row/column infinity-norm equilibration.
    // Ten passes bring both norms into roughly [0.9, 1.1] for typical
    // industrial matrices.
    [[nodiscard]] static ProblemScaling equilibrate(
        const CompiledLp& problem,
        int iterations
    );

    // Largest ratio of row infinity norms, and the same for columns. Reported
    // for diagnostics: a value near one means the matrix is equilibrated.
    [[nodiscard]] static double conditionSpread(const SparseMatrix& matrix);
};

}  // namespace pdlp
