#include "savedlayoutlistmodel.h"

SavedLayoutListModel::SavedLayoutListModel(QObject *parent)
    : QAbstractListModel(parent) {}

int SavedLayoutListModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : layouts_.size();
}

QVariant SavedLayoutListModel::data(const QModelIndex &index,
                                    int role) const {
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= layouts_.size()) {
        return {};
    }
    const TryxRuntimeSavedLayoutV1 &layout = layouts_.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return layout.name;
    case LayoutIdRole:
        return layout.layoutId;
    case RevisionRole:
        return QString::number(layout.revision);
    case ScreenModeRole:
        return layout.request.screenMode;
    case MediaNamesRole:
        return layout.request.media;
    default:
        return {};
    }
}

QHash<int, QByteArray> SavedLayoutListModel::roleNames() const {
    return {
        {LayoutIdRole, "layoutId"},
        {RevisionRole, "layoutRevision"},
        {NameRole, "layoutName"},
        {ScreenModeRole, "screenMode"},
        {MediaNamesRole, "mediaNames"},
    };
}

void SavedLayoutListModel::applyLayouts(
    const QList<TryxRuntimeSavedLayoutV1> &layouts) {
    if (layouts_ == layouts) {
        return;
    }
    beginResetModel();
    layouts_ = layouts;
    endResetModel();
}

void SavedLayoutListModel::clear() {
    if (layouts_.isEmpty()) {
        return;
    }
    beginResetModel();
    layouts_.clear();
    endResetModel();
}

bool SavedLayoutListModel::layoutById(
    const QString &layoutId,
    TryxRuntimeSavedLayoutV1 *layout) const {
    for (const TryxRuntimeSavedLayoutV1 &candidate : layouts_) {
        if (candidate.layoutId != layoutId) {
            continue;
        }
        if (layout) {
            *layout = candidate;
        }
        return true;
    }
    return false;
}

QString SavedLayoutListModel::idForName(const QString &name) const {
    for (const TryxRuntimeSavedLayoutV1 &layout : layouts_) {
        if (layout.name.compare(name, Qt::CaseInsensitive) == 0) {
            return layout.layoutId;
        }
    }
    return {};
}
