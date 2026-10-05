#pragma once

#include <QString>
#include <QtGlobal>

class DeviceWorker;

// Read-only, borrowed view of the facade's immediate thread-safe gates.
// It never owns a second generation, cancellation set or publication snapshot.
class DeviceWorkerSessionContext final {
public:
    explicit DeviceWorkerSessionContext(const DeviceWorker &worker)
        : worker_(worker) {}
    quint64 generation() const;
    bool generationIsCurrent(quint64 generation) const;
    bool operationIsCancelled(const QString &operationId) const;
    int generationCancellationFd() const;
    int operationCancellationFd() const;
    unsigned int publishedPresentationPreferences() const;

private:
    const DeviceWorker &worker_;
};
