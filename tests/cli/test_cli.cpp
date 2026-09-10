#include "cli.h"
#include "argument_parser.h"
#include "mps/mps_reader.h"
#include "presolve/presolver.h"
#include "postsolve/postsolver.h"
#include "solver/orchestrator.h"

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Helper to run cli::run with vector of string arguments
int runCli(const std::vector<std::string>& args, std::string& outStr, std::string& errStr) {
    std::vector<char*> argv;
    for (const auto& s : args) {
        argv.push_back(const_cast<char*>(s.c_str()));
    }
    std::ostringstream out;
    std::ostringstream err;
    int code = cli::run(static_cast<int>(argv.size()), argv.data(), out, err);
    outStr = out.str();
    errStr = err.str();
    return code;
}

void test_help_root() {
    std::string out, err;
    int code = runCli({"optimsolver", "--help"}, out, err);
    assert(code == 0);
    assert(out.find("OPTIMSOLVER") != std::string::npos);
    assert(out.find("Mathematical Optimization Engine") != std::string::npos);
    assert(out.find("Getting Started") != std::string::npos);
    assert(out.find("solve <model.mps>") != std::string::npos);
    assert(err.empty());

    code = runCli({"optimsolver", "-h"}, out, err);
    assert(code == 0);
    assert(out.find("OPTIMSOLVER") != std::string::npos);
    assert(out.find("Getting Started") != std::string::npos);

    std::cout << "[PASSED] test_help_root\n";
}

void test_help_solve() {
    std::string out, err;
    int code = runCli({"optimsolver", "solve", "--help"}, out, err);
    assert(code == 0);
    assert(out.find("OPTIMSOLVER") != std::string::npos);
    assert(out.find("Usage") != std::string::npos);
    assert(out.find("solve <model.mps>") != std::string::npos);
    assert(out.find("--solver") != std::string::npos);
    assert(out.find("--time-limit") != std::string::npos);
    assert(out.find("--output") != std::string::npos);
    assert(err.empty());

    code = runCli({"optimsolver", "solve", "-h"}, out, err);
    assert(code == 0);
    assert(out.find("OPTIMSOLVER") != std::string::npos);
    assert(out.find("solve <model.mps>") != std::string::npos);

    std::cout << "[PASSED] test_help_solve\n";
}

void test_no_arguments() {
    std::string out, err;
    int code = runCli({"optimsolver"}, out, err);
    assert(code == 0);
    assert(out.find("OPTIMSOLVER") != std::string::npos);
    assert(out.find("Mathematical Optimization Engine") != std::string::npos);
    assert(out.find("Getting Started") != std::string::npos);
    assert(err.empty());

    std::cout << "[PASSED] test_no_arguments\n";
}

void test_unknown_command() {
    std::string out, err;
    int code = runCli({"optimsolver", "inspect", "model.mps"}, out, err);
    assert(code != 0);
    assert(err.find("Unknown command 'inspect'") != std::string::npos);
    // Errors must remain clean and not print the giant mascot
    assert(err.find("Mathematical Optimization Engine") == std::string::npos);

    std::cout << "[PASSED] test_unknown_command\n";
}

void test_solve_missing_model_path() {
    std::string out, err;
    int code = runCli({"optimsolver", "solve"}, out, err);
    assert(code != 0);
    assert(err.find("Missing required model path") != std::string::npos);
    assert(err.find("Mathematical Optimization Engine") == std::string::npos);

    std::cout << "[PASSED] test_solve_missing_model_path\n";
}

void test_solve_extra_arguments() {
    std::string out, err;
    int code = runCli({"optimsolver", "solve", "model1.mps", "model2.mps"}, out, err);
    assert(code != 0);
    assert(err.find("Unexpected argument 'model2.mps'") != std::string::npos);

    std::cout << "[PASSED] test_solve_extra_arguments\n";
}

void test_solve_unknown_option() {
    std::string out, err;
    int code = runCli({"optimsolver", "solve", "model.mps", "--threads", "4"}, out, err);
    assert(code != 0);
    assert(err.find("Unknown option '--threads'") != std::string::npos);

    std::cout << "[PASSED] test_solve_unknown_option\n";
}

