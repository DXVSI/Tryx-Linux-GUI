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

    void applyLayouts(const QList<TryxRuntimeSavedLayoutV1> &layouts);
    void clear();
    bool layoutById(const QString &layoutId,
                    TryxRuntimeSavedLayoutV1 *layout) const;
    QString idForName(const QString &name) const;

private:
    QList<TryxRuntimeSavedLayoutV1> layouts_;
};
