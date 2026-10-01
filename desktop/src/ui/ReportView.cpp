#include "ReportView.h"

#include "Theme.h"
#include "record/Format.h"
#include "record/RecordReaders.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include <cmath>
#include <tuple>

namespace kairo::ui {
namespace {

namespace f = kairo::format;
namespace r = kairo::record;

QString esc(const QString& text) { return text.toHtmlEscaped(); }
std::optional<double> num(const QJsonValue& v) { return v.isDouble() ? std::optional<double>(v.toDouble()) : std::nullopt; }
QString str(const QJsonValue& v) { return v.isString() ? v.toString() : QString(); }
QJsonObject obj(const QJsonValue& v) { return v.isObject() ? v.toObject() : QJsonObject(); }

QLabel* label(const QString& text, const QString& role = QString(), bool rich = false) {
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setTextFormat(rich ? Qt::RichText : Qt::PlainText);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (!role.isEmpty()) l->setProperty("role", role);
    return l;
}

QString colour(const QString& tone) { return theme().tone(tone).name(); }
QString mono(const QString& text) { return QStringLiteral("<span style=\"font-family:Menlo,Consolas,'DejaVu Sans Mono',monospace\">%1</span>").arg(esc(text)); }
QString muted(const QString& text) { return QStringLiteral("<span style=\"color:%1\">%2</span>").arg(theme().muted.name(), esc(text)); }

QLabel* badge(const QString& text, const QString& tone) {
    auto* b = new QLabel(text);
    const Theme& t = theme();
    b->setStyleSheet(QStringLiteral("QLabel { color: %1; background: %2; border-radius: 9px; padding: 1px 8px; font-size: 11px; font-weight: 600; }")
                         .arg(t.tone(tone).name(), t.toneSoft(tone).name()));
    b->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    return b;
}

QLabel* notRun(const QString& reason = QString()) {
    return label(f::kNotRun + (reason.isEmpty() ? QString() : QStringLiteral(" — ") + reason), QStringLiteral("notRun"));
}

QFrame* box(bool dashed = false) {
    auto* frame = new QFrame;
    frame->setProperty("box", true);
    if (dashed) frame->setProperty("dashed", true);
    return frame;
}

QWidget* figure(const QString& title, const QString& value, bool monoValue = false) {
    QFrame* frame = box();
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(2);
    layout->addWidget(label(title.toUpper(), QStringLiteral("figureLabel")));
    QLabel* v = label(value, QStringLiteral("figureValue"));
    if (monoValue) v->setStyleSheet(QStringLiteral("font-family: Menlo, Consolas, 'DejaVu Sans Mono', monospace;"));
    layout->addWidget(v);
    return frame;
}

// Label/value rows.
class Rows {
public:
    explicit Rows(QVBoxLayout* parent) : grid_(new QGridLayout) {
        grid_->setHorizontalSpacing(18);
        grid_->setVerticalSpacing(5);
        grid_->setColumnStretch(1, 1);
        parent->addLayout(grid_);
    }
    void add(const QString& key, QWidget* value) {
        QLabel* k = label(key, QStringLiteral("muted"));
        k->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        grid_->addWidget(k, row_, 0, Qt::AlignTop);
        grid_->addWidget(value, row_++, 1);
    }
    void add(const QString& key, const QString& richValue) { add(key, label(richValue, QString(), true)); }
private:
    QGridLayout* grid_;
    int row_ = 0;
};

QString seconds(std::optional<double> s) { return f::seconds(s); }

}  // namespace

// ============================================================ StageCard

StageCard::StageCard(QString target, QWidget* parent) : QFrame(parent), target_(std::move(target)) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
}

void StageCard::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) emit activated(target_);
    QFrame::mousePressEvent(event);
}

void StageCard::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space) {
        emit activated(target_);
        return;
    }
    QFrame::keyPressEvent(event);
}

// ============================================================ ReportView

ReportView::ReportView(QWidget* parent) : QWidget(parent), layout_(new QVBoxLayout(this)) {
    setObjectName(QStringLiteral("reportPage"));
    layout_->setContentsMargins(24, 20, 24, 32);
    layout_->setSpacing(14);
    showIdle();
}

