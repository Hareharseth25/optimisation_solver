#include "MainWindow.h"

#include "ComparisonView.h"
#include "ReportView.h"
#include "Theme.h"
#include "record/Format.h"
#include "record/RecordReaders.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <cmath>
#include <stdexcept>

namespace kairo::ui {
namespace {

namespace f = kairo::format;

bool isModelFile(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("mps") || suffix == QLatin1String("qps");
}

// Engines with a CUDA backend (solver::ComputeBackend; docs/cuda.md). The
// others always run on the CPU, so the backend choice does not apply to them.
bool engineHasCudaBackend(const QString& engine) {
    return engine == QLatin1String("auto") || engine == QLatin1String("pdlp") || engine == QLatin1String("qp");
}

const QString kRecentModelsKey = QStringLiteral("recentModels");
constexpr int kMaxRecentModels = 8;

// Report sections reachable from the keyboard (View menu, Ctrl/Cmd+1..8).
const QList<QPair<QString, QString>>& sectionShortcuts() {
    static const QList<QPair<QString, QString>> sections{
        {QStringLiteral("result"), QStringLiteral("Result")},
        {QStringLiteral("evidence"), QStringLiteral("Evidence")},
        {QStringLiteral("pipeline"), QStringLiteral("Solver Pipeline")},
        {QStringLiteral("model-analysis"), QStringLiteral("Model Analysis")},
        {QStringLiteral("presolve-impact"), QStringLiteral("Presolve Impact")},
        {QStringLiteral("dispatch"), QStringLiteral("Dispatch Decision")},
        {QStringLiteral("execution"), QStringLiteral("Execution")},
        {QStringLiteral("validation"), QStringLiteral("Validation Details")},
    };
    return sections;
}

QScrollArea* scrollFor(QWidget* page) {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(page);
    return scroll;
}

}  // namespace

MainWindow::MainWindow(std::unique_ptr<history::RunStore> store, QWidget* parent)
    : QMainWindow(parent), store_(std::move(store)), watcher_(new QFutureWatcher<core::SolveRun>(this)) {
    setWindowTitle(QStringLiteral("KAIRO"));
    setAcceptDrops(true);
    setStyleSheet(theme().styleSheet());

    auto* central = new QWidget;
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* header = new QFrame;
    header->setObjectName(QStringLiteral("header"));
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(20, 12, 20, 12);
    auto* wordmark = new QLabel(QStringLiteral("KAIRO"));
    wordmark->setObjectName(QStringLiteral("wordmark"));
    auto* product = new QLabel(QStringLiteral("Desktop"));
    product->setObjectName(QStringLiteral("product"));
    auto* tagline = new QLabel(QStringLiteral("Kernel for Advanced Integer & Real Optimization"));
    tagline->setObjectName(QStringLiteral("tagline"));
    hl->addWidget(wordmark);
    hl->addWidget(product);
    hl->addSpacing(16);
    hl->addWidget(tagline);
    hl->addStretch(1);
    auto* offline = new QLabel(QStringLiteral("Local · offline"));
    offline->setProperty("role", QStringLiteral("muted"));
    hl->addWidget(offline);
    layout->addWidget(header);

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(buildSidebar());
    // One run's analysis, or two saved runs side by side.
    report_ = new ReportView;
    reportScroll_ = scrollFor(report_);
    comparison_ = new ComparisonView;
    comparisonScroll_ = scrollFor(comparison_);
    stack_ = new QStackedWidget;
    stack_->addWidget(reportScroll_);
    stack_->addWidget(comparisonScroll_);
    splitter->addWidget(stack_);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({320, 1100});
    layout->addWidget(splitter, 1);
    setCentralWidget(central);
    resize(1440, 920);

    connect(watcher_, &QFutureWatcher<core::SolveRun>::finished, this, &MainWindow::onSolveFinished);
    connect(report_, &ReportView::saveRequested, this, [this] { saveCurrentRun(); });
    connect(report_, &ReportView::navigateTo, this, &MainWindow::navigateTo);
    connect(comparison_, &ComparisonView::closeRequested, this, &MainWindow::closeComparison);
    buildMenus();
    refreshHistory();
    updateControls();
}

MainWindow::~MainWindow() {
    if (watcher_->isRunning()) watcher_->waitForFinished();
}

QWidget* MainWindow::buildSidebar() {
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("sidebarPage"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(18, 14, 18, 18);

    // ---- Model
    auto* modelBox = new QGroupBox(QStringLiteral("MODEL"));
    auto* ml = new QVBoxLayout(modelBox);
    auto* open = new QPushButton(QStringLiteral("Open Model…"));
    open->setObjectName(QStringLiteral("openModelButton"));
    open->setShortcut(QKeySequence::Open);
    open->setToolTip(QStringLiteral("Open an MPS model (%1)").arg(QKeySequence(QKeySequence::Open).toString(QKeySequence::NativeText)));
    connect(open, &QPushButton::clicked, this, &MainWindow::chooseModel);
    ml->addWidget(open);
    modelState_ = new QLabel(QStringLiteral("No model loaded. Open an .mps file or drop it on the window."));
    modelState_->setObjectName(QStringLiteral("modelState"));
    modelState_->setWordWrap(true);
    modelState_->setProperty("role", QStringLiteral("muted"));
    ml->addWidget(modelState_);
    modelDetails_ = new QLabel;
    modelDetails_->setObjectName(QStringLiteral("modelDetails"));
    modelDetails_->setWordWrap(true);
    modelDetails_->setTextFormat(Qt::RichText);
    ml->addWidget(modelDetails_);
    layout->addWidget(modelBox);

    // ---- Solver options: only options KAIRO's SolverOptions already has.
    auto* solverBox = new QGroupBox(QStringLiteral("SOLVER"));
    auto* form = new QFormLayout(solverBox);
    engine_ = new QComboBox;
    engine_->setObjectName(QStringLiteral("engineCombo"));
    engine_->addItem(QStringLiteral("Automatic (dispatcher decides)"), QStringLiteral("auto"));
    for (const auto& [engine, name] : core::selectableEngines()) engine_->addItem(record::engineDisplayName(name), name);
    backend_ = new QComboBox;
    backend_->setObjectName(QStringLiteral("backendCombo"));
    backend_->addItem(QStringLiteral("Auto (CUDA when available)"), QStringLiteral("auto"));
    backend_->addItem(QStringLiteral("CPU"), QStringLiteral("cpu"));
    backend_->addItem(QStringLiteral("CUDA"), QStringLiteral("cuda"));
    backendNote_ = new QLabel;
    backendNote_->setWordWrap(true);
    backendNote_->setProperty("role", QStringLiteral("muted"));
    timeLimit_ = new QLineEdit;
    timeLimit_->setObjectName(QStringLiteral("timeLimitEdit"));
    timeLimit_->setPlaceholderText(QStringLiteral("none"));
    threads_ = new QSpinBox;
    threads_->setRange(0, 4096);
    threads_->setSpecialValueText(QStringLiteral("auto"));
    cudaDevice_ = new QSpinBox;
    cudaDevice_->setRange(0, 1024);
    form->addRow(QStringLiteral("Engine"), engine_);
    form->addRow(QStringLiteral("Backend"), backend_);
    form->addRow(QString(), backendNote_);
    form->addRow(QStringLiteral("Time limit (s)"), timeLimit_);
    form->addRow(QStringLiteral("Threads"), threads_);
    form->addRow(QStringLiteral("CUDA device"), cudaDevice_);
    // Long entries must not widen the sidebar.
    for (QComboBox* combo : {engine_, backend_}) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(14);
    }
    connect(engine_, &QComboBox::currentIndexChanged, this, &MainWindow::updateControls);
    connect(backend_, &QComboBox::currentIndexChanged, this, &MainWindow::updateControls);
    layout->addWidget(solverBox);

    formError_ = new QLabel;
    formError_->setWordWrap(true);
    formError_->setStyleSheet(QStringLiteral("color: %1;").arg(theme().error.name()));
    formError_->hide();
    layout->addWidget(formError_);
    solveButton_ = new QPushButton(QStringLiteral("Solve"));
    solveButton_->setObjectName(QStringLiteral("solveButton"));
    connect(solveButton_, &QPushButton::clicked, this, &MainWindow::solve);
    layout->addWidget(solveButton_);

    // ---- Saved runs (this computer only)
    auto* runsBox = new QGroupBox(QStringLiteral("SAVED RUNS"));
    auto* rl = new QVBoxLayout(runsBox);
    auto* privacy = new QLabel(QStringLiteral("Stored only on this computer. Model files are not copied — only KAIRO's result record. "
                                              "Select two runs to compare them."));
    privacy->setWordWrap(true);
    privacy->setProperty("role", QStringLiteral("muted"));
    rl->addWidget(privacy);
    historyMessage_ = new QLabel;
    historyMessage_->setWordWrap(true);
    rl->addWidget(historyMessage_);
    runs_ = new QListWidget;
    runs_->setObjectName(QStringLiteral("runsList"));
    runs_->setMinimumHeight(160);
    runs_->setWordWrap(true);
    runs_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    runs_->setSpacing(2);
    runs_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // Enter or double-click opens; Delete / Backspace deletes.
    connect(runs_, &QListWidget::itemActivated, this, &MainWindow::openSelectedRun);
    connect(runs_, &QListWidget::itemSelectionChanged, this, &MainWindow::updateControls);
    for (const QKeySequence& key : {QKeySequence(QKeySequence::Delete), QKeySequence(Qt::Key_Backspace)}) {
        auto* shortcut = new QShortcut(key, runs_);
        shortcut->setContext(Qt::WidgetShortcut);
        connect(shortcut, &QShortcut::activated, this, &MainWindow::deleteSelectedRuns);
    }
    rl->addWidget(runs_);
    auto* buttons = new QHBoxLayout;
    openRun_ = new QPushButton(QStringLiteral("Open"));
    openRun_->setObjectName(QStringLiteral("openRunButton"));
    compareRuns_ = new QPushButton(QStringLiteral("Compare"));
    compareRuns_->setObjectName(QStringLiteral("compareRunsButton"));
    deleteRun_ = new QPushButton(QStringLiteral("Delete"));
    deleteRun_->setObjectName(QStringLiteral("deleteRunButton"));
    connect(openRun_, &QPushButton::clicked, this, &MainWindow::openSelectedRun);
    connect(compareRuns_, &QPushButton::clicked, this, &MainWindow::compareSelectedRuns);
    connect(deleteRun_, &QPushButton::clicked, this, &MainWindow::deleteSelectedRuns);
    buttons->addWidget(openRun_);
    buttons->addWidget(compareRuns_);
    buttons->addWidget(deleteRun_);
    rl->addLayout(buttons);
    auto* buttons2 = new QHBoxLayout;
    showLive_ = new QPushButton(QStringLiteral("Show current run"));
    showLive_->setObjectName(QStringLiteral("showLiveButton"));
    connect(showLive_, &QPushButton::clicked, this, &MainWindow::showCurrentRun);
    clearRuns_ = new QPushButton(QStringLiteral("Clear all…"));
    clearRuns_->setObjectName(QStringLiteral("clearRunsButton"));
    connect(clearRuns_, &QPushButton::clicked, this, &MainWindow::clearSavedRuns);
    buttons2->addWidget(showLive_);
    buttons2->addWidget(clearRuns_);
    rl->addLayout(buttons2);
    layout->addWidget(runsBox, 1);

    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("sidebar"));
    auto* fl = new QVBoxLayout(frame);
    fl->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(page);
    fl->addWidget(scroll);
    frame->setMinimumWidth(300);
    frame->setMaximumWidth(380);
    return frame;
}

