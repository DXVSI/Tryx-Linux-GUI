#include "releaseupdatecontroller.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>

#include <chrono>
#include <limits>

namespace {

constexpr qint64 PollIntervalMs = 60 * 60 * 1000;
constexpr int RequestDeadlineMs = 10000;

qint64 secondsHeaderMs(const QByteArray &header) {
    const QByteArray value = header.trimmed();
    if (value.isEmpty())
        return -1;
    for (char character : value) {
        if (character < '0' || character > '9')
            return -1;
    }
    bool ok = false;
    const qint64 seconds = value.toLongLong(&ok);
    return ok && seconds <= std::numeric_limits<qint64>::max() / 1000
        ? seconds * 1000 : -1;
}

QByteArray validatedEtag(const QByteArray &value) {
    // Bound reflected request metadata as well as the JSON body. Reject control
    // characters and anything other than an HTTP entity tag.
    static const QRegularExpression pattern(
        QStringLiteral("\\A(?:W/)?\"[\\x21\\x23-\\x7e]*\"\\z"));
    return value.size() <= 1024 && pattern.match(QString::fromLatin1(value)).hasMatch()
        ? value : QByteArray();
}

} // namespace

ReleaseUpdateController::ReleaseUpdateController(QObject *parent)
    : ReleaseUpdateController(QCoreApplication::applicationVersion(), nullptr,
          [] { return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count(); },
          [] { return QDateTime::currentMSecsSinceEpoch(); }) {
    setParent(parent);
}

ReleaseUpdateController::ReleaseUpdateController(
    const QString &version, QNetworkAccessManager *network,
    Clock monotonicNow, Clock utcNow)
    : network_(network ? network : new QNetworkAccessManager(this)),
      monotonicNow_(std::move(monotonicNow)), utcNow_(std::move(utcNow)),
      localVersion_(ReleaseUpdates::parseVersion(version)) {
    pollTimer_.setSingleShot(true);
    pollTimer_.setTimerType(Qt::PreciseTimer);
    deadlineTimer_.setSingleShot(true);
    deadlineTimer_.setTimerType(Qt::PreciseTimer);
    connect(&pollTimer_, &QTimer::timeout, this, &ReleaseUpdateController::poll);
    connect(&deadlineTimer_, &QTimer::timeout, this, &ReleaseUpdateController::cancelRequest);
}

ReleaseUpdateController::~ReleaseUpdateController() { stop(); }
bool ReleaseUpdateController::updateAvailable() const { return available_; }
QString ReleaseUpdateController::availableVersion() const {
    return available_ ? snapshot_->version.text() : QString();
}
QUrl ReleaseUpdateController::releaseUrl() const {
    return available_ ? snapshot_->url() : QUrl();
}
void ReleaseUpdateController::start() {
    if (running_)
        return;
    running_ = true;
    nextPollMs_ = monotonicNow_();
    schedule();
}

void ReleaseUpdateController::stop() {
    running_ = false;
    pollTimer_.stop();
    cancelRequest();
}

void ReleaseUpdateController::schedule() {
    if (running_) {
        const qint64 delay = qMax(qint64(0), nextPollMs_ - monotonicNow_());
        pollTimer_.start(int(qMin(delay, qint64(std::numeric_limits<int>::max()))));
    }
}

void ReleaseUpdateController::poll() {
    if (!running_ || reply_)
        return;
    const qint64 now = monotonicNow_();
    if (now < nextPollMs_) {
        schedule();
        return;
    }
    // Anchor at this attempt, not at an overdue interval: no catch-up requests.
    nextPollMs_ = now + PollIntervalMs;
    deadlineMs_ = now + RequestDeadlineMs;
    body_.clear();
    QNetworkRequest request(QUrl(QStringLiteral(
        "https://api.github.com/repos/DXVSI/Tryx-Linux-GUI/releases/latest")));
    // API version and public request headers:
    // https://docs.github.com/en/rest/releases/releases#get-the-latest-release
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "Tryx-Linux-GUI");
    request.setRawHeader("X-GitHub-Api-Version", "2026-03-10");
    if (snapshot_ && !etag_.isEmpty())
        request.setRawHeader("If-None-Match", etag_);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setTransferTimeout(RequestDeadlineMs); // Inactivity only; timer below is absolute.
    deadlineTimer_.start(RequestDeadlineMs);
    reply_ = network_->get(request);
    auto *reply = reply_;
    // Qt's buffer limit is approximate; readBody enforces our accumulated cap.
    // https://doc.qt.io/qt-6/qnetworkreply.html#setReadBufferSize
    reply->setReadBufferSize(64 * 1024);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] { readBody(reply); });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { finish(reply); });
    schedule();
}

