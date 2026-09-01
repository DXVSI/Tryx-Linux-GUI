#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QtGlobal>

#include <functional>

namespace tryx::printer_media_validator {

struct RecoveredH264ProbeMetadata {
    bool dimensionsAvailable = false;
    quint32 width = 0;
    quint32 height = 0;
    bool frameCountAvailable = false;
    quint64 frameCount = 0;
};

RecoveredH264ProbeMetadata parseRecoveredH264ProbeOutput(
    const QByteArray &probeOutput);
bool hasExpectedRecoveredH264ProbeOutput(const QByteArray &probeOutput);
#ifdef TRYX_PROTOCOL_TESTING
QByteArray validationDiagnosticTailForTesting(const QByteArray &output);
#endif

bool validateRecoveredH264(
    const QString &path, qint64 expectedSize,
    const QString &expectedSha256,
    const std::function<bool()> &isCancelled,
    bool *cancelled, QString *errorMessage,
    RecoveredH264ProbeMetadata *metadata = nullptr,
    quint32 expectedWidth = 2240,
    quint32 expectedHeight = 1080);

}  // namespace tryx::printer_media_validator

Q_DECLARE_METATYPE(
    tryx::printer_media_validator::RecoveredH264ProbeMetadata)
