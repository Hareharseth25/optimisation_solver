#pragma once

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 7
#pragma GCC diagnostic ignored "-Wattributes"
#endif

#include "qp/admm_solver.h"
#include "qp/qp_model.h"
#include "qp/qp_types.h"

#include <string>
#include <vector>

namespace qp {

// Top-level facade: the only type most callers should need to know about.
//   qp::QpSolver solver;
//   auto result = solver.solve(model, options);
class QpSolver {
public:
    [[nodiscard]] AdmmResult solve(
        const QpModel& problem,
        const AdmmOptions& options = {}
    ) const;
};

}  // namespace qp
