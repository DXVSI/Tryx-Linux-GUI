#pragma once

#include "printerprotocol.h"

class QFile;

namespace tryx::printer_media {

// Model policy is selected before entering the one shared ACK-driven upload.
struct PrinterMediaUploadOptions {
    quint64 fixedTrackId = 0;
    bool allowKeepalive = true;
    std::function<bool(QFile &, qint64, const std::function<bool()> &)> validateSource;
};

bool uploadPrinterMedia(PrinterTransactionChannel &channel,
                        const PrinterMediaUploadOptions &options,
                        const QString &devicePath, const QString &localPath,
                        const QString &remoteFileName, QString *uploadedName,
                        QString *errorMessage,
                        const PrinterProtocol::UploadProgress &progress,
                        const PrinterProtocol::OperationContext &context,
                        PrinterProtocol::MutationDetails *mutationDetails,
                        const QString &expectedSha256);

} // namespace tryx::printer_media
