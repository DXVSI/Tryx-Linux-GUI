#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

class QLocalServer;
class QObject;

namespace quickbootstrap {

enum class InstanceLaunchIntent {
    Manual,
    Autostart,
};

enum class SingleInstanceAcquireResult {
    Primary,
    NotifiedExisting,
    Failed,
};

class SingleInstanceGuard final {
public:
    explicit SingleInstanceGuard(QString socketPath);
    ~SingleInstanceGuard();

    SingleInstanceGuard(const SingleInstanceGuard &) = delete;
    SingleInstanceGuard &operator=(const SingleInstanceGuard &) = delete;
    SingleInstanceGuard(SingleInstanceGuard &&) = delete;
    SingleInstanceGuard &operator=(SingleInstanceGuard &&) = delete;

    SingleInstanceAcquireResult acquire(
        QLocalServer *server,
        InstanceLaunchIntent intent,
        QString *errorMessage);

private:
    struct Private;

    QString socketPath_;
    std::unique_ptr<Private> private_;
};

using InstanceLaunchIntentHandler =
    std::function<void(InstanceLaunchIntent)>;

struct RuntimeBootstrapOptions {
    QString systemctlProgram = QStringLiteral("systemctl");
    QString developmentRuntimeProgram;
    QStringList developmentRuntimeArguments;
    QString installedRuntimeFallbackProgram;
    QStringList installedRuntimeFallbackArguments;
    int dbusCallTimeoutMs = 2000;
    int startupTimeoutMs = 8000;
    bool discoverDevelopmentRuntime = true;
    bool allowInstalledRuntimeFallback = true;
};

QString instanceSocketPath();
QByteArray instanceLaunchIntentPayload(InstanceLaunchIntent intent);
std::optional<InstanceLaunchIntent> parseInstanceLaunchIntentPayload(
    const QByteArray &payload);
bool instanceLaunchIntentRequestsWindow(
    InstanceLaunchIntent intent);
bool notifyRunningInstance(const QString &path,
                           InstanceLaunchIntent intent);
bool listenForSingleInstance(QLocalServer *server,
                             const QString &path,
                             QString *errorMessage);
void drainPendingInstanceLaunchConnections(
    QLocalServer *server,
    QObject *context,
    InstanceLaunchIntentHandler handler);

namespace testing {

using AfterLeaseAcquiredBeforeSocketCleanupHook =
    void (*)(const QString &path);
using AfterPreviousLeaseProbeHook =
    void (*)(const QString &path);
using BeforeStaleSocketExchangeHook =
    void (*)(const QString &path);
using AfterStaleSocketExchangeHook =
    void (*)(const QString &path);
using AfterNativeSocketBoundHook =
    void (*)(const QString &path);
using AfterRuntimeDirectoryPinnedHook =
    void (*)(const QString &path);

void setRuntimeDirectoryOverride(const QString &directory);
void clearRuntimeDirectoryOverride();
void setAfterLeaseAcquiredBeforeSocketCleanupHook(
    AfterLeaseAcquiredBeforeSocketCleanupHook hook);
void clearAfterLeaseAcquiredBeforeSocketCleanupHook();
void setAfterPreviousLeaseProbeHook(
    AfterPreviousLeaseProbeHook hook);
void clearAfterPreviousLeaseProbeHook();
void setBeforeStaleSocketExchangeHook(
    BeforeStaleSocketExchangeHook hook);
void clearBeforeStaleSocketExchangeHook();
void setAfterStaleSocketExchangeHook(
    AfterStaleSocketExchangeHook hook);
void clearAfterStaleSocketExchangeHook();
void setAfterNativeSocketBoundHook(
    AfterNativeSocketBoundHook hook);
void clearAfterNativeSocketBoundHook();
void setAfterRuntimeDirectoryPinnedHook(
    AfterRuntimeDirectoryPinnedHook hook);
void clearAfterRuntimeDirectoryPinnedHook();

}  // namespace testing

bool ensureRuntimeService(QString *errorMessage);
bool ensureRuntimeService(
    QString *errorMessage,
    const RuntimeBootstrapOptions &options);

}  // namespace quickbootstrap
