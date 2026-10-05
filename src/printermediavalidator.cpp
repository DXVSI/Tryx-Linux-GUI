#include "printermediavalidator.h"

#include "printermediafileintegrity.h"

#include <QDeadlineTimer>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>

namespace {

constexpr qint64 kValidationDeadlineMs = 15LL * 60LL * 1000LL;
constexpr qsizetype kMaximumDiagnosticBytes = 16 * 1024;
constexpr quint64 kMaximumRecoveredFrameCount =
    7ULL * 24ULL * 60ULL * 60ULL * 30ULL;

void appendBoundedDiagnostic(
    QByteArray *diagnostic, const QByteArray &chunk) {
    if (!diagnostic || chunk.isEmpty()) {
        return;
    }
    if (chunk.size() >= kMaximumDiagnosticBytes) {
        *diagnostic = chunk.right(kMaximumDiagnosticBytes);
        return;
    }
    const qsizetype excess =
        diagnostic->size() + chunk.size() - kMaximumDiagnosticBytes;
    if (excess > 0) {
        diagnostic->remove(0, excess);
    }
    diagnostic->append(chunk);
}

bool runBoundedMediaValidationProcess(
    const QString &program, const QStringList &arguments,
    const std::function<bool()> &isCancelled,
    QDeadlineTimer *deadline,
    QByteArray *output, bool *cancelled,
    QString *errorMessage) {
    if (cancelled) {
        *cancelled = false;
    }
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setProgram(program);
    process.setArguments(arguments);
    if (!deadline || deadline->hasExpired()) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "Recovered media validation exceeded its bounded deadline");
        }
        return false;
    }
    process.start();
    const qint64 startRemaining = deadline->remainingTime();
    const int startWaitMs = static_cast<int>(
        qBound<qint64>(qint64{1}, startRemaining, qint64{5000}));
    if (!process.waitForStarted(startWaitMs)) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "Cannot start recovered media validation tool %1: %2")
                .arg(program, process.errorString());
        }
        return false;
    }
    QByteArray diagnostic;
    while (process.state() != QProcess::NotRunning) {
        if (isCancelled && isCancelled()) {
            process.kill();
            process.waitForFinished(3000);
            if (cancelled) {
                *cancelled = true;
            }
            return false;
        }
        if (deadline->hasExpired()) {
            process.kill();
            process.waitForFinished(3000);
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "Recovered media validation exceeded its bounded deadline");
            }
            return false;
        }
        process.waitForFinished(100);
        appendBoundedDiagnostic(&diagnostic, process.readAll());
    }
    appendBoundedDiagnostic(&diagnostic, process.readAll());
    if (output) {
        *output = diagnostic;
    }
    if (process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        if (errorMessage) {
            QString detail =
                QString::fromLocal8Bit(diagnostic).trimmed();
            if (detail.size() > 1000) {
                detail = detail.right(1000);
            }
            *errorMessage = detail.isEmpty()
                ? QObject::tr("Recovered media validation failed")
                : QObject::tr("Recovered media validation failed: %1")
                      .arg(detail);
        }
        return false;
    }
    return true;
}

bool recoveredH264HasRequiredNalUnits(
    const QString &path,
    const std::function<bool()> &isCancelled,
    bool *cancelled, QString *errorMessage) {
    if (cancelled) {
        *cancelled = false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "Cannot open the recovered H264 stream: %1")
                .arg(file.errorString());
        }
        return false;
    }
    bool hasSps = false;
    bool hasPps = false;
    bool hasVcl = false;
    QByteArray pending;
    while (!file.atEnd() && !(hasSps && hasPps && hasVcl)) {
        if (isCancelled && isCancelled()) {
            if (cancelled) {
                *cancelled = true;
            }
            return false;
        }
        QByteArray bytes = pending;
        bytes.append(file.read(256 * 1024));
        if (bytes.isEmpty() && file.error() != QFileDevice::NoError) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "Cannot inspect the recovered H264 stream: %1")
                    .arg(file.errorString());
            }
            return false;
        }
        for (qsizetype index = 0; index + 4 < bytes.size(); ++index) {
            qsizetype headerIndex = -1;
            if (bytes.at(index) == '\0' &&
                bytes.at(index + 1) == '\0' &&
                bytes.at(index + 2) == '\1') {
                headerIndex = index + 3;
            } else if (index + 5 < bytes.size() &&
                       bytes.at(index) == '\0' &&
                       bytes.at(index + 1) == '\0' &&
                       bytes.at(index + 2) == '\0' &&
                       bytes.at(index + 3) == '\1') {
                headerIndex = index + 4;
            }
            if (headerIndex < 0 || headerIndex >= bytes.size()) {
                continue;
            }
            const int nalType =
                static_cast<unsigned char>(bytes.at(headerIndex)) & 0x1f;
            hasSps = hasSps || nalType == 7;
            hasPps = hasPps || nalType == 8;
            hasVcl = hasVcl || (nalType >= 1 && nalType <= 5);
        }
        pending = bytes.right(qMin<qsizetype>(5, bytes.size()));
    }
    if (!(hasSps && hasPps && hasVcl)) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "Recovered device media is not a complete Annex B H264 stream");
        }
        return false;
    }
    return true;
}

}  // namespace

