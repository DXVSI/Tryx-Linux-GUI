#pragma once

#include "runtimecontract.h"

#include <QObject>
#include <QString>

class MediaEditorController;
class RuntimeClient;

class CacheManagementController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString phase READ phase NOTIFY stateChanged)
    Q_PROPERTY(QString message READ message NOTIFY stateChanged)
    Q_PROPERTY(bool canStart READ canStart NOTIFY eligibilityChanged)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY stateChanged)
    Q_PROPERTY(bool canRefresh READ canRefresh NOTIFY stateChanged)
    Q_PROPERTY(bool requiresAcknowledgment READ requiresAcknowledgment
                   NOTIFY stateChanged)
    Q_PROPERTY(qint64 totalFiles READ totalFiles NOTIFY stateChanged)
    Q_PROPERTY(qint64 removedFiles READ removedFiles NOTIFY stateChanged)
    Q_PROPERTY(qint64 removedBytes READ removedBytes NOTIFY stateChanged)
    Q_PROPERTY(qint64 runtimeRemovedFiles READ runtimeRemovedFiles
                   NOTIFY stateChanged)
    Q_PROPERTY(qint64 runtimeRemovedBytes READ runtimeRemovedBytes
                   NOTIFY stateChanged)
    Q_PROPERTY(qint64 localRemovedFiles READ localRemovedFiles
                   NOTIFY stateChanged)
    Q_PROPERTY(qint64 localRemovedBytes READ localRemovedBytes
                   NOTIFY stateChanged)
    Q_PROPERTY(quint64 eligibilityRevision READ eligibilityRevision
                   NOTIFY eligibilityChanged)

public:
    explicit CacheManagementController(
        RuntimeClient *runtime, MediaEditorController *editor,
        QObject *parent = nullptr);
    ~CacheManagementController() override;

    QString state() const;
    QString phase() const;
    QString message() const;
    bool canStart() const;
    bool canCancel() const;
    bool canRefresh() const;
    bool requiresAcknowledgment() const;
    qint64 totalFiles() const;
    qint64 removedFiles() const;
    qint64 removedBytes() const;
    qint64 runtimeRemovedFiles() const;
    qint64 runtimeRemovedBytes() const;
    qint64 localRemovedFiles() const;
    qint64 localRemovedBytes() const;
    quint64 eligibilityRevision() const;

    Q_INVOKABLE bool startCleanup(quint64 expectedEligibilityRevision);
    Q_INVOKABLE bool cancelCleanup();
    Q_INVOKABLE bool refresh();
    Q_INVOKABLE void acknowledgeUnresolved();
    void retranslate();

signals:
    void stateChanged();
    void eligibilityChanged();

private:
    bool runtimeSupported() const;
    void handleEligibilityChanged();
    void updateIdleState();
    void handleRuntimeOperation(
        const TryxRuntimeOperationInfo &info, bool fromRefresh);
    void finishRuntimeTerminal(
        const TryxRuntimeOperationInfo &info, bool fromRefresh);
    void runLocalCleanup(const QString &operationId);
    bool runtimeCountsAreValid(
        const TryxRuntimeOperationInfo &info) const;
    void releaseInterlock();
    void clearTrackedOperation();
    void resetCounts();
    void setState(const QString &state,
                  const QString &detail = QString());

    RuntimeClient *runtime_;
    MediaEditorController *editor_;
    QString state_ = QStringLiteral("NotSupported");
    QString phase_ = QStringLiteral("Idle");
    QString detail_;
    QString operationId_;
    qint64 runtimeTotalFiles_ = 0;
    qint64 runtimeRemovedFiles_ = 0;
    qint64 runtimeRemovedBytes_ = 0;
    qint64 localTotalFiles_ = 0;
    qint64 localRemovedFiles_ = 0;
    qint64 localRemovedBytes_ = 0;
    quint64 eligibilityRevision_ = 0;
    bool interlockHeld_ = false;
};
