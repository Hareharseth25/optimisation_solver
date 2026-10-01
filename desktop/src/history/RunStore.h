#pragma once

// Saved runs, kept only on this machine.
//
// One JSON file per run in a local directory (by default the platform's
// per-user application data location, from QStandardPaths). A file holds the
// unchanged optimsolver.solve.v1 record plus a few local facts (id, time
// saved, file name). No model text, no server, no network. A saved run is
// rendered by the same readers as a live run.
//
// Entries carry the model SHA-256 so same-model checks need no extra reads.
//
// Export writes the record alone (the same optimsolver.solve.v1 document the
// CLI writes with --json). Import reads such a document -- or a saved-run
// file -- checks its shape, and stores it marked "imported". Import never
// runs the solver or anything else; the record is data, rendered by the same
// readers as a run solved here.

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

namespace kairo::history {

struct RunEntry {
    QString id;          // local UUID
    QDateTime savedAt;   // local clock
    QString fileName;
    qint64 fileSize = -1;  // -1 = not recorded (imported records do not carry it)
    bool imported = false; // true when the record came from Import Run
    // Read from the record when saved, for the list only.
    QString sha256;
    QString status;
    QString engine;      // executed engine, empty if nothing executed
    std::optional<double> objective;
    std::optional<double> totalSeconds;
};

struct SavedRun {
    RunEntry entry;
    QJsonObject record;
};

// Refuses data the readers could not interpret. Returns problems, empty if fine.
QStringList checkSolveRecord(const QJsonObject& record);

// Reads an optimsolver.solve.v1 document (or a kairo.desktop.saved_run.v1
// file, whose record is taken) from disk and checks it. Throws
// std::runtime_error with a reason a person can act on.
QJsonObject readRecordFile(const QString& path);

// Writes the record alone, indented, atomically. Throws std::runtime_error.
void writeRecordFile(const QJsonObject& record, const QString& path);

class RunStore {
public:
    explicit RunStore(QString directory);
    static QString defaultDirectory();

    const QString& directory() const { return directory_; }

    // Saves a record; throws std::runtime_error with the reason on failure.
    RunEntry save(const QJsonObject& record, const QString& fileName, qint64 fileSize);
    // Reads and checks a record file, then saves it marked imported.
    RunEntry importFile(const QString& path);
    QList<RunEntry> list() const;                      // newest first
    std::optional<SavedRun> load(const QString& id) const;
    bool remove(const QString& id);
    void clear();

private:
    RunEntry write(const QJsonObject& record, const QString& fileName, qint64 fileSize, bool imported);
    QString pathFor(const QString& id) const;
    QString directory_;
};

}  // namespace kairo::history
