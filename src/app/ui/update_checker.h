#pragma once

#include <QByteArray>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

#include <memory>

class QCryptographicHash;
class QFile;
class QNetworkAccessManager;
class QNetworkReply;

namespace quarry::ui {

// What the latest GitHub release says about this platform's package.
struct ReleaseInfo
{
    QString version; // "1.2.3", without the leading "v"
    QString notes;
    QUrl pageUrl;
    QString assetName; // empty: the release has no package for this platform
    QUrl assetUrl;
    QString sha256; // lower-case hex; empty when the release gives none
    qint64 size = 0;
};

// QML singleton `UpdateChecker`: semi-automatic updates. It asks GitHub for the latest release,
// and, when the user agrees, downloads this platform's package (Windows: the Inno Setup installer;
// Linux: the AppImage), checks its SHA-256 when GitHub publishes one, and hands over to it. Nothing
// is installed without the user pressing "Update now". Nothing runs at startup except
// startupCheck(), which MainWindow calls after the first frame.
class UpdateChecker : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(State state READ state NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT FINAL)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString releaseNotes READ releaseNotes NOTIFY stateChanged FINAL)
    Q_PROPERTY(QUrl releaseUrl READ releaseUrl NOTIFY stateChanged FINAL)
    // False when the release has no package for this platform: the dialog then links to the page.
    Q_PROPERTY(bool canInstall READ canInstall NOTIFY stateChanged FINAL)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged FINAL)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged FINAL)

public:
    enum State {
        Idle,
        Checking,
        UpToDate,
        Available,
        Downloading,
        Ready, // downloaded and verified
        Failed,
    };
    Q_ENUM(State)

    explicit UpdateChecker(QObject *parent = nullptr);
    ~UpdateChecker() override;

    State state() const { return m_state; }
    QString currentVersion() const;
    QString latestVersion() const { return m_release.version; }
    QString releaseNotes() const { return m_release.notes; }
    QUrl releaseUrl() const { return m_release.pageUrl; }
    bool canInstall() const { return !m_release.assetName.isEmpty(); }
    double progress() const { return m_progress; }
    QString errorText() const { return m_error; }
    const ReleaseInfo &release() const { return m_release; }
    QString downloadedPath() const { return m_finalPath; }

    // Checks only when the app has a version and QUARRY_NO_UPDATE_CHECK is not set (tests, --smoke
    // and --bench-startup run without a version or with the variable). Failures are silent; a
    // newer release raises promptRequested().
    Q_INVOKABLE void startupCheck();
    // The Help > Check for updates command: every outcome is shown in the dialog.
    Q_INVOKABLE void check(bool silent = false);
    Q_INVOKABLE void download();
    Q_INVOKABLE void cancelDownload();
    // Runs the downloaded package and quits Quarry so that it can replace the files.
    Q_INVOKABLE void installAndQuit();

    // ---- pure helpers (tested directly) ----
    // -1, 0 or 1. Dotted numbers; with equal numbers a "-pre" suffix is older than the release.
    static int compareVersions(const QString &a, const QString &b);
    static bool isValidVersion(const QString &v);
    // `suffix` picks the asset: "-setup.exe" on Windows, ".AppImage" on Linux. False and a
    // translated message in `error` when the JSON is not a release. `allowFile` lets tests use
    // file: URLs; otherwise assets must be https.
    static bool parseRelease(const QByteArray &json, const QString &suffix, bool allowFile,
                             ReleaseInfo *out, QString *error);
    static QString platformSuffix();

    // ---- test hooks ----
    void setEndpoint(const QUrl &url) { m_endpoint = url; }
    void setCurrentVersion(const QString &v) { m_currentVersion = v; }
    void setDownloadDir(const QString &dir) { m_downloadDir = dir; }
    void setAllowFile(bool on) { m_allowFile = on; }
    void setPlatformSuffix(const QString &s) { m_suffix = s; }

signals:
    void stateChanged();
    void progressChanged();
    // A silent check found a newer release: show the "Update now / Later" alert.
    void promptRequested();

private:
    void setState(State s);
    void fail(const QString &message);
    void onCheckFinished(QNetworkReply *reply);
    void onDownloadFinished();
    QNetworkAccessManager *network();
    QString downloadDir() const;

    State m_state = Idle;
    bool m_silent = false;
    bool m_allowFile = false;
    QUrl m_endpoint;
    QString m_currentVersion;
    QString m_downloadDir;
    QString m_suffix;
    ReleaseInfo m_release;
    double m_progress = 0;
    QString m_error;
    QString m_finalPath;
    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_reply = nullptr;
    std::unique_ptr<QFile> m_file;
    std::unique_ptr<QCryptographicHash> m_hasher;
};

} // namespace quarry::ui
