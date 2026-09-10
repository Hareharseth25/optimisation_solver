#include "mascot.h"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <unistd.h>
#include <vector>

namespace cli {

namespace {

bool isTty(const std::ostream& stream) {
    if (&stream == &std::cout) {
        return isatty(STDOUT_FILENO) != 0;
    }
    if (&stream == &std::cerr) {
        return isatty(STDERR_FILENO) != 0;
    }
    return false;
}

bool hasNoColor() {
    const char* env = std::getenv("NO_COLOR");
    return env != nullptr && env[0] != '\0';
}

}  // namespace

TerminalStyle TerminalStyle::forStream(const std::ostream& stream) {
    TerminalStyle style;
    style.color = isTty(stream) && !hasNoColor();
    return style;
}

std::string TerminalStyle::reset() const {
    return color ? "\033[0m" : "";
}

std::string TerminalStyle::bold() const {
    return color ? "\033[1m" : "";
}

std::string TerminalStyle::dim() const {
    return color ? "\033[2m" : "";
}

std::string TerminalStyle::cyan() const {
    return color ? "\033[36m" : "";
}

std::string TerminalStyle::boldCyan() const {
    return color ? "\033[1;36m" : "";
}

std::string TerminalStyle::green() const {
    return color ? "\033[32m" : "";
}

std::string TerminalStyle::boldGreen() const {
    return color ? "\033[1;32m" : "";
}

std::string TerminalStyle::yellow() const {
    return color ? "\033[33m" : "";
}

std::string TerminalStyle::boldYellow() const {
    return color ? "\033[1;33m" : "";
}

std::string TerminalStyle::red() const {
    return color ? "\033[31m" : "";
}

std::string TerminalStyle::boldRed() const {
    return color ? "\033[1;31m" : "";
}

void printMascot(std::ostream& out, const TerminalStyle& s) {
    out << "        " << s.boldCyan() << "(x)" << s.dim() << "───" << s.boldCyan() << "(y)" << s.reset() << "       \n";
    out << "         " << s.dim() << "│  " << s.boldCyan() << "∇" << s.dim() << "  │" << s.reset() << "        "
        << s.boldCyan() << "OPTIMSOLVER" << s.reset() << "\n";
    out << "       " << s.dim() << "╭─┴─────┴─╮" << s.reset() << "      "
        << s.dim() << "Mathematical Optimization Engine" << s.reset() << "\n";
    out << "       " << s.dim() << "│  " << s.boldCyan() << "◈   ◈" << s.dim() << "  │" << s.reset() << "      \n";
    out << "       " << s.dim() << "│ " << s.bold() << "min f(x)" << s.dim() << "│" << s.reset() << "      "
        << s.dim() << "Problem Families:  " << s.reset()
        << s.bold() << "LP" << s.reset() << " " << s.dim() << "·" << s.reset() << " "
        << s.bold() << "QP" << s.reset() << " " << s.dim() << "·" << s.reset() << " "
        << s.bold() << "MILP" << s.reset() << "\n";
    out << "       " << s.dim() << "│ ─────── │" << s.reset() << "      "
        << s.dim() << "Solver Engines:    " << s.reset()
        << "PDLP " << s.dim() << "·" << s.reset() << " Dual Simplex "
        << s.dim() << "·" << s.reset() << " Branch & Cut "
        << s.dim() << "·" << s.reset() << " QP\n";
    out << "       " << s.dim() << "╰──┬───┬──╯" << s.reset() << "      \n";
    out << "         " << s.dim() << "═╧═══╧═" << s.reset() << "        \n";
}

void printBanner(std::ostream& out) {
    TerminalStyle s = TerminalStyle::forStream(out);
    printMascot(out, s);
    out << "\n";
}

void printWelcome(std::ostream& out) {
    TerminalStyle s = TerminalStyle::forStream(out);
    printMascot(out, s);
    out << "\n";
    out << s.bold() << "Getting Started" << s.reset() << "\n";
    out << "  optimsolver solve <model.mps>\n\n";
    out << s.bold() << "Commands" << s.reset() << "\n";
    out << "  " << s.bold() << "solve" << s.reset() << " <model.mps>       Solve an optimization problem in MPS format\n\n";
    out << s.bold() << "Options" << s.reset() << "\n";
    out << "  -h, --help              Show this help message\n\n";
    out << s.dim() << "Run 'optimsolver solve --help' for options specific to the solve command." << s.reset() << "\n";
}

void printSolveHelp(std::ostream& out) {
    TerminalStyle s = TerminalStyle::forStream(out);
    printMascot(out, s);
    out << "\n";
    out << s.bold() << "Usage" << s.reset() << "\n";
    out << "  optimsolver solve <model.mps> [options]\n\n";
    out << s.bold() << "Arguments" << s.reset() << "\n";
    out << "  <model.mps>             Path to input problem file in MPS format (required)\n\n";
    out << s.bold() << "Options" << s.reset() << "\n";
    out << "  --solver <name>         Force a specific solver engine:\n";
    out << "                          pdlp, dual_simplex, branch_and_cut, qp\n";
    out << "  --time-limit <seconds>  Maximum solve time budget in seconds (positive number)\n";
    out << "  --output <file>         Write reconstructed original-space solution to file\n";
    out << "  -h, --help              Show this help message\n\n";
    out << s.bold() << "Examples" << s.reset() << "\n";
    out << "  optimsolver solve model.mps\n";
    out << "  optimsolver solve model.mps --solver dual_simplex\n";
    out << "  optimsolver solve model.mps --time-limit 60 --output solution.txt\n";
}

void printSolveDashboard(std::ostream& out, const SolveDashboardInfo& info, const TerminalStyle& s) {
    std::string vStr = (info.originalVars == 1) ? "1 variable" : (std::to_string(info.originalVars) + " variables");
    std::string cStr = (info.originalCons == 1) ? "1 constraint" : (std::to_string(info.originalCons) + " constraints");
    std::string modelPlain = "Model      " + vStr + " · " + cStr;
    std::string modelStyled = s.dim() + "Model" + s.reset() + "      " + s.bold() + std::to_string(info.originalVars) + s.reset() +
                              " " + (info.originalVars == 1 ? "variable" : "variables") + " " + s.dim() + "·" + s.reset() + " " +
                              s.bold() + std::to_string(info.originalCons) + s.reset() + " " + (info.originalCons == 1 ? "constraint" : "constraints");

    std::string redPlain;
    std::string redStyled;
    if (info.presolveInfeasible) {
        redPlain = "Reduced    infeasible in presolve";
        redStyled = s.dim() + "Reduced" + s.reset() + "    infeasible in presolve";
    } else {
        std::string rvStr = (info.reducedVars == 1) ? "1 variable" : (std::to_string(info.reducedVars) + " variables");
        std::string rcStr = (info.reducedCons == 1) ? "1 constraint" : (std::to_string(info.reducedCons) + " constraints");
        redPlain = "Reduced    " + rvStr + " · " + rcStr;
        redStyled = s.dim() + "Reduced" + s.reset() + "    " + s.bold() + std::to_string(info.reducedVars) + s.reset() +
                    " " + (info.reducedVars == 1 ? "variable" : "variables") + " " + s.dim() + "·" + s.reset() + " " +
                    s.bold() + std::to_string(info.reducedCons) + s.reset() + " " + (info.reducedCons == 1 ? "constraint" : "constraints");
    }

    std::string enginePlain = "Engine     " + info.engineName;
    std::string engineStyled = s.dim() + "Engine" + s.reset() + "     " + info.engineName;

    std::string probName = info.problemName.empty() ? "(unnamed)" : info.problemName;
    std::string solvingPlain = "Solving " + probName;
    std::string solvingStyled = "Solving " + s.bold() + probName + s.reset();

    struct BoxLine {
        std::string styled;
        std::string plain;
    };

    std::vector<BoxLine> lines = {
        {"", ""},
        {solvingStyled, solvingPlain},
        {"", ""},
        {modelStyled, modelPlain},
        {redStyled, redPlain},
        {engineStyled, enginePlain},
        {"", ""}
    };

    auto visualWidth = [](const std::string& str) -> std::size_t {
        std::size_t width = 0;
        for (std::size_t i = 0; i < str.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(str[i]);
            if ((c & 0xC0) != 0x80) {
                ++width;
            }
        }
        return width;
    };

    std::size_t maxPlainLen = 0;
    for (const auto& line : lines) {
        maxPlainLen = std::max(maxPlainLen, visualWidth(line.plain));
    }

    std::size_t innerWidth = std::max(static_cast<std::size_t>(50), maxPlainLen);
    std::size_t totalWidth = innerWidth + 6;

    // Use Unicode box drawing horizontal dash '─'
    std::string hDash;
    for (std::size_t i = 0; i < totalWidth - 16; ++i) {
        hDash += "─";
    }
    std::string topBorder = s.dim() + "╭─" + s.reset() + " " + s.boldCyan() + "OPTIMSOLVER" + s.reset() + " " +
                            s.dim() + hDash + "╮" + s.reset();

    std::string hDashBot;
    for (std::size_t i = 0; i < totalWidth - 2; ++i) {
        hDashBot += "─";
    }
    std::string botBorder = s.dim() + "╰" + hDashBot + "╯" + s.reset();

    out << topBorder << "\n";
    for (const auto& line : lines) {
        std::size_t pad = innerWidth - visualWidth(line.plain);
        out << s.dim() << "│" << s.reset() << "  " << line.styled << std::string(pad, ' ')
            << "  " << s.dim() << "│" << s.reset() << "\n";
    }
    out << botBorder << "\n\n";
}

void printSolveResult(std::ostream& out, const SolveResultInfo& res, const TerminalStyle& s) {
    if (res.status == solver::SolveStatus::Optimal) {
        out << s.boldGreen() << "✓ Optimal" << s.reset() << "\n\n";
        out << "  " << s.dim() << "Objective       " << s.reset() << s.bold()
            << std::setprecision(9) << res.objective << s.reset() << "\n";
        if (res.iterations > 0) {
            out << "  " << s.dim() << "Iterations      " << s.reset() << res.iterations << "\n";
        }
        if (res.nodeCount > 0) {
            out << "  " << s.dim() << "Nodes           " << s.reset() << res.nodeCount << "\n";
        }
        out << "  " << s.dim() << "Solve time      " << s.reset()
            << std::fixed << std::setprecision(4) << res.solveSeconds << " s\n";
    } else if (res.status == solver::SolveStatus::Infeasible) {
        out << s.boldRed() << "✗ Infeasible" << s.reset() << "\n\n";
        if (!res.engine.empty()) {
            out << "  " << s.dim() << "Engine          " << s.reset() << res.engine << "\n";
        }
        if (!res.message.empty()) {
            out << "  " << s.dim() << "Reason          " << s.reset() << res.message << "\n";
        }
        if (res.iterations > 0) {
            out << "  " << s.dim() << "Iterations      " << s.reset() << res.iterations << "\n";
        }
        out << "  " << s.dim() << "Solve time      " << s.reset()
            << std::fixed << std::setprecision(4) << res.solveSeconds << " s\n";
    } else if (res.status == solver::SolveStatus::Unbounded) {
        out << s.boldYellow() << "! Unbounded" << s.reset() << "\n\n";
        if (!res.engine.empty()) {
            out << "  " << s.dim() << "Engine          " << s.reset() << res.engine << "\n";
        }
        if (!res.message.empty()) {
            out << "  " << s.dim() << "Reason          " << s.reset() << res.message << "\n";
        }
        out << "  " << s.dim() << "Solve time      " << s.reset()
            << std::fixed << std::setprecision(4) << res.solveSeconds << " s\n";
    } else if (res.status == solver::SolveStatus::LimitReached) {
        out << s.boldYellow() << "! Limit Reached" << s.reset() << "\n\n";
        if (res.hasObjective) {
            out << "  " << s.dim() << "Objective       " << s.reset() << s.bold()
                << std::setprecision(9) << res.objective << s.reset() << "\n";
        }
        if (res.iterations > 0) {
            out << "  " << s.dim() << "Iterations      " << s.reset() << res.iterations << "\n";
        }
        if (res.nodeCount > 0) {
            out << "  " << s.dim() << "Nodes           " << s.reset() << res.nodeCount << "\n";
        }
        out << "  " << s.dim() << "Solve time      " << s.reset()
            << std::fixed << std::setprecision(4) << res.solveSeconds << " s\n";
    } else {
        out << s.bold() << solver::toString(res.status) << s.reset() << "\n\n";
        if (res.hasObjective) {
            out << "  " << s.dim() << "Objective       " << s.reset() << res.objective << "\n";
        }
        if (!res.message.empty()) {
            out << "  " << s.dim() << "Message         " << s.reset() << res.message << "\n";
        }
        out << "  " << s.dim() << "Solve time      " << s.reset()
            << std::fixed << std::setprecision(4) << res.solveSeconds << " s\n";
    }
}

void printSolutionWritten(std::ostream& out, const std::string& path, const TerminalStyle& s) {
    out << "\n" << s.boldGreen() << "✓" << s.reset() << " Solution written to " << s.bold() << path << s.reset() << "\n";
}

void printError(std::ostream& err, const std::string& title, const std::string& details) {
    TerminalStyle s = TerminalStyle::forStream(err);
    err << s.boldRed() << "✗" << s.reset() << " " << s.bold() << title << s.reset() << "\n";
    if (!details.empty()) {
        err << "\n  " << details << "\n";
    }
}

}  // namespace cli
