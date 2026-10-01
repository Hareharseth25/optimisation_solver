#include "RunStore.h"

#include "record/RecordReaders.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <stdexcept>

namespace kairo::history {
namespace {

const QString kSchema = QStringLiteral("optimsolver.solve.v1");
const QString kFileFormat = QStringLiteral("kairo.desktop.saved_run.v1");
// Larger than any record KAIRO writes for the models it reads; refuses
// accidental multi-gigabyte files before they are read into memory.
constexpr qint64 kMaxRecordBytes = qint64(512) * 1024 * 1024;

bool isNumberArrayOrNull(const QJsonValue& value) {
    if (value.isNull() || value.isUndefined()) return true;
    if (!value.isArray()) return false;
    for (const QJsonValue& item : value.toArray()) {
        if (!item.isDouble()) return false;
    }
    return true;
}

// A saved run's id is its file's base name. The "id" inside the file is
// informational only and never used to build a path, so a file placed in
// the directory by hand cannot point load/remove anywhere else.
bool isSafeId(const QString& id) {
    return !id.isEmpty() && !id.startsWith(QLatin1Char('.')) && !id.contains(QLatin1Char('/')) &&
           !id.contains(QLatin1Char('\\')) && !id.contains(QLatin1Char(':'));
}

RunEntry entryFrom(const QJsonObject& file, const QString& id) {
    const QJsonObject record = file.value("record").toObject();
    const auto facts = record::resultFacts(record);
    RunEntry entry;
    entry.id = id;
    entry.savedAt = QDateTime::fromString(file.value("saved_at").toString(), Qt::ISODateWithMs);
    entry.fileName = file.value("file_name").toString();
    entry.fileSize = file.value("file_size").isDouble() ? static_cast<qint64>(file.value("file_size").toDouble()) : -1;
    // Files written before "source" existed were all solved on this machine.
    entry.imported = file.value("source").toString() == QLatin1String("imported");
    entry.sha256 = record.value("instance").toObject().value("sha256").toString();
    entry.status = facts.status;
    entry.engine = facts.executed;
    entry.objective = facts.objective;
    entry.totalSeconds = facts.totalSeconds;
    return entry;
}

}  // namespace

QStringList checkSolveRecord(const QJsonObject& record) {
    QStringList problems;
    if (record.value("schema").toString() != kSchema) problems << QStringLiteral("schema is not \"%1\"").arg(kSchema);
    for (const char* key : {"instance", "solver", "compute_backend", "settings", "termination", "self_reported", "stage_seconds", "work"}) {
        if (!record.value(QLatin1String(key)).isObject()) problems << QStringLiteral("missing object \"%1\"").arg(QLatin1String(key));
    }
    for (const char* key : {"classification", "presolve", "dispatch", "validation"}) {
        const QJsonValue value = record.value(QLatin1String(key));
        if (!value.isNull() && !value.isUndefined() && !value.isObject()) {
            problems << QStringLiteral("\"%1\" must be an object or null").arg(QLatin1String(key));
        }
    }
    static const QStringList statuses{"optimal", "infeasible", "unbounded", "limit_reached", "numerical_failure", "invalid_model", "unsupported"};
    if (!statuses.contains(record.value("termination").toObject().value("status").toString())) {
        problems << QStringLiteral("termination.status is not a KAIRO status");
    }
    for (const char* key : {"primal", "duals", "reduced_costs"}) {
        if (!isNumberArrayOrNull(record.value(QLatin1String(key)))) {
            problems << QStringLiteral("\"%1\" must be a number array or null").arg(QLatin1String(key));
        }
    }
    const QJsonValue objective = record.value("objective");
    if (!objective.isNull() && !objective.isUndefined() && !objective.isDouble()) {
        problems << QStringLiteral("objective must be a number or null");
    }
    return problems;
}

QJsonObject readRecordFile(const QString& path) {
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) throw std::runtime_error("cannot read the file: " + in.errorString().toStdString());
    if (in.size() > kMaxRecordBytes) throw std::runtime_error("the file is larger than 512 MiB; it is not a KAIRO run record");
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(in.readAll(), &error);
    if (error.error != QJsonParseError::NoError) {
        throw std::runtime_error(QStringLiteral("not valid JSON (%1 at byte %2)")
                                     .arg(error.errorString()).arg(error.offset).toStdString());
    }
    if (!doc.isObject()) throw std::runtime_error("not a JSON object");
    QJsonObject record = doc.object();
    // A saved-run file from another machine: take the record it holds.
    if (record.value("format").toString() == kFileFormat) record = record.value("record").toObject();
    const QStringList problems = checkSolveRecord(record);
    if (!problems.isEmpty()) {
        throw std::runtime_error(("not a KAIRO run record (optimsolver.solve.v1): " + problems.join("; ")).toStdString());
    }
    return record;
}

