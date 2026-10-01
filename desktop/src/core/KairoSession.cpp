#include "KairoSession.h"

#include "json_report.h"
#include "model/model.h"
#include "mps/mps_reader.h"
#include "solver/orchestrator.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>

#include <chrono>
#include <exception>
#include <sstream>

namespace kairo::core {
namespace {

double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

QJsonObject toRecord(const cli::JsonReportInput& input, const solver::SolveResult& result, QString* error) {
    std::ostringstream text;
    if (!cli::writeJsonReport(text, input, result)) {
        *error = QStringLiteral("KAIRO could not write the run record");
        return {};
    }
    QJsonParseError parse{};
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(text.str()), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("KAIRO wrote an unreadable record: ") + parse.errorString();
        return {};
    }
    return document.object();
}

}  // namespace

ModelInfo openModel(const QString& path) {
    ModelInfo info;
    const QFileInfo file(path);
    info.filePath = file.absoluteFilePath();
    info.fileName = file.fileName();
    info.fileSize = file.size();
    try {
        mps::MpsReader reader;
        const model::Model model = reader.read(path.toStdString());
        info.readable = true;
        info.name = QString::fromStdString(model.name);
        info.variables = static_cast<qint64>(model.variables.size());
        info.constraints = static_cast<qint64>(model.constraints.size());
        info.nonzeros = solver::countNonzeros(model);
        info.objectiveSense = model.objective.sense == model::ObjectiveSense::Maximize ? QStringLiteral("max") : QStringLiteral("min");
        info.structurallyValid = model.validate();
    } catch (const std::exception& error) {
        info.readError = QString::fromUtf8(error.what());
    }
    return info;
}

SolveRun solve(const SolveRequest& request) {
    SolveRun run;
    const QFileInfo file(request.modelPath);
    run.fileName = file.fileName();
    run.fileSize = file.size();
    const std::string path = request.modelPath.toStdString();

    solver::SolverOptions options;
    options.forceEngine = request.engine;
    options.backend = request.backend;
    options.cudaDevice = request.cudaDevice;
    if (request.timeLimitSeconds) options.timeLimitSeconds = *request.timeLimitSeconds;
    if (request.threadCount) options.threadCount = *request.threadCount;

    // Request echo, as the CLI records it. The instance path is the file name
    // only, so a saved or exported record carries no local directory.
    cli::JsonReportInput input;
    input.instancePath = run.fileName.toStdString();
    input.requestedEngine = request.engine ? solver::toString(*request.engine) : std::string{};
    input.requestedBackend = solver::toString(request.backend);
    input.cudaDevice = request.cudaDevice;
    input.timeLimitSeconds = request.timeLimitSeconds.value_or(0.0);
    input.threadCount = request.threadCount.value_or(0);
    input.tolerance = options.tolerance;
    input.instanceSha256 = cli::sha256File(path);

    // Same order and same refusals as the CLI (cli/cli.cpp, solveFile): a
    // model KAIRO cannot read or that fails structural validation produces a
    // record with no report -- the solver was never called.
    model::Model model;
    const auto parseStart = std::chrono::steady_clock::now();
    try {
        mps::MpsReader reader;
        model = reader.read(path);
    } catch (const std::exception& error) {
        input.parseSeconds = secondsSince(parseStart);
        solver::SolveResult unreadable;
        unreadable.status = solver::SolveStatus::InvalidModel;
        unreadable.message = std::string("failed to read MPS file: ") + error.what();
        run.record = toRecord(input, unreadable, &run.error);
        run.ok = !run.record.isEmpty();
        return run;
    }
    input.parseSeconds = secondsSince(parseStart);
    input.originalVariables = model.variables.size();
    input.originalConstraints = model.constraints.size();
    input.originalModel = &model;

    if (!model.validate()) {
        solver::SolveResult refused;
        refused.status = solver::SolveStatus::InvalidModel;
        refused.message = "model failed structural validation";
        run.record = toRecord(input, refused, &run.error);
        run.ok = !run.record.isEmpty();
        return run;
    }

    solver::SolveReport report;
    solver::SolveResult result;
    const auto solveStart = std::chrono::steady_clock::now();
    try {
        result = solver::solve(model, options, &report);
    } catch (const std::exception& error) {
        run.error = QStringLiteral("KAIRO raised an exception while solving: ") + QString::fromUtf8(error.what());
        return run;
    }
    input.solveSeconds = secondsSince(solveStart);
    input.report = &report;
    run.record = toRecord(input, result, &run.error);
    run.ok = !run.record.isEmpty();
    return run;
}

QList<QPair<solver::Engine, QString>> selectableEngines() {
    QList<QPair<solver::Engine, QString>> engines;
    for (solver::Engine engine : {solver::Engine::DualSimplex, solver::Engine::Pdlp, solver::Engine::Barrier,
                                  solver::Engine::BranchAndCut, solver::Engine::Qp, solver::Engine::Miqp}) {
        engines.append({engine, QString::fromLatin1(solver::toString(engine))});
    }
    return engines;
}

}  // namespace kairo::core
