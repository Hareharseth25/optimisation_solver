#pragma once

#include "solver/orchestrator.h"

#include <iostream>
#include <string>

namespace cli {

// Terminal styling supporting subtle ANSI colors with TTY and NO_COLOR detection.
struct TerminalStyle {
    bool color = false;

    static TerminalStyle forStream(const std::ostream& stream);

    std::string reset() const;
    std::string bold() const;
    std::string dim() const;
    std::string cyan() const;
    std::string boldCyan() const;
    std::string green() const;
    std::string boldGreen() const;
    std::string yellow() const;
    std::string boldYellow() const;
    std::string red() const;
    std::string boldRed() const;
};

// Mascot and header rendering
void printMascot(std::ostream& out, const TerminalStyle& style);
void printBanner(std::ostream& out);

// High-level CLI screen renderers
void printWelcome(std::ostream& out);
void printSolveHelp(std::ostream& out);

// Solve dashboard and result presentation
struct SolveDashboardInfo {
    std::string problemName;
    std::size_t originalVars = 0;
    std::size_t originalCons = 0;
    std::size_t reducedVars = 0;
    std::size_t reducedCons = 0;
    bool presolveInfeasible = false;
    std::string engineName;
};

void printSolveDashboard(std::ostream& out, const SolveDashboardInfo& info, const TerminalStyle& style);

struct SolveResultInfo {
    solver::SolveStatus status = solver::SolveStatus::Optimal;
    double objective = 0.0;
    bool hasObjective = false;
    int iterations = 0;
    int nodeCount = 0;
    double solveSeconds = 0.0;
    std::string engine;
    std::string message;
};

void printSolveResult(std::ostream& out, const SolveResultInfo& result, const TerminalStyle& style);
void printSolutionWritten(std::ostream& out, const std::string& path, const TerminalStyle& style);

// Professional error output
void printError(std::ostream& err, const std::string& title, const std::string& details = "");

}  // namespace cli