void writeRecordFile(const QJsonObject& record, const QString& path) {
    const QStringList problems = checkSolveRecord(record);
    if (!problems.isEmpty()) throw std::runtime_error(("not a solve record: " + problems.join("; ")).toStdString());
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) throw std::runtime_error("cannot write the file: " + out.errorString().toStdString());
    out.write(QJsonDocument(record).toJson(QJsonDocument::Indented));
    if (!out.commit()) throw std::runtime_error("cannot write the file: " + out.errorString().toStdString());
}

RunStore::RunStore(QString directory) : directory_(std::move(directory)) {}

QString RunStore::defaultDirectory() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("runs"));
}

QString RunStore::pathFor(const QString& id) const {
    return QDir(directory_).filePath(id + QStringLiteral(".json"));
}

RunEntry RunStore::save(const QJsonObject& record, const QString& fileName, qint64 fileSize) {
    return write(record, fileName, fileSize, false);
}

RunEntry RunStore::importFile(const QString& path) {
    const QJsonObject record = readRecordFile(path);
    // The model file name as KAIRO recorded it (a name, never a directory).
    // A CLI record may hold a full path from another machine (either separator).
    const QString recorded = record.value("instance").toObject().value("path").toString();
    const QString name = recorded.mid(std::max(recorded.lastIndexOf(QLatin1Char('/')), recorded.lastIndexOf(QLatin1Char('\\'))) + 1);
    return write(record, name, -1, true);
}

RunEntry RunStore::write(const QJsonObject& record, const QString& fileName, qint64 fileSize, bool imported) {
    const QStringList problems = checkSolveRecord(record);
    if (!problems.isEmpty()) throw std::runtime_error(("not a solve record: " + problems.join("; ")).toStdString());
    if (!QDir().mkpath(directory_)) throw std::runtime_error("cannot create " + directory_.toStdString());

    QJsonObject file;
    file.insert("format", kFileFormat);
    file.insert("id", QUuid::createUuid().toString(QUuid::WithoutBraces));
    file.insert("saved_at", QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    file.insert("file_name", fileName);
    file.insert("file_size", fileSize >= 0 ? QJsonValue(static_cast<double>(fileSize)) : QJsonValue());
    file.insert("source", imported ? QStringLiteral("imported") : QStringLiteral("live"));
    file.insert("record", record);

    // Atomic write: a crash never leaves a half-written run behind.
    const QString id = file.value("id").toString();
    QSaveFile out(pathFor(id));
    if (!out.open(QIODevice::WriteOnly)) throw std::runtime_error("cannot write " + out.fileName().toStdString());
    out.write(QJsonDocument(file).toJson(QJsonDocument::Compact));
    if (!out.commit()) throw std::runtime_error("cannot write " + out.fileName().toStdString());
    return entryFrom(file, id);
}

QList<RunEntry> RunStore::list() const {
    QList<RunEntry> entries;
    const QDir dir(directory_);
    for (const QString& name : dir.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        QFile in(dir.filePath(name));
        if (!in.open(QIODevice::ReadOnly)) continue;
        const QJsonObject file = QJsonDocument::fromJson(in.readAll()).object();
        if (file.value("format").toString() != kFileFormat) continue;
        entries.append(entryFrom(file, QFileInfo(name).completeBaseName()));
    }
    std::sort(entries.begin(), entries.end(), [](const RunEntry& a, const RunEntry& b) { return a.savedAt > b.savedAt; });
    return entries;
}

std::optional<SavedRun> RunStore::load(const QString& id) const {
    if (!isSafeId(id)) return std::nullopt;
    QFile in(pathFor(id));
    if (!in.open(QIODevice::ReadOnly)) return std::nullopt;
    const QJsonObject file = QJsonDocument::fromJson(in.readAll()).object();
    if (file.value("format").toString() != kFileFormat) return std::nullopt;
    return SavedRun{entryFrom(file, id), file.value("record").toObject()};
}

bool RunStore::remove(const QString& id) {
    return isSafeId(id) && QFile::remove(pathFor(id));
}

void RunStore::clear() {
    // Only files this store wrote; anything else in the directory is left alone.
    for (const RunEntry& entry : list()) remove(entry.id);
}

}  // namespace kairo::history