void MainWindow::updateControls() {
    const QString engine = engine_->currentData().toString();
    const bool backendApplies = engineHasCudaBackend(engine);
    backend_->setEnabled(backendApplies);
    backendNote_->setText(backendApplies ? QString()
                                         : QStringLiteral("%1 has no CUDA backend; it runs on the CPU.").arg(engine_->currentText()));
    backendNote_->setVisible(!backendApplies);
    cudaDevice_->setEnabled(backendApplies && backend_->currentData().toString() != QLatin1String("cpu"));
    const bool running = watcher_->isRunning();
    // Any chosen file can be solved: KAIRO itself records an unreadable or
    // invalid model as an invalid_model run, exactly as the CLI does.
    solveButton_->setEnabled(model_.has_value() && !running);
    solveButton_->setText(running ? QStringLiteral("Running…") : QStringLiteral("Solve"));
    const int selected = runs_->selectedItems().size();
    openRun_->setEnabled(selected == 1);
    compareRuns_->setEnabled(selected == 2);
    compareRuns_->setToolTip(selected == 2 ? QStringLiteral("Compare the two selected runs")
                                           : QStringLiteral("Select exactly two saved runs to compare"));
    deleteRun_->setEnabled(selected > 0);
    clearRuns_->setEnabled(runs_->count() > 0);
    showLive_->setEnabled(live_.has_value());
    if (solveAction_) {
        solveAction_->setEnabled(solveButton_->isEnabled());
        saveAction_->setEnabled(live_.has_value() && !liveSaved_);
        exportAction_->setEnabled(shown_.has_value() && !isComparing());
        compareAction_->setEnabled(selected == 2);
        showLiveAction_->setEnabled(live_.has_value());
        for (QAction* action : sectionActions_) action->setEnabled(shown_.has_value() && !isComparing());
    }
}

