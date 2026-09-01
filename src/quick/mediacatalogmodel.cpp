#include "mediacatalogmodel.h"
#include "applicationpaths.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

MediaCatalogModel::MediaCatalogModel(QObject *parent)
    : MediaCatalogModel(
          panorama::sharedApplicationDataLocation(),
          parent) {}

MediaCatalogModel::MediaCatalogModel(
    const QString &applicationDataRoot,
    QObject *parent)
    : QAbstractListModel(parent),
      applicationDataRoot_(
          QDir(applicationDataRoot).absolutePath()) {}

int MediaCatalogModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : entries_.size();
}

QVariant MediaCatalogModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= entries_.size()) {
        return {};
    }
    const TryxRuntimeMediaEntry &entry = entries_.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return entry.name;
    case SizeRole:
        return QVariant::fromValue(entry.size);
    case SourceRole:
        return entry.source;
    case ReadOnlyRole:
        return entry.readOnly;
    case ThumbnailUrlRole:
        return thumbnailUrl(entry);
    case ManagedOriginRole:
        return entry.managedOrigin;
    case DeleteAllowedRole:
        return entry.deleteAllowed;
    case DeleteBlockReasonRole:
        return entry.deleteBlockReason;
    case MediaIdRole:
        return entry.mediaId;
    case DeviceCopyAllowedRole:
        return deviceCopyAllowed(entry);
    case DeviceCopyBlockReasonRole:
        return deviceCopyBlockReasonForEntry(entry);
    default:
        return {};
    }
}

QHash<int, QByteArray> MediaCatalogModel::roleNames() const {
    return {
        {NameRole, "mediaName"},
        {SizeRole, "mediaSize"},
        {SourceRole, "mediaSource"},
        {ReadOnlyRole, "readOnly"},
        {ThumbnailUrlRole, "thumbnailUrl"},
        {ManagedOriginRole, "managedOrigin"},
        {DeleteAllowedRole, "deleteAllowed"},
        {DeleteBlockReasonRole, "deleteBlockReason"},
        {MediaIdRole, "mediaId"},
        {DeviceCopyAllowedRole, "deviceCopyAllowed"},
        {DeviceCopyBlockReasonRole, "deviceCopyBlockReason"},
    };
}

quint64 MediaCatalogModel::revision() const {
    return revision_;
}

QString MediaCatalogModel::deviceIdentity() const {
    return deviceIdentity_;
}

bool MediaCatalogModel::canDelete(
    const QString &mediaName) const {
    for (const TryxRuntimeMediaEntry &entry : entries_) {
        if (entry.name == mediaName) {
            return entry.deleteAllowed;
        }
    }
    return false;
}

QString MediaCatalogModel::deleteBlockReason(
    const QString &mediaName) const {
    for (const TryxRuntimeMediaEntry &entry : entries_) {
        if (entry.name == mediaName) {
            return entry.deleteBlockReason;
        }
    }
    return tr("Media is not present in the current catalog");
}

bool MediaCatalogModel::canStageDeviceCopy(
    const QString &mediaId) const {
    if (mediaId.isEmpty()) {
        return false;
    }
    for (const TryxRuntimeMediaEntry &entry : entries_) {
        if (entry.mediaId == mediaId) {
            return deviceCopyAllowed(entry);
        }
    }
    return false;
}

QString MediaCatalogModel::deviceCopyBlockReason(
    const QString &mediaId) const {
    if (!mediaId.isEmpty()) {
        for (const TryxRuntimeMediaEntry &entry : entries_) {
            if (entry.mediaId == mediaId) {
                return deviceCopyBlockReasonForEntry(entry);
            }
        }
    }
    return tr("Media is not present in the current catalog");
}

bool MediaCatalogModel::uniqueEntryByName(
    const QString &mediaName,
    TryxRuntimeMediaEntry *entry) const {
    const TryxRuntimeMediaEntry *match = nullptr;
    for (const TryxRuntimeMediaEntry &candidate : entries_) {
        if (candidate.name != mediaName) {
            continue;
        }
        if (match) {
            return false;
        }
        match = &candidate;
    }
    if (!match) {
        return false;
    }
    if (entry) {
        *entry = *match;
    }
    return true;
}

void MediaCatalogModel::applySnapshot(
    const TryxRuntimeMediaCatalogSnapshot &snapshot) {
    if (snapshot.revision <= revision_ && revision_ != 0) {
        return;
    }
    const bool identityChanged =
        snapshot.deviceIdentity != deviceIdentity_;
    beginResetModel();
    revision_ = snapshot.revision;
    deviceIdentity_ = snapshot.deviceIdentity;
    entries_ = snapshot.entries;
    endResetModel();
    emit revisionChanged();
    if (identityChanged) {
        emit deviceIdentityChanged();
    }
}

void MediaCatalogModel::applyLegacyFiles(
    const QStringList &files, quint64 revision,
    const QString &deviceIdentity) {
    TryxRuntimeMediaCatalogSnapshot snapshot;
    snapshot.revision = revision;
    snapshot.deviceIdentity = deviceIdentity;

    QSet<QString> seen;
    for (const QString &value : files) {
        const QString name = value.trimmed();
        if (name.isEmpty() || seen.contains(name)) {
            continue;
        }
        seen.insert(name);
        TryxRuntimeMediaEntry entry;
        entry.name = name;
        entry.source = 1U;
        entry.deleteAllowed = true;
        // Manager1 has no FilePull identity. Keep mediaId empty so Quick
        // cannot expose PASE-only export/edit actions for legacy files.
        snapshot.entries.append(entry);
    }
    applySnapshot(snapshot);
}

void MediaCatalogModel::clear() {
    if (entries_.isEmpty() && deviceIdentity_.isEmpty() && revision_ == 0) {
        return;
    }
    beginResetModel();
    entries_.clear();
    deviceIdentity_.clear();
    revision_ = 0;
    endResetModel();
    emit revisionChanged();
    emit deviceIdentityChanged();
}

QUrl MediaCatalogModel::thumbnailUrl(
    const TryxRuntimeMediaEntry &entry) const {
    static const QRegularExpression sha256(
        QStringLiteral("^[0-9a-f]{64}$"));
    if (!sha256.match(entry.thumbnailKey).hasMatch()) {
        return {};
    }
    const QString path = QDir(applicationDataRoot_).filePath(
        QStringLiteral("media-catalog/thumbnails/%1.jpg")
            .arg(entry.thumbnailKey));
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.isSymLink()) {
        return {};
    }
    return QUrl::fromLocalFile(info.absoluteFilePath());
}

bool MediaCatalogModel::deviceCopyAllowed(
    const TryxRuntimeMediaEntry &entry) {
    return !entry.mediaId.isEmpty() && entry.source == 1U &&
           !entry.readOnly;
}

QString MediaCatalogModel::deviceCopyBlockReasonForEntry(
    const TryxRuntimeMediaEntry &entry) {
    if (entry.mediaId.isEmpty()) {
        return tr("The current catalog cannot identify this media");
    }
    if (entry.source != 1U) {
        return tr("Built-in and preset media cannot be exported or edited");
    }
    if (entry.readOnly) {
        return tr("Read-only media cannot be exported or edited");
    }
    return {};
}
