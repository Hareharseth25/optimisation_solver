#pragma once

// KAIRO Desktop main window: model input, solver options, Solve, the run
// analysis (ReportView), locally saved runs, comparison of two saved runs
// (ComparisonView) and export/import of run records. Talks to KAIRO Core
// only through core/KairoSession (in-process calls).

#include "core/KairoSession.h"
#include "history/RunStore.h"

#include <QFutureWatcher>
#include <QJsonObject>
#include <QMainWindow>

#include <memory>
#include <optional>

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QStackedWidget;

namespace kairo::ui {

class ComparisonView;
class ReportView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(std::unique_ptr<history::RunStore> store, QWidget* parent = nullptr);
    ~MainWindow() override;

    // Also used by the --capture verification mode and the tests.
    bool openModelPath(const QString& path);
    bool setEngine(const QString& engine);        // "auto" or a solver::Engine name
    bool setBackend(const QString& backend);      // auto | cpu | cuda
    void setTimeLimit(const QString& seconds);
    void setThreads(int threads);
    core::SolveRequest currentRequest() const;
    void showRun(const core::SolveRun& run);       // display a finished live run
    bool saveCurrentRun();
    bool showSavedRun(const QString& id);
    bool compareSavedRuns(const QString& idA, const QString& idB);
    // Throw std::runtime_error with a reason; the menu actions show it in a dialog.
    history::RunEntry importRunFile(const QString& path);
    void exportShownRun(const QString& path) const;

    ReportView* report() const { return report_; }
    ComparisonView* comparisonView() const { return comparison_; }
    QScrollArea* reportScroll() const { return reportScroll_; }
    QListWidget* runsList() const { return runs_; }
    bool isComparing() const;
    bool hasShownRecord() const { return shown_.has_value(); }

protected:
    void dragEnterEvent(class QDragEnterEvent* event) override;
    void dropEvent(class QDropEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct Shown {
        QJsonObject record;
        QString fileName;
        std::optional<history::RunEntry> saved;  // empty = the live run
    };

    QWidget* buildSidebar();
    void buildMenus();
    void chooseModel();
    void openModelFromUser(const QString& path);
    void rememberRecentModel(const QString& path);
    void rebuildRecentMenu();
    void solve();
    void onSolveFinished();
    void showCurrentRun();
    void refreshHistory(const QString& selectId = QString());
    QStringList selectedRunIds() const;
    void openSelectedRun();
    void deleteSelectedRuns();
    void clearSavedRuns();
    void compareSelectedRuns();
    void importRun();
    void exportRun();
    void closeComparison();
    void showAbout();
    void updateControls();
    void navigateTo(QWidget* section);
    void goToSection(const QString& id);
    void rerender();
    void failure(const QString& title, const QString& message);

    std::unique_ptr<history::RunStore> store_;
    std::optional<core::ModelInfo> model_;
    std::optional<core::SolveRun> live_;
    std::optional<history::RunEntry> liveSaved_;
    std::optional<Shown> shown_;
    std::optional<std::pair<QString, QString>> compared_;
    QFutureWatcher<core::SolveRun>* watcher_ = nullptr;

    QLabel* modelState_ = nullptr;
    QLabel* modelDetails_ = nullptr;
    QComboBox* engine_ = nullptr;
    QComboBox* backend_ = nullptr;
    QLineEdit* timeLimit_ = nullptr;
    QSpinBox* threads_ = nullptr;
    QSpinBox* cudaDevice_ = nullptr;
    QLabel* backendNote_ = nullptr;
    QLabel* formError_ = nullptr;
    QPushButton* solveButton_ = nullptr;
    QListWidget* runs_ = nullptr;
    QPushButton* openRun_ = nullptr;
    QPushButton* compareRuns_ = nullptr;
    QPushButton* deleteRun_ = nullptr;
    QPushButton* clearRuns_ = nullptr;
    QPushButton* showLive_ = nullptr;
    QLabel* historyMessage_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    ReportView* report_ = nullptr;
    QScrollArea* reportScroll_ = nullptr;
    ComparisonView* comparison_ = nullptr;
    QScrollArea* comparisonScroll_ = nullptr;

    QMenu* recentMenu_ = nullptr;
    QAction* solveAction_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* exportAction_ = nullptr;
    QAction* compareAction_ = nullptr;
    QAction* showLiveAction_ = nullptr;
    QList<QAction*> sectionActions_;
};

}  // namespace kairo::ui
