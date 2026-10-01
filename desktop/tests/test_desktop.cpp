// KAIRO Desktop: the record readers and the core bridge, against REAL solves.
//
// Every case calls kairo::core::solve() -- the same in-process call the Solve
// button makes -- on a repository model, then checks the interpretation the
// UI shows. The expected wording is the one the original web Explorer used
// (now retired); desktop/tests/fixtures/records pins it on real records.

#include "core/KairoSession.h"
#include "history/RunStore.h"
#include "record/Comparison.h"
#include "record/Format.h"
#include "record/RecordReaders.h"
#include "ui/ComparisonView.h"
#include "ui/MainWindow.h"
#include "ui/ReportView.h"

#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QStandardPaths>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace kairo;

namespace {

QString repoPath(const char* relative) { return QStringLiteral(KAIRO_SOURCE_DIR "/") + QLatin1String(relative); }

QJsonObject solveModel(const char* relative, std::optional<solver::Engine> engine = std::nullopt,
                       solver::ComputeBackend backend = solver::ComputeBackend::Auto) {
    core::SolveRequest request;
    request.modelPath = repoPath(relative);
    request.engine = engine;
    request.backend = backend;
    request.threadCount = 1;
    const core::SolveRun run = core::solve(request);
    if (!run.ok) qFatal("no record for %s: %s", relative, qPrintable(run.error));
    return run.record;
}

QString pipelineStates(const QJsonObject& record) {
    QStringList states;
    for (const auto& stage : record::buildPipeline(record).stages) states << stage.id + QLatin1Char(':') + stage.state;
    return states.join(QLatin1Char(' '));
}

QString evidenceStates(const QJsonObject& record) {
    QStringList states;
    for (const auto& item : record::buildEvidence(record)) states << item.key + QLatin1Char(':') + item.state;
    return states.join(QLatin1Char(' '));
}

record::ComparedRun compared(const QJsonObject& record, const QString& fileName) {
    return record::ComparedRun{record, fileName, QDateTime::currentDateTime(), false};
}

// Every piece of text a comparison shows.
QString comparisonText(const record::Comparison& c) {
    QStringList all;
    for (const auto& note : c.notes) all << note.text;
    for (const auto& group : c.groups) {
        all << group.title;
        for (const auto& row : group.rows) all << row.label << row.a << row.b << row.delta;
    }
    return all.join(QLatin1Char('\n'));
}

void writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) qFatal("cannot write %s", qPrintable(path));
    file.write(bytes);
}

const QString kAllCompleted = QStringLiteral(
    "model:completed classification:completed presolve:completed dispatch:completed engine:completed "
    "postsolve:completed validation:completed result:completed");

}  // namespace

class DesktopTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // Recent-model settings and app data go to test locations, never the user's.
        QStandardPaths::setTestModeEnabled(true);
    }

    void formatParity() {
        namespace f = kairo::format;
        QCOMPARE(f::real(-464.7531428571428), QStringLiteral("-464.7531429"));
        QCOMPARE(f::real(10.666666666666666), QStringLiteral("10.66666667"));
        QCOMPARE(f::real(28.0), QStringLiteral("28"));
        QCOMPARE(f::real(0.0), QStringLiteral("0"));
        QCOMPARE(f::real(1e-8), QStringLiteral("1.000000e-8"));
        QCOMPARE(f::residual(1.77834181158687e-07), QStringLiteral("1.78e-7"));
        QCOMPARE(f::residual(0.33333333333333337), QStringLiteral("3.33e-1"));
        QCOMPARE(f::residual(0.0), QStringLiteral("0"));
        QCOMPARE(f::seconds(0.000282), QStringLiteral("282.0 µs"));
        QCOMPARE(f::seconds(std::nullopt), QStringLiteral("Not run"));
        QCOMPARE(f::count(1234567.0), QStringLiteral("1,234,567"));
        QCOMPARE(f::count(std::nullopt), QStringLiteral("Unknown"));
    }

    void optimalLp() {
        const QJsonObject r = solveModel("tests/cli/presolve_reduction.mps");
        QCOMPARE(r.value("schema").toString(), QStringLiteral("optimsolver.solve.v1"));
        QCOMPARE(record::summarize(r).title, QStringLiteral("Optimal"));
        QCOMPARE(pipelineStates(r), kAllCompleted);
        QCOMPARE(evidenceStates(r), QStringLiteral("solution:returned reduced:checked feasibility:checked integrality:not_applicable "
                                                   "objective:checked optimality:checked"));
        const auto dx = record::readDispatch(r);
        QCOMPARE(dx.state, QStringLiteral("invoked"));
        QCOMPARE(dx.mode, QStringLiteral("automatic"));
        QCOMPARE(dx.selected, QStringLiteral("dual_simplex"));
        QCOMPARE(dx.executed, QStringLiteral("dual_simplex"));
        QVERIFY(dx.reason.startsWith(QStringLiteral("small enough for the dual simplex")));
        QCOMPARE(dx.engineTimeKind, QStringLiteral("execution"));
        const auto impact = record::buildPresolveImpact(r);
        QCOMPARE(impact.state, QStringLiteral("converged"));
        QCOMPARE(*impact.rows[0].original, 3.0);
        QCOMPARE(*impact.rows[0].reduced, 2.0);
        QCOMPARE(*impact.rows[0].reduction, 100.0 / 3.0);
        QCOMPARE(impact.byType.size(), 1);
        QCOMPARE(impact.byType[0].first, QStringLiteral("fix_variable"));
        const auto analysis = record::buildModelAnalysis(r);
        QCOMPARE(analysis.problemClass, QStringLiteral("LP"));
        QCOMPARE(analysis.sense, QStringLiteral("Minimize"));
        QCOMPARE(r.value("objective").toDouble(), 28.0);
    }

    void optimalMilpAndMiqp() {
        for (const char* path : {"tests/cli/knapsack_milp.mps", "tests/mps/test_cases/20_miqp.mps"}) {
            const QJsonObject r = solveModel(path);
            QCOMPARE(record::summarize(r).title, QStringLiteral("Optimal — according to the solver"));
            QVERIFY(record::summarize(r).detail.contains(QStringLiteral("Global optimality evidence not independently recorded.")));
            QCOMPARE(evidenceStates(r), QStringLiteral("solution:returned reduced:checked feasibility:checked integrality:checked "
                                                       "objective:checked optimality:reported bound:not_available"));
        }
    }

    void optimalQp() {
        const QJsonObject r = solveModel("tests/cli/convex_qp.mps");
        QCOMPARE(record::interpretResult(r).kind, QStringLiteral("optimal_checked"));
        QCOMPARE(record::readDispatch(r).executed, QStringLiteral("qp"));
        QCOMPARE(record::buildModelAnalysis(r).problemClass, QStringLiteral("QP"));
    }

    void relaxationIsNotIntegerOptimal() {
        // A valid MILP forced onto barrier: barrier solves the continuous relaxation.
        const QJsonObject r = solveModel("benchmarks/instances/known/milp_knapsack.mps", solver::Engine::Barrier);
        QCOMPARE(r.value("termination").toObject().value("status").toString(), QStringLiteral("optimal"));
        QCOMPARE(r.value("self_reported").toObject().value("integrality_respected").toBool(), false);
        QCOMPARE(record::summarize(r).title, QStringLiteral("Optimal for the continuous relaxation"));
        QVERIFY(evidenceStates(r).contains(QStringLiteral("integrality:failed")));
        QVERIFY(evidenceStates(r).contains(QStringLiteral("optimality:not_established")));
    }

    void forcedEngineRejection() {
        // A valid MILP forced onto PDLP: PDLP refuses the model.
        const QJsonObject r = solveModel("tests/cli/knapsack_milp.mps", solver::Engine::Pdlp);
        const auto dx = record::readDispatch(r);
        QCOMPARE(dx.mode, QStringLiteral("forced"));
        QCOMPARE(dx.requested, QStringLiteral("pdlp"));
        QVERIFY(dx.engineRejectedModel);
        QCOMPARE(dx.engineTimeKind, QStringLiteral("path"));
        QCOMPARE(record::summarize(r).title, QStringLiteral("Requested engine cannot solve this model"));
        QCOMPARE(record::summarize(r).code, QStringLiteral("invalid_model"));  // KAIRO's status, kept as evidence
        QVERIFY(pipelineStates(r).endsWith(QStringLiteral("engine:refused postsolve:not_run validation:not_run result:failed")));
        QCOMPARE(record::buildPipeline(r).stages.last().stateLabel, QStringLiteral("engine rejected model"));
        QCOMPARE(evidenceStates(r), QStringLiteral("solution:none reduced:nothing feasibility:nothing integrality:nothing objective:nothing"));
    }

    void cudaRefusal() {
        const QJsonObject r = solveModel("tests/cli/simple_lp.mps", solver::Engine::Pdlp, solver::ComputeBackend::Cuda);
        const auto dx = record::readDispatch(r);
        if (!dx.executed.isEmpty()) QSKIP("this build has a usable CUDA device");
        QVERIFY(dx.backendRefusal);
        QVERIFY(dx.backend.mismatch);
        QCOMPARE(record::summarize(r).title, QStringLiteral("Requested backend unavailable"));
        QVERIFY(pipelineStates(r).contains(QStringLiteral("dispatch:completed engine:refused")));
    }

    void presolveInfeasible() {
        const QJsonObject r = solveModel("tests/mps/test_cases/01_basic_lp.mps");
        QCOMPARE(record::summarize(r).title, QStringLiteral("Proved infeasible by presolve"));
        QCOMPARE(pipelineStates(r), QStringLiteral("model:completed classification:completed presolve:infeasible dispatch:not_run "
                                                   "engine:not_run postsolve:not_run validation:not_run result:infeasible"));
        QCOMPARE(record::readDispatch(r).state, QStringLiteral("not_invoked"));
        const auto impact = record::buildPresolveImpact(r);
        QVERIFY(impact.stopped);
        QCOMPARE(impact.heading, QStringLiteral("Dimensions when presolve stopped"));
        for (const auto& row : impact.rows) QVERIFY(!row.reduction.has_value());
        QVERIFY(evidenceStates(r).contains(QStringLiteral("infeasibility:proved_presolve")));
    }

    void engineInfeasibleAndUnbounded() {
        const QJsonObject infeasible = solveModel("tests/cli/engine_infeasible_lp.mps");
        QCOMPARE(record::summarize(infeasible).title, QStringLiteral("Infeasible — reported by dual simplex"));
        QVERIFY(pipelineStates(infeasible).contains(QStringLiteral("engine:infeasible postsolve:not_run validation:not_run")));
        const QJsonObject unbounded = solveModel("tests/cli/unbounded_lp.mps");
        QCOMPARE(record::summarize(unbounded).title, QStringLiteral("Unbounded — reported by dual simplex"));
        QVERIFY(evidenceStates(unbounded).contains(QStringLiteral("unboundedness:reported")));
    }

    void unsupportedAndTrivial() {
        const QJsonObject unsupported = solveModel("tests/cli/nonconvex_qp.mps");
        QCOMPARE(record::summarize(unsupported).title, QStringLiteral("No suitable engine"));
        QVERIFY(pipelineStates(unsupported).contains(QStringLiteral("dispatch:unsupported engine:not_run")));
        const QJsonObject trivial = solveModel("tests/cli/structured_milp.mps");
        QCOMPARE(record::readDispatch(trivial).pseudo, QStringLiteral("trivial"));
        QVERIFY(record::interpretResult(trivial).detail.startsWith(QStringLiteral("Reported optimal by the trivial path")));
        const auto analysis = record::buildModelAnalysis(trivial);
        QCOMPARE(analysis.structure.size(), 4);
        QVERIFY(analysis.structure[1].detected);  // big-M
        QCOMPARE(analysis.structure[1].detail, QStringLiteral("max 1000"));
    }

    void invalidAndUnreadable() {
        // Same refusal shape as the CLI: no report, so dispatch is null.
        const QJsonObject invalid = solveModel("tests/cli/invalid_bounds.mps");
        QVERIFY(invalid.value("dispatch").isNull());
        QVERIFY(invalid.value("classification").isNull());
        QCOMPARE(record::readDispatch(invalid).state, QStringLiteral("not_called"));
        QCOMPARE(record::summarize(invalid).title, QStringLiteral("Invalid model"));
        QCOMPARE(record::buildModelAnalysis(invalid).state, QStringLiteral("unclassified"));

        QTemporaryDir dir;
        const QString bad = dir.filePath(QStringLiteral("bad.mps"));
        QFile file(bad);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("NAME          BAD\nROWS\n N  OBJ\n Q  C1\nCOLUMNS\n    X  OBJ  1.0\nENDATA\n");
        file.close();
        QVERIFY(!core::openModel(bad).readable);
        core::SolveRequest request;
        request.modelPath = bad;
        const core::SolveRun run = core::solve(request);
        QVERIFY(run.ok);
        QVERIFY(run.record.value("instance").toObject().value("variables").isNull());
        QCOMPARE(record::buildModelAnalysis(run.record).state, QStringLiteral("unreadable"));
        QVERIFY(record::buildPipeline(run.record).stages.first().stateLabel == QStringLiteral("could not be read"));
    }

    void openModelUsesTheReader() {
        const core::ModelInfo info = core::openModel(repoPath("tests/cli/presolve_reduction.mps"));
        QVERIFY(info.readable);
        QCOMPARE(info.variables, 3);
        QCOMPARE(info.constraints, 1);
        QCOMPARE(info.nonzeros, 3);
        QCOMPARE(info.objectiveSense, QStringLiteral("min"));
        QVERIFY(info.structurallyValid);
    }

    void recordPathHasNoLocalDirectory() {
        const QJsonObject r = solveModel("tests/cli/simple_lp.mps");
        QCOMPARE(r.value("instance").toObject().value("path").toString(), QStringLiteral("simple_lp.mps"));
    }

    void runStore() {
        QTemporaryDir dir;
        history::RunStore store(dir.path());
        const QJsonObject lp = solveModel("tests/cli/presolve_reduction.mps");
        const QJsonObject milp = solveModel("tests/cli/knapsack_milp.mps");
        const auto a = store.save(lp, QStringLiteral("presolve_reduction.mps"), 319);
        QTest::qWait(5);
        const auto b = store.save(milp, QStringLiteral("knapsack_milp.mps"), 493);
        QCOMPARE(a.status, QStringLiteral("optimal"));
        QCOMPARE(a.engine, QStringLiteral("dual_simplex"));
        QCOMPARE(*a.objective, 28.0);
        QCOMPARE(a.sha256, lp.value("instance").toObject().value("sha256").toString());
        const auto entries = store.list();
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries[0].id, b.id);  // newest first
        const auto loaded = store.load(a.id);
        QVERIFY(loaded.has_value());
        QCOMPARE(loaded->record, lp);   // stored unchanged
        QVERIFY(store.remove(b.id));
        QCOMPARE(store.list().size(), 1);
        // Large vectors round-trip.
        QJsonObject big = lp;
        QJsonArray primal;
        for (int i = 0; i < 200000; ++i) primal.append(i * 0.5);
        big.insert("primal", primal);
        const auto c = store.save(big, QStringLiteral("big.mps"), 1);
        QCOMPARE(store.load(c.id)->record.value("primal").toArray().size(), 200000);
        // Only solve records are accepted; unrelated files are left alone by clear().
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, store.save(QJsonObject{{"schema", "other"}}, QStringLiteral("x"), 0));
        QFile stray(dir.filePath(QStringLiteral("notes.json")));
        QVERIFY(stray.open(QIODevice::WriteOnly));
        stray.write("{}");
        stray.close();
        store.clear();
        QCOMPARE(store.list().size(), 0);
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("notes.json"))));
    }

    void noModelTextPersisted() {
        QTemporaryDir dir;
        history::RunStore store(dir.path());
        const auto entry = store.save(solveModel("tests/cli/presolve_reduction.mps"), QStringLiteral("m.mps"), 1);
        QFile saved(dir.filePath(entry.id + QStringLiteral(".json")));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const QByteArray bytes = saved.readAll();
        QVERIFY(!bytes.contains("ROWS") && !bytes.contains("COLUMNS") && !bytes.contains("ENDATA"));
    }

    // ------------------------------------------------------------ comparison

    void comparisonSameModel() {
        // The same MILP solved by branch and cut and (forced) by barrier, which
        // solves only the continuous relaxation.
        const QJsonObject bc = solveModel("benchmarks/instances/known/milp_knapsack.mps");
        const QJsonObject barrier = solveModel("benchmarks/instances/known/milp_knapsack.mps", solver::Engine::Barrier);
        const auto c = record::buildComparison(compared(bc, "milp_knapsack.mps"), compared(barrier, "milp_knapsack.mps"));
        QVERIFY(c.sameModel);
        QVERIFY(c.sameBuild);
        QVERIFY(c.notes.first().text.startsWith(QStringLiteral("Same model input (SHA-256 ")));
        QCOMPARE(c.row("requested_engine")->a, QStringLiteral("automatic"));
        QCOMPARE(c.row("requested_engine")->b, QStringLiteral("barrier"));
        QCOMPARE(c.row("requested_engine")->same, 0);
        bool optionsNote = false;
        for (const auto& note : c.notes) optionsNote |= note.text == QStringLiteral("Different solver options: requested engine.");
        QVERIFY(optionsNote);
        QCOMPARE(c.row("result")->a, QStringLiteral("Optimal — according to the solver"));
        QCOMPARE(c.row("result")->b, QStringLiteral("Optimal for the continuous relaxation"));
        QCOMPARE(c.row("executed")->a, QStringLiteral("branch and cut"));
        QCOMPARE(c.row("executed")->b, QStringLiteral("barrier"));
        QCOMPARE(c.row("integrality")->a, QStringLiteral("yes"));
        QCOMPARE(c.row("integrality")->b, QStringLiteral("no"));
        QVERIFY(c.row("objective")->delta.startsWith(QStringLiteral("B − A = ")));
        QCOMPARE(c.row("feasibility")->a, QStringLiteral("passed"));
        QVERIFY(c.row("reduced_variables") && !c.row("reduced_variables")->notCompared);
        QVERIFY(c.row("row_residual"));
        // Identical shown values carry no difference.
        QCOMPARE(c.row("requested_backend")->same, 1);
        QVERIFY(c.row("requested_backend")->delta.isEmpty());
        QCOMPARE(c.row("presolve_time")->a == c.row("presolve_time")->b, c.row("presolve_time")->delta.isEmpty());
    }

    void comparisonDifferentModels() {
        const QJsonObject lp = solveModel("tests/cli/presolve_reduction.mps");
        const QJsonObject milp = solveModel("tests/cli/knapsack_milp.mps");
        const auto c = record::buildComparison(compared(lp, "presolve_reduction.mps"), compared(milp, "knapsack_milp.mps"));
        QVERIFY(!c.sameModel);
        QCOMPARE(c.notes.first().key, QStringLiteral("model"));
        QCOMPARE(c.notes.first().text, QStringLiteral("Different model inputs — direct comparison is limited. "
                                                      "Objectives, residuals and presolve reductions are not compared."));
        // No objective, residual, validation or presolve values side by side.
        QVERIFY(c.row("objective")->notCompared);
        QCOMPARE(c.row("objective")->a, record::kNotCompared);
        QVERIFY(c.row("feasibility")->notCompared);
        QVERIFY(c.row("presolve")->notCompared);
        for (const char* key : {"row_residual", "kkt", "integrality", "reduced_variables", "reduced_constraints",
                                "reduced_nonzeros", "transformations"}) {
            QVERIFY2(!c.row(key), key);
        }
        // Nothing in the outcome group carries a stated difference.
        for (const auto& row : c.group("outcome")->rows) QVERIFY2(row.delta.isEmpty(), qPrintable(row.key));
        // Identity, request, outcome and timing remain factual.
        QCOMPARE(c.row("class")->a, QStringLiteral("LP"));
        QCOMPARE(c.row("class")->b, QStringLiteral("MILP"));
        QCOMPARE(c.row("status")->a, QStringLiteral("optimal"));
    }

    void comparisonMissingValues() {
        QJsonObject a = solveModel("tests/cli/presolve_reduction.mps");
        QJsonObject b = a;
        // A record without a hash is never the "same model", even against itself.
        QJsonObject instance = a.value("instance").toObject();
        instance.remove("sha256");
        a.insert("instance", instance);
        b.insert("instance", instance);
        QJsonObject stages = b.value("stage_seconds").toObject();
        stages.remove("presolve");
        b.insert("stage_seconds", stages);
        QJsonObject work = b.value("work").toObject();
        work.insert("iterations", QJsonValue());
        b.insert("work", work);
        QJsonObject solver = b.value("solver").toObject();
        solver.remove("commit");
        b.insert("solver", solver);
        const auto c = record::buildComparison(compared(a, ""), compared(b, "m.mps"));
        QVERIFY(!c.sameModel);
        QVERIFY(!c.sameBuild);
        QCOMPARE(c.row("sha256")->a, record::kNotRecorded);
        QCOMPARE(c.row("file")->a, record::kNotRecorded);
        QCOMPARE(c.row("presolve_time")->b, record::kNotRecorded);
        QVERIFY(c.row("presolve_time")->missing);
        QCOMPARE(c.row("presolve_time")->same, -1);
        QVERIFY(c.row("presolve_time")->delta.isEmpty());
        QCOMPARE(c.row("iterations")->b, record::kNotRecorded);
        QCOMPARE(c.row("commit")->b, record::kNotRecorded);
        QVERIFY(!comparisonText(c).contains(QStringLiteral("B − A = −0")));
    }

    void comparisonKeepsEnginePathAndExecutionApart() {
        // Forced PDLP is refused by the MILP: the engine stage time measures the
        // engine path, not an execution, and is never subtracted from one.
        const QJsonObject refused = solveModel("tests/cli/knapsack_milp.mps", solver::Engine::Pdlp);
        const QJsonObject executed = solveModel("tests/cli/knapsack_milp.mps");
        const auto c = record::buildComparison(compared(refused, "k.mps"), compared(executed, "k.mps"));
        QVERIFY(c.row("engine_time")->a.endsWith(QStringLiteral("(engine path)")));
        QVERIFY(c.row("engine_time")->b.endsWith(QStringLiteral("(execution)")));
        QVERIFY(c.row("engine_time")->delta.isEmpty());
        QCOMPARE(c.row("executed")->a, QStringLiteral("nothing executed"));
        QCOMPARE(c.row("point")->a, QStringLiteral("none"));
    }

    void comparisonHasNoVerdict() {
        const QList<QJsonObject> records{solveModel("tests/cli/knapsack_milp.mps"),
                                         solveModel("tests/cli/knapsack_milp.mps", solver::Engine::Barrier),
                                         solveModel("tests/cli/knapsack_milp.mps", solver::Engine::Pdlp),
                                         solveModel("tests/cli/presolve_reduction.mps")};
        for (const auto& a : records) {
            for (const auto& b : records) {
                const QString text = comparisonText(record::buildComparison(compared(a, "a"), compared(b, "b"))).toLower();
                for (const char* word : {"better", "best", "winner", "worse", "faster", "slower", "rank", "improve",
                                         "certified", "guarantee", "proven optimal", "recommended"}) {
                    QVERIFY2(!text.contains(QLatin1String(word)), word);
                }
            }
        }
    }

    void comparisonRendering() {
        ui::ComparisonView view;
        view.showComparison(compared(solveModel("tests/cli/presolve_reduction.mps"), "lp.mps"),
                            compared(solveModel("tests/cli/knapsack_milp.mps"), "milp.mps"));
        for (const char* group : {"compare-identity", "compare-request", "compare-outcome", "compare-timing", "compare-presolve"}) {
            QVERIFY2(view.findChild<QFrame*>(QLatin1String(group)), group);
        }
        QVERIFY(view.findChild<QFrame*>(QStringLiteral("note-model")));
        QStringList shown;
        for (const QLabel* l : view.findChildren<QLabel*>()) shown << l->text();
        QVERIFY(shown.contains(record::kNotCompared));
        QVERIFY(shown.contains(QStringLiteral("Different model inputs — direct comparison is limited. "
                                              "Objectives, residuals and presolve reductions are not compared.")));
    }

    // ------------------------------------------------------------ export / import

    void exportImportRoundTrip() {
        QTemporaryDir dir;
        const QJsonObject original = solveModel("benchmarks/instances/known/milp_knapsack.mps", solver::Engine::Barrier);
        const QString file = dir.filePath(QStringLiteral("run.json"));
        history::writeRecordFile(original, file);
        QCOMPARE(history::readRecordFile(file), original);

        history::RunStore store(dir.filePath(QStringLiteral("runs")));
        const auto entry = store.importFile(file);
        QVERIFY(entry.imported);
        QCOMPARE(entry.fileName, QStringLiteral("milp_knapsack.mps"));
        QCOMPARE(entry.fileSize, qint64(-1));
        const auto loaded = store.load(entry.id);
        QVERIFY(loaded && loaded->entry.imported);
        QCOMPARE(loaded->record, original);  // unchanged, so it reads exactly as before
        QCOMPARE(record::summarize(loaded->record).title, QStringLiteral("Optimal for the continuous relaxation"));
        QCOMPARE(evidenceStates(loaded->record), evidenceStates(original));
        QCOMPARE(pipelineStates(loaded->record), pipelineStates(original));
        // The saved-run file keeps its format and says where the record came from.
        QFile saved(dir.filePath(QStringLiteral("runs/") + entry.id + QStringLiteral(".json")));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const QJsonObject savedFile = QJsonDocument::fromJson(saved.readAll()).object();
        QCOMPARE(savedFile.value("format").toString(), QStringLiteral("kairo.desktop.saved_run.v1"));
        QCOMPARE(savedFile.value("source").toString(), QStringLiteral("imported"));
        QVERIFY(savedFile.value("file_size").isNull());
        // A saved-run file is importable too (its record is taken).
        const auto again = store.importFile(saved.fileName());
        QCOMPARE(store.load(again.id)->record, original);
        // A live save is not imported.
        QVERIFY(!store.save(original, QStringLiteral("k.mps"), 10).imported);
    }

    void importLargeVectors() {
        QTemporaryDir dir;
        QJsonObject big = solveModel("tests/cli/presolve_reduction.mps");
        QJsonArray primal;
        QJsonArray duals;
        for (int i = 0; i < 250000; ++i) {
            primal.append(i * 0.25);
            duals.append(-i * 1e-9);
        }
        big.insert("primal", primal);
        big.insert("duals", duals);
        const QString file = dir.filePath(QStringLiteral("big.json"));
        history::writeRecordFile(big, file);
        history::RunStore store(dir.filePath(QStringLiteral("runs")));
        const auto entry = store.importFile(file);
        const auto loaded = store.load(entry.id);
        QCOMPARE(loaded->record.value("primal").toArray().size(), 250000);
        QCOMPARE(loaded->record.value("duals").toArray().at(249999).toDouble(), -249999 * 1e-9);
    }

    void importRejectsWhatIsNotARecord() {
        QTemporaryDir dir;
        history::RunStore store(dir.filePath(QStringLiteral("runs")));
        const QJsonObject good = solveModel("tests/cli/presolve_reduction.mps");
        const auto rejects = [&](const QString& name, const QByteArray& bytes, const QString& reason) {
            const QString path = dir.filePath(name);
            writeFile(path, bytes);
            try {
                store.importFile(path);
                QFAIL(qPrintable(name + QStringLiteral(" was imported")));
            } catch (const std::runtime_error& error) {
                QVERIFY2(QString::fromUtf8(error.what()).contains(reason), error.what());
            }
        };
        rejects("text.json", "not json at all", "not valid JSON");
        rejects("array.json", "[1,2,3]", "not a JSON object");
        rejects("schema.json", R"({"schema":"optimsolver.solve.v2"})", "schema is not");
        QJsonObject badPrimal = good;
        badPrimal.insert("primal", QJsonArray{1, "two", 3});
        rejects("primal.json", QJsonDocument(badPrimal).toJson(), "\"primal\" must be a number array or null");
        QJsonObject badObjective = good;
        badObjective.insert("objective", "28");
        rejects("objective.json", QJsonDocument(badObjective).toJson(), "objective must be a number or null");
        QJsonObject badStatus = good;
        QJsonObject termination = good.value("termination").toObject();
        termination.insert("status", "certified");
        badStatus.insert("termination", termination);
        rejects("status.json", QJsonDocument(badStatus).toJson(), "termination.status is not a KAIRO status");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, store.importFile(dir.filePath(QStringLiteral("missing.json"))));
        QCOMPARE(store.list().size(), 0);  // nothing half-imported
    }

    void importKeepsOnlyTheFileName() {
        // A CLI record holds the path it was given, possibly from another machine.
        QTemporaryDir dir;
        history::RunStore store(dir.filePath(QStringLiteral("runs")));
        QJsonObject r = solveModel("tests/cli/presolve_reduction.mps");
        for (const QString& recorded : {QStringLiteral("C:\\models\\plant.mps"), QStringLiteral("/home/someone/models/plant.mps")}) {
            QJsonObject instance = r.value("instance").toObject();
            instance.insert("path", recorded);
            r.insert("instance", instance);
            const QString file = dir.filePath(QStringLiteral("cli.json"));
            history::writeRecordFile(r, file);
            QCOMPARE(store.importFile(file).fileName, QStringLiteral("plant.mps"));
        }
    }

    // ------------------------------------------------------------ history

    void historyPersistsAndOlderFilesReadAsLive() {
        QTemporaryDir dir;
        const QJsonObject r = solveModel("tests/cli/presolve_reduction.mps");
        QString id;
        {
            history::RunStore store(dir.path());
            id = store.save(r, QStringLiteral("p.mps"), 319).id;
        }
        history::RunStore reopened(dir.path());
        QCOMPARE(reopened.list().size(), 1);
        QCOMPARE(reopened.list().first().id, id);
        QCOMPARE(reopened.list().first().fileSize, qint64(319));
        QVERIFY(!reopened.list().first().imported);
        // A Prompt 1 file (no "source") is a run solved on this machine.
        QJsonObject legacy{{"format", "kairo.desktop.saved_run.v1"}, {"id", "legacy"},
                           {"saved_at", "2026-01-01T10:00:00.000"}, {"file_name", "old.mps"}, {"file_size", 7}, {"record", r}};
        writeFile(dir.filePath(QStringLiteral("legacy.json")), QJsonDocument(legacy).toJson());
        const auto loaded = reopened.load(QStringLiteral("legacy"));
        QVERIFY(loaded && !loaded->entry.imported);
        QCOMPARE(reopened.list().size(), 2);
        QVERIFY(reopened.remove(QStringLiteral("legacy")));
        // The id inside a file never becomes a path: a hand-placed file
        // claiming "../victim" is listed under its own name, and ids that
        // leave the directory are refused.
        QTemporaryDir outside;
        writeFile(outside.filePath(QStringLiteral("victim.json")), "{}");
        QJsonObject hostile = legacy;
        hostile.insert("id", QDir(dir.path()).relativeFilePath(outside.filePath(QStringLiteral("victim"))));
        writeFile(dir.filePath(QStringLiteral("hostile.json")), QJsonDocument(hostile).toJson());
        bool listedByFileName = false;
        for (const auto& entry : reopened.list()) listedByFileName |= entry.id == QStringLiteral("hostile");
        QVERIFY(listedByFileName);
        reopened.clear();
        QVERIFY(QFile::exists(outside.filePath(QStringLiteral("victim.json"))));
        QVERIFY(!reopened.remove(QStringLiteral("../victim")));
        QVERIFY(!reopened.load(QStringLiteral("../victim")));
        QCOMPARE(history::RunStore(dir.path()).list().size(), 0);
    }

    void windowHistoryCompareExport() {
        QTemporaryDir dir;
        ui::MainWindow window(std::make_unique<history::RunStore>(dir.filePath(QStringLiteral("runs"))));
        QVERIFY(!window.hasShownRecord());
        // A live run, saved.
        QVERIFY(window.openModelPath(repoPath("benchmarks/instances/known/milp_knapsack.mps")));
        window.showRun(core::solve(window.currentRequest()));
        QVERIFY(window.hasShownRecord());
        QVERIFY(window.saveCurrentRun());
        QVERIFY(!window.saveCurrentRun());  // already saved
        QCOMPARE(window.runsList()->count(), 1);
        // An imported run of the same model (barrier: the relaxation).
        const QString file = dir.filePath(QStringLiteral("barrier.json"));
        history::writeRecordFile(solveModel("benchmarks/instances/known/milp_knapsack.mps", solver::Engine::Barrier), file);
        const auto imported = window.importRunFile(file);
        QCOMPARE(window.runsList()->count(), 2);
        QVERIFY(window.runsList()->item(0)->text().startsWith(QStringLiteral("[Imported] ")));
        QVERIFY(window.report()->section(QStringLiteral("result")));
        QStringList shown;
        for (const QLabel* l : window.report()->findChildren<QLabel*>()) shown << l->text();
        QVERIFY(shown.join(' ').contains(QStringLiteral("imported record")));
        QVERIFY(shown.contains(QStringLiteral("Optimal for the continuous relaxation")));
        // Export writes the record shown, unchanged.
        const QString exported = dir.filePath(QStringLiteral("exported.json"));
        window.exportShownRun(exported);
        QCOMPARE(history::readRecordFile(exported), history::readRecordFile(file));
        // Compare the two.
        const QString liveId = window.runsList()->item(1)->data(Qt::UserRole).toString();
        QVERIFY(window.compareSavedRuns(liveId, imported.id));
        QVERIFY(window.isComparing());
        QVERIFY(window.comparisonView()->comparison().sameModel);
        QCOMPARE(window.comparisonView()->comparison().row("origin")->b, QStringLiteral("Imported record"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, window.importRunFile(dir.filePath(QStringLiteral("nothing.json"))));
        QCOMPARE(window.runsList()->count(), 2);
        // Back to one run.
        QVERIFY(window.showSavedRun(liveId));
        QVERIFY(!window.isComparing());
    }

    void recordFixtures() {
        // 21 real optimsolver.solve.v1 records (moved from the retired web
        // Explorer, where they were produced by real solves). expected.tsv holds
        // what the readers must show for each; it was checked against the
        // original JavaScript readers with zero differences before the web
        // Explorer was removed. Each record also goes through import.
        const QString dir = repoPath("desktop/tests/fixtures/records");
        QFile table(dir + QStringLiteral("/expected.tsv"));
        QVERIFY(table.open(QIODevice::ReadOnly | QIODevice::Text));
        QTemporaryDir runs;
        history::RunStore store(runs.path());
        int checked = 0;
        for (const QString& line : QString::fromUtf8(table.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            if (line.startsWith(QLatin1Char('#'))) continue;
            const QStringList f = line.split(QLatin1Char('\t'));
            QCOMPARE(f.size(), 5);
            const QString file = dir + QLatin1Char('/') + f[0] + QStringLiteral(".json");
            const QJsonObject r = history::readRecordFile(file);
            QCOMPARE(record::summarize(r).title, f[1]);
            QCOMPARE(record::summarize(r).code, f[2]);
            QCOMPARE(pipelineStates(r), f[3]);
            QCOMPARE(evidenceStates(r), f[4]);
            const auto entry = store.importFile(file);
            QCOMPARE(store.load(entry.id)->record, r);
            ++checked;
        }
        QCOMPARE(checked, 21);
    }

    void controlledVocabulary() {
        for (const char* path : {"tests/cli/presolve_reduction.mps", "tests/cli/knapsack_milp.mps", "tests/cli/convex_qp.mps",
                                 "tests/mps/test_cases/01_basic_lp.mps", "tests/cli/unbounded_lp.mps"}) {
            const QJsonObject r = solveModel(path);
            QStringList words{record::summarize(r).title, record::summarize(r).detail};
            for (const auto& item : record::buildEvidence(r)) words << item.text << item.stateLabel;
            const QString all = words.join(QLatin1Char(' '));
            QVERIFY2(!all.contains(QStringLiteral("ertified"), Qt::CaseInsensitive) && !all.contains(QStringLiteral("guarantee"), Qt::CaseInsensitive) &&
                         !all.contains(QStringLiteral("proven optimal"), Qt::CaseInsensitive), path);
        }
    }
};

QTEST_MAIN(DesktopTests)
#include "test_desktop.moc"
