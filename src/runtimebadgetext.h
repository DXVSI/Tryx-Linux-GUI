#pragma once

#include <QString>
#include <QStringList>
#include <QDBusArgument>
#include <QJsonObject>

// Additive C16 contract. Auto never carries user text.
struct TryxRuntimeBadgeTextV1 {
    QString mode = QStringLiteral("Auto");
    QString text;
};

inline bool operator==(const TryxRuntimeBadgeTextV1 &a, const TryxRuntimeBadgeTextV1 &b) {
    return a.mode == b.mode && a.text == b.text;
}

inline bool operator!=(const TryxRuntimeBadgeTextV1 &a, const TryxRuntimeBadgeTextV1 &b) {
    return !(a == b);
}

// Output is changed only on success. Diagnostics never contain user text.
bool tryxNormalizeBadgeTextV1(const TryxRuntimeBadgeTextV1 &input,
                              TryxRuntimeBadgeTextV1 *output, QString *error = nullptr);

struct TryxRuntimeOverlayBadgesV1 {
    quint32 schemaVersion = 1;
    TryxRuntimeBadgeTextV1 primaryCpu;
    TryxRuntimeBadgeTextV1 primaryGpu;
    TryxRuntimeBadgeTextV1 secondaryCpu;
    TryxRuntimeBadgeTextV1 secondaryGpu;
};

inline bool operator==(const TryxRuntimeOverlayBadgesV1 &a, const TryxRuntimeOverlayBadgesV1 &b) {
    return a.schemaVersion == b.schemaVersion && a.primaryCpu == b.primaryCpu
        && a.primaryGpu == b.primaryGpu && a.secondaryCpu == b.secondaryCpu
        && a.secondaryGpu == b.secondaryGpu;
}
inline bool operator!=(const TryxRuntimeOverlayBadgesV1 &a, const TryxRuntimeOverlayBadgesV1 &b) {
    return !(a == b);
}

bool tryxNormalizeOverlayBadgesV1(const TryxRuntimeOverlayBadgesV1 &input,
                                 const QStringList &primary, const QStringList &secondary,
                                 bool dualMode, TryxRuntimeOverlayBadgesV1 *output,
                                 QString *error = nullptr);
bool tryxOverlayBadgesHaveCustomText(const TryxRuntimeOverlayBadgesV1 &badges);
QJsonObject tryxOverlayBadgesV1ToJson(const TryxRuntimeOverlayBadgesV1 &badges);
bool tryxOverlayBadgesV1FromJson(const QJsonObject &json, TryxRuntimeOverlayBadgesV1 *badges);

Q_DECLARE_METATYPE(TryxRuntimeBadgeTextV1)
Q_DECLARE_METATYPE(TryxRuntimeOverlayBadgesV1)
QDBusArgument &operator<<(QDBusArgument &argument, const TryxRuntimeBadgeTextV1 &text);
const QDBusArgument &operator>>(const QDBusArgument &argument, TryxRuntimeBadgeTextV1 &text);
QDBusArgument &operator<<(QDBusArgument &argument, const TryxRuntimeOverlayBadgesV1 &badges);
const QDBusArgument &operator>>(const QDBusArgument &argument, TryxRuntimeOverlayBadgesV1 &badges);
