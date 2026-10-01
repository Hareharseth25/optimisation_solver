// The barrier engine through the orchestrator.
//
// The dual-sign convention is checked by CROSS-ENGINE AGREEMENT with the dual
// simplex, whose duals are already verified against HiGHS. A sign error in the
// barrier adapter -- the maximisation negation, the slack sign, or the fixed-
// variable shift -- shows up as a disagreement here rather than as a plausible
// number that happens to be wrong.
//
// Each case runs twice: through solveReduced(), which bypasses presolve and so
// isolates the adapter's own convention, and through solve(), which proves the
// engine survives presolve and postsolve dual reconstruction.
#include "solver/crossover.h"
#include "solver/orchestrator.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr double inf = std::numeric_limits<double>::infinity();

void check(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}
void near(double a, double b, double tolerance, const std::string& what) {
    if (!(std::isfinite(a) && std::abs(a - b) <= tolerance))
        throw std::runtime_error(what + ": expected " + std::to_string(b) + ", got " + std::to_string(a));
}

model::Variable var(const std::string& name, double lower, double upper) {
    model::Variable v;
    v.name = name; v.lowerBound = lower; v.upperBound = upper;
    return v;
}
model::Constraint row(const std::string& name, double lower, double upper,
                      std::vector<model::LinearTerm> terms) {
    model::Constraint c;
    c.name = name; c.lowerBound = lower; c.upperBound = upper; c.linearTerms = std::move(terms);
    return c;
}

solver::SolverOptions forced(solver::Engine engine) {
    solver::SolverOptions o;
    o.forceEngine = engine;
    return o;
}

// Solve with both engines, both entry points, and require agreement on the
// objective and on every constraint dual.
//
// The barrier runs with crossover OFF as well as ON. With crossover on, an LP's
// duals are replaced by the dual simplex's, so a broken sign convention in the
// barrier's OWN duals would be invisible -- and those duals are exactly what a
// caller receives whenever crossover is disabled or declines. Measured: before
// this loop existed, dropping the maximisation negation of the barrier's duals
// passed every test once crossover became the default.
void agree(const std::string& name, const model::Model& model, bool compareDuals = true) {
    const auto classification = solver::classify(model);
    for (const bool crossover : {false, true})
    for (const bool reduced : {true, false}) {
        const std::string route = std::string(reduced ? "solveReduced" : "solve") +
                                  (crossover ? "+crossover" : "");
        solver::SolverOptions barrierOptions = forced(solver::Engine::Barrier);
        barrierOptions.barrierCrossover = crossover;
        const auto barrier = reduced
            ? solver::solveReduced(model, classification, barrierOptions)
            : solver::solve(model, barrierOptions);
        const auto simplex = reduced
            ? solver::solveReduced(model, classification, forced(solver::Engine::DualSimplex))
            : solver::solve(model, forced(solver::Engine::DualSimplex));
        std::printf("  %-38s %-22s barrier=%-10s obj=% .9f  simplex obj=% .9f\n", name.c_str(), route.c_str(),
                    solver::toString(barrier.status), barrier.objectiveValue, simplex.objectiveValue);
        check(barrier.status == solver::SolveStatus::Optimal,
              name + " [" + route + "]: barrier " + solver::toString(barrier.status) + " - " + barrier.message);
        check(simplex.status == solver::SolveStatus::Optimal, name + ": simplex reference not optimal");
        // When presolve settles the model outright no engine runs, and that is
        // correct behaviour rather than a routing failure.
        if (reduced || barrier.executedEngine != solver::Engine::Trivial)
            check(barrier.executedEngine == solver::Engine::Barrier,
                  name + ": executed engine was " + std::string(solver::toString(barrier.executedEngine)));
        check(barrier.hasPrimal, name + ": barrier returned no validated primal");
        near(barrier.objectiveValue, simplex.objectiveValue, 1e-6, name + " [" + route + "] objective");
        if (!compareDuals) continue;
        check(barrier.hasDuals && simplex.hasDuals, name + " [" + route + "]: missing duals");
        check(barrier.constraintDuals.size() == simplex.constraintDuals.size(), name + ": dual length");
        for (std::size_t i = 0; i < barrier.constraintDuals.size(); ++i)
            near(barrier.constraintDuals[i], simplex.constraintDuals[i], 1e-5,
                 name + " [" + route + "] dual " + std::to_string(i));
    }
}
}  // namespace

