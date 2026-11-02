#include "update_checker.h"
#include "logger.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QVersionNumber>

namespace simvis {

namespace {

// /releases/latest skips drafts and pre-releases, so a test release that is
// marked as a pre-release never reaches users.
const char* kLatestReleaseApi =
    "https://api.github.com/repos/jalal1/TAREEK-Vis/releases/latest";

const char* kAutoCheckKey = "updates/autoCheck";
const char* kLastCheckKey = "updates/lastCheck";
const char* kLatestVersionKey = "updates/latestVersion";
const char* kLatestPageKey = "updates/latestPage";

constexpr qint64 kCheckIntervalSecs = 24 * 60 * 60;
constexpr int kTimeoutMs = 10000;

// Tags are "v1.4.0"; the build version is "1.4.0"
QVersionNumber parseVersion(QString text) {
    if (text.startsWith('v', Qt::CaseInsensitive)) text.remove(0, 1);
    return QVersionNumber::fromString(text);
}

} // namespace

UpdateChecker::UpdateChecker(QObject* parent)
    : QObject(parent)
    , net_(new QNetworkAccessManager(this)) {}

QUrl UpdateChecker::releasesPage() {
    return QUrl("https://github.com/jalal1/TAREEK-Vis/releases/latest");
}

bool UpdateChecker::autoCheckEnabled() {
    return QSettings().value(kAutoCheckKey, true).toBool();
}

void UpdateChecker::setAutoCheckEnabled(bool enabled) {
    QSettings().setValue(kAutoCheckKey, enabled);
}

void UpdateChecker::checkAutomatically() {
    if (!autoCheckEnabled()) return;

    QSettings settings;
    const QString known = settings.value(kLatestVersionKey).toString();
    if (!known.isEmpty())
        reportIfNewer(known, settings.value(kLatestPageKey).toUrl(), false);

    const QDateTime last = settings.value(kLastCheckKey).toDateTime();
    if (last.isValid() &&
        last.secsTo(QDateTime::currentDateTimeUtc()) < kCheckIntervalSecs)
        return;
    request(false);
}

void UpdateChecker::checkNow() {
    request(true);
}

void UpdateChecker::request(bool manual) {
    QNetworkRequest req{QUrl(kLatestReleaseApi)};
    // GitHub refuses API requests without a User-Agent
    req.setRawHeader("User-Agent",
                     "TAREEK-Vis/" TAREEK_VIS_VERSION " (+https://github.com/jalal1/TAREEK-Vis)");
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setTransferTimeout(kTimeoutMs);

    QNetworkReply* reply = net_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, manual]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            LOG_WARN(QString("Update check failed: %1").arg(reply->errorString()));
            if (manual) emit checkFailed(reply->errorString());
            return;
        }

        const QJsonObject release = QJsonDocument::fromJson(reply->readAll()).object();
        const QString tag = release.value("tag_name").toString();
        QUrl page(release.value("html_url").toString());
        if (parseVersion(tag).isNull()) {
            LOG_WARN(QString("Update check: no version in the reply (tag '%1')").arg(tag));
            if (manual) emit checkFailed("GitHub sent a reply without a version number.");
            return;
        }
        if (!page.isValid()) page = releasesPage();

        QSettings settings;
        settings.setValue(kLastCheckKey, QDateTime::currentDateTimeUtc());
        settings.setValue(kLatestVersionKey, tag);
        settings.setValue(kLatestPageKey, page);
        LOG_INFO(QString("Update check: latest release is %1").arg(tag));

        reportIfNewer(tag, page, manual);
    });
}

void UpdateChecker::reportIfNewer(const QString& version, const QUrl& page, bool manual) {
    const QVersionNumber latest = parseVersion(version);
    const QVersionNumber current = parseVersion(TAREEK_VIS_VERSION);
    const QString shown = latest.toString();

    if (!latest.isNull() && latest > current)
        emit updateAvailable(shown, page.isValid() ? page : releasesPage(), manual);
    else if (manual)
        emit upToDate(current.toString());
}

} // namespace simvis
