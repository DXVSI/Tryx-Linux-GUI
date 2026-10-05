#pragma once

#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QStandardPaths>
#include <QString>
#include <QUrl>

namespace tryx::packaging {

inline constexpr bool isFlatpak() {
#ifdef TRYX_FLATPAK
    return true;
#else
    return false;
#endif
}

inline QString appId() {
    return QStringLiteral("io.github.dxvsi.tryx_panorama_manager");
}

inline QString flatpakFirmwareUnavailableReason() {
    return QObject::tr("Firmware flashing is disabled in the experimental Flatpak. "
                       "Use the native application for firmware updates.");
}

inline QString nativeRuntimeService() {
    return QStringLiteral("org.tryx.Panorama");
}

inline QString flatpakRuntimeService() {
    return appId() + QStringLiteral(".Runtime");
}

inline QString runtimeDirectory() {
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (directory.isEmpty())
        return {};
    if (!isFlatpak())
        return directory;
    // Flatpak owns the app parent, which can alias /run/flatpak/app.
    // Resolve only that parent. Keep the app-id and private descendants intact
    // so the existing no-symlink, ownership and directory-pinning guards apply.
    // https://docs.flatpak.org/en/latest/sandbox-permissions.html
    const QString apps = QFileInfo(QDir(directory).filePath(QStringLiteral("app")))
                             .canonicalFilePath();
    return apps.isEmpty() ? QString() : QDir(apps).filePath(appId());
}

// Flatpak exposes $XDG_RUNTIME_DIR/doc as an alias of /run/flatpak/doc.
// Resolve only this portal-owned root, preserving every user-controlled
// descendant so writers can still reject symlinks and pin the exact directory.
inline QUrl resolveDocumentsPortalAlias(const QUrl &url) {
    if (!url.isLocalFile() || !url.host().isEmpty() || url.hasQuery() || url.hasFragment())
        return url;
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty())
        return url;
    const QString documents = QDir(runtime).filePath(QStringLiteral("doc"));
    const QString localPath = url.toLocalFile();
    if (QDir::cleanPath(localPath) != localPath ||
        (localPath != documents && !localPath.startsWith(documents + '/')))
        return url;
    const QString canonicalRoot = QFileInfo(documents).canonicalFilePath();
    if (canonicalRoot.isEmpty() || canonicalRoot == documents)
        return url;
    return QUrl::fromLocalFile(canonicalRoot + localPath.mid(documents.size()));
}

}  // namespace tryx::packaging
