#pragma once

#include "runtimecontract.h"

#include <QAbstractListModel>

class OperationListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(quint64 revision READ revision NOTIFY revisionChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        ParentIdRole,
        KindRole,
        StateRole,
        StageRole,
        ErrorCategoryRole,
        SubjectRole,
        MessageRole,
        CompletedRole,
        TotalRole,
        ProgressRole,
        RetryModeRole,
        CanRetryRole,
        TerminalRole
    };
    Q_ENUM(Role)

    explicit OperationListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    quint64 revision() const;
    bool applySnapshot(const TryxRuntimeOperationsSnapshot &snapshot);
    bool upsert(const TryxRuntimeOperationInfo &info, quint64 revision);
    bool remove(const QString &operationId, quint64 revision);
    void clear();

    static bool isTerminal(const TryxRuntimeOperationInfo &info);

signals:
    void revisionChanged();

private:
    int indexOf(const QString &operationId) const;

    quint64 revision_ = 0;
    QList<TryxRuntimeOperationInfo> operations_;
};
