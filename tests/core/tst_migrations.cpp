#include <quarry/connection_store.h>
#include <quarry/migration_service.h>
#include <quarry/migration_source.h>
#include <quarry/provider_registry.h>

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <sqlite3.h>

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

// Quotes like MySQL (`a``b`) or SQL Server ([a]]b]); the rest of the dialect is not used here.
class FakeDialect final : public ISqlDialect
{
public:
    explicit FakeDialect(bool brackets)
        : m_brackets(brackets)
    { }
    QString quoteIdentifier(QStringView name) const override
    {
        QString text = name.toString();
        if (m_brackets)
            return QLatin1Char('[') + text.replace(QLatin1Char(']'), QLatin1String("]]"))
                + QLatin1Char(']');
        return QLatin1Char('`') + text.replace(QLatin1Char('`'), QLatin1String("``"))
            + QLatin1Char('`');
    }
    QString quoteLiteral(QStringView) const override { return {}; }
    QString highlightingDefinition() const override { return {}; }
    QStringList keywords() const override { return {}; }
    QStringList builtinFunctions() const override { return {}; }
    std::vector<StatementRange> splitStatements(QStringView) const override { return {}; }
    QString limitClause(qint64, qint64) const override { return {}; }
    QString placeholder(int) const override { return QStringLiteral("?"); }
    QString qualifiedName(const ObjectPath &) const override { return {}; }
    std::optional<QString> rowIdentifier() const override { return std::nullopt; }

private:
    bool m_brackets;
};

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
    void schemaUpgradeFromV5();
    void schemaUpgradeFromV6();
    void settingsPerDatabase();
    void qualifiedQuery();
    void qualifiedQueryQuoting();
    void filePathValid();
    void filePathInvalid_data();
    void filePathInvalid();
    void readSourceText();
    void readSourceBomAndUtf8();
    void readSourceSizeLimit();
    void readSourceMissing();
    void readSourceDirectory();
    void readSourceInvalidName();
    void readSourceCancelled();
    void readSourceTwice();
    void remoteValidationAndPosixFilePath();
    void remoteSettingsRoundTrip();
    void remoteCheckInspectAndReadSource();

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
    QCOMPARE(findMigrationSource(QStringView()), nullptr);
    QCOMPARE(findMigrationSource(u"rails"), nullptr);
    QCOMPARE(laravel().defaultTable(), QStringLiteral("migrations"));
    QCOMPARE(laravel().appliedQuery(settingsFor(QStringLiteral("/x"))).value().sql,
             QStringLiteral("SELECT migration, batch FROM migrations"));
    QCOMPARE(laravel()
                 .appliedQuery(settingsFor(QStringLiteral("/x"), QStringLiteral("app.mig")))
                 .value()
                 .sql,
             QStringLiteral("SELECT migration, batch FROM app.mig"));
}

