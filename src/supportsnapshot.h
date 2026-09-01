#pragma once

#include "runtimecontract.h"

#include <QList>
#include <QPair>
#include <QString>

namespace tryx {

struct SupportSnapshotSourceV1 {
    qint64 generatedAtUtcMs = 0;
    QString runtimeVersion;
    quint32 runtimeApiVersion = 8;
    TryxRuntimeSnapshot connection;
    quint64 physicalGeneration = 0;
    bool recoveryRequired = false;
    bool firmwareRecoveryInterlockActive = false;
    qsizetype mediaCatalogEntryCount = 0;
    qsizetype artifactCount = 0;
    qsizetype operationCount = 0;
    bool retryCandidatePresent = false;
    bool retryDispatchPresent = false;
    qsizetype retryCleanupPendingCount = 0;
    bool deleteRecoveryPresent = false;
    bool replaceRecoveryPresent = false;
    QList<TryxRuntimeOperationInfo> operations;
};

qsizetype supportSnapshotMaximumBytes();

void appendSupportLifecycleEvent(
    const QString &eventName, quint64 generation,
    const QList<QPair<QString, QString>> &fields = {},
    qint64 utcMs = -1, qint64 monotonicMs = -1);

void clearSupportLifecycleEventsForTesting();

QString buildSupportSnapshotV1(const SupportSnapshotSourceV1 &source);
bool supportSnapshotV1IsValid(const QString &json,
                              QString *errorMessage = nullptr);

}  // namespace tryx
