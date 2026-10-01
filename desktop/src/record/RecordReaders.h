#pragma once

// Interpretation of one KAIRO run, read from its optimsolver.solve.v1 record.
//
// The record is produced in-process by KAIRO's own writer from the
// SolveResult and SolveReport of the solve (see core/KairoSession). These
// readers were ported from the retired web Explorer's readers, checked value
// for value against them, and are pinned by real records in
// desktop/tests/fixtures/records. The trust semantics:
//
//   * nothing is recomputed -- feasibility, residuals, objectives and
//     integrality come from KAIRO's validation (validation.*, self_reported.*);
//   * dispatch.reason is shown verbatim, never parsed;
//   * selected and executed engines stay separate; pseudo-engines
//     (infeasible, trivial, unsupported) are outcomes, not engines;
//   * a status is never worded more strongly than the recorded evidence.
//
// Qt Core only (QJsonObject); no widgets.

#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <optional>

namespace kairo::record {

using Record = QJsonObject;
using Number = std::optional<double>;

// ---------------------------------------------------------------- dispatch

struct PseudoEngine {
    QString label;
    QString title;
    QString note;
};

// solver::Engine values that are dispatch OUTCOMES, not solver engines.
const PseudoEngine* pseudoEngine(const QString& engine);
bool isPseudoEngine(const QString& engine);
// Pseudo-engines keep their outcome wording; real engines KAIRO's own name.
QString engineDisplayName(const QString& engine);

struct BackendView {
    QString requested;   // empty = not recorded
    QString executed;    // empty = none
    Number requestedDevice;
    Number executedDevice;
    QString reason;
    bool mismatch = false;  // explicit cpu/cuda request that did not come true
};

struct DispatchView {
    QString state;      // not_called | not_invoked | invoked
    QString mode;       // automatic | forced
    QString requested;  // the caller's own spelling; empty when automatic
    QString selected;   // dispatch.engine; empty when dispatch is null
    QString pseudo;     // empty, or a pseudo-engine key
    QString executed;   // empty = nothing executed
    QString reason;     // the dispatcher's sentence, verbatim
    bool selectedNotExecuted = false;
    bool backendRefusal = false;
    bool engineRejectedModel = false;
    bool hasRefusal = false;
    QString refusalMessage;
    QString refusalBackendReason;
    Number engineSeconds;
    QString engineTimeKind;  // "" | execution | path
    Number dispatchSeconds;
    bool hasReducedModel = false;
    Number reducedVariables;
    Number reducedConstraints;
    Number reducedNonzeros;
    BackendView backend;
};

DispatchView readDispatch(const Record& record);

// ---------------------------------------------------------------- result & evidence

struct ResultFacts {
    QString status;
    QString message;
    QString sense;          // min | max | ""
    Number objective;
    bool hasPoint = false;
    Number pointSize;
    std::optional<bool> integerModel;          // unknown when not classified
    std::optional<bool> integralityRespected;
    Number integralityViolation;
    bool hasDuals = false;
    Number dualResidual;
    QString dualsUnavailableReason;
    QJsonObject reduced;   // validation.reduced_space, empty when absent
    QJsonObject original;  // validation.original_space, empty when absent
    bool presolveInfeasible = false;
    QString executed;
    QString engine;        // display name of the executed engine
    QString backend;
    Number totalSeconds;
    DispatchView dispatch;
};

ResultFacts resultFacts(const Record& record);

struct Interpretation {
    QString kind;
    QString tone;   // ok | warn | error
    QString title;
    QString detail;
};

Interpretation interpretResult(const Record& record);

struct EvidenceItem {
    QString key;
    QString label;
    QString state;       // checked | failed | not_checked | not_available | not_applicable
                         // | nothing | not_established | reported | proved_presolve | returned | none
    QString stateLabel;  // controlled vocabulary
    QString tone;        // ok | error | muted | warn | neutral
    QString text;
    QString detail;
};

QList<EvidenceItem> buildEvidence(const Record& record);
QString evidenceStateLabel(const QString& state);
QString evidenceStateTone(const QString& state);

// ---------------------------------------------------------------- pipeline

struct PipelineStage {
    QString id;
    QString label;
    QString target;   // the detailed section this stage points to
    QString state;    // completed | not_run | failed | unsupported | refused | infeasible | unbounded | limit
    QString tone;
    QString stateLabel;
    QStringList lines;
    Number seconds;
    QString timeLabel;
    bool terminal = false;
};

struct Pipeline {
    QList<PipelineStage> stages;
    QString terminatedAt;
    QString summary;
};

Pipeline buildPipeline(const Record& record);

// ---------------------------------------------------------------- model & presolve

struct StructureFlag {
    QString key;
    QString label;
    bool detected = false;
    QString detail;
};

struct ModelAnalysis {
    QString state;  // classified | unclassified | unreadable
    QString problemClass;
    QString sense;  // Minimize | Maximize | ""
    Number variables;
    Number constraints;
    Number nonzeros;
    Number continuous;
    Number integer;
    Number binary;
    QList<StructureFlag> structure;
    Number coefRangeRatio;
    QString reason;
    QString message;
};

ModelAnalysis buildModelAnalysis(const Record& record);

struct ImpactRow {
    QString key;
    QString label;
    Number original;
    Number reduced;
    Number change;
    Number reduction;  // percent; absent for zero denominators and stopped presolve
};

struct PresolveImpact {
    QString state;  // not_run | infeasible | converged | not_converged
    QString stateLabel;
    QString reason;
    bool stopped = false;
    QString heading;
    QList<ImpactRow> rows;
    Number transformations;
    QList<QPair<QString, double>> byType;  // record vocabulary, nonzero only
    Number seconds;
};

Number reductionPercent(Number original, Number reduced);
PresolveImpact buildPresolveImpact(const Record& record);

// ---------------------------------------------------------------- headline

struct Headline {
    QString tone;
    QString title;
    QString detail;
    QString code;  // KAIRO status, kept as evidence
};

Headline summarize(const Record& record);

}  // namespace kairo::record
