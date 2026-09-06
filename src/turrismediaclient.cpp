#include "turrismediaclient.h"
#include "printermediaupload.h"
#include "printermediahelpers_p.h"
#include "printertransactionchannel.h"
#include "printeroperation_p.h"
#include "printerprotocolconstants_p.h"
#include "turrismediaformat.h"

#include <QFile>

using namespace tryx::printer_media;
using namespace tryx::printer_operation;
using namespace tryx::printer_protocol_constants;

namespace {

PrinterProtocol::DeviceInfo
makeTransferOnlyDeviceInfo(const QString &devicePath,
                           const PrinterProductProfile &productProfile) {
    PrinterProtocol::DeviceInfo info;
    info.devicePath = devicePath;
    info.productName = printerProductIdString(productProfile.productId);
    return info;
}

} // namespace

PrinterProtocol::Result
TurrisMediaClient::startDisplaySession(const QString &devicePath,
                                       const OperationContext &context) {
    QString error;
    if (!channel_.openSessionTransport(devicePath, context, &error)) {
        return {false, error, {}, {}};
    }

    const DeviceInfo deviceInfo =
        makeTransferOnlyDeviceInfo(devicePath, productProfile_);
    if (operationIsCancelled(context)) {
        channel_.closeDevice();
        return {
            false,
            QObject::tr(
                "TRYX USB operation was cancelled because the device state changed"),
            {},
            {}};
    }
    if (context.onDeviceInfoReady) {
        context.onDeviceInfoReady();
    }
    return {true, {}, deviceInfo, {}};
}

PrinterProtocol::Result
TurrisMediaClient::readDeviceInfo(const QString &devicePath,
                                  const OperationContext &context) {

    if (operationIsCancelled(context)) {
        return {
            false,
            QObject::tr(
                "TRYX USB operation was cancelled because the device state changed"),
            {},
            {}};
    }
    return {true, {}, makeTransferOnlyDeviceInfo(devicePath, productProfile_), {}};
}

bool TurrisMediaClient::uploadMedia(const QString &devicePath, const QString &localPath,
                                    const QString &remoteFileName,
                                    QString *uploadedName, QString *errorMessage,
                                    const UploadProgress &progress,
                                    const OperationContext &context,
                                    MutationDetails *mutationDetails,
                                    const QString &expectedSha256) {
    if (!validatePrinterUploadRequest(productProfile_, remoteFileName, errorMessage,
                                      mutationDetails)) {
        return false;
    }
    PrinterMediaUploadOptions options;
    options.allowKeepalive = productProfile_.idleMode != PrinterIdleMode::TransferOnly;
    options.fixedTrackId = kTurrisTransferTrackId;
    options.validateSource = [&remoteFileName, errorMessage, mutationDetails](
                                 QFile &file, qint64 declaredSize,
                                 const std::function<bool()> &sourceIsUnchanged) {
        if (!tryx::turris_media::validateBlob(&file, declaredSize, remoteFileName,
                                              errorMessage)) {
            if (mutationDetails) {
                mutationDetails->outcome = MutationOutcome::Rejected;
            }
            return false;
        }
        if (!sourceIsUnchanged() || !file.seek(0)) {
            if (errorMessage) {
                *errorMessage =
                    QObject::tr("Prepared Turris media changed during validation");
            }
            return false;
        }
        return true;
    };
    return uploadPrinterMedia(channel_, options, devicePath, localPath, remoteFileName,
                              uploadedName, errorMessage, progress, context,
                              mutationDetails, expectedSha256);
}
