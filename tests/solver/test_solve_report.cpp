// SolveReport: the optional observability record filled by solver::solve().
//
// Every check here compares the report against the SolveResult of the SAME
// call, or against an independent run of a deterministic stage (classify,
// Presolver::run) on the same input. Timing checks only assert sign and
// ordering, never wall-clock values.

#include "presolve/presolver.h"
#include "solver/orchestrator.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

const double INF = std::numeric_limits<double>::infinity();
int checks = 0;
int failures = 0;

void ck(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("    FAIL  %s\n", what.c_str());
    }
}

struct Builder {
    model::Model m;
    void var(const char* name, model::VariableType type, double lo, double hi) {
        model::Variable v;
        v.name = name; v.type = type; v.lowerBound = lo; v.upperBound = hi;
        m.variables.push_back(v);
    }
    void row(const char* name, double lo, double hi,
             std::vector<model::LinearTerm> terms) {
        model::Constraint c;
        c.name = name; c.lowerBound = lo; c.upperBound = hi;
        c.linearTerms = std::move(terms);
        m.constraints.push_back(c);
    }
    void obj(model::ObjectiveSense sense, std::vector<model::LinearTerm> terms) {
        m.objective.sense = sense;
        m.objective.linearTerms = std::move(terms);
    }
};

using model::VariableType;

// Product mix plus a variable presolve fixes (lb == ub), so the reduced model
// is strictly smaller than the original.
model::Model lpWithFixedVariable() {
    Builder b;
    b.var("x", VariableType::Continuous, 0, INF);
    b.var("y", VariableType::Continuous, 0, INF);
    b.var("fixed", VariableType::Continuous, 2, 2);
    b.row("labour",  -INF, 14.0, {{0, 2.0}, {1, 1.0}, {2, 1.0}});
    b.row("machine", -INF, 28.0, {{0, 4.0}, {1, 5.0}});
    b.row("material",-INF, 30.0, {{0, 2.0}, {1, 5.0}});
    b.obj(model::ObjectiveSense::Maximize, {{0, 3.0}, {1, 4.0}, {2, 1.0}});
    return b.m;
}

model::Model knapsackMilp() {
    Builder b;
    b.var("a", VariableType::Binary, 0, 1);
    b.var("b", VariableType::Binary, 0, 1);
    b.var("c", VariableType::Binary, 0, 1);
    b.row("cap", -INF, 5.0, {{0, 2.0}, {1, 3.0}, {2, 4.0}});
    b.obj(model::ObjectiveSense::Maximize, {{0, 3.0}, {1, 4.0}, {2, 5.0}});
    return b.m;
}

// min x^2 + y^2 - 2x - 4y  s.t. x + y <= 2: convex, routed to the QP engine.
model::Model convexQp() {
    Builder b;
    b.var("x", VariableType::Continuous, -10, 10);
    b.var("y", VariableType::Continuous, -10, 10);
    b.row("sum", -INF, 2.0, {{0, 1.0}, {1, 1.0}});
    b.obj(model::ObjectiveSense::Minimize, {{0, -2.0}, {1, -4.0}});
    b.m.objective.quadraticTerms = {{0, 0, 1.0}, {1, 1, 1.0}};
    return b.m;
}

// A singleton row demanding x >= 5 on x in [0, 1].
model::Model presolveInfeasible() {
    Builder b;
    b.var("x", VariableType::Continuous, 0, 1);
    b.var("y", VariableType::Continuous, 0, 1);
    b.row("impossible", 5.0, INF, {{0, 1.0}});
    b.row("other", -INF, 1.0, {{0, 1.0}, {1, 1.0}});
    b.obj(model::ObjectiveSense::Minimize, {{0, 1.0}, {1, 1.0}});
    return b.m;
}

bool ran(double seconds) { return seconds >= 0.0 && std::isfinite(seconds); }
bool skipped(double seconds) { return seconds < 0.0; }

