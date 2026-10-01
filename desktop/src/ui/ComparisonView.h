#pragma once

// Two saved runs side by side. Built only from record::buildComparison: the
// rows, the "Not recorded" / "Not compared" cells and the notes are decided
// there; this file lays them out. No ranking, no verdict.

#include "record/Comparison.h"

#include <QWidget>

class QVBoxLayout;

namespace kairo::ui {

class ComparisonView : public QWidget {
    Q_OBJECT
public:
    explicit ComparisonView(QWidget* parent = nullptr);

    void showComparison(const record::ComparedRun& a, const record::ComparedRun& b);
    const record::Comparison& comparison() const { return comparison_; }

signals:
    void closeRequested();

private:
    void clear();
    record::Comparison comparison_;
    QVBoxLayout* layout_ = nullptr;
};

}  // namespace kairo::ui
