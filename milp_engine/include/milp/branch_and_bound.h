#pragma once

#include "milp/basis_state.h"
#include "milp/dual_simplex_solver.h"
#include "model/model.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace milp {

struct MilpOptions {
    double timeLimitSeconds = 0.0; // <= 0 means no time limit
    std::int64_t nodeLimit = 0;    // <= 0 means no node limit
    int threadCount = 0;           // 0 selects std::thread::hardware_concurrency()

    double integralityTolerance = 1e-5;

    // Root-node Gomory Mixed-Integer cutting: at most this many
    // generate-cuts-then-resolve passes before branching begins, stopping
    // earlier if a pass generates no cuts or fails to meaningfully improve
    // the bound. 0 disables root cutting entirely.
    int maxRootCutPasses = 5;
};

enum class MilpStatus {
    Optimal,
    Infeasible,
    Unbounded,
    NodeLimit,
    TimeLimit
};

struct MilpResult {
    MilpStatus status = MilpStatus::NodeLimit;

    double objectiveValue = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> primal; // best known solution, size = model.variables.size() once one is found

    std::int64_t nodeCount = 0;
};

// One node in the branch-and-bound tree.
//
// lowerBounds/upperBounds hold this node's FULL current bound arrays (size =
// model.variables.size()), not a delta from the parent -- so processing a
// node needs no ancestor chain, at the cost of duplicating two
// variable-count-sized vectors per queued node. initialBasis is the parent's
// solved basis, reused to warm-start this node's LP via
// DualSimplexSolver::solveFromBasis.
//
// branchedVariable/branchFraction/isDownChild record enough about how this
// node was created to feed PseudoCostTracker::update() once THIS node's own
// LP is solved: the down and up children of a branch are independent queue
// entries that may be popped and solved by different worker threads at
// different times, so no single call site ever has both results in hand at
// once -- each child instead reports its own observation (against
// parentObjective) when it is processed. branchedVariable == -1 (the root
// node's value) means "no branch produced this node, nothing to report".
struct Node {
    std::vector<double> lowerBounds;
    std::vector<double> upperBounds;
    BasisState initialBasis;
    double parentObjective = 0.0;
    int depth = 0;

    int branchedVariable = -1;
    double branchFraction = 0.0;
    bool isDownChild = false;
};

// A multi-threaded branch-and-cut driver over DualSimplexSolver.
//
// Before the tree search begins, solve() runs a root-node Gomory
// Mixed-Integer cutting loop (see GomoryCutGenerator in gomory_cuts.h):
// generate cuts from the current root relaxation, append them as new rows,
// re-solve via DualSimplexSolver::solveFromBasis (extending the prior
// basis with the new rows' slacks starting Basic, so the dual simplex only
// has to repair whatever infeasibility the cuts introduced rather than
// re-solving from scratch), and repeat until a pass adds no cuts, a pass
// fails to meaningfully improve the objective, or options.maxRootCutPasses
// is reached. The resulting cut-augmented model and basis become the root
// of the B&B tree; every subsequent node inherits the cuts as ordinary rows
// (a valid cut, by construction, excludes no integer-feasible point, so it
// stays valid at every node, not just the root).
//
// Branching variable selection uses pseudo-cost branching (see
// PseudoCostTracker in branching_rules.h) once a variable has been branched
// on in both directions at least once; every candidate without that history
// yet falls back to the most-fractional rule.
//
// Threading model: `options.threadCount` worker loops run as long-lived
// tasks submitted to a util::ThreadPool (one per pool thread, not one task
// per node -- the pool supplies thread lifecycle management, while a single
// mutex-protected LIFO stack of Node distributes work dynamically across
// them). A worker pops a node, solves it via solveFromBasis without holding
// the lock, and re-locks only to push any children and update the shared
// incumbent. The search terminates when the stack is empty AND no worker is
// currently processing a node -- the only source of new nodes is a worker
// mid-process, so that condition is an unambiguous, race-free "no more work
// will ever appear" signal.
//
// Known limitations, documented rather than silently omitted: an Unbounded
// root LP relaxation is reported as MilpStatus::Unbounded immediately,
// without the (more rigorous, and out of scope here) check for whether
// integrality alone could still bound the MILP; a node whose LP solve throws
// (e.g. an unexpected singular basis) is conservatively discarded rather
// than aborting the whole search; node/time limits are checked once per
// popped node, so a run can overshoot a limit by up to one in-flight node
// per worker thread.
class BranchAndBoundSolver {
public:
    [[nodiscard]] MilpResult solve(
        const model::Model &model,
        const MilpOptions &options = {}) const;
};

} // namespace milp
