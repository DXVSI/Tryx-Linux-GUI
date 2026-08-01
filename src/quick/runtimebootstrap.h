#pragma once

#include <QString>

class QLocalServer;

namespace quickbootstrap {

QString instanceSocketPath();
bool notifyRunningInstance(const QString &path);
bool listenForSingleInstance(QLocalServer *server,
                             const QString &path,
                             QString *errorMessage);
bool ensureRuntimeService(QString *errorMessage);

}  // namespace quickbootstrap