void TstMigrations::allApplied()
{
    const MigrationReport r
        = laravel().reconcile(kFiles, { { kFiles[2], 2 }, { kFiles[0], 1 }, { kFiles[1], 1 } });
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
    const int constraint = int(ErrorKind::Constraint);
    QTest::newRow("empty") << QString() << constraint;
    QTest::newRow("relative") << QStringLiteral("database/migrations") << constraint;
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
    const QString qualified = QStringLiteral("public.migrations");
    QVERIFY(!laravel().validate(settingsFor(QDir::tempPath(), qualified)));
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
    QVERIFY(
        run(session.get(), QStringLiteral("DELETE FROM migrations WHERE migration LIKE '2015%'")));
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
    s.phpPath = QStringLiteral("C:/tools/php/php.exe");
    s.projectRoot = QStringLiteral("/srv/app");
    service.saveSettings(s);
    service.loadSettings();
    QTRY_COMPARE(loaded.count(), 1);
    QCOMPARE(saved.count(), 1);
    auto list = loaded.at(0).at(0).value<QList<MigrationSettings>>();
    QCOMPARE(list.size(), 1);
    QCOMPARE(list.at(0), s);
    QCOMPARE(list.at(0).phpPath, s.phpPath);
    QCOMPARE(list.at(0).projectRoot, s.projectRoot);

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

void TstMigrations::schemaUpgradeFromV5()
{
    QTemporaryDir data;
    const QString path = data.filePath(QStringLiteral("quarry.db"));
    const QUuid id = QUuid::createUuid();
    const QByteArray sid = id.toString(QUuid::WithoutBraces).toUtf8();
    {
        sqlite3 *db = nullptr;
        QCOMPARE(sqlite3_open(path.toUtf8().constData(), &db), SQLITE_OK);
        const QByteArray sql = "CREATE TABLE connections(id TEXT PRIMARY KEY NOT NULL,"
                               " name TEXT NOT NULL, provider_id TEXT NOT NULL,"
                               " values_json TEXT NOT NULL DEFAULT '{}',"
                               " color_index INTEGER NOT NULL DEFAULT -1,"
                               " production INTEGER NOT NULL DEFAULT 0,"
                               " password_mode INTEGER NOT NULL DEFAULT 0,"
                               " created_at TEXT NOT NULL, updated_at TEXT NOT NULL,"
                               " last_used_at TEXT,"
                               " record_history INTEGER NOT NULL DEFAULT 1);"
                               "CREATE TABLE migration_settings("
                               " connection_id TEXT PRIMARY KEY NOT NULL REFERENCES"
                               " connections(id) ON DELETE CASCADE,"
                               " kind TEXT NOT NULL, folder TEXT NOT NULL,"
                               " table_name TEXT NOT NULL DEFAULT '', updated_at TEXT NOT NULL);"
                               "INSERT INTO connections(id, name, provider_id, created_at,"
                               " updated_at) VALUES('"
            + sid
            + "','Old','sqlite','2024-01-01T00:00:00.000Z','2024-01-01T00:00:00.000Z');"
              "INSERT INTO migration_settings(connection_id, kind, folder, table_name, updated_at)"
              " VALUES('"
            + sid
            + "','laravel','/srv/app/database/migrations','m','2024-01-01T00:00:00.000Z');"
              "PRAGMA user_version=5";
        QCOMPARE(sqlite3_exec(db, sql.constData(), nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(db);
    }

    MigrationService service(path);
    QSignalSpy loaded(&service, &MigrationService::settingsLoaded);
    QSignalSpy failed(&service, &MigrationService::failed);
    service.loadSettings();
    QTRY_COMPARE(loaded.count(), 1);
    QCOMPARE(failed.count(), 0);
    const auto list = loaded.at(0).at(0).value<QList<MigrationSettings>>();
    QCOMPARE(list.size(), 1);
    QCOMPARE(list.at(0).connectionId, id);
    QCOMPARE(list.at(0).folder, QStringLiteral("/srv/app/database/migrations"));
    QCOMPARE(list.at(0).table, QStringLiteral("m"));
    QVERIFY(list.at(0).phpPath.isEmpty());
    QVERIFY(list.at(0).projectRoot.isEmpty());
    QVERIFY(list.at(0).database.isEmpty());

    sqlite3 *db = nullptr;
    QCOMPARE(sqlite3_open_v2(path.toUtf8().constData(), &db, SQLITE_OPEN_READONLY, nullptr),
             SQLITE_OK);
    sqlite3_stmt *stmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, nullptr), SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    QCOMPARE(sqlite3_column_int(stmt, 0), 9);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

void TstMigrations::schemaUpgradeFromV6()
{
    QTemporaryDir data;
    const QString path = data.filePath(QStringLiteral("quarry.db"));
    const QUuid a = QUuid::createUuid();
    const QUuid b = QUuid::createUuid();
    const QByteArray sa = a.toString(QUuid::WithoutBraces).toUtf8();
    const QByteArray sb = b.toString(QUuid::WithoutBraces).toUtf8();
    {
        sqlite3 *db = nullptr;
        QCOMPARE(sqlite3_open(path.toUtf8().constData(), &db), SQLITE_OK);
        const QByteArray sql
            = "CREATE TABLE connections(id TEXT PRIMARY KEY NOT NULL,"
              " name TEXT NOT NULL, provider_id TEXT NOT NULL,"
              " values_json TEXT NOT NULL DEFAULT '{}',"
              " color_index INTEGER NOT NULL DEFAULT -1,"
              " production INTEGER NOT NULL DEFAULT 0,"
              " password_mode INTEGER NOT NULL DEFAULT 0,"
              " created_at TEXT NOT NULL, updated_at TEXT NOT NULL,"
              " last_used_at TEXT, record_history INTEGER NOT NULL DEFAULT 1);"
              "CREATE TABLE migration_settings("
              " connection_id TEXT PRIMARY KEY NOT NULL REFERENCES"
              " connections(id) ON DELETE CASCADE,"
              " kind TEXT NOT NULL, folder TEXT NOT NULL,"
              " table_name TEXT NOT NULL DEFAULT '', updated_at TEXT NOT NULL,"
              " php_path TEXT NOT NULL DEFAULT '', project_root TEXT NOT NULL DEFAULT '');"
              "INSERT INTO connections(id, name, provider_id, created_at, updated_at) VALUES('"
            + sa + "','A','mysql','2024-01-01','2024-01-01'),('" + sb
            + "','B','sqlite','2024-01-01','2024-01-01');"
              "INSERT INTO migration_settings VALUES('"
            + sa
            + "','laravel','/a/migrations','m','2024-01-01','/usr/bin/php','/a');"
              "INSERT INTO migration_settings VALUES('"
            + sb
            + "','laravel','/b/migrations','','2024-01-02','','');"
              "PRAGMA user_version=6";
        QCOMPARE(sqlite3_exec(db, sql.constData(), nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(db);
    }

    MigrationService service(path);
    QSignalSpy loaded(&service, &MigrationService::settingsLoaded);
    QSignalSpy failed(&service, &MigrationService::failed);
    service.loadSettings();
    QTRY_COMPARE(loaded.count(), 1);
    QCOMPARE(failed.count(), 0);
    auto list = loaded.at(0).at(0).value<QList<MigrationSettings>>();
    QCOMPARE(list.size(), 2);
    std::sort(list.begin(), list.end(),
              [](const auto &x, const auto &y) { return x.folder < y.folder; });
    QCOMPARE(list.at(0).connectionId, a);
    QCOMPARE(list.at(0).database, QString());
    QCOMPARE(list.at(0).kind, QStringLiteral("laravel"));
    QCOMPARE(list.at(0).folder, QStringLiteral("/a/migrations"));
    QCOMPARE(list.at(0).table, QStringLiteral("m"));
    QCOMPARE(list.at(0).phpPath, QStringLiteral("/usr/bin/php"));
    QCOMPARE(list.at(0).projectRoot, QStringLiteral("/a"));
    QCOMPARE(list.at(1).connectionId, b);
    QCOMPARE(list.at(1).database, QString());
    QCOMPARE(list.at(1).folder, QStringLiteral("/b/migrations"));

    // The foreign key survived the rebuild: deleting a connection deletes its row.
    sqlite3 *db = nullptr;
    QCOMPARE(sqlite3_open(path.toUtf8().constData(), &db), SQLITE_OK);
    QCOMPARE(sqlite3_exec(db, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr), SQLITE_OK);
    const QByteArray del = "DELETE FROM connections WHERE id='" + sa + "'";
    QCOMPARE(sqlite3_exec(db, del.constData(), nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_stmt *stmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(db, "SELECT count(*) FROM migration_settings", -1, &stmt, nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    QCOMPARE(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

void TstMigrations::settingsPerDatabase()
{
    QTemporaryDir data;
    const QString path = data.filePath(QStringLiteral("quarry.db"));
    ConnectionStore connections(path);
    QSignalSpy stored(&connections, &ConnectionStore::saved);
    ConnectionProfile p;
    p.name = QStringLiteral("server");
    p.providerId = QStringLiteral("mysql");
    connections.save(p);
    QTRY_COMPARE(stored.count(), 1);
    const QUuid id = stored.at(0).at(0).value<ConnectionProfile>().id;

    MigrationService service(path);
    QSignalSpy saved(&service, &MigrationService::settingsSaved);
    QSignalSpy loaded(&service, &MigrationService::settingsLoaded);
    QSignalSpy failed(&service, &MigrationService::failed);
    MigrationSettings blog = settingsFor(QStringLiteral("/srv/blog/database/migrations"));
    blog.connectionId = id;
    blog.database = QStringLiteral("blog");
    MigrationSettings shop = settingsFor(QStringLiteral("/srv/shop/database/migrations"));
    shop.connectionId = id;
    shop.database = QStringLiteral("shop");
    QVERIFY(!(blog == shop));
    service.saveSettings(blog);
    service.saveSettings(shop);
    service.loadSettings();
    QTRY_COMPARE(loaded.count(), 1);
    QCOMPARE(saved.count(), 2);
    auto list = loaded.at(0).at(0).value<QList<MigrationSettings>>();
    QCOMPARE(list.size(), 2);
    QVERIFY(list.contains(blog));
    QVERIFY(list.contains(shop));

    // Saving one again replaces only its own row.
    blog.folder = QStringLiteral("/srv/blog2/database/migrations");
    service.saveSettings(blog);
    service.loadSettings();
    QTRY_COMPARE(loaded.count(), 2);
    list = loaded.at(1).at(0).value<QList<MigrationSettings>>();
    QCOMPARE(list.size(), 2);
    QVERIFY(list.contains(blog));
    QVERIFY(list.contains(shop));

    // "None" on one database leaves the other (and the row without database) alone.
    MigrationSettings own = settingsFor(QStringLiteral("/srv/own"));
    own.connectionId = id;
    service.saveSettings(own);
    MigrationSettings none = blog;
    none.kind.clear();
    service.saveSettings(none);
    service.loadSettings();
    QTRY_COMPARE(loaded.count(), 3);
    list = loaded.at(2).at(0).value<QList<MigrationSettings>>();
    QCOMPARE(list.size(), 2);
    QVERIFY(list.contains(shop));
    QVERIFY(list.contains(own));
    QCOMPARE(failed.count(), 0);

    // Deleting the connection removes every row.
    QSignalSpy removed(&connections, &ConnectionStore::removed);
    connections.remove(id);
    QTRY_COMPARE(removed.count(), 1);
    service.loadSettings();
    QTRY_COMPARE(loaded.count(), 4);
    QVERIFY(loaded.at(3).at(0).value<QList<MigrationSettings>>().isEmpty());
}

void TstMigrations::qualifiedQuery()
{
    const FakeDialect mysql(false);
    const FakeDialect mssql(true);
    MigrationSettings s = settingsFor(QStringLiteral("/x"));
    const auto sql
        = [&](const MigrationSettings &settings, const ISqlDialect *d, const QString &provider) {
              return laravel().appliedQuery(settings, d, provider).value().sql;
          };
    const QString plain = QStringLiteral("SELECT migration, batch FROM migrations");

    // No database: the query of before, for every engine.
    QCOMPARE(sql(s, &mysql, QStringLiteral("mysql")), plain);
    QCOMPARE(sql(s, &mssql, QStringLiteral("mssql")), plain);

    s.database = QStringLiteral("blog");
    QCOMPARE(sql(s, &mysql, QStringLiteral("mysql")),
             QStringLiteral("SELECT migration, batch FROM `blog`.migrations"));
    QCOMPARE(sql(s, &mssql, QStringLiteral("mssql")),
             QStringLiteral("SELECT migration, batch FROM [blog]..migrations"));
    // One connection is one database for these: the name is only part of the key.
    QCOMPARE(sql(s, &mysql, QStringLiteral("postgres")), plain);
    QCOMPARE(sql(s, &mysql, QStringLiteral("odbc")), plain);
    QCOMPARE(sql(s, nullptr, QStringLiteral("sqlite")), plain);
    QCOMPARE(sql(s, nullptr, QString()), plain);

    // A table that already names a schema.
    s.table = QStringLiteral("app.mig");
    QCOMPARE(sql(s, &mssql, QStringLiteral("mssql")),
             QStringLiteral("SELECT migration, batch FROM [blog].app.mig"));
    QCOMPARE(sql(s, &mysql, QStringLiteral("mysql")),
             QStringLiteral("SELECT migration, batch FROM app.mig"));

    // Engine that qualifies but no dialect to quote with: refuse rather than read another base.
    s.table.clear();
    QVERIFY(!laravel().appliedQuery(s, nullptr, QStringLiteral("mysql")));
    QVERIFY(!laravel().appliedQuery(s, nullptr, QStringLiteral("mssql")));
    // The table is still validated.
    s.table = QStringLiteral("a\"b");
    QVERIFY(!laravel().appliedQuery(s, &mysql, QStringLiteral("mysql")));
}

void TstMigrations::qualifiedQueryQuoting()
{
    const FakeDialect mysql(false);
    const FakeDialect mssql(true);
    MigrationSettings s = settingsFor(QStringLiteral("/x"));
    s.database = QStringLiteral("we`ird; DROP TABLE x");
    QCOMPARE(laravel().appliedQuery(s, &mysql, QStringLiteral("mysql")).value().sql,
             QStringLiteral("SELECT migration, batch FROM `we``ird; DROP TABLE x`.migrations"));
    s.database = QStringLiteral("we]ird");
    QCOMPARE(laravel().appliedQuery(s, &mssql, QStringLiteral("mssql")).value().sql,
             QStringLiteral("SELECT migration, batch FROM [we]]ird]..migrations"));
}

void TstMigrations::filePathValid()
{
    QTemporaryDir dir;
    const MigrationSettings s = settingsFor(dir.path());
    const QString name = QStringLiteral("2024_01_05_101500_add_avatar_to_users");
    const QString expected = QDir::toNativeSeparators(
        QDir::cleanPath(dir.path() + QLatin1Char('/') + name + QStringLiteral(".php")));
    QCOMPARE(laravel().filePath(s, name), expected);
    QCOMPARE(laravel().highlightingDefinition(), QStringLiteral("PHP"));
}

void TstMigrations::filePathInvalid_data()
{
    QTest::addColumn<QString>("name");
    QTest::newRow("parent") << QStringLiteral("../x");
    QTest::newRow("slash") << QStringLiteral("a/b");
    QTest::newRow("backslash") << QStringLiteral("a\\b");
    QTest::newRow("dots") << QStringLiteral("..");
    QTest::newRow("empty") << QString();
    QTest::newRow("nul") << QString::fromLatin1("a\0b", 3);
    QTest::newRow("drive") << QStringLiteral("C:evil");
    QTest::newRow("control") << QStringLiteral("a\nb");
    QTest::newRow("trailing dot") << QStringLiteral("a.");
}

void TstMigrations::filePathInvalid()
{
    QFETCH(QString, name);
    QTemporaryDir dir;
    QVERIFY(laravel().filePath(settingsFor(dir.path()), name).isEmpty());
    QVERIFY(laravel().filePath(settingsFor(QString()), QStringLiteral("ok")).isEmpty());
}

namespace {

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

} // namespace

void TstMigrations::readSourceText()
{
    QTemporaryDir dir;
    QTemporaryDir data;
    QVERIFY(writeFile(dir.filePath(QStringLiteral("a.php")), "<?php\r\nreturn 1;\r\n// x\rend"));
    MigrationService service(data.filePath(QStringLiteral("quarry.db")));
    QSignalSpy read(&service, &MigrationService::sourceRead);
    QSignalSpy failed(&service, &MigrationService::sourceFailed);
    const MigrationSettings s = settingsFor(dir.path());
    const quint64 id = service.readSource(s, QStringLiteral("a"));
    QVERIFY(id > 0);
    QTRY_COMPARE(read.count(), 1);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(read.at(0).at(0).toULongLong(), id);
    QCOMPARE(read.at(0).at(1).toUuid(), s.connectionId);
    QCOMPARE(read.at(0).at(2).toString(), QString());
    QCOMPARE(read.at(0).at(3).toString(), QStringLiteral("a"));
    QCOMPARE(read.at(0).at(4).toString(), QStringLiteral("<?php\nreturn 1;\n// x\nend"));
}

void TstMigrations::readSourceBomAndUtf8()
{
    QTemporaryDir dir;
    QTemporaryDir data;
    QVERIFY(writeFile(dir.filePath(QStringLiteral("bom.php")), "\xEF\xBB\xBF<?php // caf\xC3\xA9"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("bad.php")),
                      "a\xFF"
                      "b"));
    MigrationService service(data.filePath(QStringLiteral("quarry.db")));
    QSignalSpy read(&service, &MigrationService::sourceRead);
    const MigrationSettings s = settingsFor(dir.path());
    service.readSource(s, QStringLiteral("bom"));
    service.readSource(s, QStringLiteral("bad"));
    QTRY_COMPARE(read.count(), 2);
    QCOMPARE(read.at(0).at(4).toString(), QString::fromUtf8("<?php // caf\xC3\xA9"));
    QCOMPARE(read.at(1).at(4).toString(),
             QString::fromUtf8("a\xEF\xBF\xBD"
                               "b"));
}

void TstMigrations::readSourceSizeLimit()
{
    QTemporaryDir dir;
    QTemporaryDir data;
    const QByteArray exact(static_cast<qsizetype>(MigrationService::kMaxSourceBytes), 'x');
    QVERIFY(writeFile(dir.filePath(QStringLiteral("exact.php")), exact));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("big.php")), exact + "x"));
    MigrationService service(data.filePath(QStringLiteral("quarry.db")));
    QSignalSpy read(&service, &MigrationService::sourceRead);
    QSignalSpy failed(&service, &MigrationService::sourceFailed);
    const MigrationSettings s = settingsFor(dir.path());
    service.readSource(s, QStringLiteral("exact"));
    service.readSource(s, QStringLiteral("big"));
    QTRY_COMPARE(read.count() + failed.count(), 2);
    QCOMPARE(read.count(), 1);
    QCOMPARE(read.at(0).at(4).toString().size(), exact.size());
    QCOMPARE(failed.count(), 1);
    const Error error = failed.at(0).at(4).value<Error>();
    QCOMPARE(failed.at(0).at(3).toString(), QStringLiteral("big"));
    QVERIFY(!error.message.isEmpty());
    QVERIFY(error.detail.contains(QString::number(exact.size() + 1)));
}

void TstMigrations::readSourceMissing()
{
    QTemporaryDir dir;
    QTemporaryDir data;
    MigrationService service(data.filePath(QStringLiteral("quarry.db")));
    QSignalSpy read(&service, &MigrationService::sourceRead);
    QSignalSpy failed(&service, &MigrationService::sourceFailed);
    service.readSource(settingsFor(dir.path()), QStringLiteral("nope"));
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(read.count(), 0);
    QCOMPARE(failed.at(0).at(4).value<Error>().kind, ErrorKind::NotFound);
}

void TstMigrations::readSourceDirectory()
{
    QTemporaryDir dir;
    QTemporaryDir data;
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("x.php")));
    MigrationService service(data.filePath(QStringLiteral("quarry.db")));
    QSignalSpy read(&service, &MigrationService::sourceRead);
    QSignalSpy failed(&service, &MigrationService::sourceFailed);
    service.readSource(settingsFor(dir.path()), QStringLiteral("x"));
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(read.count(), 0);
    QVERIFY(!failed.at(0).at(4).value<Error>().message.isEmpty());
}

void TstMigrations::readSourceInvalidName()
{
    QTemporaryDir dir;
    QTemporaryDir data;
    // A file outside the folder that a traversal would reach.
    QVERIFY(writeFile(data.filePath(QStringLiteral("secret.php")), "<?php"));
    MigrationService service(data.filePath(QStringLiteral("quarry.db")));
    QSignalSpy read(&service, &MigrationService::sourceRead);
    QSignalSpy failed(&service, &MigrationService::sourceFailed);
    const QString evil
        = QStringLiteral("../") + QFileInfo(data.path()).fileName() + QStringLiteral("/secret");
    service.readSource(settingsFor(dir.path()), evil);
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(read.count(), 0);
    QCOMPARE(failed.at(0).at(4).value<Error>().message,
             QCoreApplication::translate("quarry::MigrationServiceWorker",
                                         "That is not a valid migration name."));
}

void TstMigrations::readSourceCancelled()
{
    QTemporaryDir dir;
    QTemporaryDir data;
    QVERIFY(writeFile(dir.filePath(QStringLiteral("a.php")), "<?php"));
    MigrationService service(data.filePath(QStringLiteral("quarry.db")));
    QSignalSpy read(&service, &MigrationService::sourceRead);
    QSignalSpy failed(&service, &MigrationService::sourceFailed);
    const quint64 id = service.readSource(settingsFor(dir.path()), QStringLiteral("a"));
    service.cancel(id);
    QTest::qWait(300);
    QCOMPARE(read.count() + failed.count(), 0);
}

void TstMigrations::readSourceTwice()
{
    QTemporaryDir dir;
    QTemporaryDir data;
    QVERIFY(writeFile(dir.filePath(QStringLiteral("a.php")), "one"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("b.php")), "two"));
    MigrationService service(data.filePath(QStringLiteral("quarry.db")));
    QSignalSpy read(&service, &MigrationService::sourceRead);
    const MigrationSettings s = settingsFor(dir.path());
    const quint64 first = service.readSource(s, QStringLiteral("a"));
    const quint64 second = service.readSource(s, QStringLiteral("b"));
    QVERIFY(first != second);
    QTRY_COMPARE(read.count(), 2);
    QCOMPARE(read.at(0).at(0).toULongLong(), first);
    QCOMPARE(read.at(0).at(4).toString(), QStringLiteral("one"));
    QCOMPARE(read.at(1).at(0).toULongLong(), second);
    QCOMPARE(read.at(1).at(4).toString(), QStringLiteral("two"));
}

void TstMigrations::remoteValidationAndPosixFilePath()
{
    const IMigrationSource *laravel = findMigrationSource(QStringLiteral("laravel"));
    QVERIFY(laravel != nullptr);

    MigrationSettings s;
    s.kind = QStringLiteral("laravel");
    s.remote = true;
    s.folder = QStringLiteral("/var/www/app/database/migrations");
    QVERIFY(!laravel->validate(s).has_value());
    QCOMPARE(laravel->filePath(s, QStringLiteral("2024_01_01_000000_create_users_table")),
             QStringLiteral("/var/www/app/database/migrations/"
                            "2024_01_01_000000_create_users_table.php"));
    QVERIFY(laravel->filePath(s, QStringLiteral("../secret")).isEmpty());

    s.folder = QStringLiteral("database/migrations");
    QVERIFY(laravel->validate(s).has_value());
}

void TstMigrations::remoteSettingsRoundTrip()
{
    QTemporaryDir data;
    const QString dbPath = data.filePath(QStringLiteral("quarry.db"));
    ConnectionStore store(dbPath);
    ConnectionProfile profile;
    profile.name = QStringLiteral("Remote DB");
    profile.providerId = QStringLiteral("sqlite");
    profile.values = { { QStringLiteral("file"), data.filePath(QStringLiteral("sample.db")) } };
    QSignalSpy savedConn(&store, &ConnectionStore::saved);
    store.save(profile);
    QTRY_COMPARE(savedConn.count(), 1);
    const QUuid id = savedConn.at(0).at(0).value<ConnectionProfile>().id;

    MigrationService service(dbPath);
    MigrationSettings s;
    s.connectionId = id;
    s.kind = QStringLiteral("laravel");
    s.folder = QStringLiteral("/srv/app/database/migrations");
    s.remote = true;
    QSignalSpy saved(&service, &MigrationService::settingsSaved);
    service.saveSettings(s);
    QTRY_COMPARE(saved.count(), 1);

    QSignalSpy loaded(&service, &MigrationService::settingsLoaded);
    service.loadSettings();
    QTRY_COMPARE(loaded.count(), 1);
    const auto list = loaded.at(0).at(0).value<QList<MigrationSettings>>();
    QCOMPARE(list.size(), 1);
    QCOMPARE(list.at(0), s);
    QVERIFY(list.at(0).remote);
}

void TstMigrations::remoteCheckInspectAndReadSource()
{
    QTemporaryDir data;
    auto session = openSession();
    QVERIFY(session != nullptr);
    QVERIFY(run(session.get(),
                QStringLiteral("CREATE TABLE migrations(id INTEGER PRIMARY KEY, migration TEXT, "
                               "batch INTEGER);")));
    QVERIFY(run(session.get(),
                QStringLiteral("INSERT INTO migrations(migration, batch) VALUES "
                               "('2024_01_01_000000_init', 1), "
                               "('2023_01_01_000000_orphan', 1);")));

    MigrationService service(data.filePath(QStringLiteral("quarry.db")));
    QString lastListedPath;
    QString lastReadPath;
    service.setRemoteBackend(RemoteMigrationBackend {
        [&](const RemoteMigrationContext &, const QString &path,
            const CancellationToken &) -> Result<QStringList> {
            lastListedPath = path;
            return QStringList { QStringLiteral("2024_01_01_000000_init.php"),
                                 QStringLiteral("2024_02_01_000000_add_posts.php"),
                                 QStringLiteral("README.md") };
        },
        [&](const RemoteMigrationContext &, const QString &path, qint64,
            const CancellationToken &) -> Result<QByteArray> {
            lastReadPath = path;
            return QByteArray("\xEF\xBB\xBF<?php\r\n// remote migration\r\n");
        },
    });

    MigrationSettings s;
    s.connectionId = QUuid::createUuid();
    s.kind = QStringLiteral("laravel");
    s.folder = QStringLiteral("/var/www/app/database/migrations");
    s.remote = true;

    RemoteMigrationContext ctx;
    ctx.tunnel.enabled = true;
    ctx.tunnel.host = QStringLiteral("ssh.example.test");
    ctx.tunnel.user = QStringLiteral("deploy");
    ctx.knownHosts = std::make_shared<KnownHostsStore>(data.filePath(QStringLiteral("hosts")));

    // 1) Missing SSH context fails cleanly.
    QSignalSpy checkFailed(&service, &MigrationService::folderCheckFailed);
    service.checkFolder(s);
    QTRY_COMPARE(checkFailed.count(), 1);
    QCOMPARE(checkFailed.at(0).at(1).value<Error>().kind, ErrorKind::Connection);

    // 2) Remote checkFolder filters to .php files.
    QSignalSpy checked(&service, &MigrationService::folderChecked);
    service.checkFolder(s, ctx);
    QTRY_COMPARE(checked.count(), 1);
    QCOMPARE(checked.at(0).at(1).toInt(), 2);
    QCOMPARE(lastListedPath, s.folder);

    // 3) Remote inspect crosses remote files with the DB's migrations table.
    QSignalSpy inspected(&service, &MigrationService::inspected);
    service.inspect(session.get(), s, nullptr, {}, ctx);
    QTRY_COMPARE(inspected.count(), 1);
    const auto report = inspected.at(0).at(1).value<MigrationReport>();
    QCOMPARE(report.files, 2);
    QCOMPARE(report.applied, 1);
    QCOMPARE(report.pending, 1);
    QCOMPARE(report.orphans, 1);

    // 4) Remote readSource fetches and normalizes the remote PHP file.
    QSignalSpy sourceRead(&service, &MigrationService::sourceRead);
    service.readSource(s, QStringLiteral("2024_02_01_000000_add_posts"), ctx);
    QTRY_COMPARE(sourceRead.count(), 1);
    QCOMPARE(lastReadPath,
             QStringLiteral("/var/www/app/database/migrations/2024_02_01_000000_add_posts.php"));
    QCOMPARE(sourceRead.at(0).at(4).toString(), QStringLiteral("<?php\n// remote migration\n"));
}

QTEST_GUILESS_MAIN(TstMigrations)
#include "tst_migrations.moc"
