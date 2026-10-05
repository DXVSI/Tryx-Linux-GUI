#include "nvidiasmiparser.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringConverter>

#include <cmath>

namespace tryx::nvidia {
namespace {

ParseResult failure(ParseFailure reason) {
    ParseResult result;
    result.failure = reason;
    return result;
}

bool normalizeUuid(const QString &field, QString *normalized) {
    static const QRegularExpression expression(
        QStringLiteral(
            "^GPU-(?:[A-Z0-9]|[A-Z0-9][A-Z0-9-]{0,126}[A-Z0-9])$"),
        QRegularExpression::CaseInsensitiveOption);
    const QString value = field.trimmed();
    if (!expression.match(value).hasMatch()) {
        return false;
    }
    *normalized = value.toUpper();
    return true;
}

bool normalizePciBdf(const QString &field, QString *normalized) {
    static const QRegularExpression expression(QStringLiteral(
        "^([0-9A-F]{4}|[0-9A-F]{8}):([0-9A-F]{2}):"
        "([0-9A-F]{2})\\.([0-7])$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = expression.match(field.trimmed());
    if (!match.hasMatch()) {
        return false;
    }
    bool domainOk = false;
    const quint64 domain = match.captured(1).toULongLong(&domainOk, 16);
    if (!domainOk || domain > 0xffffU) {
        return false;
    }
    *normalized = QStringLiteral("%1:%2:%3.%4")
        .arg(domain, 4, 16, QLatin1Char('0'))
        .arg(match.captured(2).toLower())
        .arg(match.captured(3).toLower())
        .arg(match.captured(4));
    return true;
}

bool parseIntegerMeasurement(const QString &field, qint64 minimum,
                             qint64 maximum,
                             IntegerMeasurement *measurement) {
    const QString value = field.trimmed();
    if (value == QStringLiteral("N/A")) {
        *measurement = IntegerMeasurement{};
        return true;
    }
    static const QRegularExpression expression(
        QStringLiteral("^[0-9]+$"));
    if (!expression.match(value).hasMatch()) {
        return false;
    }
    bool ok = false;
    const qint64 parsed = value.toLongLong(&ok, 10);
    if (!ok || parsed < minimum || parsed > maximum) {
        return false;
    }
    measurement->value = parsed;
    measurement->available = true;
    return true;
}

bool parseIntegralMeasurement(const QString &field, qint64 minimum,
                              qint64 maximum,
                              Measurement *measurement) {
    IntegerMeasurement parsed;
    if (!parseIntegerMeasurement(field, minimum, maximum, &parsed)) {
        return false;
    }
    measurement->value = static_cast<double>(parsed.value);
    measurement->available = parsed.available;
    return true;
}

bool parseDecimalMeasurement(const QString &field, double minimum,
                             double maximum, Measurement *measurement) {
    const QString value = field.trimmed();
    if (value == QStringLiteral("N/A")) {
        *measurement = Measurement{};
        return true;
    }
    static const QRegularExpression expression(
        QStringLiteral("^[0-9]+(?:\\.[0-9]+)?$"));
    if (!expression.match(value).hasMatch()) {
        return false;
    }
    bool ok = false;
    const double parsed = value.toDouble(&ok);
    if (!ok || !std::isfinite(parsed) || parsed < minimum ||
        parsed > maximum) {
        return false;
    }
    measurement->value = parsed;
    measurement->available = true;
    return true;
}

}  // namespace

ParseResult parseNvidiaSmiCsv(const QByteArray &payload) {
    if (payload.isEmpty()) {
        return failure(ParseFailure::EmptySnapshot);
    }
    if (payload.size() > MaximumStdoutBytes) {
        return failure(ParseFailure::OutputLimit);
    }

    QStringDecoder decoder(QStringDecoder::Utf8);
    QString text = decoder.decode(payload);
    if (decoder.hasError() || text.contains(QChar::Null)) {
        return failure(ParseFailure::InvalidUtf8);
    }

    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    if (!lines.isEmpty() && lines.constLast().isEmpty()) {
        lines.removeLast();
    }
    if (lines.isEmpty()) {
        return failure(ParseFailure::EmptySnapshot);
    }
    if (lines.size() > MaximumGpuCount) {
        return failure(ParseFailure::TooManyGpus);
    }

    ParseResult result;
    result.gpus.reserve(lines.size());
    QSet<QString> uuids;
    QSet<QString> bdfs;
    for (QString line : lines) {
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
        if (line.isEmpty() || line.contains(QLatin1Char('\r'))) {
            return failure(ParseFailure::MalformedCsv);
        }
        const QStringList fields =
            line.split(QLatin1Char(','), Qt::KeepEmptyParts);
        if (fields.size() != 8) {
            return failure(ParseFailure::MalformedCsv);
        }

        GpuSample gpu;
        if (!normalizeUuid(fields.at(0), &gpu.uuid) ||
            !normalizePciBdf(fields.at(1), &gpu.pciBdf)) {
            return failure(ParseFailure::MalformedIdentity);
        }
        if (uuids.contains(gpu.uuid) || bdfs.contains(gpu.pciBdf)) {
            return failure(ParseFailure::DuplicateIdentity);
        }
        uuids.insert(gpu.uuid);
        bdfs.insert(gpu.pciBdf);

        if (!parseIntegralMeasurement(fields.at(2), 0, 255,
                                      &gpu.temperatureC) ||
            !parseIntegralMeasurement(fields.at(3), 0, 100,
                                      &gpu.utilizationPercent) ||
            !parseIntegralMeasurement(fields.at(4), 0, 100000,
                                      &gpu.graphicsClockMHz) ||
            !parseDecimalMeasurement(fields.at(5), 0.0, 10000.0,
                                     &gpu.powerWatts) ||
            !parseIntegerMeasurement(fields.at(6), 0, 16777216,
                                     &gpu.vramUsedMiB) ||
            !parseIntegerMeasurement(fields.at(7), 1, 16777216,
                                     &gpu.vramTotalMiB) ||
            (gpu.vramUsedMiB.available && gpu.vramTotalMiB.available &&
             gpu.vramUsedMiB.value > gpu.vramTotalMiB.value)) {
            return failure(ParseFailure::MalformedValue);
        }
        result.gpus.append(gpu);
    }
    return result;
}

}  // namespace tryx::nvidia