int main() {
    try {
        using solver::Engine;
        using model::ObjectiveSense;

        solver::SolverOptions probe;
        check(solver::parseEngine("barrier") == Engine::Barrier, "engine name");
        check(solver::parseEngine("ipm") == Engine::Barrier, "engine alias ipm");
        check(std::string(solver::toString(Engine::Barrier)) == "barrier", "engine label");

        // Wyndor, a MAXIMISATION with <= rows. The dual simplex returns
        // [0, 1.5, 1] here, matching HiGHS; the barrier must too. A missing or
        // doubled negation for the objective sense shows up as [0,-1.5,-1].
        {
            model::Model m;
            m.objective.sense = ObjectiveSense::Maximize;
            m.variables = {var("x", 0, inf), var("y", 0, inf)};
            m.objective.linearTerms = {{0, 3}, {1, 5}};
            m.constraints = {row("c1", -inf, 4, {{0, 1}}),
                             row("c2", -inf, 12, {{1, 2}}),
                             row("c3", -inf, 18, {{0, 3}, {1, 2}})};
            agree("wyndor (max, <=)", m);
            solver::SolverOptions own = forced(Engine::Barrier);
            own.barrierCrossover = false;  // the barrier's own duals, not the simplex's
            const auto r = solver::solveReduced(m, solver::classify(m), own);
            near(r.constraintDuals[0], 0.0, 1e-6, "wyndor dual 0 absolute");
            near(r.constraintDuals[1], 1.5, 1e-6, "wyndor dual 1 absolute");
            near(r.constraintDuals[2], 1.0, 1e-6, "wyndor dual 2 absolute");
            near(r.objectiveValue, 36.0, 1e-6, "wyndor objective");
        }

        // Minimisation with >= rows (lower side active), so the slack's LOWER
        // multiplier is the one that binds.
        {
            model::Model m;
            m.variables = {var("x", 0, inf), var("y", 0, inf)};
            m.objective.linearTerms = {{0, 2}, {1, 3}};
            m.constraints = {row("r1", 4, inf, {{0, 1}, {1, 1}}),
                             row("r2", 6, inf, {{0, 1}, {1, 3}})};
            agree("diet (min, >=)", m);
        }

        // Equality rows plus a RANGED row active on its upper side, plus a
        // FIXED variable. The fixed variable is substituted out by the adapter
        // and its column moves to the right-hand side, which shifts the slack's
        // box -- and must not shift any multiplier.
        {
            model::Model m;
            m.variables = {var("x", 0, 10), var("y", 0, 10), var("z", 2, 2)};
            m.objective.linearTerms = {{0, -1}, {1, -2}, {2, 1}};
            m.constraints = {row("eq", 5, 5, {{0, 1}, {1, 1}, {2, 1}}),
                             row("range", 1, 4, {{1, 1}, {2, 1}})};
            agree("equality + ranged + fixed variable", m);
        }

        // Maximisation mixing >= and <= rows and a finite UPPER variable bound.
        {
            model::Model m;
            m.objective.sense = ObjectiveSense::Maximize;
            m.variables = {var("a", 0, 3), var("b", 0, inf), var("c", 0, inf)};
            m.objective.linearTerms = {{0, 4}, {1, 1}, {2, 2}};
            m.constraints = {row("cap", -inf, 10, {{0, 1}, {1, 1}, {2, 2}}),
                             row("floor", 1, inf, {{1, 1}, {2, -1}}),
                             row("mix", -inf, 8, {{0, 2}, {2, 1}})};
            agree("max, mixed senses, upper bound", m);
        }

        // Convex QP. The dual simplex cannot take it, so the reference is the
        // ADMM QP engine, and the analytic optimum pins it independently.
        // min x^2 + y^2 - 2x - 4y  s.t.  x + y = 2 -> (0.5, 1.5), objective -4.5.
        // Model quadratic terms are DIRECT coefficients: {0,0,1} means 1*x^2.
        {
            model::Model m;
            m.variables = {var("x", 0, inf), var("y", 0, inf)};
            m.objective.linearTerms = {{0, -2}, {1, -4}};
            m.objective.quadraticTerms = {{0, 0, 1.0}, {1, 1, 1.0}};
            m.constraints = {row("sum", 2, 2, {{0, 1}, {1, 1}})};
            const auto barrier = solver::solve(m, forced(Engine::Barrier));
            const auto admm = solver::solve(m, forced(Engine::Qp));
            std::printf("  %-38s barrier obj=% .9f  admm obj=% .9f\n", "convex QP",
                        barrier.objectiveValue, admm.objectiveValue);
            check(barrier.status == solver::SolveStatus::Optimal, "QP barrier: " + barrier.message);
            near(barrier.objectiveValue, -4.5, 1e-6, "QP objective (analytic)");
            near(barrier.variableValues[0], 0.5, 1e-5, "QP x");
            near(barrier.variableValues[1], 1.5, 1e-5, "QP y");
            near(barrier.objectiveValue, admm.objectiveValue, 1e-4, "QP barrier vs ADMM");
            // Crossover never runs on a QP, so these are always the barrier's
            // own multipliers. f*(b) = (b-3)^2/2 - 5 on x + y = b, so
            // df*/db = -1 at b = 2.
            check(barrier.hasDuals, "QP duals missing");
            near(barrier.constraintDuals[0], -1.0, 1e-6, "QP shadow price (analytic)");
            // With a maximisation of the negated objective, the same point.
            m.objective.sense = ObjectiveSense::Maximize;
            m.objective.linearTerms = {{0, 2}, {1, 4}};
            m.objective.quadraticTerms = {{0, 0, -1.0}, {1, 1, -1.0}};
            const auto negated = solver::solve(m, forced(Engine::Barrier));
            check(negated.status == solver::SolveStatus::Optimal, "negated QP: " + negated.message);
            near(negated.objectiveValue, 4.5, 1e-6, "negated QP objective");
            near(negated.variableValues[0], 0.5, 1e-5, "negated QP x");
            // Maximising the negated objective negates the shadow price once.
            check(negated.hasDuals, "negated QP duals missing");
            near(negated.constraintDuals[0], 1.0, 1e-6, "negated QP shadow price");
        }

        // A nonconvex objective forced onto the barrier is refused. The forced
        // route bypasses the dispatcher's convexity check, so the engine must
        // make its own -- otherwise heavy regularisation would return the
        // optimum of a different problem.
        {
            model::Model m;
            m.variables = {var("x", -1, 1)};
            m.objective.quadraticTerms = {{0, 0, -1.0}};
            m.constraints = {row("c", -inf, 1, {{0, 1}})};
            const auto r = solver::solve(m, forced(Engine::Barrier));
            std::printf("  %-38s %s: %s\n", "nonconvex QP forced", solver::toString(r.status), r.message.c_str());
            check(r.status == solver::SolveStatus::Unsupported, "nonconvex QP must be refused");
            check(!r.hasPrimal, "refused model returned a point");
        }

        // Infeasible: must not be reported Optimal, and must not be reported
        // Infeasible either -- the barrier has no certificate. LimitReached is
        // the honest verdict; the dual simplex is the engine that can prove it.
        // solveReduced is used so presolve cannot settle it first.
        {
            model::Model m;
            m.variables = {var("x", 0, inf), var("y", 0, inf)};
            m.objective.linearTerms = {{0, 1}, {1, 1}};
            m.constraints = {row("impossible", -inf, -1, {{0, 1}, {1, 1}})};
            const auto r = solver::solveReduced(m, solver::classify(m), forced(Engine::Barrier));
            std::printf("  %-38s %s: %s\n", "infeasible forced", solver::toString(r.status), r.message.c_str());
            check(r.status != solver::SolveStatus::Optimal, "infeasible model reported Optimal");
            check(r.status != solver::SolveStatus::Infeasible,
                  "barrier claimed an infeasibility certificate it does not compute");
            check(r.status == solver::SolveStatus::LimitReached, "expected LimitReached");
            check(!r.hasDuals, "duals published for a non-optimal barrier result");
        }

        // Crossover. A singleton row is turned into a variable bound by
        // presolve; postsolve returns the reduced cost to that row only when
        // the variable sits EXACTLY on the bound. An interior point never does,
        // which is how Netlib adlittle lost its duals. With crossover the
        // result is a genuine vertex: x lands on the bound to machine
        // precision, the duals survive postsolve, and they match the simplex.
        //   min -2x - y  s.t.  x <= 3 (singleton),  x + y <= 5.
        //   Optimum (3, 2), objective -8, shadow prices [-1, -1].
        {
            model::Model m;
            m.variables = {var("x", 0, inf), var("y", 0, inf)};
            m.objective.linearTerms = {{0, -2}, {1, -1}};
            m.constraints = {row("cap_x", -inf, 3, {{0, 1}}),
                             row("total", -inf, 5, {{0, 1}, {1, 1}})};
            const auto vertex = solver::solve(m, forced(Engine::Barrier));
            const auto simplex = solver::solve(m, forced(Engine::DualSimplex));
            std::printf("  %-38s %s | %s\n", "crossover to a vertex", solver::toString(vertex.status),
                        vertex.message.c_str());
            check(vertex.status == solver::SolveStatus::Optimal, "crossover case: " + vertex.message);
            check(vertex.message.find("crossover to an optimal vertex") != std::string::npos,
                  "crossover was not applied: " + vertex.message);
            // A vertex property the interior point does not have.
            check(std::abs(vertex.variableValues[0] - 3.0) <= 1e-12,
                  "x is not exactly on its active bound: " + std::to_string(vertex.variableValues[0] - 3.0));
            check(vertex.hasDuals && simplex.hasDuals, "duals lost after crossover");
            for (std::size_t i = 0; i < vertex.constraintDuals.size(); ++i)
                near(vertex.constraintDuals[i], simplex.constraintDuals[i], 1e-9,
                     "post-crossover dual " + std::to_string(i));

            // Crossover off: the interior solution stands, still optimal.
            solver::SolverOptions off = forced(Engine::Barrier);
            off.barrierCrossover = false;
            const auto interior = solver::solve(m, off);
            check(interior.status == solver::SolveStatus::Optimal, "interior-only barrier");
            check(interior.message.find("crossover") == std::string::npos, "crossover ran while disabled");
            near(interior.objectiveValue, -8.0, 1e-6, "interior objective");

            // The acceptance gate. Given a correct interior point but a CLAIMED
            // objective that is wrong, the vertex reached disagrees with the
            // claim and must be refused -- this is the check that stops a
            // warm start from a misidentified basis returning a non-optimal
            // vertex as if it were optimal.
            const std::vector<double> x = {3.0, 2.0}, y = {-1.0, -1.0};
            const auto honest = solver::crossoverToVertex(m, x, y, -8.0, solver::SolverOptions{});
            check(honest.applied, "crossover refused a correct interior point: " + honest.detail);
            const auto lied = solver::crossoverToVertex(m, x, y, -7.0, solver::SolverOptions{});
            check(!lied.applied, "crossover accepted a vertex that disagrees with the interior optimum");

            // Not applied to QPs: the optimum of a QP need not be a vertex.
            model::Model q = m;
            q.objective.quadraticTerms = {{0, 0, 1.0}};
            check(!solver::crossoverToVertex(q, x, y, -8.0, solver::SolverOptions{}).applied,
                  "crossover applied to a quadratic objective");
        }

        // One time budget covers the barrier and crossover. Crossover used to
        // receive the caller's ORIGINAL limit a second time, so the barrier
        // could spend nearly all of it and crossover then start with a fresh
        // copy. Deterministic setup: min 0 s.t. x0 - x1 = 0 over free
        // variables is optimal at the barrier's starting point, and the
        // barrier tests convergence BEFORE its time limit, so it returns
        // Optimal at iteration 0 even under a 1 ns limit -- which its own run
        // has always used up by the time crossover would start.
        {
            model::Model m;
            m.variables = {var("x0", -inf, inf), var("x1", -inf, inf)};
            m.constraints = {row("tie", 0, 0, {{0, 1}, {1, -1}})};
            const auto classification = solver::classify(m);

            solver::SolverOptions unlimited = forced(Engine::Barrier);
            const auto free = solver::solveReduced(m, classification, unlimited);
            check(free.status == solver::SolveStatus::Optimal, "budget case: " + free.message);
            check(free.message.find("crossover to an optimal vertex") != std::string::npos,
                  "without a time limit crossover must still run: " + free.message);

            solver::SolverOptions tight = forced(Engine::Barrier);
            tight.timeLimitSeconds = 1e-9;
            const auto spent = solver::solveReduced(m, classification, tight);
            std::printf("  %-38s %s | %s\n", "crossover after an exhausted budget",
                        solver::toString(spent.status), spent.message.c_str());
            check(spent.status == solver::SolveStatus::Optimal, "the barrier's own optimum must stand");
            check(spent.message.find("crossover skipped: the time limit was used up by the barrier solve") !=
                      std::string::npos,
                  "crossover was not skipped after the barrier used up the budget: " + spent.message);
            // The old path launched the simplex with a fresh budget; it only
            // stopped because the simplex's own clock then ran out.
            check(spent.message.find("simplex cleanup") == std::string::npos,
                  "crossover ran the simplex on a budget it did not have: " + spent.message);

            // Crossover charges its OWN work to the budget it is given, and
            // stops before handing the simplex a remainder it does not have.
            solver::SolverOptions almostNone;
            almostNone.timeLimitSeconds = 1e-12;
            const auto direct = solver::crossoverToVertex(m, {0.0, 0.0}, {0.0}, 0.0, almostNone);
            check(!direct.applied, "crossover applied with no budget left");
            check(direct.detail.find("time limit reached during crossover") != std::string::npos,
                  "crossover did not stop on its own budget: " + direct.detail);
        }

        // Automatic dispatch is unchanged: a small LP still goes to the dual
        // simplex. The barrier is opt-in until benchmarks justify a default.
        {
            model::Model m;
            m.variables = {var("x", 0, inf)};
            m.objective.linearTerms = {{0, 1}};
            m.constraints = {row("c", 1, inf, {{0, 1}})};
            const auto r = solver::solve(m);
            check(r.engine != Engine::Barrier, "automatic dispatch must not select the barrier yet");
        }

        std::printf("barrier pipeline tests passed\n");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
}
