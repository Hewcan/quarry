#include <quarry/connection_store.h>
#include <quarry/migration_service.h>
#include <quarry/migration_source.h>
#include <quarry/provider_registry.h>

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace quarry;

namespace {

const IMigrationSource &laravel()
{
    return *findMigrationSource(u"laravel");
}

bool touch(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write("<?php\n") > 0;
}

MigrationSettings settingsFor(const QString &folder, const QString &table = {})
{
    MigrationSettings s;
    s.connectionId = QUuid::createUuid();
    s.kind = QStringLiteral("laravel");
    s.folder = folder;
    s.table = table;
    return s;
}

const QStringList kFiles = {
    QStringLiteral("2014_10_12_000000_create_users_table"),
    QStringLiteral("2019_08_19_000000_create_failed_jobs_table"),
    QStringLiteral("2024_01_05_101500_add_avatar_to_users"),
};

} // namespace

class TstMigrations : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void registry();
    void allApplied();
    void pendingInChronologicalOrder();
    void orphans();
    void scanReadsOnlyPhpFiles();
    void emptyFolder();
    void invalidPath_data();
    void invalidPath();
    void badTableName();
    void inspectSqlite();
    void inspectMissingTable();
    void inspectInvalidFolder();
    void inspectWithoutSession();
    void cancelDropsTheRequest();
    void settingsRoundTrip();

private:
    std::unique_ptr<Session> openSession();
    bool run(Session *session, const QString &sql);
    bool inspect(MigrationService &service, Session *session, const MigrationSettings &settings,
                 MigrationReport *report, Error *error);

    ProviderRegistry m_registry;
};

void TstMigrations::initTestCase()
{
    m_registry.scan(QStringLiteral(QUARRY_PLUGIN_DIR));
}

std::unique_ptr<Session> TstMigrations::openSession()
{
    ConnectionProfile p;
    p.id = QUuid::createUuid();
    p.name = QStringLiteral("test");
    p.providerId = QStringLiteral("sqlite");
    p.values = { { QStringLiteral("file"), QStringLiteral(":memory:") } };
    auto connection = m_registry.createConnection(p, {});
    if (!connection)
        return nullptr;
    auto session = std::make_unique<Session>(std::move(connection).value());
    QSignalSpy opened(session.get(), &Session::opened);
    session->open();
    if (!opened.wait(15000))
        return nullptr;
    return session;
}

bool TstMigrations::run(Session *session, const QString &sql)
{
    QSignalSpy finished(session, &Session::finished);
    QSignalSpy failed(session, &Session::failed);
    session->execute(Statement { sql, {}, 500 });
    return QTest::qWaitFor([&] { return finished.count() + failed.count() > 0; }, 10000)
        && failed.isEmpty();
}

bool TstMigrations::inspect(MigrationService &service, Session *session,
                            const MigrationSettings &settings, MigrationReport *report,
                            Error *error)
{
    QSignalSpy done(&service, &MigrationService::inspected);
    QSignalSpy failed(&service, &MigrationService::inspectionFailed);
    const quint64 id = service.inspect(session, settings);
    if (!QTest::qWaitFor([&] { return done.count() + failed.count() > 0; }, 10000))
        return false;
    if (!done.isEmpty()) {
        *report = done.at(0).at(1).value<MigrationReport>();
        return done.at(0).at(0).toULongLong() == id;
    }
    *error = failed.at(0).at(1).value<Error>();
    return failed.at(0).at(0).toULongLong() == id;
}

void TstMigrations::registry()
{
    QVERIFY(findMigrationSource(u"laravel") != nullptr);
    QCOMPARE(findMigrationSource(u""), nullptr);
    QCOMPARE(findMigrationSource(u"rails"), nullptr);
    QCOMPARE(laravel().defaultTable(), QStringLiteral("migrations"));
    QCOMPARE(laravel().appliedQuery(settingsFor(QStringLiteral("/x"))).value().sql,
             QStringLiteral("SELECT migration, batch FROM migrations"));
    QCOMPARE(laravel().appliedQuery(settingsFor(QStringLiteral("/x"), QStringLiteral("app.mig")))
                 .value()
                 .sql,
             QStringLiteral("SELECT migration, batch FROM app.mig"));
}

