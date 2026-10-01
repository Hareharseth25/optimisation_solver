#pragma once

// The analysis of one KAIRO run: Result, Evidence, Pipeline, Model Analysis,
// Presolve Impact, Dispatch, Execution, Validation details.
//
// Built only from an optimsolver.solve.v1 record through record/RecordReaders
// -- the same path for a live run and a saved one. No solver concept is
// computed here; this file decides layout and wording of what the readers
// return.

#include "history/RunStore.h"

#include <QFrame>
#include <QHash>
#include <QJsonObject>
#include <QWidget>

#include <optional>

class QVBoxLayout;

namespace kairo::ui {

// One clickable stage of the pipeline rail.
class StageCard : public QFrame {
    Q_OBJECT
public:
    explicit StageCard(QString target, QWidget* parent = nullptr);
signals:
    void activated(const QString& target);
protected:
    void mousePressEvent(class QMouseEvent* event) override;
    void keyPressEvent(class QKeyEvent* event) override;
private:
    QString target_;
};

class ReportView : public QWidget {
    Q_OBJECT
public:
    struct Context {
        QString fileName;
        qint64 fileSize = -1;
        bool canSave = false;                          // a live run not yet saved
        std::optional<history::RunEntry> saved;        // set when showing a saved run
    };

    explicit ReportView(QWidget* parent = nullptr);

    void showIdle();
    void showRunning(const QString& fileName);
    void showError(const QString& title, const QString& message);
    void showRecord(const QJsonObject& record, const Context& context);

    QFrame* section(const QString& id) const { return sections_.value(id); }
    QStringList sectionOrder() const { return order_; }

signals:
    void navigateTo(QWidget* section);
    void saveRequested();

private:
    void clear();
    QFrame* addSection(const QString& id, const QString& title, const QString& question, QVBoxLayout** body);
    void addBanner(const QString& tone, const QString& title, const QString& code, const QString& detail);

    void buildResult(const QJsonObject& record, const Context& context);
    void buildEvidence(const QJsonObject& record);
    void buildPipeline(const QJsonObject& record);
    QFrame* buildModelAnalysis(const QJsonObject& record);
    QFrame* buildPresolveImpact(const QJsonObject& record);
    void buildDispatch(const QJsonObject& record);
    QFrame* buildExecution(const QJsonObject& record);
    QFrame* buildValidation(const QJsonObject& record);

    QVBoxLayout* layout_ = nullptr;
    QHash<QString, QFrame*> sections_;
    QStringList order_;
};

}  // namespace kairo::ui
