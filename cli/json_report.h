#pragma once

// Machine-readable record of one solve: optimsolver.solve.v1.
//
// This is the serialization contract for every consumer of solver runs -- the
// benchmark harness and KAIRO Desktop (see docs/architecture.md,
// "Solve-record contract"). Built as its own target,
// solve_report_json, so a consumer does not link the terminal application.
// The writer is a pure mapping of the SolveResult and SolveReport it is
// given; it never classifies, presolves, dispatches or validates.
//
// This is deliberately a separate output path from the dashboard and from
// --output. The dashboard is for a person, --output is a solution listing, and
// this is the only one an automated comparison should ever parse: it carries
// the fields a scored run needs (requested vs executed engine, settings,
// status, objective, primal, duals, work counters) with a stable schema.
//
// EVERY value is emitted in the ORIGINAL model's coordinates, after postsolve.
// A benchmark that compared reduced-space vectors against a reference solver
// would be comparing two different problems.
//
// A value that does not exist is emitted as JSON null, never as 0.0 or an
// empty string. "Branch-and-cut produced no duals" and "the duals are all
// zero" are different facts, and a checker must be able to tell them apart.

#include "model/model.h"
#include "solver/nlp.h"
#include "solver/solve_report.h"
#include "solver/solve_result.h"

#include <iosfwd>
#include <string>

namespace cli {

// Everything the report needs that SolveResult does not already carry.
struct JsonReportInput {
    std::string instancePath;
    std::string instanceSha256;

    // What the caller asked for, as opposed to what the dispatcher chose.
    std::string requestedBackend = "auto";
    int cudaDevice = 0;
    std::string requestedEngine;   // empty when the caller forced nothing
    double timeLimitSeconds = 0.0; // 0 means no limit
    double tolerance = 0.0;

    std::size_t originalVariables = 0;
    std::size_t originalConstraints = 0;

    // How the solve ran: classification, presolve summary, dispatch,
    // validation residuals and in-pipeline stage timings. Filled by the SAME
    // solver::solve() call whose SolveResult is being written; null when no
    // solve ran (the model was refused before the solver was called), in
    // which case every section it feeds is written as null.
    const solver::SolveReport* report = nullptr;

    const model::Model* originalModel = nullptr;

    // Wall-clock seconds measured by the CLI around its own calls. Reported
    // separately from the process's end-to-end time so a slow run can be
    // attributed. Negative means "not run". The finer in-pipeline stages come
    // from `report`.
    double parseSeconds = -1.0;
    double solveSeconds = -1.0;      // the whole solver::solve() call

    int threadCount = 0;
};

// Writes the record. Returns false if the stream went bad while writing.
bool writeJsonReport(std::ostream& out,
                     const JsonReportInput& input,
                     const solver::SolveResult& result);

// Shared additive backend object for affine and nonlinear JSON schemas.
// A null executed value means no numerical backend ran.
void writeBackendReport(std::ostream& out, const JsonReportInput& input,
                        const char* executed, const std::string& reason);

// Nonlinear reports preserve first-order status and KKT multiplier semantics.
// They share provenance/options fields, but have an explicit NLP schema.
bool writeJsonReport(std::ostream& out, const JsonReportInput& input,
                     const solver::NlpSolveResult& result);

// Dumps the parsed model exactly as model::Model holds it, so an independent
// reader can be diffed against it field by field. This is the only way to test
// the parser's OUTPUT rather than its effect on an answer: a sense flip, a
// dropped objective constant or a mis-signed range can all produce a solve that
// looks entirely healthy while describing a different problem.
bool writeModelDump(std::ostream& out, const model::Model& model);

// SHA-256 of the instance file's bytes, so a result can be tied to the exact
// input it came from. Empty string if the file cannot be read.
[[nodiscard]] std::string sha256File(const std::string& path);

}  // namespace cli
