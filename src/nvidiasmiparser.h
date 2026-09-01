#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QVector>

namespace tryx::nvidia {

enum class ParseFailure {
    None,
    EmptySnapshot,
    OutputLimit,
    InvalidUtf8,
    MalformedCsv,
    MalformedIdentity,
    DuplicateIdentity,
    MalformedValue,
    TooManyGpus,
};

struct Measurement {
    double value = 0.0;
    bool available = false;
};

struct IntegerMeasurement {
    qint64 value = 0;
    bool available = false;
};

struct GpuSample {
    QString uuid;
    QString pciBdf;
    Measurement temperatureC;
    Measurement utilizationPercent;
    Measurement graphicsClockMHz;
    Measurement powerWatts;
    IntegerMeasurement vramUsedMiB;
    IntegerMeasurement vramTotalMiB;
};

struct ParseResult {
    ParseFailure failure = ParseFailure::None;
    QVector<GpuSample> gpus;

    bool ok() const { return failure == ParseFailure::None; }
};

inline constexpr qsizetype MaximumStdoutBytes = 64 * 1024;
inline constexpr qsizetype MaximumGpuCount = 32;

ParseResult parseNvidiaSmiCsv(const QByteArray &payload);

}  // namespace tryx::nvidia

Q_DECLARE_METATYPE(tryx::nvidia::ParseFailure)
