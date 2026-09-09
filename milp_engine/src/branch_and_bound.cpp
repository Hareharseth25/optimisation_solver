#include "milp/branch_and_bound.h"

#include "milp/branching_rules.h"
#include "milp/gomory_cuts.h"
#include "milp/heuristics.h"
#include "util/parallel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <future>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace milp {
namespace {

using Clock = std::chrono::steady_clock;

bool isImprovement(double candidate, double incumbent, bool maximize, double tolerance) {
    return maximize ? candidate > incumbent + tolerance : candidate < incumbent - tolerance;
}

// State shared across every worker thread for one BranchAndBoundSolver::solve
// call. The node stack and incumbent are protected by `mutex`; `busyCount`
// (also under `mutex`) is what makes "stack empty" an unambiguous
// termination signal -- see the class-level comment in branch_and_bound.h.
struct SharedState {
    explicit SharedState(std::size_t variableCount) : pseudoCosts(variableCount) {}

    std::mutex mutex;
    std::condition_variable cv;

    std::vector<Node> activeNodes;
    int busyCount = 0;

    bool hasIncumbent = false;
    double incumbentObjective = 0.0;
    std::vector<double> incumbentSolution;

    // Thread-safe on its own (see PseudoCostTracker), so it needs no
    // additional protection from `mutex`.
    PseudoCostTracker pseudoCosts;

    std::atomic<std::int64_t> nodeCount{0};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> nodeLimitHit{false};
    std::atomic<bool> timeLimitHit{false};
};

void processNode(
    SharedState &shared,
    const model::Model &model,
    const MilpOptions &options,
    const DualSimplexSolver &solver,
    const DualSimplexOptions &lpOptions,
    bool maximize,
    Node node)
{
    const std::size_t n = model.variables.size();

    model::Model nodeModel = model;
    for (std::size_t j = 0; j < n; ++j) {
        nodeModel.variables[j].lowerBound = node.lowerBounds[j];
        nodeModel.variables[j].upperBound = node.upperBounds[j];
    }

    const DualSimplexResult lpResult = solver.solveFromBasis(nodeModel, node.initialBasis, lpOptions);

    if (lpResult.status != DualSimplexStatus::Optimal) {
        return; // Infeasible / Unbounded / IterationLimit: discard, don't branch.
    }

    // Report this node's own half of its parent's branching observation
    // (see the Node/PseudoCostTracker doc comments for why this can't
    // happen in one combined call: the sibling may be solved by a
    // different worker at a different time, or not yet at all).
    // branchedVariable == -1 marks the root, which has no parent branch to
    // report against.
    if (node.branchedVariable >= 0) {
        double degradation = maximize
            ? (node.parentObjective - lpResult.objectiveValue)
            : (lpResult.objectiveValue - node.parentObjective);
        degradation = std::max(degradation, 0.0);

        if (node.isDownChild) {
            shared.pseudoCosts.update(node.branchedVariable, degradation, -1.0, node.branchFraction);
        } else {
            shared.pseudoCosts.update(node.branchedVariable, -1.0, degradation, node.branchFraction);
        }
    }

    // Bound pruning: this relaxation's objective is a bound on anything
    // achievable in this subtree, so if it can't beat the incumbent, no
    // integer-feasible descendant can either.
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        if (shared.hasIncumbent &&
            !isImprovement(lpResult.objectiveValue, shared.incumbentObjective, maximize,
                           options.integralityTolerance)) {
            return;
        }
    }

    // Branching variable selection: pseudo-cost scoring for any candidate
    // with history on both sides, falling back to the most-fractional rule
    // otherwise (see PseudoCostTracker::selectBranchingVariable).
    const int branchVariable =
        shared.pseudoCosts.selectBranchingVariable(model, lpResult.primal, options.integralityTolerance);

    if (branchVariable < 0) {
        // Integer feasible: a candidate incumbent.
        std::lock_guard<std::mutex> lock(shared.mutex);
        if (!shared.hasIncumbent ||
            isImprovement(lpResult.objectiveValue, shared.incumbentObjective, maximize,
                          options.integralityTolerance)) {
            shared.hasIncumbent = true;
            shared.incumbentObjective = lpResult.objectiveValue;
            shared.incumbentSolution = lpResult.primal;
        }
        return;
    }

