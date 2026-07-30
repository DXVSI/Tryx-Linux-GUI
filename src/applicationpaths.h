#pragma once

#include <QDir>
#include <QStandardPaths>
#include <QString>

namespace panorama {

inline QString sharedApplicationDataLocation() {
    // Keep persistent data compatible with releases where the device runtime
    // lived inside TRYX Panorama Manager. The standalone runtime has its own
    // application identity, so AppLocalDataLocation cannot be shared safely.
    return QDir(QStandardPaths::writableLocation(
                    QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral(
            "DXVSI/TRYX Panorama Manager"));
}

}  // namespace panorama
