#pragma once

#include "pdlp/compiled_lp.h"
#include "pdlp/parallel.h"
#include "pdlp/pdlp_options.h"
#include "pdlp/pdlp_state.h"

#include <limits>
#include <vector>

namespace pdlp {

struct CandidateMetrics {
    double primalObjective = std::numeric_limits<double>::quiet_NaN();
    double dualObjective = -std::numeric_limits<double>::infinity();
    double primalResidual = std::numeric_limits<double>::infinity();
    double dualResidual = std::numeric_limits<double>::infinity();
    double relativeGap = std::numeric_limits<double>::infinity();
    double kktScore = std::numeric_limits<double>::infinity();
    bool dualObjectiveValid = false;
    bool finite = false;
};

// Evaluates the approximate-KKT triple (primal residual, dual residual,
// relative gap) for a candidate.
//
// The dual objective is a genuine bound: reduced costs that cannot be bounded
// below over the variable box (and dual values outside the domain of the row
// support function) are projected onto the dual-feasible cone, and the
// projection distance is reported as dual infeasibility instead of being
// silently discarded. The previous formulation dropped reduced costs below a
// tolerance, which produced a dual "bound" that was not one.
//
// Not thread-safe: one checker instance owns its reduction scratch.
class TerminationChecker {
public:
    TerminationChecker(
        PdlpOptions options,
        const CompiledLp& problem,
        Executor* executor = nullptr,
        const SpmvPlan* plan = nullptr
    );

    // Overload on the raw vectors so callers can score the live solver state
    // without first copying it into a CandidateIterate.
    [[nodiscard]] CandidateMetrics evaluate(
        const std::vector<double>& primal,
        const std::vector<double>& dual
    );

    [[nodiscard]] CandidateMetrics evaluate(const CandidateIterate& candidate) {
        return evaluate(candidate.primal, candidate.dual);
    }

    [[nodiscard]] bool isOptimal(const CandidateMetrics& metrics) const noexcept;

private:
    struct alignas(64) RowReduction {
        double violationSquared = 0.0;
        double support = 0.0;
        double dualInfeasibilitySquared = 0.0;
    };

    struct alignas(64) ColumnReduction {
        double objective = 0.0;
        double infimum = 0.0;
        double infeasibilitySquared = 0.0;
    };

    void rowPass(const double* primal, const double* dual, int begin, int end, RowReduction& out) const noexcept;
    void columnPass(const double* primal, const double* dual, int begin, int end, ColumnReduction& out) const noexcept;

    PdlpOptions options_;
    const CompiledLp& problem_;
    Executor* executor_ = nullptr;
    const SpmvPlan* plan_ = nullptr;
    int parts_ = 1;

    // Problem-invariant normalisers, computed once instead of on every check.
    double primalNormaliser_ = 1.0;
    double dualNormaliser_ = 1.0;

    std::vector<RowReduction> rowReductions_;
    std::vector<ColumnReduction> columnReductions_;
};

}  // namespace pdlp