// Report must not change the answer: same status, point, duals and provenance.
void checkSameResult(const solver::SolveResult& plain, const solver::SolveResult& reported,
                     const std::string& label) {
    ck(plain.status == reported.status, label + ": status unchanged by report");
    ck(plain.engine == reported.engine, label + ": engine unchanged by report");
    ck(plain.engineReason == reported.engineReason, label + ": reason unchanged by report");
    ck(plain.executedEngine == reported.executedEngine, label + ": executed engine unchanged");
    ck(plain.hasPrimal == reported.hasPrimal, label + ": hasPrimal unchanged");
    ck(plain.hasDuals == reported.hasDuals, label + ": hasDuals unchanged");
    ck(plain.variableValues == reported.variableValues, label + ": primal unchanged");
    ck(plain.constraintDuals == reported.constraintDuals, label + ": duals unchanged");
    ck(plain.objectiveValue == reported.objectiveValue ||
       (std::isnan(plain.objectiveValue) && std::isnan(reported.objectiveValue)),
       label + ": objective unchanged");
    ck(plain.reducedVariableCount == reported.reducedVariableCount &&
       plain.reducedConstraintCount == reported.reducedConstraintCount,
       label + ": reduced counts unchanged");
}

// Checks shared by every solve that reached an engine through solve().
void checkFullPipelineReport(const model::Model& m, const solver::SolveResult& r,
                             const solver::SolveReport& rep, const std::string& label) {
    // Classification is the real one: identical to classifying the input.
    const solver::Classification expected = solver::classify(m);
    ck(rep.classification.has_value(), label + ": classification present");
    if (rep.classification) {
        const auto& got = *rep.classification;
        ck(got.problemClass == expected.problemClass, label + ": problem class matches");
        ck(got.hints.numRows == m.constraints.size(), label + ": row count matches model");
        ck(got.hints.numColumns == m.variables.size(), label + ": column count matches model");
        ck(got.hints.numBinary == expected.hints.numBinary &&
           got.hints.numInteger == expected.hints.numInteger &&
           got.hints.numContinuous == expected.hints.numContinuous,
           label + ": variable type counts match");
        ck(got.hints.numNonzeros == expected.hints.numNonzeros, label + ": nonzeros match");
    }

    // Presolve counts match a fresh (deterministic) presolve of the same model.
    const presolve::PresolveResult independent = presolve::Presolver{}.run(m);
    ck(rep.presolve.has_value(), label + ": presolve summary present");
    if (rep.presolve) {
        const auto& p = *rep.presolve;
        ck(p.originalVariables == m.variables.size(), label + ": presolve original vars");
        ck(p.originalConstraints == m.constraints.size(), label + ": presolve original rows");
        ck(p.reducedVariables == independent.presolvedVariables, label + ": presolve reduced vars");
        ck(p.reducedConstraints == independent.presolvedConstraints, label + ": presolve reduced rows");
        ck(p.reducedVariables == r.reducedVariableCount &&
           p.reducedConstraints == r.reducedConstraintCount,
           label + ": presolve counts agree with SolveResult");
        ck(p.infeasible == independent.infeasible && p.converged == independent.converged,
           label + ": presolve flags match");
        ck(p.transformationCount == independent.transformations.size(),
           label + ": transformation count matches");
        const auto& t = p.transformationsByType;
        ck(t.removeVariable + t.removeConstraint + t.fixVariable + t.substituteVariable +
           t.tightenLowerBound + t.tightenUpperBound == p.transformationCount,
           label + ": per-type counts sum to the total");
        ck(p.originalNonzeros == solver::countNonzeros(m), label + ": original nonzeros");
        ck(p.reducedNonzeros == solver::countNonzeros(independent.model), label + ": reduced nonzeros");
    }

    // Dispatch is copied from the same run, never recomputed.
    ck(rep.dispatch.dispatcherInvoked, label + ": dispatcher invoked");
    ck(rep.dispatch.engine == r.engine, label + ": report engine == SolveResult::engine");
    ck(rep.dispatch.reason == r.engineReason, label + ": report reason == engineReason");
    ck(rep.dispatch.executedEngine == r.executedEngine,
       label + ": report executed engine == SolveResult::executedEngine");
    ck(!rep.dispatch.reason.empty(), label + ": dispatch reason is non-empty");

    // Validation in both spaces passed, with real residuals and the objective.
    ck(rep.reducedValidation.has_value() && rep.reducedValidation->passed,
       label + ": reduced-space validation passed");
    ck(rep.postsolve.has_value() && rep.postsolve->passed, label + ": postsolve passed");
    if (rep.postsolve && rep.postsolve->passed) {
        const auto& v = *rep.postsolve;
        ck(v.status == postsolve::PostsolveStatus::Success, label + ": postsolve status Success");
        ck(v.failure.empty(), label + ": no postsolve failure message");
        ck(std::isfinite(v.maxBoundResidual) && v.maxBoundResidual >= 0.0 &&
           std::isfinite(v.maxConstraintResidual) && v.maxConstraintResidual >= 0.0,
           label + ": postsolve residuals measured");
        ck(v.objectiveValue == r.objectiveValue, label + ": postsolve objective is the result's");
    }
    if (rep.reducedValidation && rep.reducedValidation->passed) {
        ck(std::isfinite(rep.reducedValidation->maxConstraintResidual),
           label + ": reduced residuals measured");
    }

    // Timings: every stage that ran is non-negative, stages are disjoint and
    // total is exactly the existing solveSeconds.
    const auto& s = rep.stageSeconds;
    ck(ran(s.validation) && ran(s.classification) && ran(s.presolve) && ran(s.dispatch) &&
       ran(s.engine) && ran(s.reducedValidation) && ran(s.postsolve) && ran(s.total),
       label + ": every stage timed");
    ck(s.total == r.solveSeconds, label + ": total == SolveResult::solveSeconds");
    const double sum = s.validation + s.classification + s.presolve + s.dispatch +
                       s.engine + s.reducedValidation + s.postsolve;
    ck(sum <= s.total + 1e-9, label + ": stage times do not exceed total");
}