void ReleaseUpdateController::readBody(QNetworkReply *reply) {
    if (reply != reply_)
        return;
    if (monotonicNow_() >= deadlineMs_) {
        cancelRequest();
        return;
    }
    while (reply->bytesAvailable() > 0) {
        const qsizetype remaining = ReleaseUpdates::MaximumBodyBytes - body_.size();
        const QByteArray chunk = reply->read(qMin(qint64(64 * 1024), qint64(remaining + 1)));
        if (chunk.size() > remaining) {
            cancelRequest();
            return;
        }
        if (chunk.isEmpty())
            break;
        body_ += chunk;
    }
}

void ReleaseUpdateController::finish(QNetworkReply *reply) {
    if (reply != reply_)
        return;
    readBody(reply); // Some backends deliver their last bytes with finished().
    if (reply != reply_)
        return;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    bool valid = false;
    if (status == 403 || status == 429)
        deferForRateLimit(reply);
    if (reply->error() == QNetworkReply::NoError) {
        if (status == 200) {
            const auto release = ReleaseUpdates::parseRelease(body_);
            if (release) {
                snapshot_ = release;
                etag_ = validatedEtag(reply->rawHeader("ETag"));
                valid = true;
            }
        } else if (status == 304 && snapshot_ && !etag_.isEmpty()) {
            valid = true;
        }
    }
    cancelRequest();
    if (valid)
        applySnapshot();
}

void ReleaseUpdateController::cancelRequest() {
    deadlineTimer_.stop();
    auto *reply = reply_;
    reply_ = nullptr; // abort() can synchronously emit finished().
    if (reply) {
        disconnect(reply, nullptr, this, nullptr);
        if (!reply->isFinished())
            reply->abort();
        reply->deleteLater();
    }
    body_.clear();
    schedule();
}

void ReleaseUpdateController::applySnapshot() {
    available_ = localVersion_ && *localVersion_ < snapshot_->version;
    emit updateChanged();
    if (available_ && (!highestAnnounced_ || *highestAnnounced_ < snapshot_->version)) {
        highestAnnounced_ = snapshot_->version;
        emit newReleaseAvailable(snapshot_->version.text());
    }
}

void ReleaseUpdateController::deferForRateLimit(QNetworkReply *reply) {
    // Convert server wall-clock hints once; subsequent waiting is monotonic.
    // https://docs.github.com/en/rest/using-the-rest-api/rate-limits-for-the-rest-api
    const qint64 utc = utcNow_();
    const qint64 now = monotonicNow_();
    qint64 delay = secondsHeaderMs(reply->rawHeader("Retry-After"));
    if (delay < 0) {
        auto dateHeader = reply->rawHeader("Retry-After").trimmed();
        // Qt's RFC2822 parser requires a numeric zone; HTTP dates use GMT.
        if (dateHeader.endsWith(" GMT")) {
            dateHeader.chop(3);
            dateHeader += "+0000";
        }
        const auto date = QDateTime::fromString(
            QString::fromLatin1(dateHeader), Qt::RFC2822Date);
        if (date.isValid() && date.toMSecsSinceEpoch() > utc)
            delay = date.toMSecsSinceEpoch() - utc;
    }
    if (reply->rawHeader("X-RateLimit-Remaining").trimmed() == "0") {
        const qint64 reset = secondsHeaderMs(reply->rawHeader("X-RateLimit-Reset"));
        if (reset > utc)
            delay = qMax(delay, reset - utc);
    }
    if (delay > 0 && delay <= std::numeric_limits<qint64>::max() - now)
        nextPollMs_ = qMax(nextPollMs_, now + delay);
}
