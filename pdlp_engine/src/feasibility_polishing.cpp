#include "pdlp/feasibility_polishing.h"

#include "pdlp/iterate_average.h"
#include "pdlp/pdhg_kernel.h"
#include "pdlp/step_controller.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace pdlp {

PolishingResult FeasibilityPolisher::polish(
    const PdlpOptions& options,
    const PolishingInput& input,
    TerminationChecker& checker
) {
    PolishingResult result{input.originalStart, input.startMetrics, 0};
    if (!options.useFeasibilityPolishing || options.polishingIterations <= 0) {
        return result;
    }

    const CompiledLp& working = *input.working;
    const ProblemScaling* scaling = input.scaling;

    // This CPU v1 uses a conservative refinement phase: keep the original
    // objective, halve the step, average the refined iterates, and retain only
    // candidates with a better complete KKT score. A production feasibility
    // polishing implementation can replace this component without changing the
    // solver interface.
    StepParameters steps = input.steps;
    steps.globalStep *= 0.5;

    PdlpState state;
    state.primal = input.scaledStart.primal;
    state.dual = input.scaledStart.dual;

    CpuPdhgKernel kernel(working, input.executor, input.plan);
    kernel.refreshActivity(state);

    StepController stepController(options, steps.maximumSafeGlobalStep);
    stepController.setPrimalWeight(steps.primalWeight);
    IterateAverage average(working.numColumns(), working.numRows(), input.executor);

    std::vector<double> originalPrimal;
    std::vector<double> originalDual;
    const std::vector<double>* scoredPrimal = nullptr;
    const std::vector<double>* scoredDual = nullptr;

    // Scores a working-coordinate iterate against the original problem, leaving
    // the original-coordinate vectors addressable so an improvement can be kept
    // without a second conversion.
    const auto score = [&](const std::vector<double>& primal,
                           const std::vector<double>& dual) {
        if (scaling != nullptr) {
            scaling->toOriginal(primal, dual, originalPrimal, originalDual);
            scoredPrimal = &originalPrimal;
            scoredDual = &originalDual;
        } else {
            scoredPrimal = &primal;
            scoredDual = &dual;
        }
        return checker.evaluate(*scoredPrimal, *scoredDual);
    };

    const auto start = std::chrono::steady_clock::now();
    const int checkFrequency = std::max(options.terminationCheckFrequency, 1);

    for (int iteration = 0; iteration < options.polishingIterations; ++iteration) {
        bool committed = false;
        bool broken = false;
        for (int attempt = 0; attempt < std::max(options.maximumStepTrials, 1); ++attempt) {
            const KernelTrialResult trialResult =
                kernel.trial(state, *input.preconditioner, steps);
            ++result.stepTrials;
            if (!trialResult.finite) {
                broken = true;
                break;
            }
            if (!options.useAdaptiveLinesearch) {
                kernel.commit(state);
                committed = true;
                break;
            }
            const bool accept = stepController.evaluateTrial(
                trialResult.primalMovementWeighted,
                trialResult.dualMovementWeighted,
                trialResult.interaction,
                state.iteration
            );
            steps.globalStep = stepController.parameters().globalStep;
            if (accept) {
                kernel.commit(state);
                committed = true;
                break;
            }
        }
        if (broken || !committed) {
            break;
        }

        if (options.useAveraging) {
            average.add(state.primal, state.dual, steps.globalStep);
        }
        ++result.iterations;

        const bool lastIteration = iteration + 1 == options.polishingIterations;
        if ((iteration + 1) % checkFrequency != 0 && !lastIteration) {
            continue;
        }

        // Polishing respects the solve's remaining budget; it previously could
        // run its full iteration count after a time limit had already expired.
        if (input.remainingSeconds > 0.0) {
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= input.remainingSeconds) {
                break;
            }
        }

        const CandidateMetrics currentMetrics = score(state.primal, state.dual);
        if (currentMetrics.kktScore < result.metrics.kktScore) {
            result.candidate.primal = *scoredPrimal;
            result.candidate.dual = *scoredDual;
            result.metrics = currentMetrics;
        }

        if (options.useAveraging && !average.empty()) {
            const CandidateIterate& averaged = average.candidate();
            const CandidateMetrics averagedMetrics = score(averaged.primal, averaged.dual);
            if (averagedMetrics.kktScore < result.metrics.kktScore) {
                result.candidate.primal = *scoredPrimal;
                result.candidate.dual = *scoredDual;
                result.metrics = averagedMetrics;
            }
        }

        if (checker.isOptimal(result.metrics)) {
            break;
        }
    }

    return result;
}

}  // namespace pdlp
