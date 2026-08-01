#pragma once

#include <QByteArray>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusVariant>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class QDBusPendingCallWatcher;
class QDBusServiceWatcher;

namespace linuxtray {

struct IconPixmap {
    int width = 0;
    int height = 0;
    QByteArray data;
};

using IconPixmapList = QList<IconPixmap>;

struct ToolTip {
    QString iconName;
    IconPixmapList iconPixmap;
    QString title;
    QString description;
};

struct MenuLayout {
    int id = 0;
    QVariantMap properties;
    QList<QDBusVariant> children;
};

struct MenuItemProperties {
    int id = 0;
    QVariantMap properties;
};

using MenuItemPropertiesList = QList<MenuItemProperties>;

struct MenuItemsPropertiesRemoved {
    int id = 0;
    QStringList properties;
};

using MenuItemsPropertiesRemovedList =
    QList<MenuItemsPropertiesRemoved>;

struct MenuEvent {
    int id = 0;
    QString eventId;
    QDBusVariant data;
    quint32 timestamp = 0;
};

using MenuEventList = QList<MenuEvent>;

QDBusArgument &operator<<(QDBusArgument &argument,
                          const IconPixmap &pixmap);
const QDBusArgument &operator>>(const QDBusArgument &argument,
                                IconPixmap &pixmap);
QDBusArgument &operator<<(QDBusArgument &argument,
                          const ToolTip &toolTip);
const QDBusArgument &operator>>(const QDBusArgument &argument,
                                ToolTip &toolTip);
QDBusArgument &operator<<(QDBusArgument &argument,
                          const MenuLayout &layout);
const QDBusArgument &operator>>(const QDBusArgument &argument,
                                MenuLayout &layout);
QDBusArgument &operator<<(QDBusArgument &argument,
                          const MenuItemProperties &properties);
const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    MenuItemProperties &properties);
QDBusArgument &operator<<(
    QDBusArgument &argument,
    const MenuItemsPropertiesRemoved &properties);
const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    MenuItemsPropertiesRemoved &properties);
QDBusArgument &operator<<(QDBusArgument &argument,
                          const MenuEvent &event);
const QDBusArgument &operator>>(const QDBusArgument &argument,
                                MenuEvent &event);

void registerDBusTypes();

class MenuModel final {
public:
    enum ItemId {
        Root = 0,
        Open = 1,
        Separator = 2,
        Quit = 3,
    };

    enum class Action {
        None,
        Show,
        Quit,
    };

    MenuModel();

    quint32 revision() const;
    QList<int> children(int parentId) const;
    QVariantMap properties(
        int itemId,
        const QStringList &requestedNames = {}) const;
    bool contains(int itemId) const;
    Action actionForEvent(int itemId,
                          const QString &eventId) const;
    void setLabels(const QString &openLabel,
                   const QString &quitLabel);

private:
    static QVariantMap filtered(
        const QVariantMap &source,
        const QStringList &requestedNames);

    QString openLabel_;
    QString quitLabel_;
    quint32 revision_ = 1;
};

}  // namespace linuxtray

Q_DECLARE_METATYPE(linuxtray::IconPixmap)
Q_DECLARE_METATYPE(linuxtray::IconPixmapList)
Q_DECLARE_METATYPE(linuxtray::ToolTip)
Q_DECLARE_METATYPE(linuxtray::MenuLayout)
Q_DECLARE_METATYPE(linuxtray::MenuItemProperties)
Q_DECLARE_METATYPE(linuxtray::MenuItemPropertiesList)
Q_DECLARE_METATYPE(linuxtray::MenuItemsPropertiesRemoved)
Q_DECLARE_METATYPE(linuxtray::MenuItemsPropertiesRemovedList)
Q_DECLARE_METATYPE(linuxtray::MenuEvent)
Q_DECLARE_METATYPE(linuxtray::MenuEventList)

class LinuxTrayStatusNotifierItem;
class LinuxTrayDBusMenu;

class LinuxTrayController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available
                   NOTIFY availableChanged)
    Q_PROPERTY(QString diagnostic READ diagnostic
                   NOTIFY diagnosticChanged)

public:
    explicit LinuxTrayController(QObject *parent = nullptr);
    ~LinuxTrayController() override;

    bool available() const;
    QString diagnostic() const;

    void setLabels(const QString &openLabel,
                   const QString &quitLabel);
    void setToolTip(const QString &title,
                    const QString &description);

    Q_INVOKABLE void showNotification(
        const QString &summary,
        const QString &body,
        int timeoutMs = 3000);

signals:
    void availableChanged();
    void diagnosticChanged();
    void showRequested();
    void quitRequested();
    void notificationFailed(const QString &message);

private slots:
    void handleWatcherRegistered(const QString &service);
    void handleWatcherUnregistered(const QString &service);
    void handleHostRegistered();
    void handleHostUnregistered();

private:
    void registerObjects();
    void connectWatcherSignals();
    void registerWithWatcher();
    void queryHostAvailability();
    void updateAvailable();
    void setDiagnostic(const QString &message);
    void handleRegistrationReply(
        QDBusPendingCallWatcher *watcher,
        quint64 epoch);
    void handleHostQueryReply(
        QDBusPendingCallWatcher *watcher,
        quint64 epoch);

    QDBusConnection bus_;
    QDBusServiceWatcher *watcher_ = nullptr;
    LinuxTrayStatusNotifierItem *item_ = nullptr;
    LinuxTrayDBusMenu *menu_ = nullptr;
    bool objectsRegistered_ = false;
    bool watcherPresent_ = false;
    bool hostPresent_ = false;
    bool registrationAccepted_ = false;
    bool registrationPending_ = false;
    bool available_ = false;
    QString diagnostic_;
    quint64 watcherEpoch_ = 0;
    quint32 notificationId_ = 0;
};
