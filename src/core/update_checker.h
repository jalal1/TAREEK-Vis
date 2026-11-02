#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;

namespace simvis {

// ============================================================================
// UpdateChecker
//
// Asks GitHub for the latest release of TAREEK-Vis and compares its tag with
// the version of this build. It only reads the release metadata: it does not
// download or install anything. The user gets a link to the release page and
// runs the new installer.
//
// The automatic check runs at most once a day. The result of the last check
// is kept in QSettings, so a known update still shows on the days between
// checks without a new request.
// ============================================================================

class UpdateChecker : public QObject {
    Q_OBJECT

public:
    explicit UpdateChecker(QObject* parent = nullptr);

    // Page that always shows the newest release
    static QUrl releasesPage();

    // Setting "updates/autoCheck", on by default
    static bool autoCheckEnabled();
    static void setAutoCheckEnabled(bool enabled);

    // At startup: report a known newer version, then ask GitHub again if the
    // last check is more than a day old. Does nothing when auto check is off.
    void checkAutomatically();

    // From the Help tab: always asks GitHub, and reports every outcome
    void checkNow();

signals:
    // manual is true for a checkNow() reply
    void updateAvailable(const QString& version, const QUrl& page, bool manual);
    // Only for checkNow(), so that the automatic check stays silent
    void upToDate(const QString& version);
    void checkFailed(const QString& reason);

private:
    void request(bool manual);
    // A stored or fetched version that is newer than this build
    void reportIfNewer(const QString& version, const QUrl& page, bool manual);

    QNetworkAccessManager* net_;
};

} // namespace simvis
