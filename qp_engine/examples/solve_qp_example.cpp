// Minimal end-to-end example: solve a small QP by hand.
//
//   minimise 0.5*x^2 + x + y^2 + 2*y
//   subject to  x >= 1
//               y >= 0
//               x + y = 2
//
// The equality row is expressed as a boxed pair (l = u = 2).
// The unconstrained optimum is x = -1, y = -1 with objective -2.5.
// The feasible optimum is x = 1, y = 1 with objective 3.5.
//
#include "qp/qp_solver.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

int main() {
    const double inf = std::numeric_limits<double>::infinity();

    // P = diag(1, 2)  (the 0.5 factor lives outside the matrix)
    //   0.5 * x^2  ->  P[0,0] = 1
    //   y^2         ->  P[1,1] = 2
    qp::QpModel problem;
    {
        std::vector<double> r{0.0, 1.0};
        std::vector<double> c{0.0, 1.0};
        std::vector<double> v{1.0, 2.0};
        problem.P = qp::SparseMatrix::fromTriplets(2, 2, r, c, v);
    }

    // q = [1, 2]  (the linear terms)
    problem.q = {1.0, 2.0};

    // A: three rows, each constraint is l_i <= (A x)_i <= u_i
    //   row 0: x >= 1         ->  l[0]=1, u[0]=+inf
    //   row 1: y >= 0          ->  l[1]=0, u[1]=+inf
    //   row 2: x + y = 2       ->  l[2]=u[2]=2
    {
        std::vector<double> r{0.0, 1.0, 2.0, 2.0};
        std::vector<double> c{0.0, 1.0, 0.0, 1.0};
        std::vector<double> v{1.0, 1.0, 1.0, 1.0};
        problem.A = qp::SparseMatrix::fromTriplets(3, 2, r, c, v);
    }
    problem.l = {1.0, 0.0, 2.0};
    problem.u = {inf, inf, 2.0};

    qp::AdmmOptions options;
    options.primalTolerance = 1e-6;
    options.dualTolerance   = 1e-6;
    options.iterationLimit  = 5000;
    options.rho            = 1.0;
    options.useRuizScaling  = false;  // not needed for this tiny problem
    options.usePolishing    = false;

    const qp::AdmmResult result = qp::QpSolver{}.solve(problem, options);

    std::printf("status:             %s\n", qp::toString(result.status));
    std::printf("iterations:        %lld\n", static_cast<long long>(result.iterations));
    std::printf("objective:         %.9g\n", result.primalObjective);
    std::printf("x:                 %.9g\n", result.primal[0]);
    std::printf("y:                 %.9g\n", result.primal[1]);
    std::printf("primal residual:   %.3e\n", result.primalResidual);
    std::printf("dual residual:     %.3e\n", result.dualResidual);
    std::printf("relative gap:      %.3e\n", result.relativeGap);

    // The feasible optimum is (1, 1) with objective 3.5.
    const bool ok = result.status == qp::QpStatus::Optimal
                 && std::abs(result.primal[0] - 1.0) < 5e-3
                 && std::abs(result.primal[1] - 1.0) < 5e-3;
    std::printf("\nFeasible optimum is (1, 1) obj=3.5  ->  %s\n",
                ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