void testLp() {
    const model::Model m = lpWithFixedVariable();
    const solver::SolveResult plain = solver::solve(m);
    const solver::SolveResult plainWithOptions = solver::solve(m, solver::SolverOptions{});
    solver::SolveReport rep;
    const solver::SolveResult r = solver::solve(m, {}, &rep);

    ck(r.status == solver::SolveStatus::Optimal, "LP solves to optimality");
    checkSameResult(plain, r, "LP");
    checkSameResult(plainWithOptions, r, "LP(options)");
    checkFullPipelineReport(m, r, rep, "LP");
    ck(rep.classification && rep.classification->problemClass == solver::ProblemClass::LP,
       "LP classified as LP");
    ck(rep.presolve && rep.presolve->reducedVariables < rep.presolve->originalVariables,
       "LP: presolve removed the fixed variable");
    ck(rep.presolve && rep.presolve->transformationsByType.fixVariable >= 1,
       "LP: a FixVariable transformation is counted");
    ck(rep.postsolve && rep.postsolve->dualsRequested, "LP: duals requested from postsolve");
}

void testMilp() {
    const model::Model m = knapsackMilp();
    const solver::SolveResult plain = solver::solve(m);
    solver::SolveReport rep;
    const solver::SolveResult r = solver::solve(m, {}, &rep);

    ck(r.status == solver::SolveStatus::Optimal, "MILP solves to optimality");
    ck(r.executedEngine == solver::Engine::BranchAndCut, "MILP runs branch-and-cut");
    checkSameResult(plain, r, "MILP");
    checkFullPipelineReport(m, r, rep, "MILP");
    ck(rep.classification && rep.classification->hints.numBinary == 3, "MILP: three binaries");
    ck(rep.postsolve && !rep.postsolve->dualsRequested, "MILP: no duals requested");
}

void testQp() {
    const model::Model m = convexQp();
    const solver::SolveResult plain = solver::solve(m);
    solver::SolveReport rep;
    const solver::SolveResult r = solver::solve(m, {}, &rep);

    ck(r.status == solver::SolveStatus::Optimal, "QP solves to optimality");
    ck(r.executedEngine == solver::Engine::Qp, "QP runs the QP engine");
    ck(plain.status == r.status && plain.executedEngine == r.executedEngine,
       "QP: outcome unchanged by report");
    checkFullPipelineReport(m, r, rep, "QP");
    ck(rep.classification && rep.classification->problemClass == solver::ProblemClass::QP,
       "QP classified as QP");
}

void testForcedEngine() {
    const model::Model m = lpWithFixedVariable();
    solver::SolverOptions options;
    options.forceEngine = solver::Engine::Pdlp;
    solver::SolveReport rep;
    const solver::SolveResult r = solver::solve(m, options, &rep);
    ck(rep.dispatch.dispatcherInvoked, "forced: dispatcher still decides");
    ck(rep.dispatch.engine == solver::Engine::Pdlp && r.engine == solver::Engine::Pdlp,
       "forced: PDLP recorded");
    ck(rep.dispatch.reason == r.engineReason, "forced: reason copied");
}

