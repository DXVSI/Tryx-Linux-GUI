#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

enum class TryxReplaceJournalLoadStatus {
    Missing,
    Loaded,
    Invalid
};

struct TryxReplaceJournalRecord {
    QString operationId;
    QString deviceIdentity;
    quint64 deviceGeneration = 0;
    QString originalMediaId;
    QString originalRemoteName;
    quint64 originalSize = 0;
    QString artifactId;
    QString decodedSha256;
    QString transformFingerprint;
    QString applyFingerprint;
    QStringList referenceNames;
    QString newRemoteName;
    quint64 newSize = 0;
    QString stage = QStringLiteral("Preflight");
    bool uploadVerified = false;
    bool applyMayHaveStarted = false;
    bool applyVerified = false;
    bool deleteIntentLinked = false;
    bool fileRemoveMayHaveStarted = false;
    QString disposition = QStringLiteral("OriginalRetained");
};

struct TryxReplaceJournalLoadResult {
    TryxReplaceJournalLoadStatus status =
        TryxReplaceJournalLoadStatus::Missing;
    TryxReplaceJournalRecord record;
    QString error;
};

class TryxReplaceJournal final {
public:
    static constexpr int FormatVersion = 1;
    static constexpr qint64 MaximumBytes = 256 * 1024;

    explicit TryxReplaceJournal(QString path);

    QString path() const;
    TryxReplaceJournalLoadResult load() const;
    bool write(const TryxReplaceJournalRecord &record,
               QString *errorMessage = nullptr) const;
    bool clear(QString *errorMessage = nullptr) const;

    static bool validateRecord(const TryxReplaceJournalRecord &record,
                               QString *errorMessage = nullptr);

private:
    QString path_;
};
