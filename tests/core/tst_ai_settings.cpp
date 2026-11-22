#include <quarry/ai_settings.h>
#include <quarry/connection_store.h>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <sqlite3.h>

using namespace quarry;

namespace {

// First column of the first row of `sql`, or -1.
qint64 scalar(const QString &path, const char *sql)
{
    sqlite3 *db = nullptr;
    qint64 value = -1;
    if (sqlite3_open_v2(path.toUtf8().constData(), &db, SQLITE_OPEN_READONLY, nullptr)
        == SQLITE_OK) {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK
            && sqlite3_step(stmt) == SQLITE_ROW)
            value = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return value;
}

AiConnectionSettings everythingChanged(const QUuid &id)
{
    AiConnectionSettings s;
    s.connectionId = id;
    s.enabled = true;
    s.excludedSchemas
        = { QStringLiteral("audit"), QStringLiteral("sch,ema"), QStringLiteral("ñandú") };
    s.excludedTables = { QStringLiteral("users"), QStringLiteral("public.secret\"quote"),
                         QStringLiteral("two\nlines") };
    s.sendComments = true;
    s.sendDefaults = true;
    s.sendChecks = true;
    s.sendEnumValues = true;
    s.sendDefinitions = true;
    s.maskLiterals = false;
    s.maskNumbers = true;
    s.maxTables = 75;
    return s;
}

} // namespace

class TstAiSettings : public QObject
{
    Q_OBJECT

private:
    // A connection in quarry.db; returns its id.
    static QUuid addConnection(ConnectionStore &store, const QString &name)
    {
        QSignalSpy stored(&store, &ConnectionStore::saved);
        ConnectionProfile p;
        p.name = name;
        p.providerId = QStringLiteral("sqlite");
        store.save(p);
        [&] { QTRY_COMPARE_WITH_TIMEOUT(stored.count(), 1, 10000); }();
        return stored.isEmpty() ? QUuid() : stored.at(0).at(0).value<ConnectionProfile>().id;
    }

    static QList<AiConnectionSettings> loadAll(AiSettingsService &service)
    {
        QSignalSpy loaded(&service, &AiSettingsService::loaded);
        service.loadAll();
        [&] { QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000); }();
        return loaded.isEmpty() ? QList<AiConnectionSettings>()
                                : loaded.at(0).at(0).value<QList<AiConnectionSettings>>();
    }

