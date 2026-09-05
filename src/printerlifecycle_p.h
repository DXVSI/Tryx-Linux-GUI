#pragma once

#include "printersessioncontroller.h"
#include "supportsnapshot.h"
#include <QDateTime>
#include <QDebug>
#include <initializer_list>

namespace tryx::printer_lifecycle {
struct PrinterProcessClock {
    PrinterProcessClock() { timer.start(); }

    QElapsedTimer timer;
};

inline qint64 printerMonotonicMilliseconds() {
    static const PrinterProcessClock clock;
    return clock.timer.elapsed();
}

inline QString printerStructuredValue(QString value) {
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    value.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    value.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}

inline QString printerOverlayLeaseModeName(PrinterOverlayLeaseMode mode) {
    switch (mode) {
    case PrinterOverlayLeaseMode::PingAndOverlayLease:
        return QStringLiteral("ping-and-overlay-lease");
    case PrinterOverlayLeaseMode::PingOnly:
        return QStringLiteral("ping-only");
    }
    return QStringLiteral("unknown");
}

inline QString printerKeepaliveOutcomeName(
    PrinterProtocol::KeepaliveOutcome outcome) {
    switch (outcome) {
    case PrinterProtocol::KeepaliveOutcome::Sent:
        return QStringLiteral("sent");
    case PrinterProtocol::KeepaliveOutcome::RetryableFailure:
        return QStringLiteral("retryable-failure");
    case PrinterProtocol::KeepaliveOutcome::FatalFailure:
        return QStringLiteral("fatal-failure");
    }
    return QStringLiteral("unknown");
}

inline QString printerDiscoveryStateName(
    PrinterProtocol::DiscoveryState state) {
    switch (state) {
    case PrinterProtocol::DiscoveryState::Absent:
        return QStringLiteral("absent");
    case PrinterProtocol::DiscoveryState::RockchipGadget391a0006:
        return QStringLiteral("rockchip-gadget-391a-0006");
    case PrinterProtocol::DiscoveryState::EnumeratingPrinterClass:
        return QStringLiteral("enumerating-printer-class");
    case PrinterProtocol::DiscoveryState::Ready:
        return QStringLiteral("ready");
    case PrinterProtocol::DiscoveryState::PermissionDenied:
        return QStringLiteral("permission-denied");
    case PrinterProtocol::DiscoveryState::Ambiguous:
        return QStringLiteral("ambiguous");
    case PrinterProtocol::DiscoveryState::MonitoringUnavailable:
        return QStringLiteral("monitoring-unavailable");
    }
    return QStringLiteral("unknown");
}

inline void logPrinterLifecycleEvent(
    const QString &eventName, quint64 generation,
    std::initializer_list<QPair<QString, QString>> fields = {}) {
    QStringList parts{
        QStringLiteral("tryx_lifecycle"),
        QStringLiteral("event=") + printerStructuredValue(eventName),
        QStringLiteral("utc=") +
            printerStructuredValue(
                QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)),
        QStringLiteral("monotonic_ms=") +
            QString::number(printerMonotonicMilliseconds()),
        QStringLiteral("generation=") + QString::number(generation)};
    QList<QPair<QString, QString>> supportFields;
    supportFields.reserve(static_cast<qsizetype>(fields.size()));
    for (const auto &field : fields) {
        parts.append(field.first + QLatin1Char('=') +
                     printerStructuredValue(field.second));
        supportFields.append(field);
    }
    tryx::appendSupportLifecycleEvent(eventName, generation, supportFields);
    qInfo().noquote() << parts.join(QLatin1Char(' '));
}

} // namespace tryx::printer_lifecycle
