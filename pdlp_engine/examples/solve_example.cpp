#include "pdlp/pdlp_solver.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

int main() {
    const double infinity = std::numeric_limits<double>::infinity();

    // minimize x + 2y
    // subject to x + y = 1, x >= 0, y >= 0
    pdlp::CompiledLp problem;
    problem.matrix = pdlp::SparseMatrix::fromTriplets(
        1,
        2,
        {{0, 0, 1.0}, {0, 1, 1.0}}
    );
    problem.objective = {1.0, 2.0};
    problem.rowLower = {1.0};
    problem.rowUpper = {1.0};
    problem.variableLower = {0.0, 0.0};
    problem.variableUpper = {infinity, infinity};

    pdlp::PdlpOptions options;
    options.primalTolerance = 1e-7;
    options.dualTolerance = 1e-7;
    options.gapTolerance = 1e-7;

    const pdlp::PdlpResult result = pdlp::PdlpSolver{}.solve(problem, options);

    std::cout << "status: " << pdlp::toString(result.status) << '\n';
    std::cout << "objective: " << result.primalObjective << '\n';
    std::cout << "x: " << result.primal[0] << '\n';
    std::cout << "y: " << result.primal[1] << '\n';
    std::cout << "primal residual: " << result.primalResidual << '\n';
    std::cout << "dual residual: " << result.dualResidual << '\n';
    std::cout << "relative gap: " << result.relativeGap << '\n';

    return result.status == pdlp::PdlpStatus::Optimal ? 0 : 1;
}