    const auto branchIndex = static_cast<std::size_t>(branchVariable);
    const double branchValue = lpResult.primal[branchIndex];
    const double branchFraction = branchValue - std::floor(branchValue);

    Node floorChild;
    floorChild.lowerBounds = node.lowerBounds;
    floorChild.upperBounds = node.upperBounds;
    floorChild.upperBounds[branchIndex] = std::floor(branchValue);
    floorChild.initialBasis = lpResult.basis.clone();
    floorChild.parentObjective = lpResult.objectiveValue;
    floorChild.depth = node.depth + 1;
    floorChild.branchedVariable = branchVariable;
    floorChild.branchFraction = branchFraction;
    floorChild.isDownChild = true;

    Node ceilChild;
    ceilChild.lowerBounds = node.lowerBounds;
    ceilChild.upperBounds = node.upperBounds;
    ceilChild.lowerBounds[branchIndex] = std::ceil(branchValue);
    ceilChild.initialBasis = lpResult.basis.clone();
    ceilChild.parentObjective = lpResult.objectiveValue;
    ceilChild.depth = node.depth + 1;
    ceilChild.branchedVariable = branchVariable;
    ceilChild.branchFraction = branchFraction;
    ceilChild.isDownChild = false;

    // A child is only ever infeasible-by-construction here if the branch
    // variable's own bounds were already fractional (unusual for an integer
    // variable, but not disallowed), e.g. floor(branchValue) landing below
    // an already-fractional lower bound. Skip pushing it explicitly rather
    // than relying on model::Model::validate() rejecting it downstream.
    const bool floorFeasible = floorChild.lowerBounds[branchIndex] <= floorChild.upperBounds[branchIndex];
    const bool ceilFeasible = ceilChild.lowerBounds[branchIndex] <= ceilChild.upperBounds[branchIndex];

    std::lock_guard<std::mutex> lock(shared.mutex);
    if (floorFeasible) {
        shared.activeNodes.push_back(std::move(floorChild));
    }
    if (ceilFeasible) {
        shared.activeNodes.push_back(std::move(ceilChild));
    }
}

void workerLoop(
    SharedState &shared,
    const model::Model &model,
    const MilpOptions &options,
    const DualSimplexSolver &solver,
    const DualSimplexOptions &lpOptions,
    bool maximize,
    Clock::time_point startTime)
{
    for (;;) {
        Node node;
        {
            std::unique_lock<std::mutex> lock(shared.mutex);
            shared.cv.wait(lock, [&shared] {
                return shared.stopRequested.load() || !shared.activeNodes.empty() || shared.busyCount == 0;
            });

            if (shared.stopRequested.load()) {
                return;
            }
            if (shared.activeNodes.empty()) {
                // The wait predicate guarantees busyCount == 0 here: no
                // worker is mid-process, so none can ever push more work.
                shared.stopRequested.store(true);
                shared.cv.notify_all();
                return;
            }

            node = std::move(shared.activeNodes.back());
            shared.activeNodes.pop_back();
            ++shared.busyCount;
        }

        const std::int64_t currentNodeCount = ++shared.nodeCount;
        bool limitHit = false;

        if (options.nodeLimit > 0 && currentNodeCount > options.nodeLimit) {
            shared.nodeLimitHit.store(true);
            limitHit = true;
        }
        if (!limitHit && options.timeLimitSeconds > 0.0) {
            const double elapsed = std::chrono::duration<double>(Clock::now() - startTime).count();
            if (elapsed >= options.timeLimitSeconds) {
                shared.timeLimitHit.store(true);
                limitHit = true;
            }
        }

        if (limitHit) {
            shared.stopRequested.store(true);
        } else {
            try {
                processNode(shared, model, options, solver, lpOptions, maximize, std::move(node));
            } catch (const std::exception &) {
                // Conservatively discard a node whose LP solve failed
                // unexpectedly (e.g. an unforeseen singular basis after this
                // node's bound changes) rather than aborting the search.
            }
        }

        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            --shared.busyCount;
        }
        shared.cv.notify_all();
    }
}

