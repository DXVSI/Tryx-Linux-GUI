#include "appsettingscontroller.h"

#include <panorama/config.hpp>

#include <QDir>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include <exception>

namespace {

constexpr int kAutostartCommandTimeoutMs = 8000;
const QString kAutostartUnit =
    QStringLiteral("tryx-panorama.service");

QString commandFailureMessage(const QByteArray &output,
                              int exitCode) {
    const QString detail =
        QString::fromLocal8Bit(output).trimmed();
    return detail.isEmpty()
        ? AppSettingsController::tr(
              "systemctl failed with exit code %1")
              .arg(exitCode)
        : detail;
}

}  // namespace

AppSettingsController::AppSettingsController(
    bool offline, QObject *parent)
    : QObject(parent),
      offline_(offline),
      autostartProcess_(new QProcess(this)),
      autostartDeadline_(new QTimer(this)) {
    try {
        const auto config =
            panorama::ConfigManager::load_config();
        if (!config) {
            setConfigError(
                tr("The application settings file is unreadable or invalid"));
        } else {
            const QString configured =
                QString::fromStdString(config->language)
                    .trimmed()
                    .toLower();
            if (isSupportedLanguage(configured)) {
                language_ = configured;
            } else {
                setConfigError(
                    tr("Unsupported application language setting: %1")
                        .arg(configured));
            }
            devicePort_ =
                QString::fromStdString(config->port).trimmed();
            keepaliveInterval_ =
                qBound(5, config->keepalive_interval, 60);
        }
    } catch (const std::exception &error) {
        setConfigError(
            tr("Failed to read application settings: %1")
                .arg(QString::fromLocal8Bit(error.what())));
    }

    autostartProcess_->setProcessChannelMode(
        QProcess::MergedChannels);
    connect(
        autostartProcess_,
        qOverload<int, QProcess::ExitStatus>(
            &QProcess::finished),
        this,
        &AppSettingsController::finishAutostartCommand);
    connect(
        autostartProcess_, &QProcess::errorOccurred,
        this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                handleAutostartProcessError();
            }
        });

    autostartDeadline_->setSingleShot(true);
    autostartDeadline_->setInterval(
        kAutostartCommandTimeoutMs);
    connect(
        autostartDeadline_, &QTimer::timeout,
        this, &AppSettingsController::handleAutostartTimeout);

    if (!offline_) {
        refreshSerialPorts();
        refreshAutostart();
    }
}

QString AppSettingsController::language() const {
    return language_;
}

QString AppSettingsController::devicePort() const {
    return devicePort_;
}

int AppSettingsController::keepaliveInterval() const {
    return keepaliveInterval_;
}

QStringList AppSettingsController::serialPorts() const {
    return serialPorts_;
}

bool AppSettingsController::autostartEnabled() const {
    return autostartEnabled_;
}

bool AppSettingsController::autostartAvailable() const {
    return autostartAvailable_;
}

bool AppSettingsController::busy() const {
    return busy_;
}

QString AppSettingsController::errorMessage() const {
    return errorMessage_;
}

void AppSettingsController::setLanguage(
    const QString &code) {
    const QString normalized =
        code.trimmed().toLower();
    if (!isSupportedLanguage(normalized)) {
        setConfigError(
            tr("Unsupported application language: %1")
                .arg(code));
        return;
    }

    try {
        const auto loaded =
            panorama::ConfigManager::load_config();
        if (!loaded) {
            setConfigError(
                tr("The application settings file is unreadable or invalid"));
            return;
        }

        panorama::Config config = *loaded;
        config.language = normalized.toStdString();
        if (!panorama::ConfigManager::save_config(config)) {
            setConfigError(
                tr("Failed to save the application language"));
            return;
        }
    } catch (const std::exception &error) {
        setConfigError(
            tr("Failed to save application settings: %1")
                .arg(QString::fromLocal8Bit(error.what())));
        return;
    }

    setConfigError({});
    if (language_ == normalized) {
        return;
    }
    language_ = normalized;
    emit languageChanged();
}

