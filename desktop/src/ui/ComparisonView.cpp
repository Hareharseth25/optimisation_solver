#include "ComparisonView.h"

#include "Theme.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

namespace kairo::ui {
namespace {

namespace r = kairo::record;

constexpr int kLabelWidth = 200;

QLabel* label(const QString& text, const QString& role = QString()) {
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setTextFormat(Qt::PlainText);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (!role.isEmpty()) l->setProperty("role", role);
    return l;
}

QString describe(const QString& letter, const r::ComparedRun& run) {
    QString text = letter + QStringLiteral(" · ") + (run.fileName.isEmpty() ? r::kNotRecorded : run.fileName);
    if (run.savedAt.isValid()) text += QStringLiteral(" · saved ") + QLocale().toString(run.savedAt, QLocale::ShortFormat);
    if (run.imported) text += QStringLiteral(" · imported record");
    return text;
}

}  // namespace

ComparisonView::ComparisonView(QWidget* parent) : QWidget(parent), layout_(new QVBoxLayout(this)) {
    setObjectName(QStringLiteral("comparisonPage"));
    layout_->setContentsMargins(24, 20, 24, 32);
    layout_->setSpacing(14);
}

void ComparisonView::clear() {
    while (QLayoutItem* item = layout_->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide();
            w->deleteLater();
        } else if (QLayout* l = item->layout()) {
            while (QLayoutItem* inner = l->takeAt(0)) {
                if (QWidget* w = inner->widget()) {
                    w->hide();
                    w->deleteLater();
                }
                delete inner;
            }
        }
        delete item;
    }
}

void ComparisonView::showComparison(const r::ComparedRun& a, const r::ComparedRun& b) {
    clear();
    comparison_ = r::buildComparison(a, b);
    const Theme& t = theme();

    auto* head = new QHBoxLayout;
    auto* title = label(QStringLiteral("Run comparison"));
    title->setStyleSheet(QStringLiteral("font-size: 20px; font-weight: 700;"));
    head->addWidget(title, 1);
    auto* close = new QPushButton(QStringLiteral("Close comparison"));
    close->setObjectName(QStringLiteral("closeComparisonButton"));
    close->setShortcut(QKeySequence(Qt::Key_Escape));
    connect(close, &QPushButton::clicked, this, &ComparisonView::closeRequested);
    head->addWidget(close);
    layout_->addLayout(head);
    layout_->addWidget(label(describe(QStringLiteral("Run A"), a), QStringLiteral("muted")));
    layout_->addWidget(label(describe(QStringLiteral("Run B"), b), QStringLiteral("muted")));

    for (const r::ComparisonNote& note : comparison_.notes) {
        auto* frame = new QFrame;
        frame->setObjectName(QStringLiteral("note-") + note.key);
        frame->setStyleSheet(QStringLiteral("QFrame#note-%1 { background: %2; border: 1px solid %3; border-left: 4px solid %4; border-radius: 6px; }")
                                 .arg(note.key, t.surface.name(), t.border.name(), t.tone(note.tone).name()));
        auto* fl = new QVBoxLayout(frame);
        fl->setContentsMargins(14, 8, 14, 8);
        QLabel* text = label(note.text);
        if (note.key == QLatin1String("model")) text->setStyleSheet(QStringLiteral("font-weight: 600; color: %1;").arg(t.tone(note.tone).name()));
        fl->addWidget(text);
        layout_->addWidget(frame);
    }

    for (const r::ComparisonGroup& group : comparison_.groups) {
        auto* frame = new QFrame;
        frame->setProperty("section", true);
        frame->setObjectName(QStringLiteral("compare-") + group.key);
        auto* fl = new QVBoxLayout(frame);
        fl->setContentsMargins(18, 14, 18, 16);
        fl->addWidget(label(group.title.toUpper(), QStringLiteral("sectionTitle")));
        auto* grid = new QGridLayout;
        grid->setHorizontalSpacing(18);
        grid->setVerticalSpacing(5);
        // Same column widths in every group, so the groups line up.
        grid->setColumnMinimumWidth(0, kLabelWidth);
        for (int column = 1; column < 4; ++column) grid->setColumnStretch(column, 1);
        const QStringList headers{QString(), QStringLiteral("RUN A"), QStringLiteral("RUN B"), QStringLiteral("DIFFERENCE")};
        for (int column = 0; column < headers.size(); ++column) {
            grid->addWidget(label(headers[column], QStringLiteral("figureLabel")), 0, column);
        }
        int row = 1;
        for (const r::ComparisonRow& cr : group.rows) {
            QLabel* name = label(cr.label, QStringLiteral("muted"));
            name->setFixedWidth(kLabelWidth);
            grid->addWidget(name, row, 0, Qt::AlignTop);
            if (cr.notCompared) {
                grid->addWidget(label(cr.a, QStringLiteral("notRun")), row, 1, 1, 3);
                ++row;
                continue;
            }
            for (int side = 0; side < 2; ++side) {
                const QString value = side == 0 ? cr.a : cr.b;
                QLabel* cell = label(value, value == r::kNotRecorded ? QStringLiteral("notRun") : QString());
                if (cr.same == 0) cell->setStyleSheet(QStringLiteral("font-weight: 600;"));
                grid->addWidget(cell, row, 1 + side, Qt::AlignTop);
            }
            // A stated difference, or that the values differ -- never which is better.
            const QString diff = !cr.delta.isEmpty() ? cr.delta : cr.same == 0 ? QStringLiteral("differs") : QString();
            QLabel* d = label(diff, QStringLiteral("muted"));
            d->setObjectName(QStringLiteral("diff-") + cr.key);
            grid->addWidget(d, row, 3, Qt::AlignTop);
            ++row;
        }
        fl->addLayout(grid);
        layout_->addWidget(frame);
    }
    layout_->addStretch(1);
}

}  // namespace kairo::ui
