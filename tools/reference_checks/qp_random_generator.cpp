// Adversarial QP generator + solver, exercising the option surface my earlier
// audit skipped: maximize, nonzero offset, non-diagonal P (real curvature,
// not just diagonal), equality/ranged/one-sided rows, free variables. Emits
// problem + our answer as plain text for an independent (OSQP) cross-check.
#include "solver/orchestrator.h"
#include <cstdio>
#include <limits>
#include <random>
#include <vector>
static const double INF = std::numeric_limits<double>::infinity();

int main(int argc, char** argv) {
    const int N = argc > 1 ? std::atoi(argv[1]) : 300;
    const unsigned seed = argc > 2 ? (unsigned)std::atol(argv[2]) : 20260908u;
    std::mt19937 g(seed);
    std::uniform_real_distribution<double> U(-3, 3);
    std::uniform_real_distribution<double> Small(0.1, 2.0);
    std::uniform_real_distribution<double> Off(-40, 40);

    std::printf("%d\n", N);
    for (int t = 0; t < N; ++t) {
        const int n = 2 + (int)(g() % 6);
        const int m = (int)(g() % 5);
        const bool maximise = (g() % 2) == 0;
        const bool hasOffset = (g() % 2) == 0;
        const double offset = hasOffset ? Off(g) : 0.0;

        // P_true = L L^T + eps I: guaranteed PSD, with real off-diagonal
        // curvature (not just a diagonal Hessian, which is all my earlier
        // testing ever exercised).
        std::vector<std::vector<double>> L(n, std::vector<double>(n, 0.0));
        for (int i = 0; i < n; ++i)
            for (int j = 0; j <= i; ++j)
                L[i][j] = (g() % 4 == 0) ? 0.0 : Small(g) * (g() % 2 ? 1 : -1);
        std::vector<std::vector<double>> P(n, std::vector<double>(n, 0.0));
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                double s = 0;
                for (int k = 0; k < n; ++k) s += L[i][k] * L[j][k];
                P[i][j] = s;
            }
        for (int i = 0; i < n; ++i) P[i][i] += 0.05;  // strictly PD, away from a degenerate edge
        // For maximize, a well-posed (convex) problem needs a CONCAVE objective,
        // i.e. P_true must be negative semidefinite here -- maximizing a convex
        // quadratic is itself non-convex regardless of which engine is asked.
        if (maximise) for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) P[i][j] = -P[i][j];

        std::vector<double> q(n);
        for (int j = 0; j < n; ++j) q[j] = U(g);

        std::vector<double> lo(n), hi(n);
        for (int j = 0; j < n; ++j) {
            const int kind = g() % 4;
            if (kind == 0) { lo[j] = -INF; hi[j] = INF; }
            else if (kind == 1) { lo[j] = -5 - U(g); hi[j] = INF; }
            else if (kind == 2) { lo[j] = -INF; hi[j] = 5 + U(g); }
            else { double a = U(g), b = U(g); lo[j] = std::min(a,b) - 3; hi[j] = std::max(a,b) + 3; }
        }

        std::vector<std::vector<double>> A(m, std::vector<double>(n, 0.0));
        std::vector<double> rlo(m), rhi(m);
        for (int i = 0; i < m; ++i) {
            for (int j = 0; j < n; ++j) if (g() % 2) A[i][j] = U(g);
            const int kind = g() % 4;
            const double c = 3 + U(g);
            if (kind == 0) { rlo[i] = c; rhi[i] = c; }              // equality
            else if (kind == 1) { rlo[i] = -INF; rhi[i] = c; }      // <=
            else if (kind == 2) { rlo[i] = -c; rhi[i] = INF; }      // >=
            else { rlo[i] = -c; rhi[i] = c; }                        // ranged
        }

        // Build model::Model.
        model::Model mdl;
        for (int j = 0; j < n; ++j) {
            model::Variable v; v.name = "x" + std::to_string(j);
            v.lowerBound = lo[j]; v.upperBound = hi[j];
            v.type = model::VariableType::Continuous;
            mdl.variables.push_back(v);
        }
        for (int i = 0; i < m; ++i) {
            model::Constraint c; c.name = "r" + std::to_string(i);
            c.lowerBound = rlo[i]; c.upperBound = rhi[i];
            for (int j = 0; j < n; ++j)
                if (A[i][j] != 0.0) c.linearTerms.push_back({j, A[i][j]});
            mdl.constraints.push_back(c);
        }
        mdl.objective.sense = maximise ? model::ObjectiveSense::Maximize : model::ObjectiveSense::Minimize;
        mdl.objective.offset = offset;
        for (int j = 0; j < n; ++j) mdl.objective.linearTerms.push_back({j, q[j]});
        for (int i = 0; i < n; ++i)
            for (int j = i; j < n; ++j) {
                if (i == j) { if (P[i][i] != 0.0) mdl.objective.quadraticTerms.push_back({i, i, P[i][i] / 2.0}); }
                else        { if (P[i][j] != 0.0) mdl.objective.quadraticTerms.push_back({i, j, P[i][j]}); }
            }

        const auto r = solver::solve(mdl);

        // --- emit: problem, then our answer ---
        std::printf("%d %d %d %d %.17g\n", n, m, maximise ? 1 : 0, hasOffset ? 1 : 0, offset);
        for (int i = 0; i < n; ++i) { for (int j = 0; j < n; ++j) std::printf("%.17g ", P[i][j]); std::printf("\n"); }
        for (int j = 0; j < n; ++j) std::printf("%.17g ", q[j]); std::printf("\n");
        for (int j = 0; j < n; ++j) std::printf("%.17g %.17g ", lo[j], hi[j]); std::printf("\n");
        for (int i = 0; i < m; ++i) {
            for (int j = 0; j < n; ++j) std::printf("%.17g ", A[i][j]);
            std::printf("%.17g %.17g\n", rlo[i], rhi[i]);
        }
        std::printf("%s %s %.17g\n", solver::toString(r.status), solver::toString(r.engine), r.objectiveValue);
        for (int j = 0; j < n; ++j) std::printf("%.17g ", r.variableValues.size() > (size_t)j ? r.variableValues[j] : std::nan(""));
        std::printf("\n");
        std::printf("%d\n", r.hasDuals ? 1 : 0);
        if (r.hasDuals) { for (int i = 0; i < m; ++i) std::printf("%.17g ", r.constraintDuals[i]); std::printf("\n"); }
    }
    return 0;
}
