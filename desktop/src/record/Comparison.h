#pragma once

// Side-by-side facts of two KAIRO runs, read from their optimsolver.solve.v1
// records through the same readers as a single run (originally ported from
// the retired web Explorer's compare.js).
//
// Rules:
//   * the input SHA-256 recorded by KAIRO is the only same-model check; when
//     the hashes differ (or one is missing), objectives, residuals,
//     validation and presolve reductions are not compared at all;
//   * values are shown as recorded; a value a record does not hold reads
//     "Not recorded", never 0;
//   * differences are stated as differences ("B − A = +1.2 ms"); nothing is
//     ranked and no run is called better;
//   * the engine stage time is labelled with what it measured (execution vs
//     engine path) and the two kinds are never subtracted from each other.
//
// Qt Core only; no widgets.

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace kairo::record {

inline const QString kNotRecorded = QStringLiteral("Not recorded");
inline const QString kNotCompared = QStringLiteral("Not compared — different model inputs");

// One run as the comparison sees it: the record plus the local facts the
// record does not hold.
struct ComparedRun {
    QJsonObject record;
    QString fileName;     // empty = not recorded
    QDateTime savedAt;    // invalid = not recorded
    bool imported = false;
};

struct ComparisonRow {
    QString key;
    QString label;
    QString a;
    QString b;
    bool missing = false;      // at least one side not recorded
    int same = -1;             // 1 same, 0 different, -1 not decidable
    QString delta;             // "B − A = ..." or empty
    bool notCompared = false;
};

struct ComparisonGroup {
    QString key;
    QString title;
    QList<ComparisonRow> rows;
};

struct ComparisonNote {
    QString key;   // model | build | options | timing
    QString tone;  // ok | warn | neutral
    QString text;
};

struct Comparison {
    bool sameModel = false;
    bool sameBuild = false;
    QList<ComparisonNote> notes;
    QList<ComparisonGroup> groups;  // identity, request, outcome, timing, presolve

    const ComparisonGroup* group(const QString& key) const;
    const ComparisonRow* row(const QString& key) const;
};

Comparison buildComparison(const ComparedRun& a, const ComparedRun& b);

}  // namespace kairo::record