void test_solve_missing_option_values() {
    std::string out, err;
    int code = runCli({"optimsolver", "solve", "model.mps", "--solver"}, out, err);
    assert(code != 0);
    assert(err.find("Missing value for option '--solver'") != std::string::npos);

    code = runCli({"optimsolver", "solve", "model.mps", "--time-limit"}, out, err);
    assert(code != 0);
    assert(err.find("Missing value for option '--time-limit'") != std::string::npos);

    code = runCli({"optimsolver", "solve", "model.mps", "--output"}, out, err);
    assert(code != 0);
    assert(err.find("Missing value for option '--output'") != std::string::npos);

    // Flag immediately followed by another flag
    code = runCli({"optimsolver", "solve", "model.mps", "--solver", "--time-limit", "10"}, out, err);
    assert(code != 0);
    assert(err.find("Missing value for option '--solver'") != std::string::npos);

    std::cout << "[PASSED] test_solve_missing_option_values\n";
}

void test_solve_invalid_time_limit() {
    std::string out, err;
    int code = runCli({"optimsolver", "solve", "model.mps", "--time-limit", "abc"}, out, err);
    assert(code != 0);
    assert(err.find("Invalid time limit 'abc'") != std::string::npos);

    code = runCli({"optimsolver", "solve", "model.mps", "--time-limit", "-10"}, out, err);
    assert(code != 0);
    assert(err.find("Invalid time limit '-10'") != std::string::npos);

    code = runCli({"optimsolver", "solve", "model.mps", "--time-limit", "0"}, out, err);
    assert(code != 0);
    assert(err.find("Invalid time limit '0'") != std::string::npos);

    code = runCli({"optimsolver", "solve", "model.mps", "--time-limit", "nan"}, out, err);
    assert(code != 0);
    assert(err.find("Invalid time limit 'nan'") != std::string::npos);

    code = runCli({"optimsolver", "solve", "model.mps", "--time-limit", "inf"}, out, err);
    assert(code != 0);
    assert(err.find("Invalid time limit 'inf'") != std::string::npos);

    code = runCli({"optimsolver", "solve", "model.mps", "--time-limit", "-inf"}, out, err);
    assert(code != 0);
    assert(err.find("Invalid time limit '-inf'") != std::string::npos);

    std::cout << "[PASSED] test_solve_invalid_time_limit\n";
}

void test_solve_invalid_solver() {
    std::string out, err;
    int code = runCli({"optimsolver", "solve", "model.mps", "--solver", "cplex"}, out, err);
    assert(code != 0);
    assert(err.find("Invalid solver 'cplex'") != std::string::npos);

    std::cout << "[PASSED] test_solve_invalid_solver\n";
}

void test_argument_parser_direct() {
    const char* argv[] = {"optimsolver", "solve", "test.mps", "--solver", "pdlp", "--time-limit", "30.5", "--output", "out.txt"};
    auto res = cli::ArgumentParser::parse(9, argv);
    assert(res.success);
    assert(res.command == cli::Command::Solve);
    assert(res.solveOptions.modelPath == "test.mps");
    assert(res.solveOptions.solver.has_value() && *res.solveOptions.solver == "pdlp");
    assert(res.solveOptions.timeLimitSeconds.has_value() && *res.solveOptions.timeLimitSeconds == 30.5);
    assert(res.solveOptions.outputPath.has_value() && *res.solveOptions.outputPath == "out.txt");

    std::cout << "[PASSED] test_argument_parser_direct\n";
}

void test_solve_missing_model_file() {
    std::string out, err;
    int code = runCli({"optimsolver", "solve", "non_existent_path_12345.mps"}, out, err);
    assert(code != 0);
    assert(err.find("Failed to read MPS file") != std::string::npos);
    assert(err.find("Mathematical Optimization Engine") == std::string::npos);

    std::cout << "[PASSED] test_solve_missing_model_file\n";
}

std::string getTestModelPath(const std::string& relPath) {
#ifdef TEST_SOURCE_DIR
    return std::string(TEST_SOURCE_DIR) + "/" + relPath;
#else
    return relPath;
#endif
}

void test_solve_real_model_pipeline() {
    std::string mpsPath = getTestModelPath("tests/cli/simple_lp.mps");
    std::string out, err;
    int code = runCli({"optimsolver", "solve", mpsPath}, out, err);
    assert(code == 0);
    assert(out.find("OPTIMSOLVER") != std::string::npos);
    assert(out.find("Solving SIMPLE_LP") != std::string::npos);
    assert(out.find("Optimal") != std::string::npos);
    assert(out.find("Objective") != std::string::npos);
    assert(out.find("Engine") != std::string::npos);
    assert(out.find("dual_simplex") != std::string::npos);

    std::cout << "[PASSED] test_solve_real_model_pipeline\n";
}

