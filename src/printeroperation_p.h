#pragma once

#include "printerprotocol.h"

#include <cstring>
#include <poll.h>

namespace tryx::printer_operation {

inline QString systemErrorText(int errorNumber) {
    return QString::fromLocal8Bit(std::strerror(errorNumber));
}

inline bool operationIsCancelled(const PrinterProtocol::OperationContext &context) {
    if (context.isCancelled && context.isCancelled()) {
        return true;
    }
    if (context.cancellationFd < 0) {
        return false;
    }
    pollfd descriptor{};
    descriptor.fd = context.cancellationFd;
    descriptor.events = POLLIN;
    const int result = ::poll(&descriptor, 1, 0);
    return result > 0 &&
           (descriptor.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0;
}

inline void setCancelledError(QString *errorMessage) {
    if (errorMessage) {
        *errorMessage = QObject::tr(
            "TRYX USB operation was cancelled because the device state changed");
    }
}

bool waitForReadinessBackoff(int delayMs,
                             const PrinterProtocol::OperationContext &context,
                             QString *errorMessage);

} // namespace tryx::printer_operation
