// KAIRO Desktop -- native, offline front end to KAIRO Core.
//
//   KAIRO                         normal desktop application
//   KAIRO --capture MODEL --out PNG [--engine E] [--backend B] [--time-limit S] [--threads N]
//                                 solve MODEL through KAIRO Core, render the full
//                                 analysis, save a screenshot and exit. Used for
//                                 automated verification (works with -platform offscreen).
//                                 --save also saves the run; --export FILE writes its record.
//   KAIRO --import RECORD --out PNG
//                                 import an optimsolver.solve.v1 record (never solves),
//                                 render it as an imported run, save a screenshot, exit.
//   KAIRO --compare A.json --compare B.json --out PNG
//                                 import two records, render the comparison, save a
//                                 screenshot and print its rows, exit.
//
// --import and --compare write to --runs-dir, or to a temporary directory
// when it is not given, never to the person's own saved runs.

#include "core/KairoSession.h"
#include "history/RunStore.h"
#include "record/RecordReaders.h"
#include "ui/ComparisonView.h"
#include "ui/MainWindow.h"
#include "ui/ReportView.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QIcon>
#include <QListWidget>
#include <QPixmap>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QTextStream>

#include <memory>
#include <stdexcept>

namespace {

bool saveShot(kairo::ui::MainWindow& window, QWidget* page, const QString& path) {
    window.show();
    QApplication::processEvents();
    window.resize(window.width(), page->sizeHint().height() + 120);
    QApplication::processEvents();
    page->adjustSize();
    QApplication::processEvents();
    return window.grab().save(path);
}

void printSummary(QTextStream& out, kairo::ui::MainWindow& window, const QJsonObject& record) {
    const auto headline = kairo::record::summarize(record);
    const auto pipeline = kairo::record::buildPipeline(record);
    out << "headline\t" << headline.title << "\n";
    out << "status\t" << headline.code << "\n";
    QStringList states;
    for (const auto& stage : pipeline.stages) states << stage.id + QLatin1Char(':') + stage.state;
    out << "pipeline\t" << states.join(' ') << "\n";
    QStringList evidence;
    for (const auto& item : kairo::record::buildEvidence(record)) evidence << item.key + QLatin1Char(':') + item.state;
    out << "evidence\t" << evidence.join(' ') << "\n";
    out << "sections\t" << window.report()->sectionOrder().join(',') << "\n";
}

int importAndShow(kairo::ui::MainWindow& window, const QCommandLineParser& args) {
    QTextStream out(stdout);
    QTextStream err(stderr);
    QStringList files = args.isSet("compare") ? args.values("compare") : QStringList{args.value("import")};
    if (args.isSet("compare") && files.size() != 2) {
        err << "--compare needs exactly two record files\n";
        return 2;
    }
    QStringList ids;
    for (const QString& file : files) {
        try {
            ids << window.importRunFile(file).id;
        } catch (const std::exception& error) {
            out << "error\t" << QString::fromUtf8(error.what()) << "\n";
            return 1;
        }
    }
    if (ids.size() == 2) {
        if (!window.compareSavedRuns(ids.at(0), ids.at(1))) return 1;
        if (args.isSet("out") && !saveShot(window, window.comparisonView(), args.value("out"))) return 2;
        const auto& comparison = window.comparisonView()->comparison();
        out << "same_model\t" << (comparison.sameModel ? "yes" : "no") << "\n";
        for (const auto& note : comparison.notes) out << "note\t" << note.key << "\t" << note.text << "\n";
        for (const auto& group : comparison.groups) {
            for (const auto& row : group.rows) {
                out << "row\t" << group.key << "\t" << row.label << "\t" << row.a << "\t" << row.b << "\t" << row.delta << "\n";
            }
        }
        return 0;
    }
    if (args.isSet("out") && !saveShot(window, window.report(), args.value("out"))) return 2;
    const auto saved = window.runsList()->count();
    out << "imported\t" << saved << "\n";
    printSummary(out, window, kairo::history::readRecordFile(files.first()));
    return 0;
}

int capture(kairo::ui::MainWindow& window, const QCommandLineParser& args) {
    QTextStream err(stderr);
    if (!args.isSet("out")) {
        err << "--capture needs --out <file.png>\n";
        return 2;
    }
    window.openModelPath(args.value("capture"));
    if (args.isSet("engine") && !window.setEngine(args.value("engine"))) { err << "unknown engine\n"; return 2; }
    if (args.isSet("backend") && !window.setBackend(args.value("backend"))) { err << "unknown backend\n"; return 2; }
    if (args.isSet("time-limit")) window.setTimeLimit(args.value("time-limit"));
    if (args.isSet("threads")) window.setThreads(args.value("threads").toInt());

    // The same in-process call the Solve button makes, without the worker thread.
    const kairo::core::SolveRun run = kairo::core::solve(window.currentRequest());
    window.showRun(run);
    if (args.isSet("save")) window.saveCurrentRun();
    if (args.isSet("export") && run.ok) {
        try {
            window.exportShownRun(args.value("export"));
        } catch (const std::exception& error) {
            err << "cannot export: " << error.what() << "\n";
            return 2;
        }
    }

    // Render the whole report: grow the window until the report fits.
    if (!saveShot(window, window.report(), args.value("out"))) { err << "cannot write " << args.value("out") << "\n"; return 2; }

    // A machine-checkable summary of what was shown (stdout).
    QTextStream out(stdout);
    if (!run.ok) {
        out << "error\t" << run.error << "\n";
        return 1;
    }
    printSummary(out, window, run.record);
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("KAIRO"));
    QApplication::setOrganizationName(QStringLiteral("KAIRO"));
    QApplication::setApplicationDisplayName(QStringLiteral("KAIRO"));
    QApplication::setApplicationVersion(QStringLiteral(KAIRO_DESKTOP_VERSION));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/org.kairo.desktop.png")));
    QGuiApplication::setDesktopFileName(QStringLiteral("org.kairo.desktop"));

