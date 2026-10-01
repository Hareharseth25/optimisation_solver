#include "report_view.h"

#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace cli {
namespace {

// Formatted through a private stream: the dashboard leaves std::fixed set on
// `out`, and residuals such as 1e-12 must not print as 0.0000.
std::string real(double value) {
    std::ostringstream text;
    text << std::setprecision(6) << value;
    return text.str();
}

std::string seconds(double value) {
    if (value < 0.0) return "not run";
    std::ostringstream text;
    text << std::fixed << std::setprecision(6) << value << " s";
    return text.str();
}

std::string plural(std::int64_t count, const char* singular, const char* pluralForm) {
    return formatNumber(count) + " " + (count == 1 ? singular : pluralForm);
}

void line(std::ostream& out, const TerminalStyle& s, const std::string& label,
          const std::string& value) {
    out << "  " << s.dim() << std::left << std::setw(18) << label << s.reset() << value << "\n";
}

void section(std::ostream& out, const TerminalStyle& s, const std::string& title) {
    out << "\n" << s.bold() << title << s.reset() << "\n";
}

std::string engineOrNone(solver::Engine engine) {
    return engine == solver::Engine::Unsupported ? "none" : solver::toString(engine);
}

std::string validationLine(const solver::ValidationSummary& check) {
    if (!check.passed) {
        return std::string("failed (") + postsolve::toString(check.status) + ")" +
               (check.failure.empty() ? "" : ": " + check.failure);
    }
    return "passed · max bound violation " + real(check.maxBoundResidual) +
           " · max row violation " + real(check.maxConstraintResidual);
}

}  // namespace

std::string describeClassification(const solver::SolveReport& report) {
    if (!report.classification) return {};
    const solver::Classification& c = *report.classification;
    std::string text = solver::toString(c.problemClass);
    if (c.hints.numBinary > 0) text += " · " + formatNumber(c.hints.numBinary) + " binary";
    if (c.hints.numInteger > 0) text += " · " + formatNumber(c.hints.numInteger) + " integer";
    if (c.hints.numContinuous > 0) text += " · " + formatNumber(c.hints.numContinuous) + " continuous";
    return text;
}

void printReportSummary(std::ostream& out, const solver::SolveReport& report,
                        const TerminalStyle& s) {
    // The dashboard already names the engine; the reason is what explains it.
    if (report.dispatch.dispatcherInvoked && !report.dispatch.reason.empty()) {
        out << "  " << s.dim() << "Dispatch        " << s.reset() << report.dispatch.reason << "\n";
    }
    if (report.postsolve) {
        out << "  " << s.dim() << "Validation      " << s.reset()
            << validationLine(*report.postsolve) << "\n";
    }
}

