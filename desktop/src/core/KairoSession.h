#pragma once

// The desktop's only contact with KAIRO Core. In-process, synchronous calls;
// no CLI, no HTTP, no subprocess.
//
//   openModel()  mps::MpsReader + model::Model::validate() + solver::countNonzeros()
//   solve()      the same per-run steps as `optimsolver solve --json`:
//                read MPS -> structural check -> solver::solve(model, options, &report)
//                -> cli::writeJsonReport(SolveResult, SolveReport) -> optimsolver.solve.v1
//
// The record returned by solve() is written by KAIRO's own record writer from
// the SolveResult and SolveReport of that one call. The GUI interprets only
// that record (record/RecordReaders), so a live run and a saved run go
// through exactly the same readers.

#include "solver/dispatcher.h"

#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>

#include <optional>

namespace kairo::core {

// What KAIRO's reader says about a file before any solve. Classification is
// deliberately absent: it is a solve stage, shown once the solve has run.
struct ModelInfo {
    bool readable = false;
    QString readError;              // the MPS reader's message when unreadable
    QString filePath;
    QString fileName;
    qint64 fileSize = 0;
    QString name;                   // NAME section
    qint64 variables = 0;
    qint64 constraints = 0;
    qint64 nonzeros = 0;            // solver::countNonzeros, the dispatcher's own count
    QString objectiveSense;         // min | max
    bool structurallyValid = false; // model::Model::validate()
};

ModelInfo openModel(const QString& path);

struct SolveRequest {
    QString modelPath;
    std::optional<solver::Engine> engine;  // nullopt = automatic dispatch
    solver::ComputeBackend backend = solver::ComputeBackend::Auto;
    int cudaDevice = 0;
    std::optional<double> timeLimitSeconds;
    std::optional<int> threadCount;
};

struct SolveRun {
    bool ok = false;        // false only when KAIRO could not produce a record at all
    QString error;
    QJsonObject record;     // optimsolver.solve.v1
    QString fileName;
    qint64 fileSize = 0;
};

SolveRun solve(const SolveRequest& request);

// Engines a caller may force for an MPS model (solver::Engine values that are
// real engines; NLP needs its own input and pseudo-engines are outcomes).
QList<QPair<solver::Engine, QString>> selectableEngines();

}  // namespace kairo::core