void ReportView::clear() {
    sections_.clear();
    order_.clear();
    // Hide before deleteLater(): the old report must not paint again while
    // its deletion waits for the event loop.
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

void ReportView::showIdle() {
    clear();
    auto* title = label(QStringLiteral("No run yet"));
    title->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 700;"));
    layout_->addWidget(title);
    layout_->addWidget(label(QStringLiteral("Open an MPS model, choose solver options, and press Solve. KAIRO's record of the run "
                                            "appears here: result, evidence, pipeline, model, presolve, dispatch, execution and validation."),
                             QStringLiteral("muted")));
    layout_->addStretch(1);
}

void ReportView::showRunning(const QString& fileName) {
    clear();
    // No percentages or stage names: KAIRO reports nothing until it finishes.
    auto* running = label(QStringLiteral("Running KAIRO… ") + fileName);
    running->setObjectName(QStringLiteral("runningLabel"));
    running->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 600;"));
    layout_->addWidget(running);
    layout_->addStretch(1);
}

void ReportView::showError(const QString& title, const QString& message) {
    clear();
    addBanner(QStringLiteral("error"), title, QString(), message);
    layout_->addStretch(1);
}

void ReportView::addBanner(const QString& tone, const QString& title, const QString& code, const QString& detail) {
    auto* banner = new QFrame;
    banner->setObjectName(QStringLiteral("banner"));
    const Theme& t = theme();
    banner->setStyleSheet(QStringLiteral("QFrame#banner { background: %1; border: 1px solid %2; border-left: 4px solid %3; border-radius: 6px; }")
                              .arg((tone == QLatin1String("error") ? t.errorSoft : t.surface).name(), t.border.name(), t.tone(tone).name()));
    auto* layout = new QVBoxLayout(banner);
    layout->setContentsMargins(18, 12, 18, 12);
    auto* head = new QHBoxLayout;
    QLabel* titleLabel = label(title);
    titleLabel->setObjectName(QStringLiteral("bannerTitle"));
    titleLabel->setStyleSheet(QStringLiteral("font-size: 20px; font-weight: 700; color: %1;").arg(t.tone(tone).name()));
    // The title takes the row; the status code sits at its end.
    head->addWidget(titleLabel, 1);
    if (!code.isEmpty()) {
        QLabel* codeLabel = label(code, QStringLiteral("muted"));
        codeLabel->setWordWrap(false);
        codeLabel->setStyleSheet(QStringLiteral("font-family: Menlo, Consolas, 'DejaVu Sans Mono', monospace; font-size: 12px;"));
        head->addWidget(codeLabel, 0, Qt::AlignBottom);
    }
    layout->addLayout(head);
    if (!detail.isEmpty()) layout->addWidget(label(detail));
    layout_->addWidget(banner);
}

QFrame* ReportView::addSection(const QString& id, const QString& title, const QString& question, QVBoxLayout** body) {
    auto* frame = new QFrame;
    frame->setProperty("section", true);
    frame->setObjectName(QStringLiteral("section-") + id);
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(18, 14, 18, 16);
    layout->setSpacing(8);
    layout->addWidget(label(title.toUpper(), QStringLiteral("sectionTitle")));
    if (!question.isEmpty()) layout->addWidget(label(question, QStringLiteral("question")));
    sections_.insert(id, frame);
    order_ << id;
    *body = layout;
    return frame;
}

void ReportView::showRecord(const QJsonObject& record, const Context& context) {
    clear();
    const r::Headline headline = r::summarize(record);
    addBanner(headline.tone, headline.title, headline.code, headline.detail);
    // Result -> evidence -> pipeline -> model + presolve -> dispatch -> execution + validation.
    buildResult(record, context);
    buildEvidence(record);
    buildPipeline(record);
    auto* analysis = new QHBoxLayout;
    analysis->setSpacing(14);
    analysis->addWidget(buildModelAnalysis(record), 1);
    analysis->addWidget(buildPresolveImpact(record), 1);
    layout_->addLayout(analysis);
    buildDispatch(record);
    auto* details = new QHBoxLayout;
    details->setSpacing(14);
    details->addWidget(buildExecution(record), 1);
    details->addWidget(buildValidation(record), 1);
    layout_->addLayout(details);
    layout_->addStretch(1);
}

// ------------------------------------------------------------ Result

void ReportView::buildResult(const QJsonObject& record, const Context& context) {
    QVBoxLayout* body = nullptr;
    layout_->addWidget(addSection(QStringLiteral("result"), QStringLiteral("Result"), QString(), &body));
    const r::ResultFacts facts = r::resultFacts(record);
    const QString sense = facts.sense == QLatin1String("max") ? QStringLiteral("maximize ")
                        : facts.sense == QLatin1String("min") ? QStringLiteral("minimize ") : QString();
    auto* figures = new QHBoxLayout;
    figures->setSpacing(0);
    figures->addWidget(figure(QStringLiteral("Status"), f::words(facts.status)));
    figures->addWidget(figure(QStringLiteral("Objective"), facts.objective ? sense + f::real(facts.objective) : QStringLiteral("None"), true));
    figures->addWidget(figure(QStringLiteral("Solution point"), facts.hasPoint ? QStringLiteral("Returned") : QStringLiteral("None")));
    figures->addWidget(figure(QStringLiteral("Engine executed"), facts.engine.isEmpty() ? QStringLiteral("None") : facts.engine));
    figures->addWidget(figure(QStringLiteral("Backend"), facts.backend.isEmpty() ? QStringLiteral("None") : facts.backend.toUpper()));
    figures->addWidget(figure(QStringLiteral("Total solve time"), seconds(facts.totalSeconds), true));
    body->addLayout(figures);

    Rows rows(body);
    rows.add(QStringLiteral("Termination message"), facts.message.isEmpty() ? muted(QStringLiteral("no message")) : esc(facts.message));
    rows.add(QStringLiteral("Model file"), esc(context.fileName.isEmpty() ? f::kUnknown : context.fileName) +
                                             (context.fileSize >= 0 ? esc(QStringLiteral(" (") + f::bytes(context.fileSize) + QLatin1Char(')')) : QString()));
    rows.add(QStringLiteral("Input SHA-256"), mono(f::shortHash(str(obj(record.value("instance")).value("sha256")), 16)));
    const QJsonObject solver = obj(record.value("solver"));
    rows.add(QStringLiteral("KAIRO build"), mono(f::shortHash(str(solver.value("commit")), 10) +
                                                 (str(solver.value("build_type")).isEmpty() ? QString() : QStringLiteral(" · ") + str(solver.value("build_type")))) +
                                                muted(QStringLiteral(" — commit recorded when the build was configured")));
    if (context.saved) {
        const QString when = QLocale().toString(context.saved->savedAt, QLocale::ShortFormat);
        if (context.saved->imported) {
            rows.add(QStringLiteral("Saved"), esc(QStringLiteral("imported record · added ") + when) +
                                              muted(QStringLiteral(" — shown as recorded; not re-solved on this computer")));
        } else {
            rows.add(QStringLiteral("Saved"), esc(QStringLiteral("on this computer · ") + when));
        }
    }
    if (context.canSave) {
        auto* save = new QPushButton(QStringLiteral("Save run"));
        save->setObjectName(QStringLiteral("saveRunButton"));
        connect(save, &QPushButton::clicked, this, &ReportView::saveRequested);
        auto* actions = new QHBoxLayout;
        actions->addWidget(save);
        actions->addStretch(1);
        body->addLayout(actions);
    }
}

// ------------------------------------------------------------ Evidence

void ReportView::buildEvidence(const QJsonObject& record) {
    QVBoxLayout* body = nullptr;
    layout_->addWidget(addSection(QStringLiteral("evidence"), QStringLiteral("Evidence"),
                                  QStringLiteral("What supports this result? Every state below is read from KAIRO's own record."), &body));
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(3, 1);
    int row = 0;
    for (const r::EvidenceItem& item : r::buildEvidence(record)) {
        // A check mark only for something actually checked or returned.
        const bool good = item.state == QLatin1String("checked") || item.state == QLatin1String("returned");
        const bool bad = item.state == QLatin1String("failed");
        QLabel* mark = label(good ? QStringLiteral("✓") : bad ? QStringLiteral("✗") : QStringLiteral("—"));
        mark->setStyleSheet(QStringLiteral("font-weight: 700; color: %1;").arg(colour(good ? "ok" : bad ? "error" : "muted")));
        QLabel* name = label(item.label);
        name->setStyleSheet(QStringLiteral("font-weight: 600;"));
        QString text = esc(item.text);
        if (!item.detail.isEmpty()) text += QStringLiteral("<br>") + muted(item.detail);
        grid->addWidget(mark, row, 0, Qt::AlignTop);
        grid->addWidget(name, row, 1, Qt::AlignTop);
        grid->addWidget(badge(item.stateLabel, item.tone == QLatin1String("muted") ? QStringLiteral("neutral") : item.tone), row, 2, Qt::AlignTop);
        grid->addWidget(label(text, QString(), true), row++, 3);
    }
    body->addLayout(grid);
}

// ------------------------------------------------------------ Pipeline

void ReportView::buildPipeline(const QJsonObject& record) {
    QVBoxLayout* body = nullptr;
    layout_->addWidget(addSection(QStringLiteral("pipeline"), QStringLiteral("Solver pipeline"), QString(), &body));
    const r::Pipeline pipeline = r::buildPipeline(record);
    QLabel* summary = label(pipeline.summary);
    summary->setObjectName(QStringLiteral("pipelineSummary"));
    summary->setStyleSheet(QStringLiteral("font-weight: 600;"));
    body->addWidget(summary);
    auto* rail = new QGridLayout;
    rail->setSpacing(6);
    const Theme& t = theme();
    int index = 0;
    for (const r::PipelineStage& stage : pipeline.stages) {
        auto* card = new StageCard(stage.target);
        card->setObjectName(QStringLiteral("stage-") + stage.id);
        card->setProperty("stageState", stage.state);
        const bool notRun = stage.state == QLatin1String("not_run");
        const QColor edge = notRun ? t.borderStrong : t.tone(stage.tone);
        card->setStyleSheet(QStringLiteral("kairo--ui--StageCard { background: %1; border: %2px %3 %4; border-top: 4px solid %5; border-radius: 6px; }")
                                .arg((notRun ? t.surface : t.surface2).name())
                                .arg(stage.terminal ? 2 : 1)
                                .arg(notRun ? QStringLiteral("dashed") : QStringLiteral("solid"))
                                .arg(stage.terminal ? edge.name() : t.border.name(), edge.name()));
        card->setToolTip(QStringLiteral("Show %1 details").arg(stage.target));
        auto* layout = new QVBoxLayout(card);
        layout->setContentsMargins(10, 10, 10, 8);
        layout->setSpacing(2);
        layout->addWidget(label(stage.label.toUpper(), QStringLiteral("figureLabel")));
        QLabel* state = label(stage.stateLabel);
        state->setStyleSheet(notRun ? QStringLiteral("color: %1; font-style: italic;").arg(t.muted.name())
                                    : QStringLiteral("color: %1; font-weight: 700; font-size: 13px;").arg(t.tone(stage.tone).name()));
        layout->addWidget(state);
        for (const QString& line : stage.lines) {
            QLabel* l = label(line);
            l->setStyleSheet(QStringLiteral("font-size: 11px;"));
            l->setMaximumHeight(60);
            layout->addWidget(l);
        }
        layout->addStretch(1);
        if (stage.seconds) {
            layout->addWidget(label(f::seconds(stage.seconds) + (stage.timeLabel.isEmpty() ? QString() : QLatin1Char(' ') + stage.timeLabel),
                                    QStringLiteral("muted")));
        }
        if (stage.terminal) {
            QLabel* end = label(QStringLiteral("ENDED HERE"));
            end->setStyleSheet(QStringLiteral("font-size: 10px; font-weight: 700; color: %1;").arg(edge.name()));
            layout->addWidget(end);
        }
        connect(card, &StageCard::activated, this, [this](const QString& target) {
            if (QFrame* section = sections_.value(target)) emit navigateTo(section);
        });
        // Eight across on wide windows, two rows of four otherwise.
        rail->addWidget(card, index / 8, index % 8);
        rail->setColumnStretch(index % 8, 1);
        ++index;
    }
    body->addLayout(rail);
}

// ------------------------------------------------------------ Model analysis

QFrame* ReportView::buildModelAnalysis(const QJsonObject& record) {
    QVBoxLayout* body = nullptr;
    QFrame* frame = addSection(QStringLiteral("model-analysis"), QStringLiteral("Model analysis"), QStringLiteral("What did KAIRO receive?"), &body);
    const r::ModelAnalysis a = r::buildModelAnalysis(record);
    if (a.state == QLatin1String("classified")) {
        auto* head = new QHBoxLayout;
        QLabel* tag = badge(a.problemClass, QStringLiteral("accent"));
        tag->setObjectName(QStringLiteral("analysisClass"));
        head->addWidget(tag);
        if (!a.sense.isEmpty()) head->addWidget(label(a.sense));
        head->addStretch(1);
        body->addLayout(head);
        auto* figures = new QHBoxLayout;
        figures->setSpacing(0);
        figures->addWidget(figure(QStringLiteral("Variables"), f::count(a.variables), true));
        figures->addWidget(figure(QStringLiteral("Constraints"), f::count(a.constraints), true));
        figures->addWidget(figure(QStringLiteral("Nonzeros"), f::count(a.nonzeros), true));
        body->addLayout(figures);
        Rows rows(body);
        rows.add(QStringLiteral("Variable composition"),
                 esc(QStringLiteral("%1 continuous · %2 integer · %3 binary").arg(f::count(a.continuous), f::count(a.integer), f::count(a.binary))));
        for (const r::StructureFlag& flag : a.structure) {
            rows.add(flag.label, esc(flag.detected ? QStringLiteral("Detected") : QStringLiteral("Not detected")) +
                                     (flag.detail.isEmpty() ? QString() : muted(QStringLiteral(" · ") + flag.detail)));
        }
        if (a.coefRangeRatio) rows.add(QStringLiteral("Coefficient range"), mono(f::real(a.coefRangeRatio) + QStringLiteral(" : 1")));
    } else if (a.state == QLatin1String("unreadable")) {
        body->addWidget(label(QStringLiteral("Model not available — KAIRO could not read the file."), QStringLiteral("notRun")));
        if (!a.message.isEmpty()) body->addWidget(label(a.message, QStringLiteral("muted")));
    } else {
        body->addWidget(label(QStringLiteral("Classification not run — ") + a.reason + QLatin1Char('.'), QStringLiteral("notRun")));
        Rows rows(body);
        rows.add(QStringLiteral("Variables"), mono(f::count(a.variables)));
        rows.add(QStringLiteral("Constraints"), mono(f::count(a.constraints)));
        if (!a.sense.isEmpty()) rows.add(QStringLiteral("Objective"), esc(a.sense));
    }
    body->addStretch(1);
    return frame;
}

// ------------------------------------------------------------ Presolve impact

QFrame* ReportView::buildPresolveImpact(const QJsonObject& record) {
    QVBoxLayout* body = nullptr;
    QFrame* frame = addSection(QStringLiteral("presolve-impact"), QStringLiteral("Presolve impact"), QStringLiteral("What did presolve change?"), &body);
    const r::PresolveImpact impact = r::buildPresolveImpact(record);
    if (impact.state == QLatin1String("not_run")) {
        body->addWidget(notRun(impact.reason));
        body->addStretch(1);
        return frame;
    }
    auto* state = new QHBoxLayout;
    state->addWidget(badge(impact.stateLabel, impact.state == QLatin1String("converged") ? QStringLiteral("ok") : QStringLiteral("warn")));
    if (impact.seconds) {
        QLabel* time = label(QStringLiteral("presolve time ") + f::seconds(impact.seconds), QStringLiteral("muted"));
        time->setWordWrap(false);
        state->addWidget(time);
    }
    state->addStretch(1);
    body->addLayout(state);
    QLabel* heading = label(impact.heading);
    heading->setStyleSheet(QStringLiteral("font-weight: 700;"));
    body->addWidget(heading);
    if (impact.stopped) {
        body->addWidget(label(QStringLiteral("Presolve proved the model infeasible and stopped; these are not a reduced model, so no reduction is reported."),
                              QStringLiteral("muted")));
    }
    auto* table = new QGridLayout;
    table->setHorizontalSpacing(16);
    const QStringList headers = impact.stopped ? QStringList{"", "Original", "When stopped", "Change"}
                                               : QStringList{"", "Original", "Reduced", "Change", "Reduction"};
    for (int c = 0; c < headers.size(); ++c) {
        QLabel* h = label(headers[c].toUpper(), QStringLiteral("figureLabel"));
        table->addWidget(h, 0, c, c == 0 ? Qt::AlignLeft : Qt::AlignRight);
    }
    int row = 1;
    for (const r::ImpactRow& item : impact.rows) {
        table->addWidget(label(item.label), row, 0);
        table->addWidget(label(f::count(item.original), QStringLiteral("mono")), row, 1, Qt::AlignRight);
        table->addWidget(label(f::count(item.reduced), QStringLiteral("mono")), row, 2, Qt::AlignRight);
        const QString change = !item.change ? QStringLiteral("—")
                             : *item.change == 0 ? QStringLiteral("0")
                             : (*item.change > 0 ? QStringLiteral("+") : QStringLiteral("−")) + f::count(std::fabs(*item.change));
        table->addWidget(label(change, QStringLiteral("mono")), row, 3, Qt::AlignRight);
        if (!impact.stopped) {
            // Zero denominators were left absent by the reader: never NaN or Infinity.
            const QString reduction = !item.reduction ? QStringLiteral("—")
                                    : *item.reduction == 0 ? QStringLiteral("0 %")
                                    : QString::number(std::round(*item.reduction * 10) / 10, 'f', QLocale::FloatingPointShortest) + QStringLiteral(" %");
            table->addWidget(label(reduction, QStringLiteral("mono")), row, 4, Qt::AlignRight);
        }
        ++row;
    }
    body->addLayout(table);
    // Record vocabulary: counts of logged transformations, not of entities removed.
    QStringList kinds;
    for (const auto& [type, count] : impact.byType) kinds << mono(type) + esc(QStringLiteral(" ×") + f::count(count));
    body->addWidget(label(esc(f::count(impact.transformations) +
                              (impact.transformations.value_or(0) == 1 ? QStringLiteral(" transformation logged") : QStringLiteral(" transformations logged"))) +
                              (kinds.isEmpty() ? QString() : QStringLiteral("&nbsp;&nbsp;") + kinds.join(QStringLiteral("&nbsp;&nbsp;"))),
                          QString(), true));
    body->addStretch(1);
    return frame;
}

// ------------------------------------------------------------ Dispatch

void ReportView::buildDispatch(const QJsonObject& record) {
    QVBoxLayout* body = nullptr;
    layout_->addWidget(addSection(QStringLiteral("dispatch"), QStringLiteral("Dispatch decision"), QStringLiteral("What did KAIRO decide, and why?"), &body));
    const r::DispatchView dx = r::readDispatch(record);
    Rows rows(body);
    QString request = dx.mode == QLatin1String("forced") ? QStringLiteral("Forced by caller: ") + mono(dx.requested)
                                                         : esc(QStringLiteral("Automatic — the dispatcher chooses"));
    if (dx.mode == QLatin1String("forced") && dx.state != QLatin1String("invoked")) request += muted(QStringLiteral(" · not applied, the dispatcher was not invoked"));
    rows.add(QStringLiteral("Request"), request);

    if (dx.state == QLatin1String("not_called")) {
        auto* line = new QWidget;
        auto* l = new QHBoxLayout(line);
        l->setContentsMargins(0, 0, 0, 0);
        l->addWidget(badge(QStringLiteral("Not reached"), QStringLiteral("neutral")));
        l->addWidget(label(QStringLiteral("KAIRO rejected the model before dispatch."), QStringLiteral("muted")));
        l->addStretch(1);
        rows.add(QStringLiteral("Dispatcher"), line);
        return;
    }
    auto* decision = new QWidget;
    auto* dl = new QHBoxLayout(decision);
    dl->setContentsMargins(0, 0, 0, 0);
    dl->addWidget(dx.state == QLatin1String("invoked") ? badge(QStringLiteral("Invoked"), QStringLiteral("ok"))
                                                       : badge(QStringLiteral("Not invoked"), QStringLiteral("neutral")));
    if (dx.dispatchSeconds) dl->addWidget(label(f::seconds(dx.dispatchSeconds), QStringLiteral("muted")));
    dl->addStretch(1);
    rows.add(QStringLiteral("Dispatcher"), decision);
    // dispatch.reason is the dispatcher's own sentence: shown verbatim.
    if (!dx.reason.isEmpty()) {
        QLabel* reason = label(dx.reason);
        reason->setObjectName(QStringLiteral("dispatchReason"));
        reason->setStyleSheet(QStringLiteral("background: %1; border-left: 3px solid %2; padding: 6px 10px;")
                                  .arg(theme().surface2.name(), theme().borderStrong.name()));
        rows.add(QStringLiteral("Why — KAIRO dispatcher"), reason);
    }

    auto* flow = new QHBoxLayout;
    QFrame* selected = box(!dx.pseudo.isEmpty());
    selected->setObjectName(QStringLiteral("selectedEngine"));
    auto* sl = new QVBoxLayout(selected);
    if (const r::PseudoEngine* pseudo = r::pseudoEngine(dx.pseudo)) {
        sl->addWidget(label(dx.state == QLatin1String("invoked") ? QStringLiteral("DISPATCH OUTCOME") : QStringLiteral("OUTCOME BEFORE DISPATCH"),
                            QStringLiteral("figureLabel")));
        sl->addWidget(label(pseudo->label, QStringLiteral("figureValue")));
        sl->addWidget(label(pseudo->note, QStringLiteral("muted")));
    } else {
        sl->addWidget(label(QStringLiteral("SELECTED ENGINE"), QStringLiteral("figureLabel")));
        sl->addWidget(label(dx.selected.isEmpty() ? f::kUnknown : r::engineDisplayName(dx.selected), QStringLiteral("figureValue")));
    }
    QFrame* executed = box(dx.executed.isEmpty());
    executed->setObjectName(QStringLiteral("executedEngine"));
    if (!dx.executed.isEmpty()) executed->setStyleSheet(QStringLiteral("QFrame#executedEngine { border-left: 4px solid %1; }").arg(theme().accent.name()));
    auto* el = new QVBoxLayout(executed);
    el->addWidget(label(QStringLiteral("EXECUTED"), QStringLiteral("figureLabel")));
    QLabel* executedName = label(dx.executed.isEmpty() ? QStringLiteral("Nothing executed") : r::engineDisplayName(dx.executed), QStringLiteral("figureValue"));
    if (dx.executed.isEmpty()) executedName->setStyleSheet(QStringLiteral("color: %1; font-style: italic;").arg(theme().muted.name()));
    el->addWidget(executedName);
    flow->addWidget(selected, 1);
    flow->addWidget(label(QStringLiteral("→"), QStringLiteral("muted")));
    flow->addWidget(executed, 1);
    body->addLayout(flow);

    if (dx.hasRefusal) {
        auto* refusal = new QFrame;
        refusal->setObjectName(QStringLiteral("refusal"));
        refusal->setStyleSheet(QStringLiteral("QFrame#refusal { background: %1; border-left: 4px solid %2; border-radius: 6px; }")
                                   .arg(theme().errorSoft.name(), theme().error.name()));
        auto* rl = new QVBoxLayout(refusal);
        QLabel* title = label(dx.backendRefusal ? QStringLiteral("Backend refusal")
                            : dx.engineRejectedModel ? QStringLiteral("Engine rejected the model") : QStringLiteral("Execution refusal"));
        title->setStyleSheet(QStringLiteral("font-weight: 700; color: %1;").arg(theme().error.name()));
        rl->addWidget(title);
        rl->addWidget(label(QStringLiteral("Why %1 did not run, as reported by its execution path — not the dispatcher's reason.")
                                .arg(r::engineDisplayName(dx.selected)), QStringLiteral("muted")));
        if (!dx.refusalMessage.isEmpty()) rl->addWidget(label(dx.refusalMessage));
        if (!dx.refusalBackendReason.isEmpty()) rl->addWidget(label(QStringLiteral("Backend: ") + dx.refusalBackendReason, QStringLiteral("muted")));
        if (dx.engineRejectedModel) {
            rl->addWidget(label(QStringLiteral("The model itself was accepted and classified; the selected engine cannot represent it."), QStringLiteral("muted")));
        }
        body->addWidget(refusal);
    }
    if (dx.hasReducedModel) {
        const auto part = [](std::optional<double> n, const QString& one, const QString& many) {
            return f::count(n) + QLatin1Char(' ') + (n && *n == 1 ? one : many);
        };
        body->addWidget(label(muted(QStringLiteral("Observed reduced model given to the dispatcher: ")) +
                                  mono(part(dx.reducedVariables, "variable", "variables") + QStringLiteral(" · ") +
                                       part(dx.reducedConstraints, "constraint", "constraints") + QStringLiteral(" · ") +
                                       part(dx.reducedNonzeros, "nonzero", "nonzeros")),
                              QString(), true));
    }
}

// ------------------------------------------------------------ Execution

QFrame* ReportView::buildExecution(const QJsonObject& record) {
    QVBoxLayout* body = nullptr;
    QFrame* frame = addSection(QStringLiteral("execution"), QStringLiteral("Execution"), QStringLiteral("What actually happened?"), &body);
    const r::DispatchView dx = r::readDispatch(record);
    const r::BackendView& b = dx.backend;
    const QJsonObject term = obj(record.value("termination"));
    const QJsonObject work = obj(record.value("work"));
    const QJsonObject stages = obj(record.value("stage_seconds"));
    Rows rows(body);
    rows.add(QStringLiteral("Engine"), dx.executed.isEmpty() ? muted(QStringLiteral("nothing executed"))
                                                             : QStringLiteral("<b>%1</b>").arg(esc(r::engineDisplayName(dx.executed))));
    QString backend = esc(QStringLiteral("requested ") + (b.requested.isEmpty() ? f::kUnknown : b.requested));
    if (b.requested == QLatin1String("cuda") && b.requestedDevice) backend += esc(QStringLiteral(" (device %1)").arg(*b.requestedDevice));
    backend += QStringLiteral(" → executed <b>%1</b>").arg(esc(b.executed.isEmpty() ? QStringLiteral("none") : b.executed));
    if (b.executedDevice) backend += esc(QStringLiteral(" (device %1)").arg(*b.executedDevice));
    rows.add(QStringLiteral("Backend"), backend);
    if (b.mismatch) {
        rows.add(QString(), QStringLiteral("<span style=\"color:%1\">%2</span>")
                                .arg(theme().warn.name(), esc(QStringLiteral("The %1 request was not honoured.").arg(b.requested))));
    }
    // When nothing ran, the record writer copies the termination message here; shown once.
    if (!b.reason.isEmpty() && b.reason != str(term.value("message"))) rows.add(QStringLiteral("Backend reason"), muted(b.reason));
    const QString message = str(term.value("message"));
    rows.add(QStringLiteral("Termination"), mono(str(term.value("status"))) + muted(message.isEmpty() ? QStringLiteral(" — no message") : QStringLiteral(" — ") + message));
    if (num(work.value("iterations"))) rows.add(QStringLiteral("Iterations"), mono(f::count(num(work.value("iterations")))));
    if (num(work.value("nodes"))) rows.add(QStringLiteral("Nodes"), mono(f::count(num(work.value("nodes")))));

    static const QList<QPair<QString, QString>> kStages{
        {"validation", "Model validation"}, {"classification", "Classification"}, {"presolve", "Presolve"}, {"dispatch", "Dispatch"},
        {"engine", "Engine"}, {"reduced_validation", "Reduced-space validation"}, {"postsolve", "Postsolve"}};
    QStringList skipped;
    auto* table = new QGridLayout;
    table->setColumnStretch(0, 1);
    int row = 0;
    for (const auto& [key, name] : kStages) {
        const std::optional<double> s = num(stages.value(key));
        if (!s) { skipped << name.toLower(); continue; }
        // Engine time is execution only when an engine executed.
        const QString shown = key != QLatin1String("engine") ? name
                            : dx.engineTimeKind == QLatin1String("path") ? QStringLiteral("Engine path (no engine executed)")
                                                                         : QStringLiteral("Engine execution");
        QLabel* stageName = label(shown);
        stageName->setObjectName(QStringLiteral("stageTime-") + key);
        table->addWidget(stageName, row, 0);
        table->addWidget(label(f::seconds(s), QStringLiteral("mono")), row++, 1, Qt::AlignRight);
    }
    if (row == 0) {
        body->addWidget(notRun(QStringLiteral("no pipeline stage ran")));
    } else {
        QLabel* total = label(QStringLiteral("Total"));
        total->setStyleSheet(QStringLiteral("font-weight: 700;"));
        table->addWidget(total, row, 0);
        QLabel* totalValue = label(f::seconds(num(stages.value("total"))), QStringLiteral("mono"));
        totalValue->setStyleSheet(QStringLiteral("font-weight: 700;"));
        table->addWidget(totalValue, row, 1, Qt::AlignRight);
        body->addLayout(table);
        if (!skipped.isEmpty()) body->addWidget(label(QStringLiteral("Did not run: ") + skipped.join(QStringLiteral(", ")) + QLatin1Char('.'), QStringLiteral("muted")));
    }
    body->addWidget(label(QStringLiteral("MPS read ") + f::seconds(num(stages.value("parse"))), QStringLiteral("muted")));
    body->addStretch(1);
    return frame;
}

// ------------------------------------------------------------ Validation details

QFrame* ReportView::buildValidation(const QJsonObject& record) {
    QVBoxLayout* body = nullptr;
    QFrame* frame = addSection(QStringLiteral("validation"), QStringLiteral("Validation details"), QString(), &body);
    const QJsonValue vValue = record.value("validation");
    if (!vValue.isObject()) {
        body->addWidget(notRun(QStringLiteral("the solver was not called")));
        body->addStretch(1);
        return frame;
    }
    const QJsonObject v = vValue.toObject();
    const QJsonObject reduced = obj(v.value("reduced_space"));
    const QJsonObject original = obj(v.value("original_space"));
    auto* checks = new QHBoxLayout;
    for (const auto& [title, check, name] : {std::tuple{QStringLiteral("Reduced space"), reduced, QStringLiteral("reduced")},
                                             std::tuple{QStringLiteral("Original space"), original, QStringLiteral("original")}}) {
        QFrame* panel = box();
        panel->setObjectName(QStringLiteral("check-") + name);
        auto* pl = new QVBoxLayout(panel);
        auto* head = new QHBoxLayout;
        QLabel* t = label(title);
        t->setStyleSheet(QStringLiteral("font-weight: 700;"));
        head->addWidget(t);
        if (check.isEmpty()) {
            head->addStretch(1);
            pl->addLayout(head);
            pl->addWidget(notRun(name == QLatin1String("reduced") ? QStringLiteral("no engine point to check") : QStringLiteral("postsolve did not run")));
        } else {
            const bool ok = check.value("passed").toBool(false);
            head->addWidget(badge(ok ? QStringLiteral("Passed") : QStringLiteral("Failed"), ok ? QStringLiteral("ok") : QStringLiteral("error")));
            head->addStretch(1);
            pl->addLayout(head);
            if (!ok) {
                const QString failure = str(check.value("failure"));
                QLabel* fl = label(f::words(str(check.value("status"))) + (failure.isEmpty() ? QString() : QStringLiteral(": ") + failure));
                fl->setStyleSheet(QStringLiteral("color: %1;").arg(theme().error.name()));
                pl->addWidget(fl);
            }
            Rows rows(pl);
            rows.add(QStringLiteral("Max bound violation"), mono(ok ? f::residual(num(check.value("max_bound_residual"))) : QStringLiteral("Not measured")));
            rows.add(QStringLiteral("Max constraint violation"), mono(ok ? f::residual(num(check.value("max_constraint_residual"))) : QStringLiteral("Not measured")));
            if (ok) {
                rows.add(QStringLiteral("Scaled (bound · row)"), mono(f::residual(num(check.value("max_bound_residual_scaled"))) + QStringLiteral(" · ") +
                                                                     f::residual(num(check.value("max_constraint_residual_scaled")))));
            }
            if (name == QLatin1String("original")) {
                rows.add(QStringLiteral("Duals requested"), esc(check.value("duals_requested").toBool(false) ? QStringLiteral("yes") : QStringLiteral("no")));
            }
        }
        checks->addWidget(panel, 1);
    }
    body->addLayout(checks);

    Rows rows(body);
    if (!reduced.isEmpty() && num(reduced.value("engine_reported_objective"))) {
        rows.add(QStringLiteral("Engine reported objective"), mono(f::real(num(reduced.value("engine_reported_objective")))));
    }
    if (reduced.value("passed").toBool(false)) rows.add(QStringLiteral("Recomputed, reduced model"), mono(f::real(num(reduced.value("objective")))));
    if (original.value("passed").toBool(false)) rows.add(QStringLiteral("Recomputed, original model"), mono(f::real(num(original.value("objective")))));
    if (num(record.value("objective"))) rows.add(QStringLiteral("Reported result"), mono(f::real(num(record.value("objective")))));

    const r::ResultFacts facts = r::resultFacts(record);
    const QJsonObject self = obj(record.value("self_reported"));
    if (!facts.integerModel.value_or(false)) {
        rows.add(QStringLiteral("Integrality"), muted(QStringLiteral("Not applicable (continuous model)")));
    } else if (!facts.integralityRespected) {
        rows.add(QStringLiteral("Integrality"), muted(QStringLiteral("Not applicable (no point)")));
    } else {
        rows.add(QStringLiteral("Integrality"), QStringLiteral("<span style=\"color:%1\"><b>%2</b></span> max violation %3")
                                                    .arg(colour(*facts.integralityRespected ? "ok" : "error"),
                                                         *facts.integralityRespected ? QStringLiteral("Respected") : QStringLiteral("Violated"),
                                                         mono(f::residual(num(self.value("max_integrality_violation"))))));
    }
    rows.add(QStringLiteral("Duals"), record.value("duals").isArray()
                                          ? esc(QStringLiteral("available · max dual residual ")) + mono(f::residual(num(self.value("max_dual_residual"))))
                                          : muted(str(record.value("duals_unavailable_reason")).isEmpty() ? QStringLiteral("Not available")
                                                                                                          : str(record.value("duals_unavailable_reason"))));
    body->addStretch(1);
    return frame;
}

}  // namespace kairo::ui
