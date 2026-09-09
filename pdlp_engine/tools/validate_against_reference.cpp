// Cross-validation driver: generates a family of random LPs, solves each with
// the PDLP engine, and writes both the instance and the engine's answer to disk
// so an independent reference solver can check them.
//
//   ./pdlp_validate <outputDirectory> <instanceCount>

#include "pdlp/pdlp_solver.h"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <algorithm>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();

struct Instance {
    pdlp::CompiledLp problem;
    std::vector<pdlp::MatrixTriplet> triplets;
};

// Deliberately varied: degenerate (many redundant equalities), badly scaled,
// and free-variable instances all appear in the family.
Instance makeInstance(int rows, int columns, int nnzPerRow, unsigned seed, int flavour) {
    std::mt19937 generator(seed);
    std::uniform_real_distribution<double> coefficient(-1.0, 1.0);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::uniform_int_distribution<int> pickColumn(0, columns - 1);

    Instance instance;
    // Flavour 3 is a staircase: each row couples a local block of columns to the
    // previous block, the shape multiperiod planning and supply-chain models
    // take. Its sparsity is banded rather than uniformly random, which is much
    // closer to the industrial case than a random scatter.
    const int blockColumns = std::max(columns / std::max(rows / 4, 1), 8);
    for (int row = 0; row < rows; ++row) {
        // Scale rows across several orders of magnitude in the ill-conditioned
        // flavour so the preconditioner is actually exercised.
        const double rowScale = (flavour == 2)
            ? std::pow(10.0, static_cast<double>(row % 7) - 3.0)
            : 1.0;
        for (int k = 0; k < nnzPerRow; ++k) {
            double value = coefficient(generator) * rowScale;
            if (value == 0.0) {
                value = rowScale;
            }
            int column = 0;
            if (flavour == 3) {
                const int base = (row * blockColumns) / std::max(rows, 1);
                const int span = std::min(2 * blockColumns, columns);
                column = (base + pickColumn(generator) % span) % columns;
            } else {
                column = pickColumn(generator);
            }
            instance.triplets.push_back({row, column, value});
        }
    }

    pdlp::CompiledLp& problem = instance.problem;
    problem.matrix = pdlp::SparseMatrix::fromTriplets(rows, columns, instance.triplets);

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

    if (flavour == 4) {
        // Genuinely contradictory: give two rows the SAME coefficient vector and
        // then demand a'x >= t + 1 and a'x <= t - 1. Requiring different rows to
        // disagree is not infeasible, which an earlier version of this generator
        // got wrong.
        problem.rowLower.assign(rows, -kInfinity);
        problem.rowUpper.assign(rows, kInfinity);
        std::vector<pdlp::MatrixTriplet> paired;
        for (const auto& entry : instance.triplets) {
            if (entry.row % 2 == 0) {
                paired.push_back(entry);
                paired.push_back({entry.row + 1, entry.column, entry.value});
            }
        }
        instance.triplets = paired;
        problem.matrix = pdlp::SparseMatrix::fromTriplets(rows, columns, paired);
        problem.matrix.multiply(reference, activity);
        for (int i = 0; i + 1 < rows; i += 2) {
            problem.rowLower[i] = activity[i] + 1.0;
            problem.rowUpper[i + 1] = activity[i] - 1.0;
        }
        for (int j = 0; j < columns; ++j) {
            problem.variableLower[j] = 0.0;
            problem.variableUpper[j] = 10.0;
        }
        return instance;
    }
    if (flavour == 5) {
        // Unbounded in the clearest possible way: the last column appears in no
        // row at all, has a negative cost and no upper bound, so the objective
        // decreases without limit while every constraint stays satisfied.
        for (int i = 0; i < rows; ++i) {
            problem.rowLower[i] = activity[i] - 1.0;
            problem.rowUpper[i] = activity[i] + 1.0;
        }
        instance.triplets.erase(
            std::remove_if(
                instance.triplets.begin(), instance.triplets.end(),
                [columns](const pdlp::MatrixTriplet& e) {
                    return e.column == columns - 1;
                }),
            instance.triplets.end());
        problem.matrix = pdlp::SparseMatrix::fromTriplets(rows, columns, instance.triplets);
        problem.objective[columns - 1] = -1.0;
        problem.variableLower[columns - 1] = 0.0;
        problem.variableUpper[columns - 1] = kInfinity;
        return instance;
    }

    problem.rowLower.resize(rows);
    problem.rowUpper.resize(rows);
    for (int i = 0; i < rows; ++i) {
        const int kind = (flavour == 1) ? 0 : (i % 3);
        switch (kind) {
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
    return instance;
}

void writeValue(std::FILE* file, double value) {
    if (value == kInfinity) {
        std::fprintf(file, "inf ");
    } else if (value == -kInfinity) {
        std::fprintf(file, "-inf ");
    } else {
        std::fprintf(file, "%.17g ", value);
    }
}

void writeInstance(const std::string& path, const Instance& instance) {
    std::FILE* file = std::fopen(path.c_str(), "w");
    const pdlp::CompiledLp& problem = instance.problem;
    std::fprintf(
        file,
        "%d %d %zu\n",
        problem.numRows(),
        problem.numColumns(),
        instance.triplets.size()
    );
    std::fprintf(file, "%.17g\n", problem.objectiveOffset);
    for (double value : problem.objective) { writeValue(file, value); }
    std::fprintf(file, "\n");
    for (double value : problem.variableLower) { writeValue(file, value); }
    std::fprintf(file, "\n");
    for (double value : problem.variableUpper) { writeValue(file, value); }
    std::fprintf(file, "\n");
    for (double value : problem.rowLower) { writeValue(file, value); }
    std::fprintf(file, "\n");
    for (double value : problem.rowUpper) { writeValue(file, value); }
    std::fprintf(file, "\n");
    for (const auto& entry : instance.triplets) {
        std::fprintf(file, "%d %d %.17g\n", entry.row, entry.column, entry.value);
    }
    std::fclose(file);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string directory = argc > 1 ? argv[1] : ".";
    const int count = argc > 2 ? std::atoi(argv[2]) : 8;

    std::printf("%-10s %6s %6s %10s %14s %12s %12s %10s\n",
                "instance", "rows", "cols", "status", "objective", "primalRes",
                "gap", "seconds");

    for (int index = 0; index < count; ++index) {
        const int flavour = index % 6;
        const int scale = 1 + index / 4;
        const int rows = 200 + 220 * scale + 37 * index;
        const int columns = 300 + 300 * scale + 51 * index;
        const int nnzPerRow = 5 + (index % 3) * 3;
        const Instance instance =
            makeInstance(rows, columns, nnzPerRow, 1000u + index, flavour);

        const std::string path = directory + "/instance_" + std::to_string(index) + ".lp.txt";
        writeInstance(path, instance);

        pdlp::PdlpOptions options;
        options.iterationLimit = 300000;
        options.primalTolerance = 1e-8;
        options.dualTolerance = 1e-8;
        options.gapTolerance = 1e-8;
        options.terminationCheckFrequency = 100;
        options.timeLimitSeconds = 60.0;

        const pdlp::PdlpResult result = pdlp::PdlpSolver{}.solve(instance.problem, options);

        std::printf("%-10d %6d %6d %10s %14.8g %12.3g %12.3g %10.3f\n",
                    index, rows, columns, pdlp::toString(result.status),
                    result.primalObjective, result.primalResidual,
                    result.relativeGap, result.solveTimeSeconds);

        std::FILE* answer = std::fopen(
            (directory + "/instance_" + std::to_string(index) + ".answer.txt").c_str(), "w");
        std::fprintf(answer, "%s %.17g %.17g %.17g %.17g %.17g\n",
                     pdlp::toString(result.status), result.primalObjective,
                     result.primalResidual, result.dualResidual, result.relativeGap,
                     result.solveTimeSeconds);
        for (double value : result.primal) {
            std::fprintf(answer, "%.17g ", value);
        }
        std::fprintf(answer, "\n");
        std::fclose(answer);
    }
    return 0;
}