void TstMigrations::allApplied()
{
    const MigrationReport r =
        laravel().reconcile(kFiles, { { kFiles[2], 2 }, { kFiles[0], 1 }, { kFiles[1], 1 } });
    QCOMPARE(r.applied, 3);
    QCOMPARE(r.pending, 0);
    QCOMPARE(r.orphans, 0);
    QCOMPARE(r.files, 3);
    QCOMPARE(r.entries.size(), size_t(3));
    QCOMPARE(r.entries[2].name, kFiles[2]);
    QCOMPARE(r.entries[2].state, MigrationState::Applied);
    QCOMPARE(r.entries[2].batch, std::optional<qint64>(2));
}

void TstMigrations::pendingInChronologicalOrder()
{
    const MigrationReport r = laravel().reconcile(kFiles, { { kFiles[0], 1 } });
    QCOMPARE(r.applied, 1);
    QCOMPARE(r.pending, 2);
    QCOMPARE(r.entries[0].state, MigrationState::Applied);
    QCOMPARE(r.entries[1].name, kFiles[1]);
    QCOMPARE(r.entries[1].state, MigrationState::Pending);
    QCOMPARE(r.entries[1].batch, std::nullopt);
    QCOMPARE(r.entries[2].state, MigrationState::Pending);
}

void TstMigrations::orphans()
{
    const QString gone = QStringLiteral("2016_06_01_000001_create_oauth_table");
    const MigrationReport r = laravel().reconcile(
        { kFiles[0] }, { { kFiles[0], 1 }, { gone, 1 }, { gone, 3 } }); // duplicated row
    QCOMPARE(r.applied, 1);
    QCOMPARE(r.orphans, 1);
    QCOMPARE(r.entries.size(), size_t(2));
    QCOMPARE(r.entries[1].name, gone);
    QCOMPARE(r.entries[1].state, MigrationState::Orphan);
    QCOMPARE(r.entries[1].batch, std::optional<qint64>(1));
}

void TstMigrations::scanReadsOnlyPhpFiles()
{
    QTemporaryDir dir;
    for (const QString &f : { kFiles[2], kFiles[0], kFiles[1] })
        QVERIFY(touch(dir.filePath(f + QStringLiteral(".php"))));
    QVERIFY(touch(dir.filePath(QStringLiteral("README.md"))));
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("nested.php")));
    QVERIFY(touch(dir.filePath(QStringLiteral("nested.php/2030_01_01_000000_inner.php"))));
    const auto files = laravel().scanFolder(dir.path(), {});
    QVERIFY(files.has_value());
    QCOMPARE(files.value(), kFiles);
}

void TstMigrations::emptyFolder()
{
    QTemporaryDir dir;
    const auto files = laravel().scanFolder(dir.path(), {});
    QVERIFY(files.has_value());
    QVERIFY(files.value().isEmpty());
    const MigrationReport r = laravel().reconcile(files.value(), {});
    QCOMPARE(r.files, 0);
    QVERIFY(r.entries.empty());
}

void TstMigrations::invalidPath_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<int>("kind");
    QTemporaryDir dir;
    QTest::newRow("empty") << QString() << int(ErrorKind::Constraint);
    QTest::newRow("relative") << QStringLiteral("database/migrations") << int(ErrorKind::Constraint);
    QTest::newRow("missing") << QDir::tempPath() + QStringLiteral("/quarry-no-such-folder-1234")
                             << int(ErrorKind::NotFound);
}

void TstMigrations::invalidPath()
{
    QFETCH(QString, path);
    QFETCH(int, kind);
    const auto files = laravel().scanFolder(path, {});
    QVERIFY(!files.has_value());
    QCOMPARE(int(files.error().kind), kind);
    QVERIFY(!files.error().message.isEmpty());

    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("a.php"));
    QVERIFY(touch(file));
    const auto notFolder = laravel().scanFolder(file, {});
    QVERIFY(!notFolder.has_value());
    QCOMPARE(notFolder.error().kind, ErrorKind::Constraint);
}