bool MainWindow::openModelPath(const QString& path) {
    model_ = core::openModel(path);
    const core::ModelInfo& m = *model_;
    if (!m.readable) {
        // KAIRO's reader refused the file; solving it would only repeat that.
        modelState_->setText(QStringLiteral("%1 — KAIRO could not read this file.").arg(m.fileName));
        modelState_->setStyleSheet(QStringLiteral("color: %1;").arg(theme().error.name()));
        modelDetails_->setText(m.readError.toHtmlEscaped());
        updateControls();
        return false;
    }
    modelState_->setText(QStringLiteral("%1 · %2 · ready").arg(m.fileName, f::bytes(m.fileSize)));
    modelState_->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;").arg(theme().ok.name()));
    QString details = QStringLiteral("<table cellspacing=\"2\">");
    const auto row = [&details](const QString& k, const QString& v) {
        details += QStringLiteral("<tr><td style=\"color:%1; padding-right:10px\">%2</td><td>%3</td></tr>").arg(theme().muted.name(), k, v);
    };
    row(QStringLiteral("Name"), m.name.isEmpty() ? QStringLiteral("—") : m.name.toHtmlEscaped());
    row(QStringLiteral("Variables"), f::count(static_cast<double>(m.variables)));
    row(QStringLiteral("Constraints"), f::count(static_cast<double>(m.constraints)));
    row(QStringLiteral("Nonzeros"), f::count(static_cast<double>(m.nonzeros)));
    row(QStringLiteral("Objective"), m.objectiveSense == QLatin1String("max") ? QStringLiteral("Maximize") : QStringLiteral("Minimize"));
    row(QStringLiteral("Structure check"), m.structurallyValid ? QStringLiteral("passed") : QStringLiteral("<b>failed</b>"));
    row(QStringLiteral("Classification"), QStringLiteral("<i>after solve</i>"));
    details += QStringLiteral("</table>");
    modelDetails_->setText(details);
    updateControls();
    return true;
}