// Appends `cuts` as new rows (lowerBound = cut.rhs, upperBound = +inf,
// matching the fixed coeffs.x >= rhs sense every GMI cut is produced in) to
// `model`, extends `basis` with one new Basic logical per new row (the
// classic "new slack starts basic" convention -- the row is known violated
// by the current point, so this alone is what triggers the dual simplex
// repair on the next solve), and returns the updated (model, basis) pair.
// Cut row names are suffixed by `startingIndex` (a running counter owned by
// the caller) to satisfy model::Model::validate()'s uniqueness requirement
// across passes.
std::pair<model::Model, BasisState> appendCuts(
    const model::Model &model,
    const BasisState &basis,
    const std::vector<Cut> &cuts,
    int startingIndex)
{
    model::Model augmented = model;
    BasisState extendedBasis = basis.clone();

    for (std::size_t i = 0; i < cuts.size(); ++i) {
        model::Constraint row;
        row.name = "gmi_cut_" + std::to_string(startingIndex + static_cast<int>(i));
        row.lowerBound = cuts[i].rhs;
        row.upperBound = std::numeric_limits<double>::infinity();
        for (std::size_t j = 0; j < cuts[i].coeffs.size(); ++j) {
            if (cuts[i].coeffs[j] != 0.0) {
                row.linearTerms.push_back({static_cast<int>(j), cuts[i].coeffs[j]});
            }
        }
        augmented.constraints.push_back(std::move(row));
        extendedBasis.constraintStatus.push_back(BasisStatus::Basic);
    }

    return {std::move(augmented), std::move(extendedBasis)};
}

// Runs the root-node GMI cutting loop, returning the (possibly) cut-augmented
// model and its freshly re-solved result. `rootModel`/`rootResult` are left
// unmodified if no pass ever successfully commits (e.g. maxRootCutPasses <=
// 0, the first pass generates nothing, or a re-solve after cutting fails to
// reach Optimal).
std::pair<model::Model, DualSimplexResult> runRootCuttingLoop(
    const model::Model &rootModel,
    const DualSimplexResult &rootResult,
    const MilpOptions &options,
    const DualSimplexSolver &solver,
    const DualSimplexOptions &lpOptions)
{
    GomoryCutGenerator cutGenerator;

    model::Model workingModel = rootModel;
    DualSimplexResult workingResult = rootResult;
    int cutCounter = 0;

    for (int pass = 0; pass < options.maxRootCutPasses; ++pass) {
        const std::vector<Cut> cuts = cutGenerator.generate(workingModel, workingResult);
        if (cuts.empty()) {
            break;
        }

        auto [candidateModel, candidateBasis] =
            appendCuts(workingModel, workingResult.basis, cuts, cutCounter);
        cutCounter += static_cast<int>(cuts.size());

        const DualSimplexResult candidateResult =
            solver.solveFromBasis(candidateModel, candidateBasis, lpOptions);
        if (candidateResult.status != DualSimplexStatus::Optimal) {
            // Don't commit a pass whose re-solve didn't converge cleanly;
            // keep searching from the last known-good state.
            break;
        }

        const bool maximize = rootModel.objective.sense == model::ObjectiveSense::Maximize;
        const bool meaningfulImprovement = maximize
            ? candidateResult.objectiveValue > workingResult.objectiveValue + options.integralityTolerance
            : candidateResult.objectiveValue < workingResult.objectiveValue - options.integralityTolerance;

        workingModel = std::move(candidateModel);
        workingResult = candidateResult;

        if (!meaningfulImprovement) {
            break;
        }
    }

    return {std::move(workingModel), std::move(workingResult)};
}

// Thread-safely adopts a heuristic's solution as the incumbent if it found
// one and it improves on whatever's currently there. Called from the
// single-threaded root-setup phase before any worker exists, but still goes
// through `shared.mutex` -- this is the exact "update the incumbent" path
// processNode uses mid-search, so there's only one place that logic lives,
// and it costs nothing to be correct-by-construction rather than correct
// only because nothing else happens to be running yet.
void tryAdoptHeuristic(
    SharedState &shared,
    const HeuristicResult &heuristicResult,
    bool maximize,
    double tolerance)
{
    if (!heuristicResult.foundFeasible) {
        return;
    }

    std::lock_guard<std::mutex> lock(shared.mutex);
    if (!shared.hasIncumbent ||
        isImprovement(heuristicResult.objective, shared.incumbentObjective, maximize, tolerance)) {
        shared.hasIncumbent = true;
        shared.incumbentObjective = heuristicResult.objective;
        shared.incumbentSolution = heuristicResult.solution;
    }
}

} // namespace

