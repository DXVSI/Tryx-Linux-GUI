#pragma once

#include <QDBusConnection>
#include <QObject>
#include <QTimer>

class QDBusServiceWatcher;
class FlatpakRuntimeOwnershipTests;

// The app-scoped API and the native exclusion name deliberately live on
// DIFFERENT connections. No object is ever exported by the exclusion owner.
// Keep this owner alive until after DeviceManager and its workers are destroyed.
class FlatpakRuntimeOwnership final : public QObject {
    Q_OBJECT
public:
    explicit FlatpakRuntimeOwnership(const QDBusConnection &apiBus,
                                     QObject *parent = nullptr);
    ~FlatpakRuntimeOwnership() override;
    bool acquire(QString *error);
    void stopServing();

signals:
    void ownershipLost();

private:
    friend class FlatpakRuntimeOwnershipTests;
    void invalidate();
    void release();
    QDBusConnection apiBus_;
    QDBusConnection guardBus_;
    QString guardConnectionName_;
    QDBusServiceWatcher *apiWatcher_ = nullptr;
    QDBusServiceWatcher *guardWatcher_ = nullptr;
    QTimer connectionCheck_;
    bool apiClaimed_ = false;
    bool guardClaimed_ = false;
    bool held_ = false;
};
