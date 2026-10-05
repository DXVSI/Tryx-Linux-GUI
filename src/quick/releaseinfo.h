#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <array>
#include <optional>

namespace ReleaseUpdates {

constexpr qsizetype MaximumBodyBytes = 1024 * 1024;

struct Version {
    std::array<quint32, 3> components;

    bool operator<(const Version &other) const {
        return components < other.components;
    }
    QString text() const;
};

struct Release {
    Version version;
    QString tag;

    QUrl url() const;
};

std::optional<Version> parseVersion(const QString &text);
std::optional<Release> parseRelease(const QByteArray &body);

} // namespace ReleaseUpdates