void testPresolveInfeasible() {
    const model::Model m = presolveInfeasible();
    ck(presolve::Presolver{}.run(m).infeasible, "fixture is infeasible by presolve");

    const solver::SolveResult plain = solver::solve(m);
    solver::SolveReport rep;
    const solver::SolveResult r = solver::solve(m, {}, &rep);
    checkSameResult(plain, r, "infeasible");

    ck(r.status == solver::SolveStatus::Infeasible, "infeasible: status");
    ck(rep.classification.has_value(), "infeasible: classification ran");
    ck(rep.presolve && rep.presolve->infeasible, "infeasible: presolve flag set");
    ck(!rep.dispatch.dispatcherInvoked, "infeasible: dispatcher never ran");
    ck(rep.dispatch.engine == solver::Engine::Infeasible && rep.dispatch.engine == r.engine,
       "infeasible: engine mirrors SolveResult");
    ck(rep.dispatch.executedEngine == solver::Engine::Unsupported, "infeasible: nothing executed");
    ck(rep.dispatch.reason == r.engineReason, "infeasible: reason mirrors SolveResult");
    ck(!rep.reducedValidation && !rep.postsolve, "infeasible: no validation sections");

    const auto& s = rep.stageSeconds;
    ck(ran(s.validation) && ran(s.classification) && ran(s.presolve), "infeasible: early stages timed");
    ck(skipped(s.dispatch) && skipped(s.engine) && skipped(s.reducedValidation) &&
       skipped(s.postsolve), "infeasible: later stages marked not run");
    ck(s.total == r.solveSeconds, "infeasible: total == solveSeconds");
}

void testInvalidModel() {
    model::Model m = lpWithFixedVariable();
    m.variables[0].lowerBound = 5.0;
    m.variables[0].upperBound = 1.0;
    solver::SolveReport rep;
    const solver::SolveResult r = solver::solve(m, {}, &rep);
    ck(r.status == solver::SolveStatus::InvalidModel, "invalid: status");
    ck(ran(rep.stageSeconds.validation), "invalid: validation timed");
    ck(!rep.classification && !rep.presolve && !rep.postsolve, "invalid: nothing after validation");
    ck(skipped(rep.stageSeconds.classification) && skipped(rep.stageSeconds.presolve),
       "invalid: later stages not run");
    ck(!rep.dispatch.dispatcherInvoked, "invalid: no dispatch");
    ck(rep.stageSeconds.total == r.solveSeconds, "invalid: total == solveSeconds");
}

// A reused report must describe only the latest solve.
void testReportIsReset() {
    solver::SolveReport rep;
    (void)solver::solve(lpWithFixedVariable(), {}, &rep);
    ck(rep.postsolve.has_value(), "reset: first solve fills postsolve");
    (void)solver::solve(presolveInfeasible(), {}, &rep);
    ck(!rep.postsolve && !rep.reducedValidation && !rep.dispatch.dispatcherInvoked &&
       skipped(rep.stageSeconds.engine), "reset: stale sections cleared");
}

void testSolveReduced() {
    const model::Model m = lpWithFixedVariable();
    const solver::Classification classification = solver::classify(m);
    const presolve::PresolveResult presolved = presolve::Presolver{}.run(m);

    const solver::SolveResult plain = solver::solveReduced(presolved.model, classification);
    solver::SolveReport rep;
    const solver::SolveResult r = solver::solveReduced(presolved.model, classification, {}, &rep);
    checkSameResult(plain, r, "solveReduced");

    ck(rep.classification && rep.classification->problemClass == classification.problemClass,
       "solveReduced: supplied classification recorded");
    ck(!rep.presolve && !rep.postsolve, "solveReduced: no presolve or postsolve");
    ck(rep.dispatch.dispatcherInvoked && rep.dispatch.engine == r.engine &&
       rep.dispatch.executedEngine == r.executedEngine, "solveReduced: dispatch mirrors result");
    ck(rep.reducedValidation && rep.reducedValidation->passed, "solveReduced: validation passed");
    ck(rep.reducedValidation && rep.reducedValidation->objectiveValue == r.objectiveValue,
       "solveReduced: validated objective is the result's");
    ck(skipped(rep.stageSeconds.presolve) && skipped(rep.stageSeconds.postsolve) &&
       skipped(rep.stageSeconds.classification), "solveReduced: skipped stages marked");
    ck(ran(rep.stageSeconds.dispatch) && ran(rep.stageSeconds.engine), "solveReduced: stages timed");
    ck(rep.stageSeconds.total == r.solveSeconds, "solveReduced: total == solveSeconds");
}

}  // namespace

int main() {
    testLp();
    testMilp();
    testQp();
    testForcedEngine();
    testPresolveInfeasible();
    testInvalidModel();
    testReportIsReset();
    testSolveReduced();
    std::printf("test_solve_report: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
