#include "ui/update_checker.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

using quarry::ui::ReleaseInfo;
using quarry::ui::UpdateChecker;

class TstUpdateChecker : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;

    QString write(const QString &name, const QByteArray &bytes) const
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        if (f.open(QIODevice::WriteOnly))
            f.write(bytes);
        return path;
    }

    static QByteArray releaseJson(const QString &tag, const QJsonArray &assets)
    {
        return QJsonDocument(QJsonObject{
                                 {QStringLiteral("tag_name"), tag},
                                 {QStringLiteral("html_url"),
                                  QStringLiteral("https://github.com/Hewcan/quarry/releases/x")},
                                 {QStringLiteral("body"), QStringLiteral("Notes")},
                                 {QStringLiteral("assets"), assets},
                             })
            .toJson();
    }

    static QJsonObject asset(const QString &name, const QString &url, const QString &digest = {})
    {
        QJsonObject o{{QStringLiteral("name"), name},
                      {QStringLiteral("browser_download_url"), url},
                      {QStringLiteral("size"), 5}};
        if (!digest.isEmpty())
            o.insert(QStringLiteral("digest"), digest);
        return o;
    }

    // A checker that reads `json` from a local file and installs into its own folder.
    void prepare(UpdateChecker &c, const QByteArray &json, const QString &current) const
    {
        c.setEndpoint(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("latest.json"))));
        c.setCurrentVersion(current);
        c.setAllowFile(true);
        c.setPlatformSuffix(QStringLiteral(".AppImage"));
        c.setDownloadDir(m_dir.filePath(QStringLiteral("dl")));
        QFile f(m_dir.filePath(QStringLiteral("latest.json")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(json);
    }

private slots:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    void comparesVersions()
    {
        QCOMPARE(UpdateChecker::compareVersions(QStringLiteral("1.2.0"), QStringLiteral("1.1.9")), 1);
        QCOMPARE(UpdateChecker::compareVersions(QStringLiteral("0.0.1"), QStringLiteral("0.0.1")), 0);
        QCOMPARE(UpdateChecker::compareVersions(QStringLiteral("1.9"), QStringLiteral("1.10")), -1);
        QCOMPARE(UpdateChecker::compareVersions(QStringLiteral("1.0"), QStringLiteral("1.0.0")), 0);
        // A pre-release is older than its release, newer than the one before.
        QCOMPARE(UpdateChecker::compareVersions(QStringLiteral("1.0.0-rc1"), QStringLiteral("1.0.0")), -1);
        QCOMPARE(UpdateChecker::compareVersions(QStringLiteral("1.0.1-rc1"), QStringLiteral("1.0.0")), 1);
    }

    void validatesVersions()
    {
        QVERIFY(UpdateChecker::isValidVersion(QStringLiteral("1.2.3")));
        QVERIFY(UpdateChecker::isValidVersion(QStringLiteral("1.2.3-rc.1")));
        QVERIFY(!UpdateChecker::isValidVersion(QStringLiteral("latest")));
        QVERIFY(!UpdateChecker::isValidVersion(QStringLiteral("1.2.3; rm -rf")));
        QVERIFY(!UpdateChecker::isValidVersion(QString()));
    }

    void parsePicksThePlatformAsset()
    {
        const QString sha(64, QLatin1Char('A'));
        const QByteArray json = releaseJson(
            QStringLiteral("v1.2.3"),
            {asset(QStringLiteral("Quarry-1.2.3-x64-setup.exe"),
                   QStringLiteral("https://example.test/a.exe")),
             asset(QStringLiteral("Quarry-1.2.3-x86_64.AppImage"),
                   QStringLiteral("https://example.test/a.AppImage"), QStringLiteral("sha256:") + sha)});
        ReleaseInfo info;
        QString error;
        QVERIFY(UpdateChecker::parseRelease(json, QStringLiteral("-setup.exe"), false, &info, &error));
        QCOMPARE(info.version, QStringLiteral("1.2.3"));
        QCOMPARE(info.assetName, QStringLiteral("Quarry-1.2.3-x64-setup.exe"));
        QVERIFY(info.sha256.isEmpty());
        QVERIFY(UpdateChecker::parseRelease(json, QStringLiteral(".AppImage"), false, &info, &error));
        QCOMPARE(info.assetName, QStringLiteral("Quarry-1.2.3-x86_64.AppImage"));
        QCOMPARE(info.sha256, sha.toLower());
        // No suffix (a platform without package): the release is still understood.
        QVERIFY(UpdateChecker::parseRelease(json, QString(), false, &info, &error));
        QVERIFY(info.assetName.isEmpty());
    }

    void parseRefusesUnsafeInput()
    {
        ReleaseInfo info;
        QString error;
        QVERIFY(!UpdateChecker::parseRelease("not json", QStringLiteral(".AppImage"), false, &info, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!UpdateChecker::parseRelease(releaseJson(QStringLiteral("nightly"), {}),
                                             QStringLiteral(".AppImage"), false, &info, &error));
        // Plain http, file: and odd names are never taken as the package.
        const QByteArray json = releaseJson(
            QStringLiteral("1.0.0"),
            {asset(QStringLiteral("a.AppImage"), QStringLiteral("http://example.test/a.AppImage")),
             asset(QStringLiteral("b.AppImage"), QStringLiteral("file:///etc/passwd")),
             asset(QStringLiteral("../c.AppImage"), QStringLiteral("https://example.test/c"))});
        QVERIFY(UpdateChecker::parseRelease(json, QStringLiteral(".AppImage"), false, &info, &error));
        QVERIFY(info.assetName.isEmpty());
    }

    void startupCheckNeedsAVersion()
    {
        UpdateChecker c;
        c.setCurrentVersion(QString());
        c.setEndpoint(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("nothing.json"))));
        QSignalSpy spy(&c, &UpdateChecker::stateChanged);
        c.startupCheck(); // the test executable has no application version: no request
        QCOMPARE(c.state(), UpdateChecker::Idle);
        QCOMPARE(spy.count(), 0);
    }

    void silentCheckFindsANewerRelease()
    {
        UpdateChecker c;
        prepare(c, releaseJson(QStringLiteral("v2.0.0"), {}), QStringLiteral("1.0.0"));
        QSignalSpy prompt(&c, &UpdateChecker::promptRequested);
        c.check(true);
        QCOMPARE(c.state(), UpdateChecker::Checking);
        QTRY_COMPARE(c.state(), UpdateChecker::Available);
        QCOMPARE(c.latestVersion(), QStringLiteral("2.0.0"));
        QVERIFY(!c.canInstall());
        QCOMPARE(prompt.count(), 1);
    }

    void manualCheckDoesNotPrompt()
    {
        UpdateChecker c;
        prepare(c, releaseJson(QStringLiteral("v2.0.0"), {}), QStringLiteral("1.0.0"));
        QSignalSpy prompt(&c, &UpdateChecker::promptRequested);
        c.check(false);
        QTRY_COMPARE(c.state(), UpdateChecker::Available);
        QCOMPARE(prompt.count(), 0); // the dialog is already open for a manual check
    }

    void sameVersionIsUpToDate()
    {
        UpdateChecker c;
        prepare(c, releaseJson(QStringLiteral("v1.0.0"), {}), QStringLiteral("1.0.0"));
        QSignalSpy prompt(&c, &UpdateChecker::promptRequested);
        c.check(true);
        QTRY_COMPARE(c.state(), UpdateChecker::UpToDate);
        QCOMPARE(prompt.count(), 0);
    }

    void unreachableServerFailsWithAMessage()
    {
        UpdateChecker c;
        prepare(c, {}, QStringLiteral("1.0.0"));
        QFile::remove(m_dir.filePath(QStringLiteral("latest.json")));
        c.check(false);
        QTRY_COMPARE(c.state(), UpdateChecker::Failed);
        QVERIFY(!c.errorText().isEmpty());
        // And it can be tried again.
        prepare(c, releaseJson(QStringLiteral("v1.0.0"), {}), QStringLiteral("1.0.0"));
        c.check(false);
        QTRY_COMPARE(c.state(), UpdateChecker::UpToDate);
    }

    void downloadsAndVerifiesThePackage()
    {
        const QByteArray payload = "hello";
        const QString pkg = write(QStringLiteral("pkg.AppImage"), payload);
        const QString digest = QStringLiteral("sha256:")
            + QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
        UpdateChecker c;
        prepare(c,
                releaseJson(QStringLiteral("v2.0.0"),
                            {asset(QStringLiteral("Quarry-2.0.0-x86_64.AppImage"),
                                   QUrl::fromLocalFile(pkg).toString(), digest)}),
                QStringLiteral("1.0.0"));
        c.check(false);
        QTRY_COMPARE(c.state(), UpdateChecker::Available);
        QVERIFY(c.canInstall());
        c.download();
        QCOMPARE(c.state(), UpdateChecker::Downloading);
        QTRY_COMPARE(c.state(), UpdateChecker::Ready);
        QCOMPARE(c.progress(), 1.0);
        QFile f(c.downloadedPath());
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), payload);
        QVERIFY(!QFile::exists(c.downloadedPath() + QStringLiteral(".part")));
    }

    void aChecksumMismatchDiscardsTheDownload()
    {
        const QString pkg = write(QStringLiteral("bad.AppImage"), "hello");
        UpdateChecker c;
        prepare(c,
                releaseJson(QStringLiteral("v3.0.0"),
                            {asset(QStringLiteral("Quarry-3.0.0-x86_64.AppImage"),
                                   QUrl::fromLocalFile(pkg).toString(),
                                   QStringLiteral("sha256:") + QString(64, QLatin1Char('0')))}),
                QStringLiteral("1.0.0"));
        c.check(false);
        QTRY_COMPARE(c.state(), UpdateChecker::Available);
        c.download();
        QTRY_COMPARE(c.state(), UpdateChecker::Failed);
        QVERIFY(c.errorText().contains(QStringLiteral("checksum")));
        QVERIFY(!QFile::exists(c.downloadedPath()));
        QVERIFY(!QFile::exists(c.downloadedPath() + QStringLiteral(".part")));
    }

    void installNeedsAReadyPackage()
    {
        UpdateChecker c;
        c.installAndQuit(); // nothing downloaded: no process, no quit
        c.download(); // nothing found: ignored
        QCOMPARE(c.state(), UpdateChecker::Idle);
    }
};

QTEST_MAIN(TstUpdateChecker)
#include "tst_update_checker.moc"