namespace tryx::printer_media_validator {

RecoveredH264ProbeMetadata parseRecoveredH264ProbeOutput(
    const QByteArray &probeOutput) {
    RecoveredH264ProbeMetadata result;
    if (probeOutput.size() > kMaximumDiagnosticBytes) {
        return result;
    }

    bool hasCodec = false;
    bool hasWidth = false;
    bool hasHeight = false;
    bool hasFrameCount = false;
    bool frameCountValid = false;
    quint64 frameCount = 0;
    const QList<QByteArray> lines = probeOutput.split('\n');
    for (QByteArray line : lines) {
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (line.isEmpty()) {
            continue;
        }
        const qsizetype separator = line.indexOf('=');
        if (separator <= 0 || separator != line.lastIndexOf('=')) {
            return {};
        }
        const QByteArray key = line.left(separator);
        const QByteArray value = line.mid(separator + 1);
        if (key == QByteArrayLiteral("codec_name")) {
            if (hasCodec || value != QByteArrayLiteral("h264")) {
                return {};
            }
            hasCodec = true;
        } else if (key == QByteArrayLiteral("width")) {
            if (hasWidth ||
                (value != QByteArrayLiteral("2240") &&
                 value != QByteArrayLiteral("1120"))) {
                return {};
            }
            hasWidth = true;
            result.width = value == QByteArrayLiteral("1120")
                ? 1120U : 2240U;
        } else if (key == QByteArrayLiteral("height")) {
            if (hasHeight || value != QByteArrayLiteral("1080")) {
                return {};
            }
            hasHeight = true;
        } else if (key == QByteArrayLiteral("nb_read_frames")) {
            if (hasFrameCount) {
                return {};
            }
            hasFrameCount = true;
            bool canonicalDecimal = !value.isEmpty();
            for (const char character : value) {
                canonicalDecimal = canonicalDecimal &&
                    character >= '0' && character <= '9';
            }
            canonicalDecimal = canonicalDecimal &&
                (value.size() == 1 || value.at(0) != '0');
            bool parsed = false;
            const quint64 candidate = canonicalDecimal
                ? value.toULongLong(&parsed, 10)
                : 0;
            if (parsed && candidate > 0 &&
                candidate <= kMaximumRecoveredFrameCount) {
                frameCountValid = true;
                frameCount = candidate;
            }
        } else {
            return {};
        }
    }
    if (!hasCodec || !hasWidth || !hasHeight) {
        return {};
    }
    result.dimensionsAvailable = true;
    if (result.width == 0) {
        return {};
    }
    result.height = 1080;
    result.frameCountAvailable = hasFrameCount && frameCountValid;
    result.frameCount = result.frameCountAvailable ? frameCount : 0;
    return result;
}

bool hasExpectedRecoveredH264ProbeOutput(const QByteArray &probeOutput) {
    return parseRecoveredH264ProbeOutput(probeOutput)
        .dimensionsAvailable;
}

#ifdef TRYX_PROTOCOL_TESTING
QByteArray validationDiagnosticTailForTesting(const QByteArray &output) {
    QByteArray diagnostic;
    appendBoundedDiagnostic(&diagnostic, output);
    return diagnostic;
}
#endif

bool validateRecoveredH264(
    const QString &path, qint64 expectedSize,
    const QString &expectedSha256,
    const std::function<bool()> &isCancelled,
    bool *cancelled, QString *errorMessage,
    RecoveredH264ProbeMetadata *metadata,
    quint32 expectedWidth, quint32 expectedHeight) {
    if (cancelled) {
        *cancelled = false;
    }
    if (metadata) {
        *metadata = {};
    }
    QDeadlineTimer validationDeadline(kValidationDeadlineMs);
    const auto userCancelled = [&isCancelled]() {
        return isCancelled && isCancelled();
    };
    const auto validationStopped = [&userCancelled,
                                    &validationDeadline]() {
        return userCancelled() || validationDeadline.hasExpired();
    };
    const auto reportValidationStop =
        [&userCancelled, &validationDeadline, cancelled,
         errorMessage]() {
            if (userCancelled()) {
                if (cancelled) {
                    *cancelled = true;
                }
                return;
            }
            if (validationDeadline.hasExpired() && errorMessage) {
                *errorMessage = QObject::tr(
                    "Recovered media validation exceeded its bounded deadline");
            }
        };
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.isSymLink() ||
        info.size() != expectedSize || expectedSize <= 0) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "Recovered device media does not match the validated file size");
        }
        return false;
    }
    bool stoppedDuringNalInspection = false;
    if (!recoveredH264HasRequiredNalUnits(
            path, validationStopped, &stoppedDuringNalInspection,
            errorMessage)) {
        if (stoppedDuringNalInspection) {
            reportValidationStop();
        }
        return false;
    }
    if (validationStopped()) {
        reportValidationStop();
        return false;
    }
    const QString actualSha256 =
        printer_media_file_integrity::sha256File(path, validationStopped);
    if (actualSha256.isEmpty() || actualSha256 != expectedSha256) {
        if (validationStopped()) {
            reportValidationStop();
        } else if (errorMessage) {
            *errorMessage = QObject::tr(
                "Recovered device media hash changed before validation");
        }
        return false;
    }

    const QString ffprobe =
        QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    const QString ffmpeg =
        QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffprobe.isEmpty() || ffmpeg.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "ffprobe and ffmpeg are required to validate recovered device media");
        }
        return false;
    }
    QByteArray probeOutput;
    if (!runBoundedMediaValidationProcess(
            ffprobe,
            {QStringLiteral("-v"), QStringLiteral("error"),
             QStringLiteral("-f"), QStringLiteral("h264"),
             QStringLiteral("-select_streams"), QStringLiteral("v:0"),
             QStringLiteral("-show_entries"),
             QStringLiteral("stream=codec_name,width,height"),
             QStringLiteral("-of"),
             QStringLiteral("default=noprint_wrappers=1"),
             path},
            isCancelled, &validationDeadline, &probeOutput, cancelled,
            errorMessage)) {
        return false;
    }
    const RecoveredH264ProbeMetadata geometry =
        parseRecoveredH264ProbeOutput(probeOutput);
    if (!geometry.dimensionsAvailable ||
        geometry.width != expectedWidth ||
        geometry.height != expectedHeight) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "Recovered device media is not H264 at %1x%2")
                .arg(expectedWidth)
                .arg(expectedHeight);
        }
        return false;
    }
    if (metadata) {
        *metadata = geometry;
    }
    if (!runBoundedMediaValidationProcess(
        ffmpeg,
        {QStringLiteral("-v"), QStringLiteral("error"),
         QStringLiteral("-f"), QStringLiteral("h264"),
         QStringLiteral("-i"), path, QStringLiteral("-map"),
         QStringLiteral("0:v:0"), QStringLiteral("-f"),
         QStringLiteral("null"), QStringLiteral("-")},
        isCancelled, &validationDeadline, nullptr, cancelled,
        errorMessage)) {
        return false;
    }

    QByteArray frameCountOutput;
    bool frameCountCancelled = false;
    QString frameCountError;
    const bool frameCountProbeSucceeded =
        runBoundedMediaValidationProcess(
            ffprobe,
            {QStringLiteral("-v"), QStringLiteral("error"),
             QStringLiteral("-f"), QStringLiteral("h264"),
             QStringLiteral("-select_streams"), QStringLiteral("v:0"),
             QStringLiteral("-count_frames"),
             QStringLiteral("-show_entries"),
             QStringLiteral(
                 "stream=codec_name,width,height,nb_read_frames"),
             QStringLiteral("-of"),
             QStringLiteral("default=noprint_wrappers=1"),
             path},
            isCancelled, &validationDeadline, &frameCountOutput,
            &frameCountCancelled, &frameCountError);
    if (frameCountCancelled) {
        if (cancelled) {
            *cancelled = true;
        }
        return false;
    }
    if (metadata) {
        if (frameCountProbeSucceeded) {
            const RecoveredH264ProbeMetadata counted =
                parseRecoveredH264ProbeOutput(frameCountOutput);
            if (counted.dimensionsAvailable &&
                counted.frameCountAvailable) {
                *metadata = counted;
            }
        }
    }
    return true;
}

}  // namespace tryx::printer_media_validator
