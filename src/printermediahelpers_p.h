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

} // namespace tryx::printer_media
