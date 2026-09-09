#pragma once

#include "milp/milp_types.h"
#include "model/model.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace milp {

struct PseudoCostStats {
    double psiDownSum = 0.0;
    std::int64_t psiDownCount = 0;
    double psiUpSum = 0.0;
    std::int64_t psiUpCount = 0;
};

// Thread-safe per-variable pseudo-cost history for branching variable
// selection. A single mutex guards the whole table -- updates happen once
// per processed branching node, not per LP pivot, so contention at this call
// frequency is not a concern.
//
// This engine's branch-and-bound evaluates each child node independently
// (see branch_and_bound.cpp's SharedState/workerLoop): the down and up
// children of a branch are pushed onto a shared work queue and may be popped
// and solved by different worker threads at different times, so no single
// call site ever has both children's results in hand simultaneously. update()
// therefore accepts a negative value for whichever side has no observation
// yet -- a real (non-negative) degradation only updates its own side, so a
// single API matching this task's requested signature still works correctly
// under that deferred, distributed evaluation model.
class PseudoCostTracker {
public:
    explicit PseudoCostTracker(std::size_t variableCount);

    // Records one observation. `changeDown`/`changeUp` are the (non-negative)
    // objective degradation observed in the down/up child relative to the
    // parent; pass a negative value for whichever side has no observation in
    // this call (see class comment). `frac` is the branching variable's
    // fractional part in the parent -- the amount each child's bound moved
    // by, used to normalise the observed change into a per-unit pseudo-cost.
    // A side is also skipped if its corresponding fractional span (frac for
    // down, 1-frac for up) is non-positive, guarding the division.
    void update(int variableIndex, double changeDown, double changeUp, double frac);

    // True only once at least one observation has been recorded on BOTH the
    // down and up side for this variable -- the "sufficient observation
    // history" gate selectBranchingVariable() uses to decide whether a
    // pseudo-cost score is trustworthy yet.
    [[nodiscard]] bool hasHistory(int variableIndex) const;

    [[nodiscard]] PseudoCostStats stats(int variableIndex) const;

    // S_j = min(D-, D+) + mu * max(D-, D+), the standard pseudo-cost
    // branching score (mu = 0.16, per this task's spec). D- = Psi_j^- *
    // (val - floor(val)), D+ = Psi_j^+ * (ceil(val) - val), using the
    // AVERAGE per-unit pseudo-costs accumulated so far (sum / count on each
    // side). Returns 0 if either side has no history yet rather than
    // dividing by a zero count -- callers should check hasHistory() first to
    // decide whether the score is meaningful at all.
    [[nodiscard]] double score(int variableIndex, double value) const;

    // Picks a branching variable among every fractional Integer/Binary
    // column in `primal`: the highest pseudo-cost score among variables with
    // history on both sides, or -- if none have full history yet -- the
    // most-fractional variable (the rule this engine used before pseudo-cost
    // branching existed). Returns -1 if `primal` is already integer-feasible
    // (no fractional candidate at all).
    [[nodiscard]] int selectBranchingVariable(
        const model::Model &model,
        const std::vector<double> &primal,
        double integralityTolerance) const;

private:
    mutable std::mutex mutex_;
    std::vector<PseudoCostStats> stats_;
};

} // namespace milp
