#include "Format.h"

#include <QLocale>

#include <cmath>

namespace kairo::format {
namespace {

// JavaScript's Number.prototype.toExponential(digits): "1.23e-7", "4.5e+21".
QString jsExponential(double value, int digits) {
    QString text = QString::number(value, 'e', digits);  // e.g. 1.23e-07
    const int e = text.indexOf(QLatin1Char('e'));
    if (e < 0) return text;
    QString mantissa = text.left(e);
    QString exponent = text.mid(e + 1);
    QChar sign = QLatin1Char('+');
    if (exponent.startsWith(QLatin1Char('-')) || exponent.startsWith(QLatin1Char('+'))) {
        sign = exponent.at(0);
        exponent = exponent.mid(1);
    }
    while (exponent.size() > 1 && exponent.startsWith(QLatin1Char('0'))) exponent = exponent.mid(1);
    return mantissa + QLatin1Char('e') + sign + exponent;
}

}  // namespace

QString count(std::optional<double> value) {
    if (!value) return kUnknown;
    const qint64 whole = static_cast<qint64>(std::llround(*value));
    QString digits = QString::number(std::llabs(whole));
    for (int i = digits.size() - 3; i > 0; i -= 3) digits.insert(i, QLatin1Char(','));
    return whole < 0 ? QStringLiteral("-") + digits : digits;
}

QString real(std::optional<double> value) {
    if (!value) return kUnknown;
    const double v = *value;
    if (v == 0.0) return QStringLiteral("0");
    const double magnitude = std::fabs(v);
    if (magnitude >= 1e-4 && magnitude < 1e12) {
        // String(Number(v.toPrecision(10))): round to 10 significant digits,
        // then the shortest round-trip decimal, never exponent notation here.
        const double rounded = QString::number(v, 'g', 10).toDouble();
        return QString::number(rounded, 'f', QLocale::FloatingPointShortest);
    }
    return jsExponential(v, 6);
}

QString residual(std::optional<double> value) {
    if (!value) return kUnknown;
    if (*value == 0.0) return QStringLiteral("0");
    return jsExponential(*value, 2);
}

QString seconds(std::optional<double> value) {
    if (!value) return kNotRun;
    const double v = *value;
    if (v < 1e-3) return QString::number(v * 1e6, 'f', 1) + QStringLiteral(" µs");
    if (v < 1) return QString::number(v * 1e3, 'f', 2) + QStringLiteral(" ms");
    return QString::number(v, 'f', 3) + QStringLiteral(" s");
}

QString bytes(qint64 value) {
    if (value < 1024) return QString::number(value) + QStringLiteral(" B");
    if (value < 1024 * 1024) return QString::number(value / 1024.0, 'f', 1) + QStringLiteral(" KiB");
    return QString::number(value / (1024.0 * 1024.0), 'f', 1) + QStringLiteral(" MiB");
}

QString words(const QString& value) {
    QString text = value;
    return text.replace(QLatin1Char('_'), QLatin1Char(' '));
}

QString shortHash(const QString& value, int length) {
    return value.isEmpty() ? kUnknown : value.left(length);
}

}  // namespace kairo::format