void AppSettingsController::setDevicePort(
    const QString &port) {
    const QString normalized = port.trimmed();
    if (!normalized.isEmpty() &&
        (!normalized.startsWith(QStringLiteral("/dev/ttyACM")) ||
         normalized.contains(QStringLiteral("/../")))) {
        setConfigError(
            tr("Only Auto or a /dev/ttyACM device can be selected"));
        return;
    }
    if (devicePort_ == normalized) {
        return;
    }
    if (!saveDeviceSettings(normalized, keepaliveInterval_)) {
        return;
    }
    devicePort_ = normalized;
    emit deviceSettingsChanged();
}

void AppSettingsController::setKeepaliveInterval(
    int seconds) {
    const int bounded = qBound(5, seconds, 60);
    if (keepaliveInterval_ == bounded) {
        return;
    }
    if (!saveDeviceSettings(devicePort_, bounded)) {
        return;
    }
    keepaliveInterval_ = bounded;
    emit deviceSettingsChanged();
}

void AppSettingsController::refreshSerialPorts() {
    QStringList ports;
    const QDir devices(QStringLiteral("/dev"));
    const QFileInfoList entries = devices.entryInfoList(
        {QStringLiteral("ttyACM*")},
        QDir::System | QDir::Files | QDir::Readable,
        QDir::Name);
    for (const QFileInfo &entry : entries) {
        ports.append(
            QStringLiteral("/dev/") + entry.fileName());
    }
    if (!devicePort_.isEmpty() &&
        !ports.contains(devicePort_)) {
        ports.prepend(devicePort_);
    }
    ports.removeDuplicates();
    if (serialPorts_ == ports) {
        return;
    }
    serialPorts_ = ports;
    emit serialPortsChanged();
}

void AppSettingsController::setAutostartEnabled(
    bool enabled) {
    if (offline_) {
        setAutostartError(
            tr("Autostart management is unavailable in offline mode"));
        return;
    }
    if (busy_ ||
        autostartProcess_->state() != QProcess::NotRunning) {
        setAutostartError(
            tr("Another autostart operation is still in progress"));
        return;
    }
    if (autostartAvailable_ &&
        autostartEnabled_ == enabled) {
        setAutostartError({});
        return;
    }

    startAutostartCommand(
        enabled
            ? AutostartOperation::Enable
            : AutostartOperation::Disable);
}

void AppSettingsController::refreshAutostart() {
    if (offline_) {
        setAutostartAvailableState(false);
        return;
    }
    if (busy_ ||
        autostartProcess_->state() != QProcess::NotRunning) {
        setAutostartError(
            tr("Another autostart operation is still in progress"));
        return;
    }
    startAutostartCommand(AutostartOperation::Query);
}

bool AppSettingsController::isSupportedLanguage(
    const QString &code) {
    return code == QStringLiteral("en") ||
           code == QStringLiteral("ru") ||
           code == QStringLiteral("system");
}

bool AppSettingsController::saveDeviceSettings(
    const QString &port, int keepaliveInterval) {
    try {
        const auto loaded =
            panorama::ConfigManager::load_config();
        if (!loaded) {
            setConfigError(
                tr("The application settings file is unreadable or invalid"));
            return false;
        }
        panorama::Config config = *loaded;
        config.port = port.toStdString();
        config.keepalive_interval = keepaliveInterval;
        if (!panorama::ConfigManager::save_config(config)) {
            setConfigError(
                tr("Failed to save device connection settings"));
            return false;
        }
    } catch (const std::exception &error) {
        setConfigError(
            tr("Failed to save device connection settings: %1")
                .arg(QString::fromLocal8Bit(error.what())));
        return false;
    }
    setConfigError({});
    return true;
}

bool AppSettingsController::isEnabledState(
    const QString &state) {
    return state == QStringLiteral("enabled") ||
           state == QStringLiteral("enabled-runtime") ||
           state == QStringLiteral("linked") ||
           state == QStringLiteral("linked-runtime");
}

bool AppSettingsController::isDisabledState(
    const QString &state) {
    return state == QStringLiteral("disabled") ||
           state == QStringLiteral("disabled-runtime");
}

