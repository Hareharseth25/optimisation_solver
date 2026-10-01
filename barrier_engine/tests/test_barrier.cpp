// The interior-point method on problems with known answers.
//
// Every Optimal result is re-verified INDEPENDENTLY of Result's own residual
// fields: primal feasibility, dual feasibility and the duality gap are
// recomputed here from the returned vectors and the original data.
#include "barrier/barrier_solver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace barrier;

namespace {
void check(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}
void near(double a, double b, double tolerance, const std::string& what) {
    if (!(std::isfinite(a) && std::abs(a - b) <= tolerance))
        throw std::runtime_error(what + ": expected " + std::to_string(b) + ", got " + std::to_string(a));
}

SparseCsc dense(Index rows, Index cols, const std::vector<std::vector<double>>& entries) {
    TripletBuilder builder(rows, cols);
    for (Index i = 0; i < rows; ++i)
        for (Index j = 0; j < cols; ++j) {
            const double v = entries[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
            if (v != 0.0) builder.add(i, j, v);
        }
    return builder.build();
}

struct Verified { double primal, dual, gap, bound; };

// Independent KKT check on the ORIGINAL data. Absolute infinity norms.
Verified verify(const BarrierProblem& p, const Result& r) {
    const auto n = static_cast<std::size_t>(p.variableCount());
    const auto m = static_cast<std::size_t>(p.constraintCount());
    Verified v{0, 0, 0, 0};
    std::vector<double> ax, aty, qx(n, 0.0);
    p.constraints.multiply(r.primal, ax);
    p.constraints.transposeMultiply(r.equalityDual, aty);
    if (p.hessian.cols == p.variableCount()) multiplySymmetric(p.hessian, r.primal, qx);
    for (std::size_t i = 0; i < m; ++i) v.primal = std::max(v.primal, std::abs(ax[i] - p.rightHandSide[i]));
    for (std::size_t j = 0; j < n; ++j) {
        v.bound = std::max({v.bound, p.lower[j] - r.primal[j], r.primal[j] - p.upper[j]});
        const double stationarity = p.objective[j] + qx[j] - aty[j] - r.lowerDual[j] + r.upperDual[j];
        v.dual = std::max(v.dual, std::abs(stationarity));
        check(r.lowerDual[j] >= 0.0 && r.upperDual[j] >= 0.0, "bound multiplier negative");
        if (!std::isfinite(p.lower[j])) check(r.lowerDual[j] == 0.0, "multiplier on a missing lower bound");
        if (!std::isfinite(p.upper[j])) check(r.upperDual[j] == 0.0, "multiplier on a missing upper bound");
    }
    double quadratic = 0.0, linear = 0.0, dual = 0.0;
    for (std::size_t j = 0; j < n; ++j) { quadratic += r.primal[j] * qx[j]; linear += p.objective[j] * r.primal[j]; }
    for (std::size_t i = 0; i < m; ++i) dual += p.rightHandSide[i] * r.equalityDual[i];
    for (std::size_t j = 0; j < n; ++j) {
        if (std::isfinite(p.lower[j])) dual += p.lower[j] * r.lowerDual[j];
        if (std::isfinite(p.upper[j])) dual -= p.upper[j] * r.upperDual[j];
    }
    const double primalObjective = linear + 0.5 * quadratic;
    v.gap = std::abs(primalObjective - (dual - 0.5 * quadratic)) / (1.0 + std::abs(primalObjective));
    return v;
}

Result optimal(const std::string& name, const BarrierProblem& p, Options o = {}) {
    const Result r = BarrierSolver{}.solve(p, o);
    std::printf("  %-34s %-9s obj=% .10f it=%-3d corr=%-3d fill=%zu\n", name.c_str(),
                toString(r.status), r.primalObjective, r.iterations, r.correctorsAccepted,
                r.factorNonzeros);
    check(r.status == Status::Optimal, name + ": " + toString(r.status) + " (" + r.message + ")");
    const Verified v = verify(p, r);
    check(v.primal <= 1e-6, name + ": independent primal residual " + std::to_string(v.primal));
    check(v.dual <= 1e-6, name + ": independent dual residual " + std::to_string(v.dual));
    check(v.gap <= 1e-6, name + ": independent gap " + std::to_string(v.gap));
    check(v.bound <= 1e-9, name + ": bound violated by " + std::to_string(v.bound));
    return r;
}
}  // namespace

int main() {
    try {
        const double inf = infinity;

        // Wyndor glass (Hillier & Lieberman): max 3x + 5y, as a minimisation,
        // with explicit slacks. Optimum x=2, y=6, objective -36.
        {
            BarrierProblem p;
            p.constraints = dense(3, 5, {{1, 0, 1, 0, 0}, {0, 2, 0, 1, 0}, {3, 2, 0, 0, 1}});
            p.objective = {-3, -5, 0, 0, 0};
            p.rightHandSide = {4, 12, 18};
            p.lower.assign(5, 0.0);
            p.upper.assign(5, inf);
            const Result r = optimal("wyndor", p);
            near(r.primalObjective, -36.0, 1e-6, "wyndor objective");
            near(r.primal[0], 2.0, 1e-5, "wyndor x");
            near(r.primal[1], 6.0, 1e-5, "wyndor y");
            // Equality duals of the minimisation: y = [0, -1.5, -1]. The row
            // x<=4 is slack at the optimum, so its multiplier is zero.
            near(r.equalityDual[0], 0.0, 1e-5, "wyndor dual 0");
            near(r.equalityDual[1], -1.5, 1e-5, "wyndor dual 1");
            near(r.equalityDual[2], -1.0, 1e-5, "wyndor dual 2");
        }

        // Finite upper bounds, and a FREE variable (no bounds at all), which is
        // the case normal-equations codes cannot factorise without special
        // handling. min x - y s.t. x + y = 1, 0 <= x <= 3, y free, plus
        // y <= 5 through a bounded slack: y + s = 5, s >= 0.
        {
            BarrierProblem p;
            p.constraints = dense(2, 3, {{1, 1, 0}, {0, 1, 1}});
            p.objective = {1, -1, 0};
            p.rightHandSide = {1, 5};
            p.lower = {0, -inf, 0};
            p.upper = {3, inf, inf};
            const Result r = optimal("free variable + upper bound", p);
            // Minimising x - y with x + y = 1: y as large as allowed (5), x = -4
            // would violate x >= 0, so x = 0 and y = 1? No: x = 1 - y, so the
            // objective is 1 - 2y, minimised by the largest feasible y. y <= 5
            // and x = 1 - y >= 0 gives y <= 1. So y = 1, x = 0, objective -1.
            near(r.primalObjective, -1.0, 1e-6, "free objective");
            near(r.primal[0], 0.0, 1e-5, "free x");
            near(r.primal[1], 1.0, 1e-5, "free y");
        }

        // Convex QP: min x^2 + y^2 - 2x - 4y s.t. x + y = 2, x,y >= 0.
        // Hessian diag(2,2). Unconstrained optimum (1,2) violates x+y=2;
        // on the line, minimise (x-1)^2 + (y-2)^2 -> x = 0.5, y = 1.5.
        {
            BarrierProblem p;
            p.constraints = dense(1, 2, {{1, 1}});
            TripletBuilder q(2, 2);
            q.add(0, 0, 2.0); q.add(1, 1, 2.0);
            p.hessian = q.build();
            p.objective = {-2, -4};
            p.rightHandSide = {2};
            p.lower = {0, 0};
            p.upper = {inf, inf};
            const Result r = optimal("convex QP", p);
            near(r.primal[0], 0.5, 1e-5, "qp x");
            near(r.primal[1], 1.5, 1e-5, "qp y");
            near(r.primalObjective, 0.25 + 2.25 - 1.0 - 6.0, 1e-6, "qp objective");
        }

        // No finite bounds anywhere: an equality-constrained QP over free
        // variables. There is no barrier term, so the Newton step on the
        // augmented system is the exact KKT solve and the method must finish in
        // a couple of steps. It used to be refused outright -- and 3 of the 14
        // Maros-Meszaros smoke instances have exactly this shape.
        //   min x^2 + y^2  s.t.  x + y = 2  ->  (1, 1), objective 2, y = 2.
        {
            BarrierProblem p;
            p.constraints = dense(1, 2, {{1, 1}});
            TripletBuilder q(2, 2);
            q.add(0, 0, 2.0); q.add(1, 1, 2.0);
            p.hessian = q.build();
            p.objective = {0, 0};
            p.rightHandSide = {2};
            p.lower = {-inf, -inf};
            p.upper = {inf, inf};
            const Result r = optimal("equality QP, no bounds at all", p);
            near(r.primal[0], 1.0, 1e-8, "free QP x");
            near(r.primal[1], 1.0, 1e-8, "free QP y");
            near(r.equalityDual[0], 2.0, 1e-7, "free QP multiplier");
            check(r.iterations <= 3, "exact Newton case took " + std::to_string(r.iterations) + " iterations");
        }

        // Degenerate LP: several constraints active at a single vertex. The
        // simplex method can cycle or stall here; a barrier method approaches
        // the optimal face from the interior and is indifferent to it.
        // min -x - y s.t. x <= 1, y <= 1, x + y <= 2, x + 2y <= 3, 2x + y <= 3.
        // All five are tight at (1,1).
        {
            BarrierProblem p;
            p.constraints = dense(5, 7, {{1, 0, 1, 0, 0, 0, 0},
                                         {0, 1, 0, 1, 0, 0, 0},
                                         {1, 1, 0, 0, 1, 0, 0},
                                         {1, 2, 0, 0, 0, 1, 0},
                                         {2, 1, 0, 0, 0, 0, 1}});
            p.objective = {-1, -1, 0, 0, 0, 0, 0};
            p.rightHandSide = {1, 1, 2, 3, 3};
            p.lower.assign(7, 0.0);
            p.upper.assign(7, inf);
            const Result r = optimal("degenerate vertex (5 active)", p);
            near(r.primalObjective, -2.0, 1e-6, "degenerate objective");
            near(r.primal[0], 1.0, 1e-5, "degenerate x");
            near(r.primal[1], 1.0, 1e-5, "degenerate y");
        }

        // Badly scaled: coefficients spanning 1e-4 to 1e4. The relative
        // tolerances and the regularisation must cope without the caller
        // equilibrating first.
        {
            BarrierProblem p;
            p.constraints = dense(2, 4, {{1e4, 1e-4, 1, 0}, {1e-4, 1e4, 0, 1}});
            p.objective = {-1, -1, 0, 0};
            p.rightHandSide = {1e4, 1e4};
            p.lower.assign(4, 0.0);
            p.upper.assign(4, inf);
            optimal("badly scaled (1e-4..1e4)", p);
        }

        // Gondzio correctors must not change the answer, only the path.
        {
            BarrierProblem p;
            p.constraints = dense(3, 5, {{1, 0, 1, 0, 0}, {0, 2, 0, 1, 0}, {3, 2, 0, 0, 1}});
            p.objective = {-3, -5, 0, 0, 0};
            p.rightHandSide = {4, 12, 18};
            p.lower.assign(5, 0.0);
            p.upper.assign(5, inf);
            Options plain; plain.centralityCorrectors = 0;
            const Result a = optimal("wyndor, no correctors", p, plain);
            const Result b = optimal("wyndor, 4 correctors", p, [] { Options o; o.centralityCorrectors = 4; return o; }());
            near(a.primalObjective, b.primalObjective, 1e-7, "correctors changed the optimum");
        }

        // Infeasible: x + y = -1 with x, y >= 0. Must NOT report Optimal, and
        // must not claim a certificate it does not have.
        {
            BarrierProblem p;
            p.constraints = dense(1, 2, {{1, 1}});
            p.objective = {1, 1};
            p.rightHandSide = {-1};
            p.lower = {0, 0};
            p.upper = {inf, inf};
            const Result r = BarrierSolver{}.solve(p);
            std::printf("  %-34s %-9s %s\n", "infeasible", toString(r.status), r.message.c_str());
            check(r.status != Status::Optimal, "infeasible problem reported Optimal");
            check(r.status == Status::SuspectedInfeasibleOrUnbounded,
                  std::string("infeasible problem should be reported as suspected, got ") + toString(r.status));
        }

        // Unbounded: min -x with x >= 0 and no other constraint on x.
        {
            BarrierProblem p;
            p.constraints = dense(1, 2, {{0, 1}});
            p.objective = {-1, 0};
            p.rightHandSide = {1};
            p.lower = {0, 0};
            p.upper = {inf, inf};
            const Result r = BarrierSolver{}.solve(p);
            std::printf("  %-34s %-9s %s\n", "unbounded", toString(r.status), r.message.c_str());
            check(r.status != Status::Optimal, "unbounded problem reported Optimal");
            check(r.status == Status::SuspectedInfeasibleOrUnbounded,
                  std::string("unbounded problem should be reported as suspected, got ") + toString(r.status));
        }

        // The inertia check. For a genuinely quasidefinite matrix the Schur
        // complements stay quasidefinite, so in exact arithmetic the inertia is
        // always right and the check never fires -- which is why no convex test
        // exercises it. It fires when the (1,1) block itself loses definiteness.
        // A nonconvex Hessian handed DIRECTLY to the solver does that (the
        // orchestrator refuses such models before they get here): at the start
        // x = 0 with D = 2, so the primal pivot is -(Q + D + Rp) = -(-20 + 2)
        // = +18, the wrong sign. The check must catch it and escalate.
        {
            BarrierProblem p;
            p.constraints = dense(1, 2, {{1, 1}});
            TripletBuilder q(2, 2);
            q.add(0, 0, -20.0);
            p.hessian = q.build();
            p.objective = {0, 0};
            p.rightHandSide = {1};
            p.lower = {-1, 0};
            p.upper = {1, inf};
            const Result r = BarrierSolver{}.solve(p);
            std::printf("  %-34s %-9s escalations=%d\n", "indefinite (1,1) block",
                        toString(r.status), r.regularizationEscalations);
            check(r.regularizationEscalations > 0,
                  "inertia check never fired on an indefinite (1,1) block");
        }

        // Active-set polishing. An interior point stops at distance ~mu/z from
        // an active bound, never on it. min x^2 - 4x s.t. x + y = 1.5 with
        // 0 <= x <= 1, y >= 0: x wants 2, stops at its upper bound 1, y = 0.5.
        // Stationarity in x: 2 - 4 + zUpper = 0, so zUpper = 2; the row
        // multiplier is 0 because y sits strictly inside its bounds.
        {
            BarrierProblem p;
            p.constraints = dense(1, 2, {{1, 1}});
            TripletBuilder q(2, 2);
            q.add(0, 0, 2.0);
            p.hessian = q.build();
            p.objective = {-4, 0};
            p.rightHandSide = {1.5};
            p.lower = {0, 0};
            p.upper = {1, inf};

            const Result polished = optimal("polished to an active bound", p);
            check(polished.polished, "polishing did not apply: " + polished.polishDetail);
            check(polished.primal[0] == 1.0, "x not EXACTLY on its active bound after polishing");
            near(polished.upperDual[0], 2.0, 1e-9, "active bound multiplier");
            check(polished.lowerDual[1] == 0.0 && polished.upperDual[1] == 0.0,
                  "inactive bound kept a nonzero multiplier after polishing");
            near(polished.equalityDual[0], 0.0, 1e-9, "row multiplier");

            Options off; off.polish = false;
            const Result interior = optimal("same, polishing off", p, off);
            check(!interior.polished, "polishing ran while disabled");
            check(interior.primal[0] < 1.0, "interior point landed exactly on the bound without polishing");

            // The gate, with deliberately WRONG active sets. Each must be
            // refused rather than yield a plausible wrong answer.
            PolishedPoint out;
            check(polishToActiveSet(p, {1, 0}, Options{}, out), "correct active set refused: " + out.detail);
            // x left free: the KKT solution is x = 2, outside its box.
            check(!polishToActiveSet(p, {0, 0}, Options{}, out), "a free variable left its box and was accepted");
            check(out.detail.find("left its bounds") != std::string::npos, "wrong reason: " + out.detail);
            // ...and the refusal proposes the correct set. Both x = 2 and
            // y = -0.5 left their boxes; only the most violated, x, is
            // activated. Activating both would make the row infeasible.
            check(out.correctedActiveSet == std::vector<int>({1, 0}),
                  "correction after leaving the box should activate only x's upper bound");
            // x held at its LOWER bound: stationarity gives zLower = -4 < 0.
            check(!polishToActiveSet(p, {-1, 0}, Options{}, out), "a wrong-sign multiplier was accepted");
            check(out.detail.find("negative multiplier") != std::string::npos, "wrong reason: " + out.detail);
            check(out.correctedActiveSet == std::vector<int>({0, 0}),
                  "correction after a wrong-sign multiplier should release x");
            // Two corrections from the worst start: {-1,0} -> {0,0} -> {1,0}.
            // The corrected sequence must reach the right set and verify.
            std::vector<int> set = {-1, 0};
            bool reached = false;
            for (int round = 0; round < 4 && !reached; ++round) {
                if (polishToActiveSet(p, set, Options{}, out)) reached = true;
                else set = out.correctedActiveSet;
            }
            check(reached && out.primal[0] == 1.0, "active-set correction did not converge to the vertex");
        }

        // Input validation.
        {
            BarrierProblem p;
            p.constraints = dense(1, 1, {{1}});
            p.objective = {1}; p.rightHandSide = {1};
            p.lower = {2}; p.upper = {2};
            check(BarrierSolver{}.solve(p).status == Status::InvalidProblem,
                  "fixed variable must be rejected, not divided by zero");
            p.lower = {3}; p.upper = {1};
            check(BarrierSolver{}.solve(p).status == Status::InvalidProblem, "crossed bounds");
            p.lower = {0}; p.upper = {inf}; p.objective = {};
            check(BarrierSolver{}.solve(p).status == Status::InvalidProblem, "dimension mismatch");
        }

        std::printf("barrier solver tests passed\n");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
}
