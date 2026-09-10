#pragma once

#include <optional>
#include <string>
#include <vector>

namespace cli {

enum class Command {
    None,
    Help,
    Solve
};

struct SolveOptions {
    std::string modelPath;
    std::optional<std::string> solver;
    std::optional<double> timeLimitSeconds;
    std::optional<std::string> outputPath;
    bool help = false;
};

struct ParseResult {
    Command command = Command::None;
    SolveOptions solveOptions;
    bool success = false;
    bool isHelp = false;
    std::string errorMessage;
    std::string errorTitle;
    std::string errorDetails;
};

class ArgumentParser {
public:
    static ParseResult parse(int argc, const char* const argv[]);
    static std::string getRootHelp();
    static std::string getSolveHelp();

private:
    static bool isValidSolverName(const std::string& name);
};

}  // namespace cli