bool MainWindow::setEngine(const QString& engine) {
    const int index = engine_->findData(engine);
    if (index < 0) return false;
    engine_->setCurrentIndex(index);
    return true;
}

bool MainWindow::setBackend(const QString& backend) {
    const int index = backend_->findData(backend);
    if (index < 0) return false;
    backend_->setCurrentIndex(index);
    return true;
}

void MainWindow::setTimeLimit(const QString& seconds) { timeLimit_->setText(seconds); }
void MainWindow::setThreads(int threads) { threads_->setValue(threads); }

core::SolveRequest MainWindow::currentRequest() const {
    core::SolveRequest request;
    request.modelPath = model_ ? model_->filePath : QString();
    const QString engine = engine_->currentData().toString();
    if (engine != QLatin1String("auto")) request.engine = solver::parseEngine(engine.toStdString());
    if (backend_->isEnabled()) {
        request.backend = solver::parseComputeBackend(backend_->currentData().toString().toStdString()).value_or(solver::ComputeBackend::Auto);
    }
    if (cudaDevice_->isEnabled()) request.cudaDevice = cudaDevice_->value();
    if (!timeLimit_->text().trimmed().isEmpty()) request.timeLimitSeconds = timeLimit_->text().trimmed().toDouble();
    if (threads_->value() > 0) request.threadCount = threads_->value();
    return request;
}

