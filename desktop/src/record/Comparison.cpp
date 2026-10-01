#include "Comparison.h"

#include "Format.h"
#include "RecordReaders.h"

#include <QLocale>

#include <cmath>
#include <functional>
#include <optional>

namespace kairo::record {
namespace {

namespace f = kairo::format;

// One side of a row: shown text when recorded, and the number behind it
// when a difference can be stated.
struct Value {
    bool present = false;
    QString shown;
    std::optional<double> number;
};

Value text(const QString& shown) {
    return shown.isEmpty() ? Value{} : Value{true, shown, std::nullopt};
}
Value numeric(std::optional<double> value, const std::function<QString(double)>& format) {
    return value ? Value{true, format(*value), value} : Value{};
}

using DeltaFn = std::function<QString(double a, double b)>;

ComparisonRow makeRow(const QString& key, const QString& label, const Value& a, const Value& b,
                      const DeltaFn& delta = nullptr) {
    ComparisonRow r;
    r.key = key;
    r.label = label;
    r.a = a.present ? a.shown : kNotRecorded;
    r.b = b.present ? b.shown : kNotRecorded;
    r.missing = !a.present || !b.present;
    r.same = a.present && b.present ? (r.a == r.b ? 1 : 0) : -1;
    // A difference only where the shown values differ (two times that print
    // the same are not reported as "+0.0 µs").
    if (delta && a.number && b.number && r.same == 0) r.delta = delta(*a.number, *b.number);
    return r;
}

ComparisonRow notCompared(const QString& key, const QString& label) {
    ComparisonRow r;
    r.key = key;
    r.label = label;
    r.a = kNotCompared;
    r.b = kNotCompared;
    r.notCompared = true;
    return r;
}

QString signedDelta(double a, double b, const std::function<QString(double)>& magnitude) {
    const double d = b - a;
    return QStringLiteral("B − A = ") + (d < 0 ? QStringLiteral("−") : QStringLiteral("+")) + magnitude(std::fabs(d));
}
QString secondsDelta(double a, double b) { return signedDelta(a, b, [](double v) { return f::seconds(v); }); }
QString countDelta(double a, double b) { return signedDelta(a, b, [](double v) { return f::count(v); }); }
QString realDelta(double a, double b) { return QStringLiteral("B − A = ") + f::real(b - a); }

QString fmtSeconds(double v) { return f::seconds(v); }
QString fmtCount(double v) { return f::count(v); }
QString fmtReal(double v) { return f::real(v); }
QString fmtResidual(double v) { return f::residual(v); }

std::optional<double> num(const QJsonObject& o, const char* key) {
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? std::optional<double>(v.toDouble()) : std::nullopt;
}
QString str(const QJsonObject& o, const char* key) {
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isString() ? v.toString() : QString();
}

// One side's facts, read through the shared readers.
struct Side {
    QJsonObject record;
    DispatchView dx;
    ResultFacts facts;
    QString title;
    QString sha;
    QString commit;
    QString buildType;
    QJsonObject stages;
    QJsonObject work;
    QJsonObject settings;
    QJsonObject backend;
    QJsonObject presolve;
    bool hasClassification = false;
    QJsonObject c;
};

Side side(const ComparedRun& run) {
    Side s;
    s.record = run.record;
    s.dx = readDispatch(run.record);
    s.facts = resultFacts(run.record);
    s.title = interpretResult(run.record).title;
    const QJsonObject solver = run.record.value("solver").toObject();
    s.sha = str(run.record.value("instance").toObject(), "sha256");
    s.commit = str(solver, "commit");
    s.buildType = str(solver, "build_type");
    s.stages = run.record.value("stage_seconds").toObject();
    s.work = run.record.value("work").toObject();
    s.settings = run.record.value("settings").toObject();
    s.backend = run.record.value("compute_backend").toObject();
    s.presolve = run.record.value("presolve").toObject();
    s.hasClassification = run.record.value("classification").isObject();
    s.c = run.record.value("classification").toObject();
    return s;
}

Value dims(const Side& s) {
    if (!s.hasClassification) return {};
    return text(QStringLiteral("%1 · %2 · %3")
                    .arg(QString::number(s.c.value("num_columns").toDouble()),
                         QString::number(s.c.value("num_rows").toDouble()),
                         QString::number(s.c.value("nonzeros").toDouble())));
}

Value integrality(const Side& s) {
    if (s.facts.integerModel == std::optional<bool>(false)) return text(QStringLiteral("not applicable"));
    if (!s.facts.integralityRespected) return {};
    return text(*s.facts.integralityRespected ? QStringLiteral("yes") : QStringLiteral("no"));
}

Value originalValidation(const Side& s) {
    if (s.facts.original.isEmpty()) return {};
    if (s.facts.original.value("passed").toBool(false)) return text(QStringLiteral("passed"));
    return text(f::words(str(s.facts.original, "status")));
}

Value engineTime(const Side& s) {
    if (!s.dx.engineSeconds) return {};
    const QString kind = s.dx.engineTimeKind == QLatin1String("path") ? QStringLiteral("engine path")
                                                                       : QStringLiteral("execution");
    return text(f::seconds(*s.dx.engineSeconds) + QStringLiteral(" (") + kind + QLatin1Char(')'));
}

}  // namespace

const ComparisonGroup* Comparison::group(const QString& key) const {
    for (const ComparisonGroup& g : groups) {
        if (g.key == key) return &g;
    }
    return nullptr;
}

const ComparisonRow* Comparison::row(const QString& key) const {
    for (const ComparisonGroup& g : groups) {
        for (const ComparisonRow& r : g.rows) {
            if (r.key == key) return &r;
        }
    }
    return nullptr;
}

Comparison buildComparison(const ComparedRun& runA, const ComparedRun& runB) {
    const Side A = side(runA);
    const Side B = side(runB);
    Comparison out;
    out.sameModel = !A.sha.isEmpty() && A.sha == B.sha;
    out.sameBuild = !A.commit.isEmpty() && A.commit == B.commit && A.buildType == B.buildType;

    const auto savedText = [](const ComparedRun& run) {
        return run.savedAt.isValid() ? text(QLocale().toString(run.savedAt, QLocale::ShortFormat)) : Value{};
    };
    const auto origin = [](const ComparedRun& run) {
        return text(run.imported ? QStringLiteral("Imported record") : QStringLiteral("Solved on this machine"));
    };

    ComparisonGroup identity{QStringLiteral("identity"), QStringLiteral("Identity"), {
        makeRow("file", "Model file", text(runA.fileName), text(runB.fileName)),
        makeRow("sha256", "Input SHA-256", text(A.sha.isEmpty() ? QString() : f::shortHash(A.sha, 16)),
                text(B.sha.isEmpty() ? QString() : f::shortHash(B.sha, 16))),
        makeRow("class", "Problem class", text(str(A.c, "problem_class")), text(str(B.c, "problem_class"))),
        makeRow("dims", "Variables · constraints · nonzeros", dims(A), dims(B)),
        makeRow("commit", "KAIRO build commit", text(A.commit.isEmpty() ? QString() : f::shortHash(A.commit, 10)),
                text(B.commit.isEmpty() ? QString() : f::shortHash(B.commit, 10))),
        makeRow("build_type", "Build type", text(A.buildType), text(B.buildType)),
        makeRow("origin", "Origin", origin(runA), origin(runB)),
        makeRow("saved", "Saved", savedText(runA), savedText(runB)),
    }};

    const auto requestedEngine = [](const Side& s) {
        return text(s.dx.requested.isEmpty() ? QStringLiteral("automatic") : s.dx.requested);
    };
    const auto timeLimit = [](const Side& s) {
        const auto v = num(s.settings, "time_limit_seconds");
        return text(v ? QString::number(*v, 'f', QLocale::FloatingPointShortest) + QStringLiteral(" s")
                      : QStringLiteral("none"));
    };
    const auto threads = [](const Side& s) {
        return numeric(num(s.settings, "thread_count"),
                       [](double v) { return v == 0 ? QStringLiteral("0 (auto)") : QString::number(v); });
    };
    ComparisonGroup request{QStringLiteral("request"), QStringLiteral("Request"), {
        makeRow("requested_engine", "Requested engine", requestedEngine(A), requestedEngine(B)),
        makeRow("requested_backend", "Requested backend", text(str(A.backend, "requested")), text(str(B.backend, "requested"))),
        makeRow("requested_device", "Requested CUDA device",
                numeric(num(A.backend, "requested_device"), [](double v) { return QString::number(v); }),
                numeric(num(B.backend, "requested_device"), [](double v) { return QString::number(v); })),
        makeRow("time_limit", "Time limit", timeLimit(A), timeLimit(B)),
        makeRow("threads", "Threads (requested)", threads(A), threads(B)),
        makeRow("tolerance", "Engine tolerance", numeric(num(A.settings, "tolerance"), fmtReal),
                numeric(num(B.settings, "tolerance"), fmtReal)),
    }};

    const auto executed = [](const Side& s) {
        return text(s.dx.executed.isEmpty() ? QStringLiteral("nothing executed") : engineDisplayName(s.dx.executed));
    };
    const auto backendExecuted = [](const Side& s) {
        const QString v = str(s.backend, "executed");
        return text(v.isEmpty() ? QStringLiteral("none") : v);
    };
    const auto point = [](const Side& s) { return text(s.facts.hasPoint ? QStringLiteral("returned") : QStringLiteral("none")); };
    ComparisonGroup outcome{QStringLiteral("outcome"), QStringLiteral("Outcome"), {
        makeRow("status", "Status", text(A.facts.status), text(B.facts.status)),
        makeRow("result", "Result", text(A.title), text(B.title)),
        makeRow("selected", "Selected (dispatch)", text(engineDisplayName(A.dx.selected)), text(engineDisplayName(B.dx.selected))),
        makeRow("executed", "Engine executed", executed(A), executed(B)),
        makeRow("backend", "Backend executed", backendExecuted(A), backendExecuted(B)),
        makeRow("point", "Solution point", point(A), point(B)),
    }};
    if (out.sameModel) {
        const auto rowResidual = [](const Side& s) {
            return s.facts.original.value("passed").toBool(false)
                       ? numeric(num(s.facts.original, "max_constraint_residual"), fmtResidual)
                       : Value{};
        };
        outcome.rows << makeRow("objective", "Objective", numeric(A.facts.objective, fmtReal),
                                numeric(B.facts.objective, fmtReal), realDelta)
                     << makeRow("integrality", "Integrality respected", integrality(A), integrality(B))
                     << makeRow("feasibility", "Original-space validation", originalValidation(A), originalValidation(B))
                     << makeRow("row_residual", "Max constraint violation", rowResidual(A), rowResidual(B))
                     << makeRow("kkt", "Max dual (KKT) residual", numeric(A.facts.dualResidual, fmtResidual),
                                numeric(B.facts.dualResidual, fmtResidual));
    } else {
        outcome.rows << notCompared("objective", "Objective")
                     << notCompared("feasibility", "Validation and residuals");
    }

    const auto stage = [](const Side& s, const char* key) { return numeric(num(s.stages, key), fmtSeconds); };
    ComparisonRow engineRow = makeRow("engine_time", "Engine", engineTime(A), engineTime(B));
    // A delta only between two measurements of the same kind.
    if (A.dx.engineSeconds && B.dx.engineSeconds && A.dx.engineTimeKind == B.dx.engineTimeKind
        && engineRow.same == 0) {
        engineRow.delta = secondsDelta(*A.dx.engineSeconds, *B.dx.engineSeconds);
    }
    ComparisonGroup timing{QStringLiteral("timing"), QStringLiteral("Work and timing"), {
        makeRow("total", "Total solve time", numeric(A.facts.totalSeconds, fmtSeconds), numeric(B.facts.totalSeconds, fmtSeconds), secondsDelta),
        makeRow("dispatch_time", "Dispatch", stage(A, "dispatch"), stage(B, "dispatch"), secondsDelta),
        makeRow("presolve_time", "Presolve", stage(A, "presolve"), stage(B, "presolve"), secondsDelta),
        engineRow,
        makeRow("postsolve_time", "Postsolve", stage(A, "postsolve"), stage(B, "postsolve"), secondsDelta),
        makeRow("reduced_validation_time", "Reduced-space validation", stage(A, "reduced_validation"), stage(B, "reduced_validation"), secondsDelta),
        makeRow("validation_time", "Model validation", stage(A, "validation"), stage(B, "validation"), secondsDelta),
        makeRow("iterations", "Iterations", numeric(num(A.work, "iterations"), fmtCount), numeric(num(B.work, "iterations"), fmtCount), countDelta),
        makeRow("nodes", "Nodes", numeric(num(A.work, "nodes"), fmtCount), numeric(num(B.work, "nodes"), fmtCount), countDelta),
    }};

    ComparisonGroup presolve{QStringLiteral("presolve"), QStringLiteral("Presolve"), {}};
    if (out.sameModel) {
        const auto p = [](const Side& s, const char* key) { return numeric(num(s.presolve, key), fmtCount); };
        presolve.rows << makeRow("reduced_variables", "Reduced variables", p(A, "reduced_variables"), p(B, "reduced_variables"))
                      << makeRow("reduced_constraints", "Reduced constraints", p(A, "reduced_constraints"), p(B, "reduced_constraints"))
                      << makeRow("reduced_nonzeros", "Reduced nonzeros", p(A, "reduced_nonzeros"), p(B, "reduced_nonzeros"))
                      << makeRow("transformations", "Presolve transformations", p(A, "transformations"), p(B, "transformations"));
    } else {
        presolve.rows << notCompared("presolve", "Presolve reductions");
    }

    if (out.sameModel) {
        out.notes << ComparisonNote{"model", "ok", QStringLiteral("Same model input (SHA-256 %1).").arg(f::shortHash(A.sha, 16))};
    } else {
        out.notes << ComparisonNote{"model", "warn", QStringLiteral(
            "Different model inputs — direct comparison is limited. "
            "Objectives, residuals and presolve reductions are not compared.")};
    }
    if (!out.sameBuild) {
        out.notes << ComparisonNote{"build", "warn", QStringLiteral(
            "Different KAIRO build identity. The commit is recorded when the build is configured, "
            "so it may not reflect rebuilt or uncommitted code.")};
    }
    QStringList optionsDiffer;
    for (const ComparisonRow& r : request.rows) {
        if (r.same == 0) optionsDiffer << r.label.toLower();
    }
    if (!optionsDiffer.isEmpty()) {
        out.notes << ComparisonNote{"options", "neutral", QStringLiteral("Different solver options: %1.").arg(optionsDiffer.join(", "))};
    }
    if (runA.imported || runB.imported) {
        out.notes << ComparisonNote{"imported", "neutral", QStringLiteral(
            "An imported record is shown as it was recorded elsewhere; it was not re-solved here.")};
    }
    out.notes << ComparisonNote{"timing", "neutral",
                                QStringLiteral("Times are single measurements from each run; the host machine is not recorded.")};

    out.groups << identity << request << outcome << timing << presolve;
    return out;
}

}  // namespace kairo::record
