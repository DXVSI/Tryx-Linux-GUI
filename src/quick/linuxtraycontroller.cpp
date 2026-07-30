#include "linuxtraycontroller.h"

#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QSet>
#include <QVariant>

namespace {

const QString kWatcherService =
    QStringLiteral("org.kde.StatusNotifierWatcher");
const QString kWatcherPath =
    QStringLiteral("/StatusNotifierWatcher");
const QString kWatcherInterface =
    QStringLiteral("org.kde.StatusNotifierWatcher");
const QString kPropertiesInterface =
    QStringLiteral("org.freedesktop.DBus.Properties");
const QString kItemPath =
    QStringLiteral("/StatusNotifierItem");
const QString kMenuPath =
    QStringLiteral("/StatusNotifierItem/Menu");

const QString kNotificationsService =
    QStringLiteral("org.freedesktop.Notifications");
const QString kNotificationsPath =
    QStringLiteral("/org/freedesktop/Notifications");
const QString kNotificationsInterface =
    QStringLiteral("org.freedesktop.Notifications");

linuxtray::MenuLayout buildMenuLayout(
    const linuxtray::MenuModel &model, int itemId,
    int recursionDepth, const QStringList &propertyNames) {
    linuxtray::MenuLayout layout;
    layout.id = itemId;
    layout.properties =
        model.properties(itemId, propertyNames);
    if (recursionDepth == 0) {
        return layout;
    }

    const int childDepth =
        recursionDepth < 0 ? -1 : recursionDepth - 1;
    const QList<int> childIds = model.children(itemId);
    layout.children.reserve(childIds.size());
    for (const int childId : childIds) {
        const linuxtray::MenuLayout child =
            buildMenuLayout(
                model, childId, childDepth, propertyNames);
        layout.children.append(
            QDBusVariant(QVariant::fromValue(child)));
    }
    return layout;
}

}  // namespace

