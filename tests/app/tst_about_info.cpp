#include "ui/about_info.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using quarry::ui::AboutInfo;

class TstAboutInfo : public QObject
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

private slots:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    void readsTheSectionsOfTheFile()
    {
        const QString path = write(QStringLiteral("licenses.txt"),
                                   "Third-party software in Quarry\n"
                                   "==============================\n\n"
                                   "Qt 6\n---\nLGPLv3, dynamic.\n\n"
                                   "Lucide icons\n------------\nISC License \xC2\xA9 Lucide\n");
        const QString text = AboutInfo::readLicenses(path);
        QVERIFY(text.contains(QStringLiteral("Qt 6\n---\n")));
        QVERIFY(text.contains(QStringLiteral("Lucide icons\n------------\n")));
        QVERIFY(text.contains(QChar(0x00A9))); // UTF-8, not Latin-1
    }

    void missingFileGivesAClearMessage()
    {
        const QString text = AboutInfo::readLicenses(m_dir.filePath(QStringLiteral("nope.txt")));
        QVERIFY(!text.isEmpty());
        QVERIFY(text.contains(QStringLiteral("not available")));
        QVERIFY(!text.contains(QStringLiteral("nope.txt")));
    }

    void emptyFileGivesTheSameMessage()
    {
        const QString text = AboutInfo::readLicenses(write(QStringLiteral("empty.txt"), {}));
        QVERIFY(text.contains(QStringLiteral("not available")));
    }

    void defaultsToTheEmbeddedResourceAndNothingIsReadBeforeAsked()
    {
        const AboutInfo info;
        QCOMPARE(info.licensesPath(), QStringLiteral(":/licenses/THIRD_PARTY_LICENSES.txt"));
        QCOMPARE(info.licensesPath(), AboutInfo::defaultLicensesPath());
    }

    void licenseTextFollowsThePathProperty()
    {
        AboutInfo info;
        QSignalSpy spy(&info, &AboutInfo::licensesPathChanged);
        const QString path = write(QStringLiteral("own.txt"), "Own licenses\n");
        info.setLicensesPath(path);
        info.setLicensesPath(path); // same value: no second signal
        QCOMPARE(spy.count(), 1);
        QCOMPARE(info.licenseText(), QStringLiteral("Own licenses\n"));
        info.setLicensesPath(m_dir.filePath(QStringLiteral("gone.txt")));
        QVERIFY(info.licenseText().contains(QStringLiteral("not available")));
    }
};

QTEST_MAIN(TstAboutInfo)
#include "tst_about_info.moc"
