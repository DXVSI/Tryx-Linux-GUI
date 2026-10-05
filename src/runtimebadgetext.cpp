#include "runtimebadgetext.h"

#include <QList>

namespace {
bool invalid(QString *error) {
    if (error) {
        *error = QStringLiteral("Invalid badge text: use Auto or a single Custom line of 1-32 Unicode characters (up to 128 UTF-8 bytes), without control or format characters.");
    }
    return false;
}

bool validScalars(const QString &text) {
    // QChar is UTF-16; classify complete scalars, not surrogate code units.
    // https://doc.qt.io/qt-6/qchar.html#category-1
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar first = text.at(i);
        char32_t scalar = first.unicode();
        if (first.isHighSurrogate()) {
            if (++i == text.size() || !text.at(i).isLowSurrogate()) {
                return false;
            }
            scalar = QChar::surrogateToUcs4(first, text.at(i));
        } else if (first.isLowSurrogate()) {
            return false;
        }
        switch (QChar::category(scalar)) {
        case QChar::Other_Control:
        case QChar::Other_Format:
        case QChar::Other_Surrogate:
        case QChar::Separator_Line:
        case QChar::Separator_Paragraph:
            return false;
        default:
            break;
        }
    }
    return true;
}
}

bool tryxNormalizeBadgeTextV1(const TryxRuntimeBadgeTextV1 &input,
                              TryxRuntimeBadgeTextV1 *output, QString *error) {
    if (error) {
        error->clear();
    }
    if (input.mode == QStringLiteral("Auto")) {
        if (!input.text.isEmpty()) {
            return invalid(error);
        }
        if (output) {
            *output = input;
        }
        return true;
    }
    if (input.mode != QStringLiteral("Custom") || !validScalars(input.text)) {
        return invalid(error);
    }
    const QString text = input.text.trimmed();
    if (text.isEmpty() || text.size() > 64 || text.toUcs4().size() > 32
        || text.toUtf8().size() > 128) {
        return invalid(error);
    }
    if (output) {
        *output = {input.mode, text};
    }
    return true;
}

bool tryxNormalizeOverlayBadgesV1(const TryxRuntimeOverlayBadgesV1 &input,
                                 const QStringList &primary, const QStringList &secondary,
                                 bool dualMode, TryxRuntimeOverlayBadgesV1 *output,
                                 QString *error) {
    if (error) {
        error->clear();
    }
    TryxRuntimeOverlayBadgesV1 result;
    if (input.schemaVersion != 1
        || !tryxNormalizeBadgeTextV1(input.primaryCpu, &result.primaryCpu, error)
        || !tryxNormalizeBadgeTextV1(input.primaryGpu, &result.primaryGpu, error)
        || !tryxNormalizeBadgeTextV1(input.secondaryCpu, &result.secondaryCpu, error)
        || !tryxNormalizeBadgeTextV1(input.secondaryGpu, &result.secondaryGpu, error)) {
        return invalid(error);
    }
    if (!primary.contains(QStringLiteral("CPU Badge"))) result.primaryCpu = {};
    if (!primary.contains(QStringLiteral("GPU Badge"))) result.primaryGpu = {};
    if (!dualMode || !secondary.contains(QStringLiteral("CPU Badge"))) result.secondaryCpu = {};
    if (!dualMode || !secondary.contains(QStringLiteral("GPU Badge"))) result.secondaryGpu = {};
    if (output) {
        *output = result;
    }
    return true;
}

bool tryxOverlayBadgesHaveCustomText(const TryxRuntimeOverlayBadgesV1 &badges) {
    return badges.primaryCpu.mode == QStringLiteral("Custom")
        || badges.primaryGpu.mode == QStringLiteral("Custom")
        || badges.secondaryCpu.mode == QStringLiteral("Custom")
        || badges.secondaryGpu.mode == QStringLiteral("Custom");
}

QJsonObject tryxOverlayBadgesV1ToJson(const TryxRuntimeOverlayBadgesV1 &badges) {
    const auto slot = [](const TryxRuntimeBadgeTextV1 &value) {
        return QJsonObject{{QStringLiteral("mode"), value.mode}, {QStringLiteral("text"), value.text}};
    };
    return {{QStringLiteral("schemaVersion"), static_cast<qint64>(badges.schemaVersion)},
            {QStringLiteral("primaryCpu"), slot(badges.primaryCpu)},
            {QStringLiteral("primaryGpu"), slot(badges.primaryGpu)},
            {QStringLiteral("secondaryCpu"), slot(badges.secondaryCpu)},
            {QStringLiteral("secondaryGpu"), slot(badges.secondaryGpu)}};
}

bool tryxOverlayBadgesV1FromJson(const QJsonObject &json, TryxRuntimeOverlayBadgesV1 *badges) {
    if (!badges || json.size() != 5 || json.value(QStringLiteral("schemaVersion")) != QJsonValue(1)) {
        return false;
    }
    const auto slot = [&json](const QString &key, TryxRuntimeBadgeTextV1 *out) {
        if (!json.value(key).isObject()) return false;
        const QJsonObject value = json.value(key).toObject();
        if (value.size() != 2 || !value.value(QStringLiteral("mode")).isString()
            || !value.value(QStringLiteral("text")).isString()) return false;
        const TryxRuntimeBadgeTextV1 input{value.value(QStringLiteral("mode")).toString(),
                                          value.value(QStringLiteral("text")).toString()};
        return tryxNormalizeBadgeTextV1(input, out) && *out == input;
    };
    TryxRuntimeOverlayBadgesV1 result;
    if (!slot(QStringLiteral("primaryCpu"), &result.primaryCpu)
        || !slot(QStringLiteral("primaryGpu"), &result.primaryGpu)
        || !slot(QStringLiteral("secondaryCpu"), &result.secondaryCpu)
        || !slot(QStringLiteral("secondaryGpu"), &result.secondaryGpu)) return false;
    *badges = result;
    return true;
}

QDBusArgument &operator<<(QDBusArgument &argument, const TryxRuntimeBadgeTextV1 &text) {
    argument.beginStructure();
    argument << text.mode << text.text;
    argument.endStructure();
    return argument;
}
const QDBusArgument &operator>>(const QDBusArgument &argument, TryxRuntimeBadgeTextV1 &text) {
    argument.beginStructure();
    argument >> text.mode >> text.text;
    argument.endStructure();
    return argument;
}
QDBusArgument &operator<<(QDBusArgument &argument, const TryxRuntimeOverlayBadgesV1 &badges) {
    argument.beginStructure();
    argument << badges.schemaVersion << badges.primaryCpu << badges.primaryGpu
             << badges.secondaryCpu << badges.secondaryGpu;
    argument.endStructure();
    return argument;
}
const QDBusArgument &operator>>(const QDBusArgument &argument, TryxRuntimeOverlayBadgesV1 &badges) {
    argument.beginStructure();
    argument >> badges.schemaVersion >> badges.primaryCpu >> badges.primaryGpu
             >> badges.secondaryCpu >> badges.secondaryGpu;
    argument.endStructure();
    return argument;
}
