#include "shutdownguard.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace tryx::runtime_shutdown {

QByteArray stopTimeoutExtensionMessage(int extensionMs) {
    const qint64 microseconds = static_cast<qint64>(extensionMs > 0 ? extensionMs : 1) * 1000;
    return QByteArrayLiteral("EXTEND_TIMEOUT_USEC=") + QByteArray::number(microseconds);
}

bool notifyServiceManager(const QByteArray &state) {
    const char *socketPath = std::getenv("NOTIFY_SOCKET");
    if (!socketPath || state.isEmpty()) {
        return false;
    }
    const size_t pathLength = std::strlen(socketPath);
    sockaddr_un address{};
    if (pathLength < 2 || pathLength >= sizeof(address.sun_path) ||
        (socketPath[0] != '/' && socketPath[0] != '@')) {
        return false;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socketPath, pathLength);
    if (socketPath[0] == '@') {
        // Abstract namespace: the leading '@' stands for a NUL byte.
        address.sun_path[0] = '\0';
    }
    const int descriptor = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0) {
        return false;
    }
    const socklen_t addressLength =
        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + pathLength);
    const ssize_t sent =
        ::sendto(descriptor, state.constData(), static_cast<size_t>(state.size()),
                 MSG_NOSIGNAL, reinterpret_cast<const sockaddr *>(&address), addressLength);
    ::close(descriptor);
    return sent == state.size();
}

void armExitDeadline(int deadlineMs) {
    static std::atomic<bool> armed{false};
    if (armed.exchange(true)) {
        return;
    }
    std::thread([deadlineMs]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(deadlineMs > 0 ? deadlineMs : 1));
        // Only async-signal-safe calls from here: the main thread may hold any
        // lock while it waits for a worker that never returns.
        static const char message[] =
            "tryx_lifecycle event=\"shutdown_deadline_exceeded\" "
            "detail=\"the runtime did not finish its shutdown in time and ends itself\"\n";
        const ssize_t ignored = ::write(STDERR_FILENO, message, sizeof(message) - 1);
        static_cast<void>(ignored);
        ::_exit(kExitDeadlineExitCode);
    }).detach();
}

}  // namespace tryx::runtime_shutdown