void test_solve_infeasible_model() {
    std::string mpsPath = getTestModelPath("tests/mps/test_cases/01_basic_lp.mps");
    std::string out, err;
    int code = runCli({"optimsolver", "solve", mpsPath}, out, err);
    assert(code == 0);
    assert(out.find("OPTIMSOLVER") != std::string::npos);
    assert(out.find("Infeasible") != std::string::npos);
    assert(out.find("presolve proved the model infeasible") != std::string::npos ||
           out.find("Presolve proved the model infeasible") != std::string::npos);

    std::cout << "[PASSED] test_solve_infeasible_model\n";
}

void test_solve_output_file() {
    std::string mpsPath = getTestModelPath("tests/cli/simple_lp.mps");
    const std::string solPath = "test_cli_solution.txt";
    // Remove if exists
    std::remove(solPath.c_str());

    std::string out, err;
    int code = runCli({"optimsolver", "solve", mpsPath, "--output", solPath}, out, err);
    assert(code == 0);
    assert(err.empty());
    assert(out.find("Solution written to") != std::string::npos);

    // Verify file was written
    std::ifstream file(solPath);
    assert(file.is_open());
    std::string line;
    bool foundHeader = false;
    bool foundVariable = false;
    while (std::getline(file, line)) {
        if (line.rfind("# Status: optimal", 0) == 0) {
            foundHeader = true;
        }
        if (line.find("X1 ") != std::string::npos || line.find("X2 ") != std::string::npos) {
            foundVariable = true;
        }
        // Machine-readable check: no ANSI codes in output file
        assert(line.find("\033[") == std::string::npos);
    }
    file.close();
    std::remove(solPath.c_str());

    assert(foundHeader);
    assert(foundVariable);

    std::cout << "[PASSED] test_solve_output_file\n";
}

void test_solve_with_forced_engines() {
    std::string mpsPath = getTestModelPath("tests/cli/simple_lp.mps");
    std::string out, err;
    // PDLP
    int code = runCli({"optimsolver", "solve", mpsPath, "--solver", "pdlp"}, out, err);
    assert(code == 0);
    assert(out.find("pdlp") != std::string::npos);

    // Dual Simplex
    code = runCli({"optimsolver", "solve", mpsPath, "--solver", "dual_simplex"}, out, err);
    assert(code == 0);
    assert(out.find("dual_simplex") != std::string::npos);

    std::cout << "[PASSED] test_solve_with_forced_engines\n";
}

