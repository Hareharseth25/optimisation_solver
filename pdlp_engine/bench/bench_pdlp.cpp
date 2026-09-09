// Benchmark harness: generates large, feasible, sparse LPs and reports
// wall-clock throughput of the PDLP engine.
//
//   ./pdlp_bench [rows] [cols] [nnzPerRow] [iterationLimit] [threads]

#include "pdlp/pdlp_solver.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();

// Builds a feasible LP with a known interior point x0, so that every generated
// instance is guaranteed solvable and the reported residuals are meaningful.
pdlp::CompiledLp makeInstance(int rows, int columns, int nnzPerRow, unsigned seed) {
    std::mt19937 generator(seed);
    std::uniform_real_distribution<double> coefficient(-1.0, 1.0);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::uniform_int_distribution<int> column(0, columns - 1);

    std::vector<pdlp::MatrixTriplet> triplets;
    triplets.reserve(static_cast<std::size_t>(rows) * nnzPerRow);
    for (int row = 0; row < rows; ++row) {
        for (int k = 0; k < nnzPerRow; ++k) {
            double value = coefficient(generator);
            if (value == 0.0) {
                value = 1.0;
            }
            triplets.push_back({row, column(generator), value});
        }
    }

    pdlp::CompiledLp problem;
    problem.matrix = pdlp::SparseMatrix::fromTriplets(rows, columns, std::move(triplets));

    problem.objective.resize(columns);
    problem.variableLower.assign(columns, 0.0);
    problem.variableUpper.assign(columns, 10.0);

    std::vector<double> reference(columns);
    for (int j = 0; j < columns; ++j) {
        problem.objective[j] = coefficient(generator);
        reference[j] = 10.0 * unit(generator);
    }

    std::vector<double> activity;
    problem.matrix.multiply(reference, activity);

    problem.rowLower.resize(rows);
    problem.rowUpper.resize(rows);
    for (int i = 0; i < rows; ++i) {
        // Mix equalities, one-sided rows and ranged rows.
        switch (i % 3) {
            case 0:
                problem.rowLower[i] = activity[i];
                problem.rowUpper[i] = activity[i];
                break;
            case 1:
                problem.rowLower[i] = -kInfinity;
                problem.rowUpper[i] = activity[i] + 1.0;
                break;
            default:
                problem.rowLower[i] = activity[i] - 1.0;
                problem.rowUpper[i] = activity[i] + 1.0;
                break;
        }
    }
    return problem;
}

int argOr(int argc, char** argv, int index, int fallback) {
    return index < argc ? std::atoi(argv[index]) : fallback;
}

}  // namespace

int main(int argc, char** argv) {
    const int rows = argOr(argc, argv, 1, 20000);
    const int columns = argOr(argc, argv, 2, 40000);
    const int nnzPerRow = argOr(argc, argv, 3, 10);
    const int iterationLimit = argOr(argc, argv, 4, 2000);
    const int threads = argOr(argc, argv, 5, 0);

    const auto buildStart = std::chrono::steady_clock::now();
    const pdlp::CompiledLp problem = makeInstance(rows, columns, nnzPerRow, 12345u);
    const double buildSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - buildStart).count();

    pdlp::PdlpOptions options;
    options.iterationLimit = iterationLimit;
    options.terminationCheckFrequency = 200;
    options.useFeasibilityPolishing = false;
    options.threadCount = threads;

    const pdlp::PdlpResult result = pdlp::PdlpSolver{}.solve(problem, options);

    const double nnz = static_cast<double>(problem.matrix.nonzeros());
    const double perIteration = result.iterations > 0
        ? result.solveTimeSeconds / static_cast<double>(result.iterations)
        : 0.0;

    std::cout << "rows            " << rows << '\n'
              << "columns         " << columns << '\n'
              << "nonzeros        " << nnz << '\n'
              << "build seconds   " << buildSeconds << '\n'
              << "status          " << pdlp::toString(result.status) << '\n'
              << "iterations      " << result.iterations << '\n'
              << "solve seconds   " << result.solveTimeSeconds << '\n'
              << "us / iteration  " << perIteration * 1e6 << '\n'
              << "Mnnz/s (2 SpMV) " << (2.0 * nnz / perIteration) / 1e6 << '\n'
              << "primal residual " << result.primalResidual << '\n'
              << "dual residual   " << result.dualResidual << '\n'
              << "relative gap    " << result.relativeGap << '\n'
              << "objective       " << result.primalObjective << '\n';
    return 0;
}