private slots:
    void defaults()
    {
        const AiConnectionSettings d;
        QVERIFY(d.isDefault());
        QVERIFY(!d.enabled);
        QVERIFY(d.excludedSchemas.isEmpty() && d.excludedTables.isEmpty());
        QVERIFY(!d.sendComments && !d.sendDefaults && !d.sendChecks);
        QVERIFY(!d.sendEnumValues && !d.sendDefinitions);
        QVERIFY(d.maskLiterals);
        QVERIFY(!d.maskNumbers);
        QCOMPARE(d.maxTables, 40);

        AiConnectionSettings withId = d;
        withId.connectionId = QUuid::createUuid();
        QVERIFY(withId.isDefault()); // the id is not a setting

        const QList<AiConnectionSettings> changes = [&] {
            QList<AiConnectionSettings> list;
            for (int i = 0; i < 9; ++i)
                list.append(d);
            list[0].enabled = true;
            list[1].excludedSchemas = { QStringLiteral("a") };
            list[2].excludedTables = { QStringLiteral("a") };
            list[3].sendComments = true;
            list[4].maskLiterals = false;
            list[5].maskNumbers = true;
            list[6].maxTables = 41;
            list[7].sendDefinitions = true;
            list[8].sendChecks = true;
            return list;
        }();
        for (const AiConnectionSettings &c : changes)
            QVERIFY(!c.isDefault());
    }

    void normalization()
    {
        AiConnectionSettings s;
        s.excludedSchemas
            = { QStringLiteral("  Audit "), QStringLiteral("audit"), QStringLiteral(""),
                QStringLiteral("   "),      QStringLiteral("AUDIT"), QStringLiteral("Sales") };
        s.excludedTables = { QStringLiteral("Users"), QStringLiteral(" users"),
                             QStringLiteral("public.Users"), QStringLiteral("PUBLIC.USERS") };
        s.maxTables = 0;
        AiConnectionSettings n = AiSettingsService::normalized(s);
        QCOMPARE(n.excludedSchemas,
                 QStringList({ QStringLiteral("Audit"), QStringLiteral("Sales") }));
        QCOMPARE(n.excludedTables,
                 QStringList({ QStringLiteral("Users"), QStringLiteral("public.Users") }));
        QCOMPARE(n.maxTables, 1);

        s.maxTables = -5;
        QCOMPARE(AiSettingsService::normalized(s).maxTables, 1);
        s.maxTables = 201;
        QCOMPARE(AiSettingsService::normalized(s).maxTables, 200);
        s.maxTables = 200;
        QCOMPARE(AiSettingsService::normalized(s).maxTables, 200);
        s.maxTables = 1;
        QCOMPARE(AiSettingsService::normalized(s).maxTables, 1);

        // Idempotent, and it does not touch the rest.
        n = AiSettingsService::normalized(everythingChanged(QUuid::createUuid()));
        QCOMPARE(AiSettingsService::normalized(n), n);
        QCOMPARE(n.maxTables, 75);
        QVERIFY(n.enabled && n.maskNumbers && !n.maskLiterals);
    }

    void emptyDatabaseLoadsNothing()
    {
        QTemporaryDir dir;
        AiSettingsService service(dir.filePath(QStringLiteral("quarry.db")));
        QVERIFY(loadAll(service).isEmpty());
    }

    void roundTripOfEveryField()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("quarry.db"));
        ConnectionStore connections(path);
        const QUuid a = addConnection(connections, QStringLiteral("a"));
        const QUuid b = addConnection(connections, QStringLiteral("b"));
        QVERIFY(!a.isNull() && !b.isNull());

        AiSettingsService service(path);
        QSignalSpy saved(&service, &AiSettingsService::saved);
        QSignalSpy failed(&service, &AiSettingsService::failed);
        const AiConnectionSettings full = everythingChanged(a);
        AiConnectionSettings other;
        other.connectionId = b;
        other.enabled = true; // only that: the rest stays at the defaults
        service.save(full);
        service.save(other);
        QTRY_COMPARE(saved.count(), 2);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(saved.at(0).at(0).value<AiConnectionSettings>(), full);
        QCOMPARE(scalar(path, "SELECT COUNT(*) FROM ai_settings"), 2);

        // A new service on the same file: it is on disk, not in memory.
        AiSettingsService again(path);
        const QList<AiConnectionSettings> list = loadAll(again);
        QCOMPARE(list.size(), 2);
        for (const AiConnectionSettings &item : list) {
            if (item.connectionId == a)
                QCOMPARE(item, full);
            else
                QCOMPARE(item, other);
        }
    }

    void saveStoresTheNormalizedValue()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("quarry.db"));
        ConnectionStore connections(path);
        const QUuid id = addConnection(connections, QStringLiteral("a"));
        AiSettingsService service(path);
        QSignalSpy saved(&service, &AiSettingsService::saved);

        AiConnectionSettings s;
        s.connectionId = id;
        s.enabled = true;
        s.excludedSchemas = { QStringLiteral(" x "), QStringLiteral("X"), QString() };
        s.maxTables = 9999;
        service.save(s);
        QTRY_COMPARE(saved.count(), 1);
        const auto stored = saved.at(0).at(0).value<AiConnectionSettings>();
        QCOMPARE(stored.excludedSchemas, QStringList { QStringLiteral("x") });
        QCOMPARE(stored.maxTables, 200);
        const QList<AiConnectionSettings> list = loadAll(service);
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.at(0), stored);
        QCOMPARE(scalar(path, "SELECT max_tables FROM ai_settings"), 200);
    }

    void savingTheDefaultsDeletesTheRow()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("quarry.db"));
        ConnectionStore connections(path);
        const QUuid id = addConnection(connections, QStringLiteral("a"));
        AiSettingsService service(path);
        QSignalSpy saved(&service, &AiSettingsService::saved);
        QSignalSpy failed(&service, &AiSettingsService::failed);

        service.save(everythingChanged(id));
        QTRY_COMPARE(saved.count(), 1);
        QCOMPARE(scalar(path, "SELECT COUNT(*) FROM ai_settings"), 1);

        AiConnectionSettings defaults;
        defaults.connectionId = id;
        service.save(defaults);
        QTRY_COMPARE(saved.count(), 2);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(scalar(path, "SELECT COUNT(*) FROM ai_settings"), 0);
        QVERIFY(loadAll(service).isEmpty());

        // Lists that normalize to nothing are the defaults too: no row.
        defaults.excludedTables = { QStringLiteral("  "), QString() };
        service.save(defaults);
        QTRY_COMPARE(saved.count(), 3);
        QCOMPARE(scalar(path, "SELECT COUNT(*) FROM ai_settings"), 0);
    }

    void unknownConnectionIsRefused()
    {
        QTemporaryDir dir;
        AiSettingsService service(dir.filePath(QStringLiteral("quarry.db")));
        QSignalSpy saved(&service, &AiSettingsService::saved);
        QSignalSpy failed(&service, &AiSettingsService::failed);

        service.save(everythingChanged(QUuid::createUuid()));
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).value<Error>().kind, ErrorKind::NotFound);
        QVERIFY(!failed.at(0).at(0).value<Error>().message.isEmpty());

        AiConnectionSettings defaults; // deleting the row of a connection that is not there
        defaults.connectionId = QUuid::createUuid();
        service.save(defaults);
        QTRY_COMPARE(failed.count(), 2);
        QCOMPARE(failed.at(1).at(0).value<Error>().kind, ErrorKind::NotFound);

        service.save(everythingChanged(QUuid())); // no id at all
        QTRY_COMPARE(failed.count(), 3);
        QCOMPARE(failed.at(2).at(0).value<Error>().kind, ErrorKind::Constraint);
        QCOMPARE(saved.count(), 0);
    }

    void deletingTheConnectionDeletesItsSettings()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("quarry.db"));
        ConnectionStore connections(path);
        const QUuid a = addConnection(connections, QStringLiteral("a"));
        const QUuid b = addConnection(connections, QStringLiteral("b"));
        AiSettingsService service(path);
        QSignalSpy saved(&service, &AiSettingsService::saved);
        service.save(everythingChanged(a));
        service.save(everythingChanged(b));
        QTRY_COMPARE(saved.count(), 2);

        QSignalSpy removed(&connections, &ConnectionStore::removed);
        connections.remove(a);
        QTRY_COMPARE(removed.count(), 1);
        const QList<AiConnectionSettings> list = loadAll(service);
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.at(0).connectionId, b);
    }

    void upgradesFromVersion7KeepingEverything()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("quarry.db"));
        const QUuid id = QUuid::createUuid();
        const QByteArray sid = id.toString(QUuid::WithoutBraces).toUtf8();
        {
            sqlite3 *db = nullptr;
            QCOMPARE(sqlite3_open(path.toUtf8().constData(), &db), SQLITE_OK);
            // The schema of version 7, as the earlier migrations leave it (what the AI needs of it:
            // connections, plus migration_settings and its rows to check they survive).
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
                  " connection_id TEXT NOT NULL REFERENCES connections(id) ON DELETE CASCADE,"
                  " database TEXT NOT NULL DEFAULT '', kind TEXT NOT NULL,"
                  " folder TEXT NOT NULL, table_name TEXT NOT NULL DEFAULT '',"
                  " updated_at TEXT NOT NULL, php_path TEXT NOT NULL DEFAULT '',"
                  " project_root TEXT NOT NULL DEFAULT '',"
                  " PRIMARY KEY(connection_id, database));"
                  "INSERT INTO connections(id, name, provider_id, created_at, updated_at)"
                  " VALUES('"
                + sid
                + "','Old','sqlite','2024-01-01','2024-01-01');"
                  "INSERT INTO migration_settings(connection_id, kind, folder, updated_at)"
                  " VALUES('"
                + sid
                + "','laravel','/srv/app/database/migrations','2024-01-01');"
                  "PRAGMA user_version=7";
            QCOMPARE(sqlite3_exec(db, sql.constData(), nullptr, nullptr, nullptr), SQLITE_OK);
            sqlite3_close(db);
        }
        QCOMPARE(scalar(path, "PRAGMA user_version"), 7);

        AiSettingsService service(path);
        QVERIFY(loadAll(service).isEmpty()); // the connection has no row: the defaults
        QCOMPARE(scalar(path, "PRAGMA user_version"), 8);
        QCOMPARE(scalar(path, "SELECT COUNT(*) FROM sqlite_master WHERE name='ai_settings'"), 1);
        QCOMPARE(scalar(path, "SELECT COUNT(*) FROM connections"), 1);
        QCOMPARE(scalar(path, "SELECT COUNT(*) FROM migration_settings"), 1);

        // The existing connection takes settings now.
        QSignalSpy saved(&service, &AiSettingsService::saved);
        service.save(everythingChanged(id));
        QTRY_COMPARE(saved.count(), 1);
        const QList<AiConnectionSettings> list = loadAll(service);
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.at(0), everythingChanged(id));
        QCOMPARE(scalar(path, "SELECT COUNT(*) FROM migration_settings"), 1);
    }
};

QTEST_GUILESS_MAIN(TstAiSettings)
#include "tst_ai_settings.moc"