void test_single_presolve_integration() {
    std::string mpsPath = getTestModelPath("tests/cli/presolve_reduction.mps");
    const std::string solPath = "test_presolve_reduction_solution.txt";
    std::remove(solPath.c_str());

    std::string out, err;
    int code = runCli({"optimsolver", "solve", mpsPath, "--output", solPath}, out, err);
    assert(code == 0);
    assert(err.empty());

    // 1. Output must reflect that the original model has 3 variables and reduced model has 2 variables
    assert(out.find("3 variables") != std::string::npos);
    assert(out.find("2 variables") != std::string::npos);
    assert(out.find("Optimal") != std::string::npos);
    assert(out.find("Objective") != std::string::npos && out.find("28") != std::string::npos);

    // 2. Output solution file must reconstruct all 3 original-space variables
    std::ifstream file(solPath);
    assert(file.is_open());
    std::string line;
    bool foundX1 = false, foundX2 = false, foundX3 = false;
    double x1Val = 0.0, x2Val = 0.0, x3Val = 0.0;
    while (std::getline(file, line)) {
        if (line.rfind("X1 ", 0) == 0) {
            foundX1 = true;
            x1Val = std::stod(line.substr(3));
        } else if (line.rfind("X2 ", 0) == 0) {
            foundX2 = true;
            x2Val = std::stod(line.substr(3));
        } else if (line.rfind("X3 ", 0) == 0) {
            foundX3 = true;
            x3Val = std::stod(line.substr(3));
        }
    }
    file.close();
    std::remove(solPath.c_str());

    assert(foundX1 && foundX2 && foundX3);
    assert(std::abs(x1Val - 4.0) < 1e-4);
    assert(std::abs(x2Val - 0.0) < 1e-4);
    assert(std::abs(x3Val - 5.0) < 1e-4);

    // 3. Directly verify the architectural contract:
    // Solve original model -> validate -> classify original -> presolve ONCE -> solveReduced(presolved.model, classification) -> postsolve with SAME presolveResult.
    mps::MpsReader reader;
    model::Model model = reader.read(mpsPath);
    assert(model.validate());
    assert(model.variables.size() == 3);

    solver::Classification classification = solver::classify(model);

    presolve::Presolver presolver;
    presolve::PresolveResult presolveRes = presolver.run(model);
    assert(!presolveRes.infeasible);
    assert(presolveRes.model.variables.size() == 2);

    // solveReduced takes reduced model and original classification
    solver::SolveResult reducedResult = solver::solveReduced(presolveRes.model, classification);
    assert(reducedResult.status == solver::SolveStatus::Optimal);
    assert(reducedResult.variableValues.size() == 2);
    assert(reducedResult.reducedVariableCount == 2);

    // postsolver reconstructs to original space using SAME presolveResult
    postsolve::Postsolver postsolver;
    postsolve::PostsolveResult postsolveRes =
        postsolver.process(model, presolveRes, reducedResult.variableValues);
    assert(postsolveRes.isSuccess());
    assert(postsolveRes.primalSolution.size() == 3);
    assert(std::abs(postsolveRes.originalObjectiveValue - 28.0) < 1e-4);
    assert(std::abs(postsolveRes.primalSolution[0] - 4.0) < 1e-4);
    assert(std::abs(postsolveRes.primalSolution[1] - 0.0) < 1e-4);
    assert(std::abs(postsolveRes.primalSolution[2] - 5.0) < 1e-4);

    std::cout << "[PASSED] test_single_presolve_integration\n";
}

void test_non_tty_no_ansi() {
    std::string out, err;
    runCli({"optimsolver", "--help"}, out, err);
    assert(out.find("\033[") == std::string::npos);

    out.clear();
    err.clear();
    std::string mpsPath = getTestModelPath("tests/cli/simple_lp.mps");
    runCli({"optimsolver", "solve", mpsPath}, out, err);
    assert(out.find("\033[") == std::string::npos);

    std::cout << "[PASSED] test_non_tty_no_ansi\n";
}

void test_binary_execution() {
#ifdef OPTIMSOLVER_BIN_PATH
    std::string binPath = OPTIMSOLVER_BIN_PATH;
    std::string mpsPath = getTestModelPath("tests/cli/simple_lp.mps");
    // 1. Test help on real binary
    std::string cmdHelp = binPath + " --help > /dev/null 2>&1";
    int ret = std::system(cmdHelp.c_str());
    assert(ret == 0);

    // 2. Test root binary without args
    std::string cmdRoot = binPath + " > /dev/null 2>&1";
    ret = std::system(cmdRoot.c_str());
    assert(ret == 0);

    // 3. Test solve command on real binary
    std::string cmdSolve = binPath + " solve " + mpsPath + " > /dev/null 2>&1";
    ret = std::system(cmdSolve.c_str());
    assert(ret == 0);

    // 4. Test invalid arg on real binary
    std::string cmdInvalid = binPath + " solve " + mpsPath + " --solver bogus > /dev/null 2>&1";
    ret = std::system(cmdInvalid.c_str());
    assert(ret != 0);

    std::cout << "[PASSED] test_binary_execution\n";
#endif
}

}  // namespace

int main() {
    test_help_root();
    test_help_solve();
    test_no_arguments();
    test_unknown_command();
    test_solve_missing_model_path();
    test_solve_extra_arguments();
    test_solve_unknown_option();
    test_solve_missing_option_values();
    test_solve_invalid_time_limit();
    test_solve_invalid_solver();
    test_argument_parser_direct();
    test_solve_missing_model_file();
    test_solve_real_model_pipeline();
    test_solve_infeasible_model();
    test_solve_output_file();
    test_solve_with_forced_engines();
    test_single_presolve_integration();
    test_non_tty_no_ansi();
    test_binary_execution();

    std::cout << "All CLI tests passed successfully!\n";
    return 0;
}