void printReportDetails(std::ostream& out, const model::Model& model,
                        const solver::SolveResult& result,
                        const solver::SolveReport& report, const TerminalStyle& s) {
    section(out, s, "MODEL");
    if (report.classification) {
        const solver::StructureHints& h = report.classification->hints;
        line(out, s, "Class", solver::toString(report.classification->problemClass));
        line(out, s, "Variables", formatNumber(static_cast<std::int64_t>(h.numColumns)) + "  (" +
             formatNumber(h.numContinuous) + " continuous, " + formatNumber(h.numInteger) +
             " integer, " + formatNumber(h.numBinary) + " binary)");
        line(out, s, "Constraints", formatNumber(static_cast<std::int64_t>(h.numRows)));
        line(out, s, "Nonzeros", formatNumber(h.numNonzeros));
        if (h.numNonzeros > 0) line(out, s, "Coef. range ratio", real(h.coefRangeRatio));
        std::vector<std::string> structure;
        if (h.hasNetworkStructure) structure.push_back("network");
        if (h.hasBigM) structure.push_back("big-M (max " + real(h.maxBigM) + ")");
        if (h.hasSetPartitioning) structure.push_back("set partitioning");
        if (h.symmetricGroups > 0) structure.push_back(plural(h.symmetricGroups, "symmetric group", "symmetric groups"));
        if (!structure.empty()) {
            std::string joined;
            for (const auto& item : structure) joined += (joined.empty() ? "" : ", ") + item;
            line(out, s, "Structure", joined);
        }
    } else {
        line(out, s, "Classification", "not run");
    }
    line(out, s, "Objective",
         std::string(model.objective.sense == model::ObjectiveSense::Maximize ? "maximize" : "minimize") +
         " · " + plural(static_cast<std::int64_t>(model.objective.linearTerms.size()), "linear term", "linear terms") +
         " · " + plural(static_cast<std::int64_t>(model.objective.quadraticTerms.size()), "quadratic term", "quadratic terms"));

    section(out, s, "PRESOLVE");
    if (report.presolve) {
        const solver::PresolveSummary& p = *report.presolve;
        const auto arrow = [](std::int64_t from, std::int64_t to) {
            return formatNumber(from) + " → " + formatNumber(to);
        };
        line(out, s, "Variables", arrow(static_cast<std::int64_t>(p.originalVariables),
                                        static_cast<std::int64_t>(p.reducedVariables)));
        line(out, s, "Constraints", arrow(static_cast<std::int64_t>(p.originalConstraints),
                                          static_cast<std::int64_t>(p.reducedConstraints)));
        line(out, s, "Nonzeros", arrow(p.originalNonzeros, p.reducedNonzeros));
        line(out, s, "State", p.infeasible ? "proved infeasible"
                              : p.converged ? "converged" : "stopped at pass limit (not converged)");
        const auto& t = p.transformationsByType;
        std::string kinds;
        const auto add = [&](std::size_t count, const char* name) {
            if (count == 0) return;
            kinds += (kinds.empty() ? "" : ", ") + formatNumber(static_cast<std::int64_t>(count)) + " " + name;
        };
        add(t.fixVariable, "fix variable");
        add(t.removeVariable, "remove variable");
        add(t.removeConstraint, "remove constraint");
        add(t.substituteVariable, "substitute variable");
        add(t.tightenLowerBound, "tighten lower bound");
        add(t.tightenUpperBound, "tighten upper bound");
        line(out, s, "Transformations", formatNumber(static_cast<std::int64_t>(p.transformationCount)) +
             (kinds.empty() ? "" : "  (" + kinds + ")"));
    } else {
        line(out, s, "Presolve", "not run");
    }

    section(out, s, "DISPATCH");
    line(out, s, "Dispatcher", report.dispatch.dispatcherInvoked ? "invoked" : "not invoked");
    line(out, s, report.dispatch.dispatcherInvoked ? "Selected" : "Outcome",
         solver::toString(report.dispatch.engine));
    if (!report.dispatch.reason.empty()) line(out, s, "Reason", report.dispatch.reason);
    line(out, s, "Executed", engineOrNone(report.dispatch.executedEngine));

    section(out, s, "EXECUTION");
    // Same rule as the JSON compute_backend record: a refusal before or during
    // engine setup is not a CPU execution.
    const bool engineRan = result.executedEngine != solver::Engine::Unsupported &&
                           result.status != solver::SolveStatus::InvalidModel;
    if (engineRan) {
        line(out, s, "Backend", std::string(solver::toString(result.executedBackend)) +
             (result.backendReason.empty() ? "" : "  (" + result.backendReason + ")"));
    } else {
        line(out, s, "Backend", "none (no engine ran)");
    }
    if (result.iterations > 0) line(out, s, "Iterations", formatNumber(result.iterations));
    if (result.nodeCount > 0) line(out, s, "Nodes", formatNumber(result.nodeCount));
    const solver::StageTimings& t = report.stageSeconds;
    line(out, s, "Validation", seconds(t.validation));
    line(out, s, "Classification", seconds(t.classification));
    line(out, s, "Presolve", seconds(t.presolve));
    line(out, s, "Dispatch", seconds(t.dispatch));
    line(out, s, "Engine", seconds(t.engine));
    line(out, s, "Reduced check", seconds(t.reducedValidation));
    line(out, s, "Postsolve", seconds(t.postsolve));
    line(out, s, "Total", seconds(t.total));

    section(out, s, "VALIDATION");
    if (report.reducedValidation) {
        const auto& check = *report.reducedValidation;
        line(out, s, "Reduced space", validationLine(check));
        if (check.passed) {
            line(out, s, "", "objective " + real(check.objectiveValue) +
                 " (engine reported " + real(check.engineReportedObjective) + ")");
        }
    } else {
        line(out, s, "Reduced space", "not run");
    }
    if (report.postsolve) {
        const auto& check = *report.postsolve;
        line(out, s, "Original space", validationLine(check));
        if (check.passed) {
            line(out, s, "", "scaled bound " + real(check.maxBoundResidualScaled) +
                 " · scaled row " + real(check.maxConstraintResidualScaled) +
                 " · objective " + real(check.objectiveValue));
        }
    } else {
        line(out, s, "Original space", "not run");
    }
    const bool integerModel = report.classification &&
        (report.classification->hints.numBinary + report.classification->hints.numInteger) > 0;
    if (integerModel && result.hasPrimal) {
        line(out, s, "Integrality", "max violation " + real(result.maxIntegralityViolation) +
             (result.integralityRespected ? "" : "  (relaxation only)"));
    }
    if (result.hasDuals) {
        line(out, s, "Duals", "max dual residual " + real(result.maxDualResidual));
    } else if (report.postsolve && report.postsolve->dualsRequested) {
        line(out, s, "Duals", "unavailable: " + result.dualsUnavailableReason);
    }
    out << std::right;
}

}  // namespace cli
