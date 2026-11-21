#include "migration_controller.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace quarry;
using namespace quarry::ui;

class TstMigrationController : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    int m_files = 0;

    // A controller that knows `id` as a Laravel project in `folder`. The connection does not exist
    // in the (temporary) quarry.db, so saving the settings fails there; what is asked here only
    // needs the settings in memory. Never the real quarry.db.
    void configure(ConnectionController &connections, MigrationController &c, const QString &id,
                   const QString &folder)
    {
        connections.setDatabasePath(m_dir.filePath(QStringLiteral("q-%1.db").arg(++m_files)));
        c.setConnections(&connections);
        c.saveSettings(id, QStringLiteral("laravel"), folder, {});
    }

    static void write(const QString &path, const QByteArray &content)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(content);
    }

private slots:
    void kindsStartWithNone();
    void validateWithoutDisk();
    void checkFolder();
    void readSourceGivesTheCodeOfTheFile();
    void readSourceFailsInPlainLanguage();
    void readSourceWithoutSettingsFails();
    void releaseSourceForgetsTheText();
    void entryOfAnUnreadConnectionIsNotFound();
};

void TstMigrationController::kindsStartWithNone()
{
    MigrationController c;
    const QVariantList kinds = c.kinds();
    QVERIFY(kinds.size() >= 2);
    QCOMPARE(kinds.at(0).toMap().value(QStringLiteral("id")).toString(), QString());
    QCOMPARE(kinds.at(1).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("laravel"));
    QCOMPARE(kinds.at(1).toMap().value(QStringLiteral("defaultTable")).toString(),
             QStringLiteral("migrations"));
}

void TstMigrationController::validateWithoutDisk()
{
    MigrationController c;
    QCOMPARE(c.validate({}, {}, {}), QString());
    QVERIFY(!c.validate(QStringLiteral("laravel"), {}, {}).isEmpty());
    QVERIFY(!c.validate(QStringLiteral("laravel"), QStringLiteral("rel/path"), {}).isEmpty());
    const QString laravel = QStringLiteral("laravel");
    QVERIFY(!c.validate(laravel, QDir::tempPath(), QStringLiteral("a;b")).isEmpty());
    QCOMPARE(c.validate(QStringLiteral("laravel"), QDir::tempPath(), {}), QString());
}

void TstMigrationController::checkFolder()
{
    QTemporaryDir folder;
    MigrationController c; // the folder check never opens quarry.db
    c.checkFolder(QStringLiteral("laravel"), folder.path(), {});
    QCOMPARE(c.folderState(), QStringLiteral("checking"));
    QTRY_COMPARE(c.folderState(), QStringLiteral("ok"));
    c.checkFolder(QStringLiteral("laravel"),
                  QDir::tempPath() + QStringLiteral("/quarry-missing-migrations-4242"), {});
    QTRY_COMPARE(c.folderState(), QStringLiteral("failed"));
    QVERIFY(!c.folderMessage().isEmpty());
    c.resetFolderCheck();
    QCOMPARE(c.folderState(), QStringLiteral("idle"));
}

void TstMigrationController::readSourceGivesTheCodeOfTheFile()
{
    QTemporaryDir folder;
    const QString name = QStringLiteral("2024_01_01_000000_create_users_table");
    write(folder.filePath(name + QStringLiteral(".php")),
          "<?php\nSchema::create('users', function () {});\n");
    ConnectionController connections;
    MigrationController c;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    configure(connections, c, id, folder.path());
    QVERIFY(c.isConfigured(id));
    QCOMPARE(c.sourceOf(id, name).value(QStringLiteral("state")).toString(),
             QStringLiteral("none"));

    QSignalSpy changed(&c, &MigrationController::sourceChanged);
    c.readSource(id, name);
    QCOMPARE(c.sourceOf(id, name).value(QStringLiteral("state")).toString(),
             QStringLiteral("loading"));
    c.readSource(id, name); // already loading: not a second request
    QTRY_COMPARE(c.sourceOf(id, name).value(QStringLiteral("state")).toString(),
                 QStringLiteral("ready"));
    const QVariantMap source = c.sourceOf(id, name);
    QVERIFY(
        source.value(QStringLiteral("text")).toString().contains(QStringLiteral("Schema::create")));
    QCOMPARE(source.value(QStringLiteral("file")).toString(), name + QStringLiteral(".php"));
    QCOMPARE(source.value(QStringLiteral("definition")).toString(), QStringLiteral("PHP"));
    QCOMPARE(changed.size(), 2); // loading, ready
    QCOMPARE(changed.at(1).at(0).toString(), id);
    QCOMPARE(changed.at(1).at(1).toString(), name);
}

