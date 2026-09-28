#pragma once

#include <QByteArray>

// Keeps a runtime that cannot finish its shutdown from blocking the user
// session. The service has a finite stop timeout; an irreversible firmware
// write extends it through the service manager, and a shutdown that does not
// complete on its own ends the process after a deadline.
namespace tryx::runtime_shutdown {

// How long a requested shutdown may take before the process ends itself.
inline constexpr int kExitDeadlineMs = 10000;
// Sent while firmware flashing defers the shutdown: how long the service
// manager waits for the next extension, and how often one is sent.
inline constexpr int kStopTimeoutExtensionMs = 60000;
inline constexpr int kStopTimeoutExtensionIntervalMs = 15000;
inline constexpr int kExitDeadlineExitCode = 70;

// Minimal sd_notify: sends one datagram to $NOTIFY_SOCKET. Returns false when
// no service manager listens or the message cannot be sent.
bool notifyServiceManager(const QByteArray &state);
// The EXTEND_TIMEOUT_USEC message for the given extension.
QByteArray stopTimeoutExtensionMessage(int extensionMs);
// Ends the process with kExitDeadlineExitCode once the deadline passes, even
// when the main thread is blocked waiting for a worker that is stuck in a
// kernel USB call. Arming twice keeps the first deadline.
void armExitDeadline(int deadlineMs);

}  // namespace tryx::runtime_shutdown
