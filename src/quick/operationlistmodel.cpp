#include "operationlistmodel.h"

#include <QtGlobal>

OperationListModel::OperationListModel(QObject *parent)
    : QAbstractListModel(parent) {}

int OperationListModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : operations_.size();
}

QVariant OperationListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= operations_.size()) {
        return {};
    }
    const TryxRuntimeOperationInfo &info = operations_.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case SubjectRole:
        return info.subject;
    case IdRole:
        return info.id;
    case ParentIdRole:
        return info.parentId;
    case KindRole:
        return info.kind;
    case StateRole:
        return info.state;
    case StageRole:
        return info.stage;
    case ErrorCategoryRole:
        return info.errorCategory;
    case MessageRole:
        return info.message;
    case CompletedRole:
        return info.completed;
    case TotalRole:
        return info.total;
    case ProgressRole:
        return info.total > 0
            ? qBound(0.0,
                     static_cast<double>(info.completed) /
                         static_cast<double>(info.total),
                     1.0)
            : 0.0;
    case RetryModeRole:
        return info.retryMode;
    case CanRetryRole:
        return info.state == QStringLiteral("RetryAvailable");
    case TerminalRole:
        return isTerminal(info);
    default:
        return {};
    }
}

QHash<int, QByteArray> OperationListModel::roleNames() const {
    return {
        {IdRole, "operationId"},
        {ParentIdRole, "parentId"},
        {KindRole, "kind"},
        {StateRole, "operationState"},
        {StageRole, "stage"},
        {ErrorCategoryRole, "errorCategory"},
        {SubjectRole, "subject"},
        {MessageRole, "message"},
        {CompletedRole, "completed"},
        {TotalRole, "total"},
        {ProgressRole, "progress"},
        {RetryModeRole, "retryMode"},
        {CanRetryRole, "canRetry"},
        {TerminalRole, "terminal"},
    };
}

quint64 OperationListModel::revision() const {
    return revision_;
}

bool OperationListModel::applySnapshot(
    const TryxRuntimeOperationsSnapshot &snapshot) {
    if (snapshot.revision <= revision_ && revision_ != 0) {
        return false;
    }
    beginResetModel();
    revision_ = snapshot.revision;
    operations_ = snapshot.operations;
    endResetModel();
    emit revisionChanged();
    return true;
}

bool OperationListModel::upsert(const TryxRuntimeOperationInfo &info,
                                quint64 revision) {
    if (revision <= revision_ && revision_ != 0) {
        return false;
    }
    revision_ = revision;
    const int existing = indexOf(info.id);
    if (existing < 0) {
        beginInsertRows(QModelIndex(), operations_.size(),
                        operations_.size());
        operations_.append(info);
        endInsertRows();
    } else {
        operations_[existing] = info;
        emit dataChanged(index(existing), index(existing));
    }
    emit revisionChanged();
    return true;
}

bool OperationListModel::remove(const QString &operationId,
                                quint64 revision) {
    if (revision <= revision_ && revision_ != 0) {
        return false;
    }
    revision_ = revision;
    const int existing = indexOf(operationId);
    if (existing >= 0) {
        beginRemoveRows(QModelIndex(), existing, existing);
        operations_.removeAt(existing);
        endRemoveRows();
    }
    emit revisionChanged();
    return true;
}

void OperationListModel::clear() {
    if (operations_.isEmpty() && revision_ == 0) {
        return;
    }
    beginResetModel();
    operations_.clear();
    revision_ = 0;
    endResetModel();
    emit revisionChanged();
}

bool OperationListModel::isTerminal(
    const TryxRuntimeOperationInfo &info) {
    return info.state == QStringLiteral("Succeeded") ||
           info.state == QStringLiteral("Completed") ||
           info.state == QStringLiteral("Failed") ||
           info.state == QStringLiteral("Cancelled") ||
           info.state == QStringLiteral("RetryAvailable");
}

int OperationListModel::indexOf(const QString &operationId) const {
    for (int index = 0; index < operations_.size(); ++index) {
        if (operations_.at(index).id == operationId) {
            return index;
        }
    }
    return -1;
}