void TstMigrationController::readSourceFailsInPlainLanguage()
{
    QTemporaryDir folder;
    const QString big = QStringLiteral("2024_02_01_000000_huge");
    write(folder.filePath(big + QStringLiteral(".php")),
          QByteArray(int(MigrationService::kMaxSourceBytes) + 1, 'x'));
    ConnectionController connections;
    MigrationController c;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    configure(connections, c, id, folder.path());

    const QString missing = QStringLiteral("2024_03_01_000000_not_there");
    for (const QString &name : { missing, big, QStringLiteral("../escape") }) {
        c.readSource(id, name);
        QTRY_COMPARE(c.sourceOf(id, name).value(QStringLiteral("state")).toString(),
                     QStringLiteral("failed"));
        const QVariantMap source = c.sourceOf(id, name);
        QVERIFY(!source.value(QStringLiteral("message")).toString().isEmpty());
        QVERIFY(source.value(QStringLiteral("text")).toString().isEmpty());
    }
    // Asking again is a new request (the file may exist by now).
    write(folder.filePath(missing + QStringLiteral(".php")), "<?php\n");
    c.readSource(id, missing);
    QTRY_COMPARE(c.sourceOf(id, missing).value(QStringLiteral("state")).toString(),
                 QStringLiteral("ready"));
}

void TstMigrationController::readSourceWithoutSettingsFails()
{
    MigrationController c;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSignalSpy changed(&c, &MigrationController::sourceChanged);
    c.readSource(id, QStringLiteral("x"));
    QCOMPARE(changed.size(), 1);
    const QVariantMap source = c.sourceOf(id, QStringLiteral("x"));
    QCOMPARE(source.value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
    QVERIFY(!source.value(QStringLiteral("message")).toString().isEmpty());
}

void TstMigrationController::releaseSourceForgetsTheText()
{
    QTemporaryDir folder;
    const QString name = QStringLiteral("2024_01_01_000000_a");
    write(folder.filePath(name + QStringLiteral(".php")), "<?php\n");
    ConnectionController connections;
    MigrationController c;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    configure(connections, c, id, folder.path());
    c.readSource(id, name);
    QTRY_COMPARE(c.sourceOf(id, name).value(QStringLiteral("state")).toString(),
                 QStringLiteral("ready"));
    c.releaseSource(id, name);
    QCOMPARE(c.sourceOf(id, name).value(QStringLiteral("state")).toString(),
             QStringLiteral("none"));
    // A late answer for a tab that is gone is dropped.
    c.readSource(id, name);
    c.releaseSource(id, name);
    QTest::qWait(200);
    QCOMPARE(c.sourceOf(id, name).value(QStringLiteral("state")).toString(),
             QStringLiteral("none"));
}

void TstMigrationController::entryOfAnUnreadConnectionIsNotFound()
{
    MigrationController c;
    const QVariantMap e
        = c.entryOf(QUuid::createUuid().toString(QUuid::WithoutBraces), QStringLiteral("x"));
    QVERIFY(!e.value(QStringLiteral("found")).toBool());
}

QTEST_GUILESS_MAIN(TstMigrationController)
#include "tst_migration_controller.moc"