MilpResult BranchAndBoundSolver::solve(const model::Model &model, const MilpOptions &options) const {
    DualSimplexSolver solver;
    const DualSimplexOptions lpOptions{};

    const DualSimplexResult coldStartResult = solver.solve(model, lpOptions);

    MilpResult result;

    if (coldStartResult.status == DualSimplexStatus::Infeasible) {
        result.status = MilpStatus::Infeasible;
        return result;
    }
    if (coldStartResult.status == DualSimplexStatus::Unbounded) {
        result.status = MilpStatus::Unbounded;
        return result;
    }
    if (coldStartResult.status != DualSimplexStatus::Optimal) {
        // The root LP relaxation itself failed to converge -- extremely
        // unlikely for a well-posed LP. Report a truncated search rather
        // than an unearned Infeasible/Optimal claim.
        result.status = MilpStatus::NodeLimit;
        return result;
    }

    // Root-node GMI cutting: tighten the relaxation before any branching.
    // The resulting model (original rows + accepted cuts) and its basis
    // become the actual root the tree search grows from -- every subsequent
    // node's LP therefore already carries the cuts as ordinary rows.
    //
    // Unpacked into plain named variables rather than a structured binding:
    // `workingModel` is captured by reference in a lambda below, and
    // capturing a structured-binding name by reference is only reliably
    // standard-conformant from C++20 onward.
    std::pair<model::Model, DualSimplexResult> rootCutOutcome =
        runRootCuttingLoop(model, coldStartResult, options, solver, lpOptions);
    model::Model workingModel = std::move(rootCutOutcome.first);
    DualSimplexResult rootResult = std::move(rootCutOutcome.second);

    const bool maximize = workingModel.objective.sense == model::ObjectiveSense::Maximize;
    const std::size_t n = workingModel.variables.size();

    SharedState shared(n);

    // Root-node primal heuristics, run right after cutting and before any
    // node (including the root itself) is processed: a heuristic-seeded
    // incumbent makes bound pruning effective from the very first node
    // instead of only after the tree search stumbles onto its first integer
    // point on its own.
    {
        SimpleRoundingHeuristic roundingHeuristic;
        const HeuristicResult roundingResult = roundingHeuristic.run(workingModel, rootResult.primal);
        tryAdoptHeuristic(shared, roundingResult, maximize, options.integralityTolerance);

        FractionalDivingHeuristic divingHeuristic;
        const HeuristicResult divingResult = divingHeuristic.run(workingModel, rootResult);
        tryAdoptHeuristic(shared, divingResult, maximize, options.integralityTolerance);
    }

    Node root;
    root.lowerBounds.resize(n);
    root.upperBounds.resize(n);
    for (std::size_t j = 0; j < n; ++j) {
        root.lowerBounds[j] = workingModel.variables[j].lowerBound;
        root.upperBounds[j] = workingModel.variables[j].upperBound;
    }
    root.initialBasis = rootResult.basis.clone();
    root.parentObjective = rootResult.objectiveValue;
    root.depth = 0;
    shared.activeNodes.push_back(std::move(root));

    const Clock::time_point startTime = Clock::now();

    util::ThreadPool pool(options.threadCount > 0 ? static_cast<std::size_t>(options.threadCount) : 0);
    const std::size_t workerCount = pool.threadCount();

    std::vector<std::future<void>> futures;
    futures.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i) {
        futures.push_back(pool.submit([&shared, &workingModel, &options, &solver, &lpOptions, maximize, startTime] {
            workerLoop(shared, workingModel, options, solver, lpOptions, maximize, startTime);
        }));
    }
    for (auto &future : futures) {
        future.get();
    }

    result.nodeCount = shared.nodeCount.load();

    const bool truncated = shared.nodeLimitHit.load() || shared.timeLimitHit.load();
    const MilpStatus truncationStatus =
        shared.nodeLimitHit.load() ? MilpStatus::NodeLimit : MilpStatus::TimeLimit;

    if (shared.hasIncumbent) {
        result.objectiveValue = shared.incumbentObjective;
        result.primal = shared.incumbentSolution;
        result.status = truncated ? truncationStatus : MilpStatus::Optimal;
    } else {
        result.status = truncated ? truncationStatus : MilpStatus::Infeasible;
    }

    return result;
}

} // namespace milp
