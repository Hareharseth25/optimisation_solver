#include "cli.h"
#include "argument_parser.h"
#include "mascot.h"

#include "model/model.h"
#include "mps/mps_reader.h"
#include "solver/orchestrator.h"

#include <fstream>
#include <iomanip>

namespace cli {

int run(int argc, char* argv[], std::ostream& out, std::ostream& err) {
    ParseResult parseResult = ArgumentParser::parse(argc, argv);

    if (!parseResult.success) {
        printError(err, parseResult.errorTitle, parseResult.errorDetails);
        return 1;
    }

    if (parseResult.isHelp) {
        if (parseResult.command == Command::Solve) {
            printSolveHelp(out);
        } else {
            printWelcome(out);
        }
        return 0;
    }

    if (parseResult.command == Command::Solve) {
        const auto& opts = parseResult.solveOptions;

        // 1. Parse MPS file
        mps::MpsReader reader;
        model::Model model;
        try {
            model = reader.read(opts.modelPath);
        } catch (const std::exception& ex) {
            printError(err, "Failed to read MPS file", "Path: " + opts.modelPath + "\n" + ex.what());
            return 1;
        }

        // 2. Validate original Model IR
        if (!model.validate()) {
            printError(err, "Invalid model", "Model failed structural validation.");
            return 1;
        }

        // 3. Configure solver options
        solver::SolverOptions solverOptions;
        if (opts.solver.has_value()) {
            solverOptions.forceEngine = solver::parseEngine(*opts.solver);
        }
        if (opts.timeLimitSeconds.has_value()) {
            solverOptions.timeLimitSeconds = *opts.timeLimitSeconds;
        }

        // 4. Run the common pipeline, including original-model reconstruction
        solver::SolveResult solveResult;
        try {
            solveResult = solver::solve(model, solverOptions);
        } catch (const std::exception& ex) {
            printError(err, "Solver failed", ex.what());
            return 1;
        }

        if (solveResult.status == solver::SolveStatus::InvalidModel) {
            printError(err, "Solver error: Invalid model", solveResult.message);
            return 1;
        }
        if (solveResult.status == solver::SolveStatus::NumericalFailure) {
            printError(err, "Solver error: Numerical failure", solveResult.message);
            return 1;
        }
        if (solveResult.status == solver::SolveStatus::Unsupported) {
            printError(err, "Solver error: Unsupported problem", solveResult.message);
            return 1;
        }

        // 5. Output solve dashboard and summary
        TerminalStyle style = TerminalStyle::forStream(out);
        SolveDashboardInfo dash;
        dash.problemName = model.name;
        dash.originalVars = model.variables.size();
        dash.originalCons = model.constraints.size();
        dash.reducedVars = solveResult.reducedVariableCount;
        dash.reducedCons = solveResult.reducedConstraintCount;
        dash.presolveInfeasible = solveResult.engine == solver::Engine::Infeasible;
        dash.engineName = dash.presolveInfeasible ? "presolve"
                                                : solver::toString(solveResult.executedEngine);
        printSolveDashboard(out, dash, style);

        SolveResultInfo resInfo;
        resInfo.status = solveResult.status;
        resInfo.objective = solveResult.objectiveValue;
        resInfo.hasObjective = solveResult.hasPrimal;
        resInfo.engine = dash.engineName;
        resInfo.iterations = solveResult.iterations;
        resInfo.nodeCount = solveResult.nodeCount;
        resInfo.solveSeconds = solveResult.solveSeconds;
        resInfo.message = solveResult.message;
        printSolveResult(out, resInfo, style);

        if (!solveResult.hasPrimal) {
            if (solveResult.status == solver::SolveStatus::LimitReached) {
                out << "  No feasible solution available.\n";
                if (opts.outputPath.has_value()) {
                    printError(err, "Output unavailable", "No feasible solution was found to write.");
                    return 1;
                }
            }
            return 0;
        }
        if (solveResult.hasDuals) {
            out << "  Duals available: " << solveResult.constraintDuals.size()
                << " shadow prices, " << solveResult.reducedCosts.size() << " reduced costs\n";
        } else {
            out << "  Duals unavailable: " << solveResult.dualsUnavailableReason << "\n";
        }
        if (!solveResult.integralityRespected)
            out << "  Continuous relaxation; returned values are not integer-feasible.\n";

        // 6. Write original-model values and sensitivities if requested
        if (opts.outputPath.has_value()) {
            std::ofstream outFile(*opts.outputPath);
            if (!outFile.is_open()) {
                printError(err, "Output error", "Unable to open output file '" + *opts.outputPath + "' for writing.");
                return 1;
            }

            outFile << "# Solution for " << (model.name.empty() ? "model" : model.name) << "\n";
            outFile << "# Status: " << solver::toString(solveResult.status) << "\n";
            outFile << "# Objective: " << std::setprecision(9) << solveResult.objectiveValue << "\n";
            for (std::size_t i = 0; i < model.variables.size(); ++i) {
                const std::string& name = model.variables[i].name.empty() ? ("x" + std::to_string(i)) : model.variables[i].name;
                outFile << name << " " << std::setprecision(9) << solveResult.variableValues[i] << "\n";
            }
            if (solveResult.hasDuals) {
                for (std::size_t i = 0; i < model.constraints.size(); ++i) {
                    const auto name = model.constraints[i].name.empty()
                        ? "c" + std::to_string(i) : model.constraints[i].name;
                    outFile << "# Dual " << name << " " << solveResult.constraintDuals[i] << "\n";
                }
                for (std::size_t j = 0; j < model.variables.size(); ++j) {
                    const auto name = model.variables[j].name.empty()
                        ? "x" + std::to_string(j) : model.variables[j].name;
                    outFile << "# Reduced cost " << name << " " << solveResult.reducedCosts[j] << "\n";
                }
            } else {
                outFile << "# Duals unavailable: " << solveResult.dualsUnavailableReason << "\n";
            }
            outFile.close();
            if (!outFile) {
                printError(err, "Output error", "Failed to write complete solution to '" + *opts.outputPath + "'.");
                return 1;
            }
            printSolutionWritten(out, *opts.outputPath, style);
        }

        return 0;
    }

    printError(err, "Unhandled command", "");
    return 1;
}

}  // namespace cli