    QCommandLineParser args;
    args.setApplicationDescription(QStringLiteral("KAIRO — Kernel for Advanced Integer & Real Optimization (desktop)"));
    args.addHelpOption();
    args.addOption({"capture", "Solve MODEL, render the analysis, save a screenshot and exit.", "model"});
    args.addOption({"out", "Screenshot file for --capture.", "png"});
    args.addOption({"engine", "Force an engine (auto or a KAIRO engine name).", "engine"});
    args.addOption({"backend", "Compute backend: auto, cpu or cuda.", "backend"});
    args.addOption({"time-limit", "Time limit in seconds.", "seconds"});
    args.addOption({"threads", "Worker threads (0 = auto).", "n"});
    args.addOption({"save", "With --capture: also save the run locally."});
    args.addOption({"export", "With --capture: also export the run record to FILE.", "file"});
    args.addOption({"import", "Import RECORD (an optimsolver.solve.v1 file), render it and exit.", "record"});
    args.addOption({"compare", "Import two records (give twice), render their comparison and exit.", "record"});
    args.addOption({"runs-dir", "Directory for saved runs (default: the per-user application data location).", "dir"});
    args.process(app);

    const bool importing = args.isSet("import") || args.isSet("compare");
    QTemporaryDir scratch;
    const QString runsDir = args.isSet("runs-dir") ? QDir(args.value("runs-dir")).absolutePath()
                          : importing              ? scratch.path()
                                                   : kairo::history::RunStore::defaultDirectory();
    kairo::ui::MainWindow window(std::make_unique<kairo::history::RunStore>(runsDir));
    if (args.isSet("capture")) return capture(window, args);
    if (importing) return importAndShow(window, args);
    window.show();
    return QApplication::exec();
}