namespace linuxtray {

QDBusArgument &operator<<(QDBusArgument &argument,
                          const IconPixmap &pixmap) {
    argument.beginStructure();
    argument << pixmap.width << pixmap.height << pixmap.data;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                IconPixmap &pixmap) {
    argument.beginStructure();
    argument >> pixmap.width >> pixmap.height >> pixmap.data;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument,
                          const ToolTip &toolTip) {
    argument.beginStructure();
    argument << toolTip.iconName << toolTip.iconPixmap
             << toolTip.title << toolTip.description;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                ToolTip &toolTip) {
    argument.beginStructure();
    argument >> toolTip.iconName >> toolTip.iconPixmap
             >> toolTip.title >> toolTip.description;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument,
                          const MenuLayout &layout) {
    argument.beginStructure();
    argument << layout.id << layout.properties
             << layout.children;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                MenuLayout &layout) {
    argument.beginStructure();
    argument >> layout.id >> layout.properties
             >> layout.children;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(
    QDBusArgument &argument,
    const MenuItemProperties &properties) {
    argument.beginStructure();
    argument << properties.id << properties.properties;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    MenuItemProperties &properties) {
    argument.beginStructure();
    argument >> properties.id >> properties.properties;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(
    QDBusArgument &argument,
    const MenuItemsPropertiesRemoved &properties) {
    argument.beginStructure();
    argument << properties.id << properties.properties;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    MenuItemsPropertiesRemoved &properties) {
    argument.beginStructure();
    argument >> properties.id >> properties.properties;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument,
                          const MenuEvent &event) {
    argument.beginStructure();
    argument << event.id << event.eventId
             << event.data << event.timestamp;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                MenuEvent &event) {
    argument.beginStructure();
    argument >> event.id >> event.eventId
             >> event.data >> event.timestamp;
    argument.endStructure();
    return argument;
}

void registerDBusTypes() {
    static const bool registered = []() {
        qRegisterMetaType<IconPixmap>();
        qRegisterMetaType<IconPixmapList>();
        qRegisterMetaType<ToolTip>();
        qRegisterMetaType<MenuLayout>();
        qRegisterMetaType<MenuItemProperties>();
        qRegisterMetaType<MenuItemPropertiesList>();
        qRegisterMetaType<MenuItemsPropertiesRemoved>();
        qRegisterMetaType<MenuItemsPropertiesRemovedList>();
        qRegisterMetaType<MenuEvent>();
        qRegisterMetaType<MenuEventList>();
        qDBusRegisterMetaType<IconPixmap>();
        qDBusRegisterMetaType<IconPixmapList>();
        qDBusRegisterMetaType<ToolTip>();
        qDBusRegisterMetaType<MenuLayout>();
        qDBusRegisterMetaType<MenuItemProperties>();
        qDBusRegisterMetaType<MenuItemPropertiesList>();
        qDBusRegisterMetaType<MenuItemsPropertiesRemoved>();
        qDBusRegisterMetaType<
            MenuItemsPropertiesRemovedList>();
        qDBusRegisterMetaType<MenuEvent>();
        qDBusRegisterMetaType<MenuEventList>();
        return true;
    }();
    Q_UNUSED(registered);
}

MenuModel::MenuModel()
    : openLabel_(QStringLiteral("Open")),
      quitLabel_(QStringLiteral("Quit")) {}

quint32 MenuModel::revision() const {
    return revision_;
}

QList<int> MenuModel::children(int parentId) const {
    if (parentId != Root) {
        return {};
    }
    return {Open, Separator, Quit};
}

QVariantMap MenuModel::properties(
    int itemId, const QStringList &requestedNames) const {
    QVariantMap result;
    switch (itemId) {
    case Root:
        break;
    case Open:
        result.insert(
            QStringLiteral("label"), openLabel_);
        result.insert(
            QStringLiteral("enabled"), true);
        result.insert(
            QStringLiteral("visible"), true);
        break;
    case Separator:
        result.insert(
            QStringLiteral("type"),
            QStringLiteral("separator"));
        result.insert(
            QStringLiteral("visible"), true);
        break;
    case Quit:
        result.insert(
            QStringLiteral("label"), quitLabel_);
        result.insert(
            QStringLiteral("enabled"), true);
        result.insert(
            QStringLiteral("visible"), true);
        break;
    default:
        return {};
    }
    return filtered(result, requestedNames);
}

bool MenuModel::contains(int itemId) const {
    return itemId >= Root && itemId <= Quit;
}

MenuModel::Action MenuModel::actionForEvent(
    int itemId, const QString &eventId) const {
    if (eventId != QStringLiteral("clicked")) {
        return Action::None;
    }
    if (itemId == Open) {
        return Action::Show;
    }
    if (itemId == Quit) {
        return Action::Quit;
    }
    return Action::None;
}

void MenuModel::setLabels(
    const QString &openLabel,
    const QString &quitLabel) {
    const QString normalizedOpen =
        openLabel.trimmed().isEmpty()
        ? QStringLiteral("Open")
        : openLabel.trimmed();
    const QString normalizedQuit =
        quitLabel.trimmed().isEmpty()
        ? QStringLiteral("Quit")
        : quitLabel.trimmed();
    if (openLabel_ == normalizedOpen &&
        quitLabel_ == normalizedQuit) {
        return;
    }
    openLabel_ = normalizedOpen;
    quitLabel_ = normalizedQuit;
    ++revision_;
    if (revision_ == 0) {
        revision_ = 1;
    }
}

QVariantMap MenuModel::filtered(
    const QVariantMap &source,
    const QStringList &requestedNames) {
    if (requestedNames.isEmpty()) {
        return source;
    }
    const QSet<QString> requested(
        requestedNames.cbegin(), requestedNames.cend());
    QVariantMap result;
    for (auto it = source.cbegin(); it != source.cend(); ++it) {
        if (requested.contains(it.key())) {
            result.insert(it.key(), it.value());
        }
    }
    return result;
}

}  // namespace linuxtray

class LinuxTrayStatusNotifierItem final : public QObject {
    Q_OBJECT
    Q_CLASSINFO(
        "D-Bus Interface",
        "org.kde.StatusNotifierItem")
    Q_PROPERTY(QString Category READ category)
    Q_PROPERTY(QString Id READ id)
    Q_PROPERTY(QString Title READ title)
    Q_PROPERTY(QString Status READ status)
    Q_PROPERTY(quint32 WindowId READ windowId)
    Q_PROPERTY(QString IconName READ iconName)
    Q_PROPERTY(linuxtray::IconPixmapList IconPixmap
                   READ iconPixmap)
    Q_PROPERTY(QString OverlayIconName READ overlayIconName)
    Q_PROPERTY(linuxtray::IconPixmapList OverlayIconPixmap
                   READ overlayIconPixmap)
    Q_PROPERTY(QString AttentionIconName
                   READ attentionIconName)
    Q_PROPERTY(linuxtray::IconPixmapList AttentionIconPixmap
                   READ attentionIconPixmap)
    Q_PROPERTY(QString AttentionMovieName
                   READ attentionMovieName)
    Q_PROPERTY(linuxtray::ToolTip ToolTip READ toolTip)
    Q_PROPERTY(bool ItemIsMenu READ itemIsMenu)
    Q_PROPERTY(QDBusObjectPath Menu READ menu)

public:
    explicit LinuxTrayStatusNotifierItem(
        QObject *parent = nullptr)
        : QObject(parent) {
        toolTip_.iconName = iconName();
        toolTip_.title =
            QStringLiteral("TRYX Panorama Manager");
        toolTip_.description =
            QStringLiteral("TRYX Panorama display control");
    }

    QString category() const {
        return QStringLiteral("ApplicationStatus");
    }
    QString id() const {
        return QStringLiteral("tryx-panorama-manager");
    }
    QString title() const {
        return QStringLiteral("TRYX Panorama Manager");
    }
    QString status() const {
        return QStringLiteral("Active");
    }
    quint32 windowId() const {
        return 0;
    }
    QString iconName() const {
        return QStringLiteral("tryx-panorama");
    }
    linuxtray::IconPixmapList iconPixmap() const {
        return {};
    }
    QString overlayIconName() const {
        return {};
    }
    linuxtray::IconPixmapList overlayIconPixmap() const {
        return {};
    }
    QString attentionIconName() const {
        return {};
    }
    linuxtray::IconPixmapList attentionIconPixmap() const {
        return {};
    }
    QString attentionMovieName() const {
        return {};
    }
    linuxtray::ToolTip toolTip() const {
        return toolTip_;
    }
    bool itemIsMenu() const {
        return false;
    }
    QDBusObjectPath menu() const {
        return QDBusObjectPath(kMenuPath);
    }

    void setToolTip(const QString &title,
                    const QString &description) {
        linuxtray::ToolTip next = toolTip_;
        next.title = title.trimmed().isEmpty()
            ? QStringLiteral("TRYX Panorama Manager")
            : title.trimmed();
        next.description = description.trimmed();
        if (next.title == toolTip_.title &&
            next.description == toolTip_.description) {
            return;
        }
        toolTip_ = next;
        emit NewToolTip();
    }

public slots:
    void ContextMenu(int x, int y) {
        Q_UNUSED(x);
        Q_UNUSED(y);
    }

    void Activate(int x, int y) {
        Q_UNUSED(x);
        Q_UNUSED(y);
        emit showRequested();
    }

    void SecondaryActivate(int x, int y) {
        Q_UNUSED(x);
        Q_UNUSED(y);
        emit showRequested();
    }

    void Scroll(int delta, const QString &orientation) {
        Q_UNUSED(delta);
        Q_UNUSED(orientation);
    }

signals:
    void NewTitle();
    void NewIcon();
    void NewAttentionIcon();
    void NewOverlayIcon();
    void NewToolTip();
    void NewStatus(const QString &status);

    void showRequested();

private:
    linuxtray::ToolTip toolTip_;
};

class LinuxTrayDBusMenu final : public QObject {
    Q_OBJECT
    Q_CLASSINFO(
        "D-Bus Interface",
        "com.canonical.dbusmenu")
    Q_PROPERTY(uint Version READ version)
    Q_PROPERTY(QString TextDirection READ textDirection)
    Q_PROPERTY(QString Status READ status)
    Q_PROPERTY(QStringList IconThemePath READ iconThemePath)

public:
    explicit LinuxTrayDBusMenu(QObject *parent = nullptr)
        : QObject(parent) {}

    uint version() const {
        return 3;
    }
    QString textDirection() const {
        return QStringLiteral("ltr");
    }
    QString status() const {
        return QStringLiteral("normal");
    }
    QStringList iconThemePath() const {
        return {};
    }

    void setLabels(const QString &openLabel,
                   const QString &quitLabel) {
        const quint32 oldRevision = model_.revision();
        model_.setLabels(openLabel, quitLabel);
        if (oldRevision != model_.revision()) {
            emit LayoutUpdated(
                model_.revision(),
                linuxtray::MenuModel::Root);
        }
    }

public slots:
    uint GetLayout(
        int parentId, int recursionDepth,
        const QStringList &propertyNames,
        linuxtray::MenuLayout &layout) const {
        const int requestedParent =
            model_.contains(parentId)
            ? parentId
            : linuxtray::MenuModel::Root;
        layout = buildMenuLayout(
            model_, requestedParent,
            recursionDepth, propertyNames);
        return model_.revision();
    }

    linuxtray::MenuItemPropertiesList GetGroupProperties(
        const QList<int> &itemIds,
        const QStringList &propertyNames) const {
        linuxtray::MenuItemPropertiesList result;
        for (const int itemId : itemIds) {
            if (!model_.contains(itemId)) {
                continue;
            }
            linuxtray::MenuItemProperties entry;
            entry.id = itemId;
            entry.properties =
                model_.properties(itemId, propertyNames);
            result.append(entry);
        }
        return result;
    }

    QDBusVariant GetProperty(
        int itemId, const QString &name) const {
        const QVariantMap properties =
            model_.properties(itemId, {name});
        return QDBusVariant(properties.value(name));
    }

    void Event(
        int itemId, const QString &eventId,
        const QDBusVariant &data, uint timestamp) {
        Q_UNUSED(data);
        dispatchAction(
            model_.actionForEvent(itemId, eventId),
            itemId, timestamp);
    }

    QList<int> EventGroup(
        const linuxtray::MenuEventList &events) {
        QList<int> rejected;
        for (const linuxtray::MenuEvent &event : events) {
            if (!model_.contains(event.id)) {
                rejected.append(event.id);
                continue;
            }
            dispatchAction(
                model_.actionForEvent(
                    event.id, event.eventId),
                event.id, event.timestamp);
        }
        return rejected;
    }

    bool AboutToShow(int itemId) const {
        Q_UNUSED(itemId);
        return false;
    }

    void AboutToShowGroup(
        const QList<int> &itemIds,
        QList<int> &updatesNeeded,
        QList<int> &idErrors) const {
        updatesNeeded.clear();
        idErrors.clear();
        for (const int itemId : itemIds) {
            if (!model_.contains(itemId)) {
                idErrors.append(itemId);
            }
        }
    }

signals:
    void ItemsPropertiesUpdated(
        const linuxtray::MenuItemPropertiesList &updatedProperties,
        const linuxtray::MenuItemsPropertiesRemovedList
            &removedProperties);
    void LayoutUpdated(uint revision, int parentId);
    void ItemActivationRequested(int itemId, uint timestamp);

    void showRequested();
    void quitRequested();

private:
    void dispatchAction(
        linuxtray::MenuModel::Action action,
        int itemId, uint timestamp) {
        switch (action) {
        case linuxtray::MenuModel::Action::Show:
            emit ItemActivationRequested(
                itemId, timestamp);
            emit showRequested();
            break;
        case linuxtray::MenuModel::Action::Quit:
            emit ItemActivationRequested(
                itemId, timestamp);
            emit quitRequested();
            break;
        case linuxtray::MenuModel::Action::None:
            break;
        }
    }

    linuxtray::MenuModel model_;
};

LinuxTrayController::LinuxTrayController(QObject *parent)
    : QObject(parent),
      bus_(QDBusConnection::sessionBus()),
      watcher_(new QDBusServiceWatcher(
          kWatcherService, bus_,
          QDBusServiceWatcher::WatchForRegistration |
              QDBusServiceWatcher::WatchForUnregistration,
          this)),
      item_(new LinuxTrayStatusNotifierItem(this)),
      menu_(new LinuxTrayDBusMenu(this)) {
    linuxtray::registerDBusTypes();

    connect(
        item_, &LinuxTrayStatusNotifierItem::showRequested,
        this, &LinuxTrayController::showRequested);
    connect(
        menu_, &LinuxTrayDBusMenu::showRequested,
        this, &LinuxTrayController::showRequested);
    connect(
        menu_, &LinuxTrayDBusMenu::quitRequested,
        this, &LinuxTrayController::quitRequested);
    connect(
        watcher_, &QDBusServiceWatcher::serviceRegistered,
        this, &LinuxTrayController::handleWatcherRegistered);
    connect(
        watcher_, &QDBusServiceWatcher::serviceUnregistered,
        this, &LinuxTrayController::handleWatcherUnregistered);

    if (!bus_.isConnected()) {
        setDiagnostic(
            tr("The user D-Bus session is unavailable"));
        return;
    }

    registerObjects();
    connectWatcherSignals();
    if (!objectsRegistered_) {
        return;
    }

    const QDBusReply<bool> present =
        bus_.interface()->isServiceRegistered(
            kWatcherService);
    if (present.isValid() && present.value()) {
        handleWatcherRegistered(kWatcherService);
    } else {
        setDiagnostic(
            tr("No StatusNotifier host is available"));
    }
}

LinuxTrayController::~LinuxTrayController() {
    if (!bus_.isConnected()) {
        return;
    }
    bus_.unregisterObject(kMenuPath);
    bus_.unregisterObject(kItemPath);
}

bool LinuxTrayController::available() const {
    return available_;
}

QString LinuxTrayController::diagnostic() const {
    return diagnostic_;
}

void LinuxTrayController::setLabels(
    const QString &openLabel,
    const QString &quitLabel) {
    menu_->setLabels(openLabel, quitLabel);
}

void LinuxTrayController::setToolTip(
    const QString &title,
    const QString &description) {
    item_->setToolTip(title, description);
}

void LinuxTrayController::showNotification(
    const QString &summary, const QString &body,
    int timeoutMs) {
    if (!bus_.isConnected()) {
        const QString message =
            tr("The user D-Bus session is unavailable");
        emit notificationFailed(message);
        return;
    }

    QDBusInterface notifications(
        kNotificationsService, kNotificationsPath,
        kNotificationsInterface, bus_);
    notifications.setTimeout(2000);
    QVariantMap hints;
    const QVariantList arguments = {
        QStringLiteral("TRYX Panorama Manager"),
        notificationId_,
        QStringLiteral("tryx-panorama"),
        summary,
        body,
        QStringList{},
        hints,
        qBound(0, timeoutMs, 60000),
    };
    auto *pending = new QDBusPendingCallWatcher(
        notifications.asyncCallWithArgumentList(
            QStringLiteral("Notify"), arguments),
        this);
    connect(
        pending, &QDBusPendingCallWatcher::finished,
        this, [this, pending]() {
            const QDBusPendingReply<uint> reply = *pending;
            pending->deleteLater();
            if (!reply.isValid()) {
                emit notificationFailed(
                    reply.error().message());
                return;
            }
            notificationId_ = reply.value();
        });
}

void LinuxTrayController::handleWatcherRegistered(
    const QString &service) {
    if (service != kWatcherService ||
        !objectsRegistered_) {
        return;
    }
    ++watcherEpoch_;
    watcherPresent_ = true;
    hostPresent_ = false;
    registrationAccepted_ = false;
    registrationPending_ = false;
    updateAvailable();
    connectWatcherSignals();
    queryHostAvailability();
    registerWithWatcher();
}

void LinuxTrayController::handleWatcherUnregistered(
    const QString &service) {
    if (service != kWatcherService) {
        return;
    }
    ++watcherEpoch_;
    watcherPresent_ = false;
    hostPresent_ = false;
    registrationAccepted_ = false;
    registrationPending_ = false;
    updateAvailable();
    setDiagnostic(
        tr("No StatusNotifier host is available"));
}

void LinuxTrayController::handleHostRegistered() {
    if (!watcherPresent_) {
        return;
    }
    hostPresent_ = true;
    if (!registrationAccepted_ &&
        !registrationPending_) {
        registerWithWatcher();
    }
    updateAvailable();
}

void LinuxTrayController::handleHostUnregistered() {
    hostPresent_ = false;
    updateAvailable();
    setDiagnostic(
        tr("No StatusNotifier host is available"));
}

void LinuxTrayController::registerObjects() {
    const auto flags =
        QDBusConnection::ExportAllProperties |
        QDBusConnection::ExportAllSlots |
        QDBusConnection::ExportAllSignals;
    const bool itemRegistered =
        bus_.registerObject(kItemPath, item_, flags);
    const bool menuRegistered =
        bus_.registerObject(kMenuPath, menu_, flags);
    objectsRegistered_ =
        itemRegistered && menuRegistered;
    if (objectsRegistered_) {
        return;
    }
    if (itemRegistered) {
        bus_.unregisterObject(kItemPath);
    }
    if (menuRegistered) {
        bus_.unregisterObject(kMenuPath);
    }
    setDiagnostic(
        tr("Could not export the StatusNotifier D-Bus objects: %1")
            .arg(bus_.lastError().message()));
}

void LinuxTrayController::connectWatcherSignals() {
    bus_.disconnect(
        kWatcherService, kWatcherPath,
        kWatcherInterface,
        QStringLiteral("StatusNotifierHostRegistered"),
        this, SLOT(handleHostRegistered()));
    bus_.disconnect(
        kWatcherService, kWatcherPath,
        kWatcherInterface,
        QStringLiteral("StatusNotifierHostUnregistered"),
        this, SLOT(handleHostUnregistered()));
    bus_.connect(
        kWatcherService, kWatcherPath,
        kWatcherInterface,
        QStringLiteral("StatusNotifierHostRegistered"),
        this, SLOT(handleHostRegistered()));
    bus_.connect(
        kWatcherService, kWatcherPath,
        kWatcherInterface,
        QStringLiteral("StatusNotifierHostUnregistered"),
        this, SLOT(handleHostUnregistered()));
}

void LinuxTrayController::registerWithWatcher() {
    if (!watcherPresent_ || registrationPending_ ||
        !objectsRegistered_) {
        return;
    }
    registrationPending_ = true;
    const quint64 epoch = watcherEpoch_;
    QDBusInterface watcher(
        kWatcherService, kWatcherPath,
        kWatcherInterface, bus_);
    watcher.setTimeout(2000);
    auto *pending = new QDBusPendingCallWatcher(
        watcher.asyncCall(
            QStringLiteral("RegisterStatusNotifierItem"),
            kItemPath),
        this);
    connect(
        pending, &QDBusPendingCallWatcher::finished,
        this, [this, pending, epoch]() {
            handleRegistrationReply(pending, epoch);
        });
}

void LinuxTrayController::queryHostAvailability() {
    if (!watcherPresent_) {
        return;
    }
    const quint64 epoch = watcherEpoch_;
    QDBusInterface properties(
        kWatcherService, kWatcherPath,
        kPropertiesInterface, bus_);
    properties.setTimeout(2000);
    auto *pending = new QDBusPendingCallWatcher(
        properties.asyncCall(
            QStringLiteral("Get"),
            kWatcherInterface,
            QStringLiteral(
                "IsStatusNotifierHostRegistered")),
        this);
    connect(
        pending, &QDBusPendingCallWatcher::finished,
        this, [this, pending, epoch]() {
            handleHostQueryReply(pending, epoch);
        });
}

void LinuxTrayController::updateAvailable() {
    const bool next =
        objectsRegistered_ && watcherPresent_ &&
        hostPresent_ && registrationAccepted_;
    if (available_ == next) {
        return;
    }
    available_ = next;
    emit availableChanged();
    if (available_) {
        setDiagnostic({});
    }
}

void LinuxTrayController::setDiagnostic(
    const QString &message) {
    if (diagnostic_ == message) {
        return;
    }
    diagnostic_ = message;
    emit diagnosticChanged();
}

void LinuxTrayController::handleRegistrationReply(
    QDBusPendingCallWatcher *watcher,
    quint64 epoch) {
    const QDBusPendingReply<> reply = *watcher;
    watcher->deleteLater();
    if (epoch != watcherEpoch_ ||
        !watcherPresent_) {
        return;
    }
    registrationPending_ = false;
    if (!reply.isValid()) {
        registrationAccepted_ = false;
        updateAvailable();
        setDiagnostic(
            tr("StatusNotifier registration failed: %1")
                .arg(reply.error().message()));
        return;
    }
    registrationAccepted_ = true;
    updateAvailable();
    queryHostAvailability();
}

void LinuxTrayController::handleHostQueryReply(
    QDBusPendingCallWatcher *watcher,
    quint64 epoch) {
    const QDBusPendingReply<QDBusVariant> reply = *watcher;
    watcher->deleteLater();
    if (epoch != watcherEpoch_ ||
        !watcherPresent_) {
        return;
    }
    if (!reply.isValid()) {
        hostPresent_ = false;
        updateAvailable();
        setDiagnostic(
            tr("Could not query the StatusNotifier host: %1")
                .arg(reply.error().message()));
        return;
    }
    hostPresent_ =
        reply.value().variant().toBool();
    updateAvailable();
    if (!hostPresent_) {
        setDiagnostic(
            tr("No StatusNotifier host is available"));
    }
}

#include "linuxtraycontroller.moc"
