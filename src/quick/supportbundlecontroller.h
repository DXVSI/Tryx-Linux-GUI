#pragma once

#include "supportbundle.h"

#include <QObject>
#include <QString>
#include <QUrl>

class RuntimeClient;

class SupportBundleController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString message READ message NOTIFY stateChanged)
    Q_PROPERTY(QString lastExportPath READ lastExportPath
                   NOTIFY stateChanged)
    Q_PROPERTY(QUrl homeFolder READ homeFolder CONSTANT)
    Q_PROPERTY(bool runtimeDetailsAvailable
                   READ runtimeDetailsAvailable
                   NOTIFY runtimeDetailsAvailableChanged)

public:
    explicit SupportBundleController(
        RuntimeClient *runtime, QObject *parent = nullptr);

    bool busy() const;
    QString state() const;
    QString message() const;
    QString lastExportPath() const;
    QUrl homeFolder() const;
    bool runtimeDetailsAvailable() const;

    Q_INVOKABLE void exportToFolder(const QUrl &folder);
    void retranslate();

signals:
    void stateChanged();
    void runtimeDetailsAvailableChanged();

private:
    enum class ErrorKind {
        None,
        Collection,
        InvalidSnapshot,
        UnsafeFolder,
        Write,
    };

    void onRuntimeSnapshotReady(const QString &snapshot);
    void onRuntimeSnapshotFailed();
    void writeReport(
        const QString &runtimeSnapshot,
        tryx::support_bundle::RuntimeSnapshotStatus runtimeStatus);
    void finishWithError(ErrorKind errorKind);
    void setState(const QString &state);
    void updateMessage();

    RuntimeClient *runtime_ = nullptr;
    QUrl pendingFolder_;
    QString state_ = QStringLiteral("idle");
    QString message_;
    QString lastExportPath_;
    ErrorKind errorKind_ = ErrorKind::None;
    bool busy_ = false;
    bool awaitingRuntimeSnapshot_ = false;
};
