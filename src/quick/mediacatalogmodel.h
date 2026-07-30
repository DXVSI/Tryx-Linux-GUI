#pragma once

#include "runtimecontract.h"

#include <QAbstractListModel>

class MediaCatalogModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(quint64 revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(QString deviceIdentity READ deviceIdentity
                   NOTIFY deviceIdentityChanged)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        SizeRole,
        SourceRole,
        ReadOnlyRole,
        ThumbnailUrlRole,
        ManagedOriginRole,
        DeleteAllowedRole,
        DeleteBlockReasonRole,
        MediaIdRole,
        DeviceCopyAllowedRole,
        DeviceCopyBlockReasonRole
    };
    Q_ENUM(Role)

    explicit MediaCatalogModel(QObject *parent = nullptr);
    explicit MediaCatalogModel(
        const QString &applicationDataRoot,
        QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    quint64 revision() const;
    QString deviceIdentity() const;
    Q_INVOKABLE bool canDelete(const QString &mediaName) const;
    Q_INVOKABLE QString deleteBlockReason(
        const QString &mediaName) const;
    Q_INVOKABLE bool canStageDeviceCopy(
        const QString &mediaId) const;
    Q_INVOKABLE QString deviceCopyBlockReason(
        const QString &mediaId) const;
    void applySnapshot(const TryxRuntimeMediaCatalogSnapshot &snapshot);
    void applyLegacyFiles(const QStringList &files,
                          quint64 revision,
                          const QString &deviceIdentity);
    void clear();

signals:
    void revisionChanged();
    void deviceIdentityChanged();

private:
    QUrl thumbnailUrl(const TryxRuntimeMediaEntry &entry) const;
    static bool deviceCopyAllowed(
        const TryxRuntimeMediaEntry &entry);
    static QString deviceCopyBlockReasonForEntry(
        const TryxRuntimeMediaEntry &entry);

    quint64 revision_ = 0;
    QString deviceIdentity_;
    QList<TryxRuntimeMediaEntry> entries_;
    QString applicationDataRoot_;
};