void MainWindow::buildMenus() {
    // ---- File
    QMenu* file = menuBar()->addMenu(QStringLiteral("&File"));
    QAction* open = file->addAction(QStringLiteral("&Open Model…"), this, &MainWindow::chooseModel);
    open->setShortcut(QKeySequence::Open);
    recentMenu_ = file->addMenu(QStringLiteral("Open &Recent"));
    rebuildRecentMenu();
    file->addSeparator();
    QAction* import = file->addAction(QStringLiteral("&Import Run…"), this, &MainWindow::importRun);
    import->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
    exportAction_ = file->addAction(QStringLiteral("&Export Run…"), this, &MainWindow::exportRun);
    exportAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    file->addSeparator();
    QAction* quit = file->addAction(QStringLiteral("&Quit"), this, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);
    quit->setMenuRole(QAction::QuitRole);

    // ---- Run
    QMenu* run = menuBar()->addMenu(QStringLiteral("&Run"));
    solveAction_ = run->addAction(QStringLiteral("&Solve"), this, &MainWindow::solve);
    solveAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    saveAction_ = run->addAction(QStringLiteral("Sa&ve Run"), this, [this] { saveCurrentRun(); });
    saveAction_->setShortcut(QKeySequence::Save);
    showLiveAction_ = run->addAction(QStringLiteral("Show &Current Run"), this, &MainWindow::showCurrentRun);
    showLiveAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    run->addSeparator();
    compareAction_ = run->addAction(QStringLiteral("&Compare Selected Runs"), this, &MainWindow::compareSelectedRuns);
    compareAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));

    // ---- View: jump to a report section from the keyboard.
    QMenu* view = menuBar()->addMenu(QStringLiteral("&View"));
    int n = 1;
    for (const auto& [id, title] : sectionShortcuts()) {
        QAction* action = view->addAction(QStringLiteral("Go to ") + title, this, [this, id = id] { goToSection(id); });
        action->setShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_0 + n++)));
        sectionActions_ << action;
    }
    view->addSeparator();
    QAction* focusRuns = view->addAction(QStringLiteral("Saved Runs List"), this, [this] { runs_->setFocus(Qt::ShortcutFocusReason); });
    focusRuns->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));

    // ---- Help
    QMenu* help = menuBar()->addMenu(QStringLiteral("&Help"));
    QAction* about = help->addAction(QStringLiteral("&About KAIRO"), this, &MainWindow::showAbout);
    about->setMenuRole(QAction::AboutRole);
}

void MainWindow::chooseModel() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Open Model"), QString(),
                                                      QStringLiteral("MPS models (*.mps *.MPS *.qps *.QPS);;All files (*)"));
    if (!path.isEmpty()) openModelFromUser(path);
}

void MainWindow::openModelFromUser(const QString& path) {
    if (!QFileInfo::exists(path)) {
        failure(QStringLiteral("Model not found"), QStringLiteral("%1 no longer exists.").arg(QDir::toNativeSeparators(path)));
        QSettings settings;
        QStringList recent = settings.value(kRecentModelsKey).toStringList();
        recent.removeAll(path);
        settings.setValue(kRecentModelsKey, recent);
        rebuildRecentMenu();
        return;
    }
    openModelPath(path);
    rememberRecentModel(path);
}