void TstMigrations::badTableName()
{
    QVERIFY(laravel().validate(settingsFor(QDir::tempPath(), QStringLiteral("m; DROP TABLE x"))));
    QVERIFY(!laravel().appliedQuery(settingsFor(QDir::tempPath(), QStringLiteral("a\"b"))));
    QVERIFY(!laravel().validate(settingsFor(QDir::tempPath(), QStringLiteral("public.migrations"))));
}

void TstMigrations::inspectSqlite()
{
    QTemporaryDir dir;
    QTemporaryDir data;
    for (const QString &f : kFiles)
        QVERIFY(touch(dir.filePath(f + QStringLiteral(".php"))));
    auto session = openSession();
    QVERIFY(session);
    QVERIFY(run(session.get(),
                QStringLiteral("CREATE TABLE migrations(id INTEGER PRIMARY KEY, migration TEXT, "
                               "batch INTEGER)")));
    QVERIFY(run(session.get(),
                QStringLiteral("INSERT INTO migrations(migration, batch) VALUES "
                               "('2014_10_12_000000_create_users_table', 1), "
                               "('2015_01_01_000000_deleted_one', 1)")));

    MigrationService service(data.filePath(QStringLiteral("quarry.db")));
    MigrationReport report;
    Error error;
    QVERIFY(inspect(service, session.get(), settingsFor(dir.path()), &report, &error));
    QCOMPARE(error.message, QString());
    QVERIFY(!report.tableMissing);
    QCOMPARE(report.applied, 1);
    QCOMPARE(report.pending, 2);
    QCOMPARE(report.orphans, 1);
    QCOMPARE(report.entries.size(), size_t(4));
    QCOMPARE(report.entries[1].name, QStringLiteral("2015_01_01_000000_deleted_one"));
    QCOMPARE(report.entries[1].state, MigrationState::Orphan);

    // All applied: the remaining two are registered in batch 2.
    QVERIFY(run(session.get(),
                QStringLiteral("DELETE FROM migrations WHERE batch = 1 AND migration LIKE '2015%'; ")));
    QVERIFY(run(session.get(),
                QStringLiteral("INSERT INTO migrations(migration, batch) VALUES "
                               "('2019_08_19_000000_create_failed_jobs_table', 2), "
                               "('2024_01_05_101500_add_avatar_to_users', 2)")));
    QVERIFY(inspect(service, session.get(), settingsFor(dir.path()), &report, &error));
    QCOMPARE(report.applied, 3);
    QCOMPARE(report.pending, 0);
    QCOMPARE(report.orphans, 0);
    QCOMPARE(report.entries[2].batch, std::optional<qint64>(2));
}

void TstMigrations::inspectMissingTable()
{
    QTemporaryDir dir;
    QTemporaryDir data;
    QVERIFY(touch(dir.filePath(kFiles[0] + QStringLiteral(".php"))));
    auto session = openSession();
    QVERIFY(session);
    MigrationService service(data.filePath(QStringLiteral("quarry.db")));
    MigrationReport report;
    Error error;
    QVERIFY(inspect(service, session.get(), settingsFor(dir.path()), &report, &error));
    QCOMPARE(error.message, QString());
    QVERIFY(report.tableMissing);
    QCOMPARE(report.pending, 1);
    // The session is still usable afterwards.
    QVERIFY(run(session.get(), QStringLiteral("SELECT 1")));
}

void TstMigrations::inspectInvalidFolder()
{
    QTemporaryDir data;
    auto session = openSession();
    QVERIFY(session);
    MigrationService service(data.filePath(QStringLiteral("quarry.db")));
    MigrationReport report;
    Error error;
    QVERIFY(inspect(service, session.get(),
                    settingsFor(QDir::tempPath() + QStringLiteral("/quarry-missing-98765")),
                    &report, &error));
    QCOMPARE(error.kind, ErrorKind::NotFound);
    QVERIFY(!error.message.isEmpty());

    QSignalSpy checked(&service, &MigrationService::folderCheckFailed);
    const quint64 id = service.checkFolder(settingsFor(QStringLiteral("relative/path")));
    QTRY_COMPARE(checked.count(), 1);
    QCOMPARE(checked.at(0).at(0).toULongLong(), id);
}

