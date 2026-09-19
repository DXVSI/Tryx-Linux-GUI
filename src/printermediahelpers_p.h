#pragma once

#include "printerprotocol.h"
#include "transport.pb.h"
#include "configuration.pb.h"

namespace tryx::printer_media {

bool validatePrinterUploadRequest(const PrinterProductProfile &productProfile,
                                  const QString &remoteFileName, QString *errorMessage,
                                  PrinterProtocol::MutationDetails *mutationDetails);

QString unsupportedCapabilityError(const PrinterProductProfile &productProfile,
                                   const QString &capability);
QString normalizedMediaName(const panorama::wire::v1::MediaEntry &media);
QString normalizedMediaReference(const std::string &value);
bool decodePaseActiveLayout(const panorama::wire::v1::WorkConfiguration &work,
                            QString *screenMode, QString *playMode, QStringList *media,
                            QString *errorMessage);
bool isSafeDeviceMediaName(const QString &fileName);
bool isSafeUploadFileName(const QString &fileName);
bool isSafeUploadFileNameForProfile(const QString &fileName,
                                    const PrinterProductProfile &productProfile);
struct MediaPullCandidate {
    QByteArray rawPath;
    QString mediaName;
    qint64 fileSize = 0;
};

bool validateMediaPullPath(const QByteArray &rawPath, const QString &mediaName);
QByteArray applyMediaPullXor(const QByteArray &bytes, quint64 absoluteOffset,
                             const PrinterProtocol::OperationContext *context,
                             bool *cancelled);
qint64 mediaPullDeadlineForSize(qint64 fileSize);
bool resolveMediaPullCandidate(const panorama::wire::v1::MediaCatalog &catalog,
                               const QString &mediaName, qint64 expectedSize,
                               qint64 maximumBytes, MediaPullCandidate *candidate,
                               QString *errorMessage);
QString transmitStatusText(panorama::wire::v1::TransferStatus::Code status);

// Keeps a device-internal path or suffix for diagnostics only when it is short
// and made of plain path characters; anything else is reported as "other".
QString diagnosticDeviceToken(const std::string &value, qsizetype maximumLength);
// Structure of a Turris file list: per entry the device directory, path and
// name lengths, extension, size and read-only flag. Media names stay out of
// the log.
void logTurrisMediaCatalog(const char *stage,
                           const panorama::wire::v1::MediaCatalog &catalog);

} // namespace tryx::printer_media