void MainWindow::rememberRecentModel(const QString& path) {
    // Paths of models the person opened, kept in the platform's settings
    // store; never the model contents.
    QSettings settings;
    QStringList recent = settings.value(kRecentModelsKey).toStringList();
    const QString absolute = QFileInfo(path).absoluteFilePath();
    recent.removeAll(absolute);
    recent.prepend(absolute);
    while (recent.size() > kMaxRecentModels) recent.removeLast();
    settings.setValue(kRecentModelsKey, recent);
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu() {
    recentMenu_->clear();
    const QStringList recent = QSettings().value(kRecentModelsKey).toStringList();
    for (const QString& path : recent) {
        QAction* action = recentMenu_->addAction(QFileInfo(path).fileName(), this, [this, path] { openModelFromUser(path); });
        action->setToolTip(QDir::toNativeSeparators(path));
        action->setStatusTip(QDir::toNativeSeparators(path));
    }
    if (recent.isEmpty()) recentMenu_->addAction(QStringLiteral("No recent models"))->setEnabled(false);
    recentMenu_->addSeparator();
    QAction* clear = recentMenu_->addAction(QStringLiteral("Clear Menu"), this, [this] {
        QSettings().remove(kRecentModelsKey);
        rebuildRecentMenu();
    });
    clear->setEnabled(!recent.isEmpty());
}

void MainWindow::solve() {
    if (watcher_->isRunning() || !model_) return;
    // The CLI's own rule for --time-limit: a positive, finite number of seconds.
    const QString limit = timeLimit_->text().trimmed();
    bool ok = true;
    const double seconds = limit.isEmpty() ? 1.0 : limit.toDouble(&ok);
    if (!ok || !std::isfinite(seconds) || seconds <= 0) {
        formError_->setText(QStringLiteral("Time limit must be a positive number of seconds."));
        formError_->show();
        timeLimit_->setFocus();
        return;
    }
    formError_->hide();
    compared_.reset();
    shown_.reset();
    stack_->setCurrentWidget(reportScroll_);
    // No progress figures: KAIRO Core reports nothing until the solve returns,
    // and a running solve cannot be cancelled (the core has no cancellation).
    report_->showRunning(model_->fileName);
    watcher_->setFuture(QtConcurrent::run(core::solve, currentRequest()));
    updateControls();
}

void MainWindow::onSolveFinished() {
    showRun(watcher_->result());
    updateControls();
}

void MainWindow::showRun(const core::SolveRun& run) {
    compared_.reset();
    stack_->setCurrentWidget(reportScroll_);
    if (!run.ok) {
        live_.reset();
        shown_.reset();
        report_->showError(QStringLiteral("KAIRO did not produce a record"), run.error);
        updateControls();
        return;
    }
    if (!live_ || live_->record != run.record) liveSaved_.reset();
    live_ = run;
    shown_ = Shown{run.record, run.fileName, std::nullopt};
    ReportView::Context context;
    context.fileName = run.fileName;
    context.fileSize = run.fileSize;
    context.canSave = !liveSaved_;
    context.saved = liveSaved_;
    report_->showRecord(run.record, context);
    reportScroll_->verticalScrollBar()->setValue(0);
    updateControls();
}

void MainWindow::showCurrentRun() {
    if (live_) showRun(*live_);
}

bool MainWindow::saveCurrentRun() {
    if (!live_ || liveSaved_) return false;
    try {
        liveSaved_ = store_->save(live_->record, live_->fileName, live_->fileSize);
        historyMessage_->setText(QStringLiteral("Saved %1 on this computer.").arg(live_->fileName));
    } catch (const std::exception& error) {
        failure(QStringLiteral("Could not save the run"), QString::fromUtf8(error.what()));
        return false;
    }
    refreshHistory(liveSaved_->id);
    if (shown_ && !shown_->saved && !isComparing()) showRun(*live_);
    return true;
}

void MainWindow::refreshHistory(const QString& selectId) {
    const QSignalBlocker block(runs_);
    runs_->clear();
    for (const history::RunEntry& entry : store_->list()) {
        QString line = (entry.imported ? QStringLiteral("[Imported] ") : QString()) +
                       (entry.fileName.isEmpty() ? record::kNotRecorded : entry.fileName);
        line += QStringLiteral("\n%1 · %2").arg(f::words(entry.status),
                                                entry.engine.isEmpty() ? QStringLiteral("nothing executed") : record::engineDisplayName(entry.engine));
        if (entry.objective) line += QStringLiteral(" · obj ") + f::real(entry.objective);
        if (entry.totalSeconds) line += QStringLiteral(" · ") + f::seconds(entry.totalSeconds);
        line += QLatin1Char('\n') + QLocale().toString(entry.savedAt, QLocale::ShortFormat);
        auto* item = new QListWidgetItem(line, runs_);
        item->setData(Qt::UserRole, entry.id);
        item->setToolTip(entry.imported ? QStringLiteral("Imported record — shown as recorded; not re-solved on this computer.")
                                        : QStringLiteral("Solved on this computer."));
        if (entry.id == selectId) {
            item->setSelected(true);
            runs_->setCurrentItem(item);
        }
    }
    if (runs_->count() == 0 && historyMessage_->text().isEmpty()) {
        historyMessage_->setText(QStringLiteral("No saved runs yet. Use “Save run” on a result, or File → Import Run."));
    }
    updateControls();
}

QStringList MainWindow::selectedRunIds() const {
    // In list order (newest first), independent of the order of clicks.
    QStringList ids;
    for (int i = 0; i < runs_->count(); ++i) {
        if (runs_->item(i)->isSelected()) ids << runs_->item(i)->data(Qt::UserRole).toString();
    }
    return ids;
}

bool MainWindow::showSavedRun(const QString& id) {
    const auto saved = store_->load(id);
    if (!saved) {
        failure(QStringLiteral("Could not open the saved run"), QStringLiteral("The saved run file could not be read. It may have been removed or changed outside KAIRO."));
        refreshHistory();
        return false;
    }
    compared_.reset();
    stack_->setCurrentWidget(reportScroll_);
    shown_ = Shown{saved->record, saved->entry.fileName, saved->entry};
    ReportView::Context context;
    context.fileName = saved->entry.fileName;
    context.fileSize = saved->entry.fileSize;
    context.saved = saved->entry;
    // The same renderer as a live run, from the stored record.
    report_->showRecord(saved->record, context);
    reportScroll_->verticalScrollBar()->setValue(0);
    updateControls();
    return true;
}

void MainWindow::openSelectedRun() {
    const QStringList ids = selectedRunIds();
    if (ids.size() == 1) showSavedRun(ids.first());
}

void MainWindow::deleteSelectedRuns() {
    const QStringList ids = selectedRunIds();
    if (ids.isEmpty()) return;
    if (ids.size() > 1 && QMessageBox::question(this, QStringLiteral("Delete saved runs"),
                                                QStringLiteral("Delete %1 saved runs from this computer?").arg(ids.size())) != QMessageBox::Yes) {
        return;
    }
    for (const QString& id : ids) {
        store_->remove(id);
        if (liveSaved_ && liveSaved_->id == id) liveSaved_.reset();
        if (shown_ && shown_->saved && shown_->saved->id == id) shown_->saved.reset();
    }
    historyMessage_->setText(ids.size() == 1 ? QStringLiteral("Deleted the saved run.")
                                             : QStringLiteral("Deleted %1 saved runs.").arg(ids.size()));
    refreshHistory();
    if (compared_ && (ids.contains(compared_->first) || ids.contains(compared_->second))) closeComparison();
    else rerender();
}

void MainWindow::clearSavedRuns() {
    if (runs_->count() == 0) return;
    if (QMessageBox::question(this, QStringLiteral("Clear saved runs"),
                              QStringLiteral("Delete all %1 saved runs from this computer? This cannot be undone.").arg(runs_->count())) != QMessageBox::Yes) {
        return;
    }
    store_->clear();
    liveSaved_.reset();
    if (shown_) shown_->saved.reset();
    historyMessage_->setText(QStringLiteral("Deleted all saved runs from this computer."));
    refreshHistory();
    if (compared_) closeComparison();
    else rerender();
}

bool MainWindow::compareSavedRuns(const QString& idA, const QString& idB) {
    auto a = store_->load(idA);
    auto b = store_->load(idB);
    if (!a || !b) {
        failure(QStringLiteral("Could not compare"), QStringLiteral("A selected saved run could not be read."));
        refreshHistory();
        return false;
    }
    // Run A is the earlier one, so a stated difference reads "later − earlier".
    if (b->entry.savedAt < a->entry.savedAt) std::swap(a, b);
    const auto compared = [](const history::SavedRun& run) {
        return record::ComparedRun{run.record, run.entry.fileName, run.entry.savedAt, run.entry.imported};
    };
    comparison_->showComparison(compared(*a), compared(*b));
    compared_ = std::make_pair(a->entry.id, b->entry.id);
    stack_->setCurrentWidget(comparisonScroll_);
    comparisonScroll_->verticalScrollBar()->setValue(0);
    updateControls();
    return true;
}

void MainWindow::compareSelectedRuns() {
    const QStringList ids = selectedRunIds();
    if (ids.size() != 2) {
        historyMessage_->setText(QStringLiteral("Select exactly two saved runs to compare."));
        return;
    }
    compareSavedRuns(ids.at(0), ids.at(1));
}

bool MainWindow::isComparing() const { return stack_->currentWidget() == comparisonScroll_; }

void MainWindow::closeComparison() {
    compared_.reset();
    stack_->setCurrentWidget(reportScroll_);
    rerender();
}

history::RunEntry MainWindow::importRunFile(const QString& path) {
    const history::RunEntry entry = store_->importFile(path);
    historyMessage_->setText(QStringLiteral("Imported %1. It is shown as recorded; it was not re-solved here.")
                                 .arg(entry.fileName.isEmpty() ? QFileInfo(path).fileName() : entry.fileName));
    refreshHistory(entry.id);
    showSavedRun(entry.id);
    return entry;
}

void MainWindow::importRun() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import Run"), QString(),
                                                      QStringLiteral("KAIRO run records (*.json);;All files (*)"));
    if (path.isEmpty()) return;
    try {
        importRunFile(path);
    } catch (const std::exception& error) {
        failure(QStringLiteral("Could not import the run"),
                QStringLiteral("%1\n\n%2").arg(QFileInfo(path).fileName(), QString::fromUtf8(error.what())));
    }
}

