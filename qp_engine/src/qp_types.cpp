#include "qp/qp_types.h"

namespace qp {

const char* toString(QpStatus status) noexcept {
    switch (status) {
        case QpStatus::Optimal:          return "Optimal";
        case QpStatus::IterationLimit:   return "IterationLimit";
        case QpStatus::TimeLimit:        return "TimeLimit";
        case QpStatus::NumericalFailure: return "NumericalFailure";
        case QpStatus::Unbounded:        return "Unbounded";
        case QpStatus::Infeasible:       return "Infeasible";
        case QpStatus::InvalidProblem:   return "InvalidProblem";
    }
    return "Unknown";
}

}  // namespace qp