void AppSettingsController::startAutostartCommand(
    AutostartOperation operation) {
    autostartOperation_ = operation;
    autostartTimedOut_ = false;
    setAutostartError({});
    setBusy(true);

    QString action;
    switch (operation) {
    case AutostartOperation::Query:
        action = QStringLiteral("is-enabled");
        break;
    case AutostartOperation::Enable:
        action = QStringLiteral("enable");
        break;
    case AutostartOperation::Disable:
        action = QStringLiteral("disable");
        break;
    case AutostartOperation::None:
        setBusy(false);
        return;
    }

    autostartDeadline_->start();
    autostartProcess_->start(
        QStringLiteral("systemctl"),
        {QStringLiteral("--user"), action,
         kAutostartUnit});
}

void AppSettingsController::finishAutostartCommand(
    int exitCode, QProcess::ExitStatus exitStatus) {
    if (autostartOperation_ ==
        AutostartOperation::None) {
        return;
    }

    autostartDeadline_->stop();
    const AutostartOperation completed =
        autostartOperation_;
    autostartOperation_ = AutostartOperation::None;
    const QByteArray output =
        autostartProcess_->readAll();
    const bool succeeded =
        !autostartTimedOut_ &&
        exitStatus == QProcess::NormalExit &&
        exitCode == 0;
    const bool timedOut = autostartTimedOut_;
    autostartTimedOut_ = false;
    setBusy(false);

    if (completed == AutostartOperation::Query) {
        if (succeeded) {
            const QString state =
                QString::fromLocal8Bit(output)
                    .trimmed()
                    .toLower();
            if (isEnabledState(state)) {
                setAutostartAvailableState(true);
                setAutostartEnabledState(true);
                setAutostartError({});
                return;
            }
        } else {
            const QString state =
                QString::fromLocal8Bit(output)
                    .trimmed()
                    .toLower();
            if (isDisabledState(state)) {
                setAutostartAvailableState(true);
                setAutostartEnabledState(false);
                setAutostartError({});
                return;
            }
        }

        setAutostartAvailableState(false);
        setAutostartEnabledState(false);
        setAutostartError(
            timedOut
                ? tr("Timed out while checking autostart")
                : commandFailureMessage(output, exitCode));
        return;
    }

    if (succeeded) {
        setAutostartAvailableState(true);
        setAutostartEnabledState(
            completed == AutostartOperation::Enable);
        setAutostartError({});
        return;
    }

    setAutostartError(
        timedOut
            ? tr("Timed out while changing autostart")
            : commandFailureMessage(output, exitCode));
}

void AppSettingsController::handleAutostartProcessError() {
    if (autostartOperation_ ==
        AutostartOperation::None) {
        return;
    }

    autostartDeadline_->stop();
    autostartOperation_ = AutostartOperation::None;
    autostartTimedOut_ = false;
    setBusy(false);
    setAutostartAvailableState(false);
    setAutostartError(
        tr("Failed to start systemctl: %1")
            .arg(autostartProcess_->errorString()));
}

void AppSettingsController::handleAutostartTimeout() {
    if (autostartOperation_ ==
        AutostartOperation::None) {
        return;
    }
    autostartTimedOut_ = true;
    autostartProcess_->kill();
}

void AppSettingsController::setAutostartEnabledState(
    bool enabled) {
    if (autostartEnabled_ == enabled) {
        return;
    }
    autostartEnabled_ = enabled;
    emit autostartEnabledChanged();
}

void AppSettingsController::setAutostartAvailableState(
    bool available) {
    if (autostartAvailable_ == available) {
        return;
    }
    autostartAvailable_ = available;
    emit autostartAvailableChanged();
}

void AppSettingsController::setBusy(bool busy) {
    if (busy_ == busy) {
        return;
    }
    busy_ = busy;
    emit busyChanged();
}

void AppSettingsController::setConfigError(
    const QString &message) {
    if (configError_ == message) {
        return;
    }
    configError_ = message;
    updateErrorMessage();
}

void AppSettingsController::setAutostartError(
    const QString &message) {
    if (autostartError_ == message) {
        return;
    }
    autostartError_ = message;
    updateErrorMessage();
}

void AppSettingsController::updateErrorMessage() {
    QStringList messages;
    if (!configError_.isEmpty()) {
        messages.append(configError_);
    }
    if (!autostartError_.isEmpty()) {
        messages.append(autostartError_);
    }
    const QString combined =
        messages.join(QLatin1Char('\n'));
    if (errorMessage_ == combined) {
        return;
    }
    errorMessage_ = combined;
    emit errorMessageChanged();
}
