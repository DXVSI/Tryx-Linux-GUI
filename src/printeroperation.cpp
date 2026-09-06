#include "printeroperation_p.h"
#include "printerprotocolconstants_p.h"

#include <QElapsedTimer>
#include <cerrno>

namespace tryx::printer_operation {

using namespace tryx::printer_protocol_constants;

bool waitForReadinessBackoff(int delayMs,
                             const PrinterProtocol::OperationContext &context,
                             QString *errorMessage) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < delayMs) {
        if (operationIsCancelled(context)) {
            setCancelledError(errorMessage);
            return false;
        }

        const int remaining = delayMs - static_cast<int>(timer.elapsed());
        const int waitMs =
            context.isCancelled ? qMin(remaining, kPollCancellationSliceMs) : remaining;
        pollfd cancellationDescriptor{};
        nfds_t descriptorCount = 0;
        if (context.cancellationFd >= 0) {
            cancellationDescriptor.fd = context.cancellationFd;
            cancellationDescriptor.events = POLLIN;
            descriptorCount = 1;
        }
        const int pollResult =
            ::poll(descriptorCount == 0 ? nullptr : &cancellationDescriptor,
                   descriptorCount, qMax(1, waitMs));
        if (pollResult < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errorMessage) {
                *errorMessage = QObject::tr("TRYX readiness backoff poll failed: %1")
                                    .arg(systemErrorText(errno));
            }
            return false;
        }
        if (operationIsCancelled(context) ||
            (descriptorCount == 1 && (cancellationDescriptor.revents &
                                      (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0)) {
            setCancelledError(errorMessage);
            return false;
        }
    }
    return true;
}

} // namespace tryx::printer_operation