void MainWindow::exportShownRun(const QString& path) const {
    if (!shown_) throw std::runtime_error("there is no run to export");
    history::writeRecordFile(shown_->record, path);
}

void MainWindow::exportRun() {
    if (!shown_ || isComparing()) return;
    const QString base = QFileInfo(shown_->fileName).completeBaseName();
    const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                                  .filePath((base.isEmpty() ? QStringLiteral("kairo-run") : base) + QStringLiteral(".kairo-run.json"));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export Run"), suggested,
                                                      QStringLiteral("KAIRO run records (*.json)"));
    if (path.isEmpty()) return;
    try {
        exportShownRun(path);
        historyMessage_->setText(QStringLiteral("Exported the run record to %1.").arg(QFileInfo(path).fileName()));
    } catch (const std::exception& error) {
        failure(QStringLiteral("Could not export the run"), QString::fromUtf8(error.what()));
    }
}

void MainWindow::showAbout() {
    QMessageBox::about(this, QStringLiteral("About KAIRO"),
                       QStringLiteral("<b>KAIRO Desktop %1</b><br>Kernel for Advanced Integer &amp; Real Optimization<br><br>"
                                      "Solves run in this process through KAIRO Core. Nothing is sent over a network; "
                                      "saved runs stay in<br><code>%2</code><br><br>Qt %3")
                           .arg(QApplication::applicationVersion().toHtmlEscaped(),
                                QDir::toNativeSeparators(store_->directory()).toHtmlEscaped(), QString::fromLatin1(qVersion())));
}

