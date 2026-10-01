#pragma once

// Presentation of record values. Originally ported from the retired web
// Explorer's format.js; the printed forms are pinned by the desktop tests.
// Every formatter takes "absent" (std::nullopt) to mean absent and says so;
// none turns an absent value into 0.

#include <QString>

#include <optional>

namespace kairo::format {

inline const QString kNotRun = QStringLiteral("Not run");
inline const QString kUnknown = QStringLiteral("Unknown");

QString count(std::optional<double> value);
QString real(std::optional<double> value);      // up to 10 significant digits
QString residual(std::optional<double> value);  // exact 0 stays "0", else d.dde±x
QString seconds(std::optional<double> value);   // µs / ms / s; absent -> "Not run"
QString bytes(qint64 value);
QString words(const QString& value);             // snake_case -> words
QString shortHash(const QString& value, int length = 12);

}  // namespace kairo::format