void TstMigrations::inspectWithoutSession()
{
    QTemporaryDir dir;
    QTemporaryDir data;
    MigrationService service(data.filePath(QStringLiteral("quarry.db")));
    MigrationReport report;
    Error error;
    QVERIFY(inspect(service, nullptr, settingsFor(dir.path()), &report, &error));
    QCOMPARE(error.kind, ErrorKind::Connection);

    QSignalSpy ok(&service, &MigrationService::folderChecked);
    service.checkFolder(settingsFor(dir.path()));
    QTRY_COMPARE(ok.count(), 1);
    QCOMPARE(ok.at(0).at(1).toInt(), 0);
}

void TstMigrations::cancelDropsTheRequest()
{
    QTemporaryDir dir;
    QTemporaryDir data;
    auto session = openSession();
    QVERIFY(session);
    MigrationService service(data.filePath(QStringLiteral("quarry.db")));
    QSignalSpy done(&service, &MigrationService::inspected);
    QSignalSpy failed(&service, &MigrationService::inspectionFailed);
    const quint64 id = service.inspect(session.get(), settingsFor(dir.path()));
    QVERIFY(service.isRunning(id));
    service.cancel(id);
    QVERIFY(!service.isRunning(id));
    QTest::qWait(200);
    QCOMPARE(done.count() + failed.count(), 0);
}

void TstMigrations::settingsRoundTrip()
{
    QTemporaryDir data;
    const QString path = data.filePath(QStringLiteral("quarry.db"));
    ConnectionStore connections(path);
    QSignalSpy stored(&connections, &ConnectionStore::saved);
    ConnectionProfile p;
    p.name = QStringLiteral("app");
    p.providerId = QStringLiteral("sqlite");
    connections.save(p);
    QTRY_COMPARE(stored.count(), 1);
    const QUuid id = stored.at(0).at(0).value<ConnectionProfile>().id;

    MigrationService service(path);
    QSignalSpy saved(&service, &MigrationService::settingsSaved);
    QSignalSpy loaded(&service, &MigrationService::settingsLoaded);
    QSignalSpy failed(&service, &MigrationService::failed);
    MigrationSettings s = settingsFor(QStringLiteral("/srv/app/database/migrations"),
                                      QStringLiteral("schema_migrations"));
    s.connectionId = id;
    service.saveSettings(s);
    service.loadSettings();
    QTRY_COMPARE(loaded.count(), 1);
    QCOMPARE(saved.count(), 1);
    auto list = loaded.at(0).at(0).value<QList<MigrationSettings>>();
    QCOMPARE(list.size(), 1);
    QCOMPARE(list.at(0), s);

    // Unknown connection: refused with a clear error.
    service.saveSettings(settingsFor(QStringLiteral("/x")));
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(failed.at(0).at(0).value<Error>().kind, ErrorKind::NotFound);

    // "None" removes the row; deleting the connection removes it too.
    s.kind.clear();
    service.saveSettings(s);
    service.loadSettings();
    QTRY_COMPARE(loaded.count(), 2);
    QVERIFY(loaded.at(1).at(0).value<QList<MigrationSettings>>().isEmpty());
    s.kind = QStringLiteral("laravel");
    service.saveSettings(s);
    QTRY_COMPARE(saved.count(), 3);
    QSignalSpy removed(&connections, &ConnectionStore::removed);
    connections.remove(id);
    QTRY_COMPARE(removed.count(), 1);
    service.loadSettings();
    QTRY_COMPARE(loaded.count(), 3);
    QVERIFY(loaded.at(2).at(0).value<QList<MigrationSettings>>().isEmpty());
}

QTEST_GUILESS_MAIN(TstMigrations)
#include "tst_migrations.moc"
