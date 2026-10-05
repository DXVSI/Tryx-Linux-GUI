#pragma once

#include <QString>
#include <QtGlobal>

enum class TryxFirmwareRecoveryJournalLoadStatus {
    Missing,
    Loaded,
    Invalid
};

struct TryxFirmwareRecoveryRecord {
    QString attemptId;
    QString phase = QStringLiteral("Armed");
    QString packageKind;
    QString packageSha256;
    qint64 createdUtcMs = 0;
    qint64 updatedUtcMs = 0;
};

struct TryxFirmwareRecoveryJournalLoadResult {
    TryxFirmwareRecoveryJournalLoadStatus status =
        TryxFirmwareRecoveryJournalLoadStatus::Missing;
    TryxFirmwareRecoveryRecord record;
    QString error;
};

class TryxFirmwareRecoveryJournal final {
public:
    static constexpr int FormatVersion = 1;
    static constexpr qint64 MaximumBytes =
        64 * 1024;

    explicit TryxFirmwareRecoveryJournal(
        QString path = {});

    QString path() const;
    TryxFirmwareRecoveryJournalLoadResult
    load() const;
    bool write(
        const TryxFirmwareRecoveryRecord &record,
        QString *errorMessage = nullptr) const;
    bool clear(
        QString *errorMessage = nullptr) const;
    bool acknowledgeAndClear(
        QString *errorMessage = nullptr) const;

    static QString defaultPath();
    static bool validateRecord(
        const TryxFirmwareRecoveryRecord &record,
        QString *errorMessage = nullptr);

private:
    bool unlinkExactEntry(
        bool requireValidRecord,
        QString *errorMessage) const;

    QString path_;
};
