#pragma once

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 7
#pragma GCC diagnostic ignored "-Wattributes"
#endif

#include "qp/qp_model.h"
#include "qp/qp_types.h"

#include <vector>

namespace qp {

// Ruiz-style equilibration for the QP.
//
// On an ill-conditioned problem the ADMM step size is governed by the operator
// norm of the KKT block [P  A^T; A  0], and the KKT residual normaliser is
// dominated by the largest row. Equilibration scales rows and columns of A and
// the diagonal of P toward unit infinity norm, which both accelerates
// convergence and lets the residual normaliser see all constraints equally.
//
// The solver iterates on the scaled problem and reports the result back in the
// original coordinates, so the tolerances the caller asks for stay in the
// caller's units.
struct QpScaling {
    std::vector<double> rowScale;     // length m  (used for A rows)
    std::vector<double> columnScale;  // length n  (used for P and A columns)

    // Original P, q, A, l, u rescaled into the solver's working coordinates.
    QpModel scaled;

    // Iterates back to the original problem.
    void toOriginal(
        const std::vector<double>& scaledPrimal,
        std::vector<double>& primal
    ) const;
    void toOriginalDual(
        const std::vector<double>& scaledDual,
        std::vector<double>& dual
    ) const;
};

class RuizScaler {
public:
    // `iterations` passes of alternating row/column infinity-norm
    // equilibration. The m == 0 case is allowed: rowScale is empty and the
    // column scaling is a pure diagonal equilibration of P.
    [[nodiscard]] static QpScaling equilibrate(
        const QpModel& model,
        int iterations
    );

    // Largest row infinity norm, and the same for columns, ignoring the m == 0
    // and n == 0 cases. Used for diagnostics.
    [[nodiscard]] static double conditionSpread(const SparseMatrix& matrix);
};

}  // namespace qp
