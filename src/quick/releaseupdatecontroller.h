#pragma once

#include "releaseinfo.h"

#include <QObject>
#include <QTimer>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class ReleaseUpdateControllerTests;

// GUI-owned, process-local release notification state. Construction never polls.
class ReleaseUpdateController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY updateChanged)
    Q_PROPERTY(QString availableVersion READ availableVersion NOTIFY updateChanged)
    Q_PROPERTY(QUrl releaseUrl READ releaseUrl NOTIFY updateChanged)

public:
    explicit ReleaseUpdateController(QObject *parent = nullptr);
    ~ReleaseUpdateController() override;

    bool updateAvailable() const;
    QString availableVersion() const;
    QUrl releaseUrl() const;
    void start();
    void stop();

signals:
    void updateChanged();
    void newReleaseAvailable(const QString &version);

private:
    friend class ReleaseUpdateControllerTests;
    using Clock = std::function<qint64()>;
    // Borrowed transport and clocks are an offline test seam, not a URL/config knob.
    ReleaseUpdateController(const QString &localVersion,
                            QNetworkAccessManager *network,
                            Clock monotonicNow, Clock utcNow);
    void poll();
    void schedule();
    void readBody(QNetworkReply *reply);
    void finish(QNetworkReply *reply);
    void cancelRequest();
    void applySnapshot();
    void deferForRateLimit(QNetworkReply *reply);

    QNetworkAccessManager *network_;
    Clock monotonicNow_;
    Clock utcNow_;
    QTimer pollTimer_;
    QTimer deadlineTimer_;
    QNetworkReply *reply_ = nullptr;
    QByteArray body_;
    QByteArray etag_;
    std::optional<ReleaseUpdates::Version> localVersion_;
    std::optional<ReleaseUpdates::Version> highestAnnounced_;
    std::optional<ReleaseUpdates::Release> snapshot_;
    bool running_ = false;
    bool available_ = false;
    qint64 nextPollMs_ = 0;
    qint64 deadlineMs_ = 0;
};
