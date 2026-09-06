#include "releaseinfo.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace ReleaseUpdates {

QString Version::text() const {
    return QStringLiteral("%1.%2.%3")
        .arg(components[0]).arg(components[1]).arg(components[2]);
}

QUrl Release::url() const {
    return QUrl(QStringLiteral("https://github.com/DXVSI/Tryx-Linux-GUI/releases/tag/") + tag);
}

std::optional<Version> parseVersion(const QString &text) {
    // Stable SemVer only, with optional Git tag prefix. No suffixes, Unicode
    // digits, leading zeroes or permissive partial parsing of development builds.
    if (text.size() > 33)
        return std::nullopt;
    static const QRegularExpression pattern(QStringLiteral(
        "\\Av?(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\z"));
    const auto match = pattern.match(text);
    if (!match.hasMatch())
        return std::nullopt;
    Version version{};
    for (int index = 0; index < 3; ++index) {
        bool ok = false;
        version.components[index] = match.captured(index + 1).toUInt(&ok);
        if (!ok)
            return std::nullopt;
    }
    return version;
}

std::optional<Release> parseRelease(const QByteArray &body) {
    if (body.size() > MaximumBodyBytes)
        return std::nullopt;
    const auto document = QJsonDocument::fromJson(body);
    if (!document.isObject())
        return std::nullopt;
    const auto object = document.object();
    const auto draft = object.value(QStringLiteral("draft"));
    const auto prerelease = object.value(QStringLiteral("prerelease"));
    const auto published = object.value(QStringLiteral("published_at"));
    const auto tag = object.value(QStringLiteral("tag_name"));
    if (!draft.isBool() || draft.toBool() ||
        !prerelease.isBool() || prerelease.toBool() ||
        !published.isString() || published.toString().trimmed().isEmpty() ||
        !tag.isString()) {
        return std::nullopt;
    }
    const auto version = parseVersion(tag.toString());
    if (!version)
        return std::nullopt;
    // Never consume html_url, release body or assets from the remote response.
    return Release{*version, tag.toString()};
}

} // namespace ReleaseUpdates
