#pragma once

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 7
#pragma GCC diagnostic ignored "-Wattributes"
#endif

#include <cstddef>
#include <cstdint>

namespace qp {

// Index type for all logical array positions (variables, constraints).
using Index = int;

// Offset type for CSR/CSC structural arrays (row starts, column starts).
// Must be large enough for problems with more than 2^31 nonzeros.
using Offset = std::int64_t;

// ADMM state index — distinct from model indices because the ADMM keeps
// additional auxiliary and scaled variables.
using StateIndex = int;

// ---------------------------------------------------------------------------
// Solver return status
// ---------------------------------------------------------------------------

enum class QpStatus {
    Optimal,
    IterationLimit,
    TimeLimit,
    NumericalFailure,
    Unbounded,
    Infeasible,
    InvalidProblem
};

// Human-readable label for a status code.
[[nodiscard]] const char* toString(QpStatus status) noexcept;

// ---------------------------------------------------------------------------
// Problem-size limits used for validation and zero-sized edge cases
// ---------------------------------------------------------------------------

// Tolerances used throughout the solver unless overridden in options.
struct DefaultTolerances {
    static constexpr double primalFeasibility = 1e-6;
    static constexpr double dualFeasibility   = 1e-6;
    static constexpr double gap              = 1e-6;
    static constexpr double singularity      = 1e-12;  // for KKT factorization
    static constexpr double zero            = 1e-30;  // treated as zero in scaling
};

}  // namespace qp
