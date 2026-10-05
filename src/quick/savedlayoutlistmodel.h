#pragma once

#include "runtimecontract.h"

#include <QAbstractListModel>

class SavedLayoutListModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        LayoutIdRole = Qt::UserRole + 1,
        RevisionRole,
        NameRole,
        ScreenModeRole,
        MediaNamesRole,
    };
    Q_ENUM(Role)

    explicit SavedLayoutListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void applyLayoutsV2(const QList<TryxRuntimeSavedLayoutV2> &layouts);
    void applyLayouts(const QList<TryxRuntimeSavedLayoutV1> &layouts);
    void clear();
    bool layoutV2ById(const QString &layoutId,
                    TryxRuntimeSavedLayoutV2 *layout) const;
    bool layoutById(const QString &layoutId, TryxRuntimeSavedLayoutV1 *layout) const;
    QString idForName(const QString &name) const;

private:
    QList<TryxRuntimeSavedLayoutV2> layouts_;
};
