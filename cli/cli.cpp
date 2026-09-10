#include "cli.h"
#include "argument_parser.h"
#include "mascot.h"

#include "model/model.h"
#include "mps/mps_reader.h"
#include "presolve/presolver.h"
#include "postsolve/postsolver.h"
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

        // 3. Classify original model
        const solver::Classification classification = solver::classify(model);

        // 4. Presolve
        presolve::Presolver presolver;
        presolve::PresolveResult presolveResult;
        try {
            presolveResult = presolver.run(model);
        } catch (const std::exception& ex) {
            printError(err, "Presolve failed", ex.what());
            return 1;
        }

        if (presolveResult.infeasible) {
            TerminalStyle style = TerminalStyle::forStream(out);
            SolveDashboardInfo dash;
            dash.problemName = model.name;
            dash.originalVars = model.variables.size();
            dash.originalCons = model.constraints.size();
            dash.presolveInfeasible = true;
            dash.engineName = "presolve";
            printSolveDashboard(out, dash, style);

            SolveResultInfo resInfo;
            resInfo.status = solver::SolveStatus::Infeasible;
            resInfo.engine = "presolve";
            resInfo.message = "Presolve proved the model infeasible";
            printSolveResult(out, resInfo, style);
            return 0;
        }

        // 5. Configure solver options
        solver::SolverOptions solverOptions;
        if (opts.solver.has_value()) {
            solverOptions.forceEngine = solver::parseEngine(*opts.solver);
        }
        if (opts.timeLimitSeconds.has_value()) {
            solverOptions.timeLimitSeconds = *opts.timeLimitSeconds;
        }

        // 6. Solve the reduced model
        solver::SolveResult solveResult;
        try {
            solveResult = solver::solveReduced(presolveResult.model, classification, solverOptions);
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

        // 7. Handle Infeasible / Unbounded from engine
        if (solveResult.status == solver::SolveStatus::Infeasible ||
            solveResult.status == solver::SolveStatus::Unbounded) {
            TerminalStyle style = TerminalStyle::forStream(out);
            SolveDashboardInfo dash;
            dash.problemName = model.name;
            dash.originalVars = model.variables.size();
            dash.originalCons = model.constraints.size();
            dash.reducedVars = solveResult.reducedVariableCount;
            dash.reducedCons = solveResult.reducedConstraintCount;
            dash.engineName = solver::toString(solveResult.executedEngine);
            printSolveDashboard(out, dash, style);

            SolveResultInfo resInfo;
            resInfo.status = solveResult.status;
            resInfo.engine = solver::toString(solveResult.executedEngine);
            resInfo.message = solveResult.message;
            resInfo.iterations = solveResult.iterations;
            resInfo.nodeCount = solveResult.nodeCount;
            resInfo.solveSeconds = solveResult.solveSeconds;
            printSolveResult(out, resInfo, style);
            return 0;
        }

        // 8. For Optimal or LimitReached, run postsolve
        postsolve::Postsolver postsolver;
        postsolve::PostsolveResult postsolveResult =
            postsolver.process(model, presolveResult, solveResult.variableValues);

        if (!postsolveResult.isSuccess()) {
            printError(err, "Postsolve error", postsolveResult.errorMessage);
            return 1;
        }

        // 9. Output solve dashboard and summary
        TerminalStyle style = TerminalStyle::forStream(out);
        SolveDashboardInfo dash;
        dash.problemName = model.name;
        dash.originalVars = model.variables.size();
        dash.originalCons = model.constraints.size();
        dash.reducedVars = solveResult.reducedVariableCount;
        dash.reducedCons = solveResult.reducedConstraintCount;
        dash.engineName = solver::toString(solveResult.executedEngine);
        printSolveDashboard(out, dash, style);

        SolveResultInfo resInfo;
        resInfo.status = solveResult.status;
        resInfo.objective = postsolveResult.originalObjectiveValue;
        resInfo.hasObjective = true;
        resInfo.engine = solver::toString(solveResult.executedEngine);
        resInfo.iterations = solveResult.iterations;
        resInfo.nodeCount = solveResult.nodeCount;
        resInfo.solveSeconds = solveResult.solveSeconds;
        resInfo.message = solveResult.message;
        printSolveResult(out, resInfo, style);

        // 10. Write solution file if requested
        if (opts.outputPath.has_value()) {
            std::ofstream outFile(*opts.outputPath);
            if (!outFile.is_open()) {
                printError(err, "Output error", "Unable to open output file '" + *opts.outputPath + "' for writing.");
                return 1;
            }

            outFile << "# Solution for " << (model.name.empty() ? "model" : model.name) << "\n";
            outFile << "# Status: " << solver::toString(solveResult.status) << "\n";
            outFile << "# Objective: " << std::setprecision(9) << postsolveResult.originalObjectiveValue << "\n";
            for (std::size_t i = 0; i < model.variables.size(); ++i) {
                const std::string& name = model.variables[i].name.empty() ? ("x" + std::to_string(i)) : model.variables[i].name;
                outFile << name << " " << std::setprecision(9) << postsolveResult.primalSolution[i] << "\n";
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