void MainWindow::failure(const QString& title, const QString& message) {
    historyMessage_->setText(title + QStringLiteral(": ") + message);
    // No modal dialogs in the headless verification modes.
    if (QGuiApplication::platformName() == QLatin1String("offscreen")) return;
    QMessageBox::warning(this, title, message);
}

void MainWindow::goToSection(const QString& id) {
    if (isComparing()) return;
    if (QFrame* section = report_->section(id)) {
        navigateTo(section);
        section->setFocus(Qt::ShortcutFocusReason);
    }
}

void MainWindow::navigateTo(QWidget* section) {
    reportScroll_->ensureWidgetVisible(section, 0, 16);
    section->setProperty("highlighted", true);
    section->style()->unpolish(section);
    section->style()->polish(section);
    QTimer::singleShot(1600, section, [section] {
        section->setProperty("highlighted", false);
        section->style()->unpolish(section);
        section->style()->polish(section);
    });
}

// Rebuilds whatever is on screen (after a theme change or a history change).
void MainWindow::rerender() {
    if (compared_) {
        compareSavedRuns(compared_->first, compared_->second);
    } else if (watcher_->isRunning()) {
        report_->showRunning(model_ ? model_->fileName : QString());
    } else if (shown_ && shown_->saved) {
        showSavedRun(shown_->saved->id);
    } else if (shown_ && live_ && shown_->record == live_->record) {
        showRun(*live_);
    } else if (shown_) {
        // A saved run that was just deleted: still shown, no longer saved.
        ReportView::Context context;
        context.fileName = shown_->fileName;
        report_->showRecord(shown_->record, context);
    } else {
        report_->showIdle();
    }
    updateControls();
}

void MainWindow::changeEvent(QEvent* event) {
    if ((event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::ThemeChange) && reloadTheme()) {
        setStyleSheet(theme().styleSheet());
        rerender();
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.size() == 1 && urls.first().isLocalFile() && isModelFile(urls.first().toLocalFile())) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event) {
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.size() == 1 && urls.first().isLocalFile()) openModelFromUser(urls.first().toLocalFile());
}

}  // namespace kairo::ui
