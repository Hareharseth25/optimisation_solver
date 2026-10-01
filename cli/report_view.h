#pragma once

// Human-readable rendering of solver::SolveReport.
//
// Everything printed here is read from the SolveReport (and SolveResult) of
// the solve that just ran, or from the input model itself. Nothing is
// recomputed: a stage that did not run is shown as "not run", never as zero.

#include "mascot.h"
#include "model/model.h"
#include "solver/solve_report.h"
#include "solver/solve_result.h"

#include <ostream>

namespace cli {

// The few report lines worth showing on every solve: why the dispatcher chose
// its engine, and whether the answer passed original-space validation.
// Prints nothing for a stage that did not run.
void printReportSummary(std::ostream& out, const solver::SolveReport& report,
                        const TerminalStyle& style);

// The full report, section by section (--verbose).
void printReportDetails(std::ostream& out, const model::Model& model,
                        const solver::SolveResult& result,
                        const solver::SolveReport& report, const TerminalStyle& style);

// "LP · 2 continuous", "MILP · 3 binary · 1 continuous". Empty when the
// solve never reached classification.
std::string describeClassification(const solver::SolveReport& report);

}  // namespace cli
