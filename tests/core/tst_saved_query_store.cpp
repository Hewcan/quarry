#include <quarry/connection_store.h>
#include <quarry/saved_query_store.h>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <sqlite3.h>

using namespace quarry;

namespace {

SavedQuery makeQuery(const QString &name, int position = 0,
                     const std::optional<QUuid> &folderId = std::nullopt)
{
    SavedQuery q;
    q.name = name;
    q.sql = QStringLiteral("SELECT '%1' AS é;").arg(name);
    q.position = position;
    q.folderId = folderId;
    return q;
}

SavedFolder makeFolder(const QString &name, int position = 0)
{
    SavedFolder f;
    f.name = name;
    f.position = position;
    return f;
}

bool saveFolder(SavedQueryStore &store, QSignalSpy &saved, const SavedFolder &folder,
                SavedFolder &out)
{
    const qsizetype before = saved.count();
    store.saveFolder(folder);
    if (!QTest::qWaitFor([&] { return saved.count() > before; }))
        return false;
    out = saved.at(before).at(0).value<SavedFolder>();
    return true;
}

bool saveQuery(SavedQueryStore &store, QSignalSpy &saved, const SavedQuery &query, SavedQuery &out)
{
    const qsizetype before = saved.count();
    store.saveQuery(query);
    if (!QTest::qWaitFor([&] { return saved.count() > before; }))
        return false;
    out = saved.at(before).at(0).value<SavedQuery>();
    return true;
}

QStringList names(const QList<SavedQuery> &queries)
{
    QStringList list;
    list.reserve(queries.size());
    for (const SavedQuery &q : queries)
        list.append(q.name);
    return list;
}

qint64 scalar(const QString &path, const QByteArray &sql)
{
    sqlite3 *db = nullptr;
    qint64 value = -1;
    if (sqlite3_open_v2(path.toUtf8().constData(), &db, SQLITE_OPEN_READONLY, nullptr)
        == SQLITE_OK) {
        sqlite3_busy_timeout(db, 3000);
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.constData(), -1, &stmt, nullptr) == SQLITE_OK
            && sqlite3_step(stmt) == SQLITE_ROW)
            value = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return value;
}

bool execRaw(const QString &path, const QByteArray &sql)
{
    sqlite3 *db = nullptr;
    bool ok = sqlite3_open_v2(path.toUtf8().constData(), &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr)
        == SQLITE_OK;
    if (ok)
        ok = sqlite3_exec(db, sql.constData(), nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(db);
    return ok;
}

} // namespace

class TstSavedQueryStore : public QObject
{
    Q_OBJECT

private slots:
    void folderSaveAssignsIdAndUpdatesById();
    void emptyFolderNameFails();
    void queryRoundtripAndTimestamps();
    void updateKeepsCreatedAtAndIgnoresInputTimestamps();
    void emptyQueryNameFails();
    void unknownFolderOrConnectionFails();
    void loadOrdering();
    void removeFolderMovesQueriesToTopLevel();
    void removeUnknownFolderAndQuery();
    void deletingConnectionClearsQueryConnection();
    void upgradesVersion2Database();
    void futureVersionIsRefused();
    void destructorFlushesQueuedSaves();
};

void TstSavedQueryStore::folderSaveAssignsIdAndUpdatesById()
{
    QTemporaryDir dir;
    SavedQueryStore store(dir.filePath(QStringLiteral("quarry.db")));
    QSignalSpy saved(&store, &SavedQueryStore::folderSaved);
    QSignalSpy loaded(&store, &SavedQueryStore::loaded);
    SavedFolder first;
    QVERIFY(saveFolder(store, saved, makeFolder(QStringLiteral("Reports"), 3), first));
    QVERIFY(!first.id.isNull());
    QCOMPARE(first.position, 3);

    auto renamed = first;
    renamed.name = QStringLiteral("Monthly");
    SavedFolder second;
    QVERIFY(saveFolder(store, saved, renamed, second));
    QCOMPARE(second.id, first.id);

    store.load();
    QTRY_COMPARE(loaded.count(), 1);
    const auto folders = loaded.at(0).at(0).value<QList<SavedFolder>>();
    QCOMPARE(folders.size(), 1);
    QCOMPARE(folders.at(0).name, QStringLiteral("Monthly"));
    QCOMPARE(folders.at(0).id, first.id);
}

void TstSavedQueryStore::emptyFolderNameFails()
{
    QTemporaryDir dir;
    SavedQueryStore store(dir.filePath(QStringLiteral("quarry.db")));
    QSignalSpy saved(&store, &SavedQueryStore::folderSaved);
    QSignalSpy failed(&store, &SavedQueryStore::failed);
    store.saveFolder(makeFolder(QStringLiteral("   ")));
    QTRY_COMPARE(failed.count(), 1);
    const auto error = failed.at(0).at(0).value<Error>();
    QCOMPARE(error.kind, ErrorKind::Constraint);
    QVERIFY(!error.message.isEmpty());
    QCOMPARE(saved.count(), 0);
}

void TstSavedQueryStore::queryRoundtripAndTimestamps()
{
    QTemporaryDir dir;
    SavedQueryStore store(dir.filePath(QStringLiteral("quarry.db")));
    QSignalSpy saved(&store, &SavedQueryStore::querySaved);
    QSignalSpy loaded(&store, &SavedQueryStore::loaded);
    const QDateTime before = QDateTime::currentDateTimeUtc().addMSecs(-5);
    SavedQuery q;
    QVERIFY(saveQuery(store, saved, makeQuery(QStringLiteral("Daily"), 2), q));
    QVERIFY(!q.id.isNull());
    QVERIFY(!q.folderId.has_value());
    QVERIFY(!q.connectionId.has_value());
    QCOMPARE(q.createdAt.timeSpec(), Qt::UTC);
    QVERIFY(q.createdAt >= before);
    QCOMPARE(q.createdAt, q.updatedAt);

    store.load();
    QTRY_COMPARE(loaded.count(), 1);
    const auto queries = loaded.at(0).at(1).value<QList<SavedQuery>>();
    QCOMPARE(queries.size(), 1);
    QCOMPARE(queries.at(0).id, q.id);
    QCOMPARE(queries.at(0).name, QStringLiteral("Daily"));
    QCOMPARE(queries.at(0).sql, q.sql);
    QCOMPARE(queries.at(0).position, 2);
    QCOMPARE(queries.at(0).createdAt, q.createdAt);
}

void TstSavedQueryStore::updateKeepsCreatedAtAndIgnoresInputTimestamps()
{
    QTemporaryDir dir;
    SavedQueryStore store(dir.filePath(QStringLiteral("quarry.db")));
    QSignalSpy saved(&store, &SavedQueryStore::querySaved);
    SavedQuery first;
    QVERIFY(saveQuery(store, saved, makeQuery(QStringLiteral("Q")), first));
    QTest::qWait(20);

    auto edited = first;
    edited.name = QStringLiteral("Q2");
    edited.sql = QStringLiteral("SELECT 2");
    edited.createdAt = QDateTime(QDate(1999, 1, 1), QTime(0, 0), QTimeZone::UTC);
    edited.updatedAt = edited.createdAt;
    SavedQuery second;
    QVERIFY(saveQuery(store, saved, edited, second));
    QCOMPARE(second.id, first.id);
    QCOMPARE(second.name, QStringLiteral("Q2"));
    QCOMPARE(second.sql, QStringLiteral("SELECT 2"));
    QCOMPARE(second.createdAt, first.createdAt);
    QVERIFY(second.updatedAt > first.updatedAt);
}

void TstSavedQueryStore::emptyQueryNameFails()
{
    QTemporaryDir dir;
    SavedQueryStore store(dir.filePath(QStringLiteral("quarry.db")));
    QSignalSpy saved(&store, &SavedQueryStore::querySaved);
    QSignalSpy failed(&store, &SavedQueryStore::failed);
    store.saveQuery(makeQuery(QString()));
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(failed.at(0).at(0).value<Error>().kind, ErrorKind::Constraint);
    QCOMPARE(saved.count(), 0);
}

void TstSavedQueryStore::unknownFolderOrConnectionFails()
{
    QTemporaryDir dir;
    SavedQueryStore store(dir.filePath(QStringLiteral("quarry.db")));
    QSignalSpy saved(&store, &SavedQueryStore::querySaved);
    QSignalSpy failed(&store, &SavedQueryStore::failed);
    QSignalSpy loaded(&store, &SavedQueryStore::loaded);

    store.saveQuery(makeQuery(QStringLiteral("A"), 0, QUuid::createUuid()));
    QTRY_COMPARE(failed.count(), 1);
    auto q = makeQuery(QStringLiteral("B"));
    q.connectionId = QUuid::createUuid();
    store.saveQuery(q);
    QTRY_COMPARE(failed.count(), 2);
    for (int i = 0; i < 2; ++i) {
        const auto error = failed.at(i).at(0).value<Error>();
        QCOMPARE(error.kind, ErrorKind::Constraint);
        QVERIFY(!error.message.isEmpty());
        QVERIFY(!error.detail.isEmpty());
    }
    QCOMPARE(saved.count(), 0);
    store.load();
    QTRY_COMPARE(loaded.count(), 1);
    QVERIFY(loaded.at(0).at(1).value<QList<SavedQuery>>().isEmpty());
}

void TstSavedQueryStore::loadOrdering()
{
    QTemporaryDir dir;
    SavedQueryStore store(dir.filePath(QStringLiteral("quarry.db")));
    QSignalSpy folderSaved(&store, &SavedQueryStore::folderSaved);
    QSignalSpy saved(&store, &SavedQueryStore::querySaved);
    QSignalSpy loaded(&store, &SavedQueryStore::loaded);
    SavedFolder zeta, alpha, beta;
    QVERIFY(saveFolder(store, folderSaved, makeFolder(QStringLiteral("zeta"), 0), zeta));
    QVERIFY(saveFolder(store, folderSaved, makeFolder(QStringLiteral("beta"), 1), beta));
    QVERIFY(saveFolder(store, folderSaved, makeFolder(QStringLiteral("Alpha"), 1), alpha));

    SavedQuery ignored;
    QVERIFY(saveQuery(store, saved, makeQuery(QStringLiteral("in beta"), 0, beta.id), ignored));
    QVERIFY(saveQuery(store, saved, makeQuery(QStringLiteral("top b"), 1), ignored));
    QVERIFY(saveQuery(store, saved, makeQuery(QStringLiteral("in zeta 2"), 2, zeta.id), ignored));
    QVERIFY(saveQuery(store, saved, makeQuery(QStringLiteral("top a"), 1), ignored));
    QVERIFY(saveQuery(store, saved, makeQuery(QStringLiteral("in zeta 1"), 1, zeta.id), ignored));
    QVERIFY(saveQuery(store, saved, makeQuery(QStringLiteral("top first"), 0), ignored));
    QVERIFY(saveQuery(store, saved, makeQuery(QStringLiteral("in alpha"), 0, alpha.id), ignored));

    store.load();
    QTRY_COMPARE(loaded.count(), 1);
    const auto folders = loaded.at(0).at(0).value<QList<SavedFolder>>();
    QCOMPARE(folders.size(), 3);
    QCOMPARE(folders.at(0).name, QStringLiteral("zeta")); // position 0
    QCOMPARE(folders.at(1).name, QStringLiteral("Alpha")); // then by name
    QCOMPARE(folders.at(2).name, QStringLiteral("beta"));
    const auto queries = loaded.at(0).at(1).value<QList<SavedQuery>>();
    QCOMPARE(
        names(queries),
        QStringList({ QStringLiteral("top first"), QStringLiteral("top a"), QStringLiteral("top b"),
                      QStringLiteral("in zeta 1"), QStringLiteral("in zeta 2"),
                      QStringLiteral("in alpha"), QStringLiteral("in beta") }));
}

void TstSavedQueryStore::removeFolderMovesQueriesToTopLevel()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("quarry.db"));
    SavedQueryStore store(path);
    QSignalSpy folderSaved(&store, &SavedQueryStore::folderSaved);
    QSignalSpy saved(&store, &SavedQueryStore::querySaved);
    QSignalSpy folderRemoved(&store, &SavedQueryStore::folderRemoved);
    QSignalSpy loaded(&store, &SavedQueryStore::loaded);
    QSignalSpy failed(&store, &SavedQueryStore::failed);
    SavedFolder doomed, other;
    QVERIFY(saveFolder(store, folderSaved, makeFolder(QStringLiteral("Doomed")), doomed));
    QVERIFY(saveFolder(store, folderSaved, makeFolder(QStringLiteral("Other"), 1), other));
    SavedQuery q1, q2, q3;
    QVERIFY(saveQuery(store, saved, makeQuery(QStringLiteral("b"), 2, doomed.id), q1));
    QVERIFY(saveQuery(store, saved, makeQuery(QStringLiteral("a"), 1, doomed.id), q2));
    QVERIFY(saveQuery(store, saved, makeQuery(QStringLiteral("stays"), 0, other.id), q3));

    store.removeFolder(doomed.id);
    QTRY_COMPARE(folderRemoved.count(), 1);
    QCOMPARE(folderRemoved.at(0).at(0).toUuid(), doomed.id);
    const auto moved = folderRemoved.at(0).at(1).value<QList<SavedQuery>>();
    QCOMPARE(names(moved), QStringList({ QStringLiteral("a"), QStringLiteral("b") }));
    for (const SavedQuery &q : moved)
        QVERIFY(!q.folderId.has_value()); // as they now are stored
    QCOMPARE(moved.at(0).id, q2.id);
    QCOMPARE(moved.at(0).position, 1);
    QCOMPARE(moved.at(0).sql, q2.sql);

    store.load();
    QTRY_COMPARE(loaded.count(), 1);
    const auto folders = loaded.at(0).at(0).value<QList<SavedFolder>>();
    QCOMPARE(folders.size(), 1);
    QCOMPARE(folders.at(0).id, other.id);
    const auto queries = loaded.at(0).at(1).value<QList<SavedQuery>>();
    QCOMPARE(queries.size(), 3); // nothing was deleted
    QCOMPARE(scalar(path, "SELECT count(*) FROM saved_queries WHERE folder_id IS NULL"), 2);
    QCOMPARE(failed.count(), 0);
}

void TstSavedQueryStore::removeUnknownFolderAndQuery()
{
    QTemporaryDir dir;
    SavedQueryStore store(dir.filePath(QStringLiteral("quarry.db")));
    QSignalSpy folderRemoved(&store, &SavedQueryStore::folderRemoved);
    QSignalSpy queryRemoved(&store, &SavedQueryStore::queryRemoved);
    QSignalSpy saved(&store, &SavedQueryStore::querySaved);
    QSignalSpy loaded(&store, &SavedQueryStore::loaded);
    QSignalSpy failed(&store, &SavedQueryStore::failed);

    store.removeFolder(QUuid::createUuid());
    QTRY_COMPARE(folderRemoved.count(), 1);
    QVERIFY(folderRemoved.at(0).at(1).value<QList<SavedQuery>>().isEmpty());

    SavedQuery q;
    QVERIFY(saveQuery(store, saved, makeQuery(QStringLiteral("x")), q));
    store.removeQuery(q.id);
    QTRY_COMPARE(queryRemoved.count(), 1);
    QCOMPARE(queryRemoved.at(0).at(0).toUuid(), q.id);
    store.removeQuery(QUuid::createUuid());
    QTRY_COMPARE(queryRemoved.count(), 2);
    store.load();
    QTRY_COMPARE(loaded.count(), 1);
    QVERIFY(loaded.at(0).at(1).value<QList<SavedQuery>>().isEmpty());
    QCOMPARE(failed.count(), 0);
}

void TstSavedQueryStore::deletingConnectionClearsQueryConnection()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("quarry.db"));
    ConnectionStore connections(path);
    SavedQueryStore store(path);
    QSignalSpy connSaved(&connections, &ConnectionStore::saved);
    QSignalSpy connRemoved(&connections, &ConnectionStore::removed);
    QSignalSpy saved(&store, &SavedQueryStore::querySaved);
    QSignalSpy loaded(&store, &SavedQueryStore::loaded);

    ConnectionProfile p;
    p.name = QStringLiteral("Local");
    p.providerId = QStringLiteral("sqlite");
    connections.save(p);
    QTRY_COMPARE(connSaved.count(), 1);
    const QUuid connectionId = connSaved.at(0).at(0).value<ConnectionProfile>().id;

    auto query = makeQuery(QStringLiteral("Tied"));
    query.connectionId = connectionId;
    SavedQuery stored;
    QVERIFY(saveQuery(store, saved, query, stored));
    QCOMPARE(stored.connectionId, std::optional<QUuid>(connectionId));

    connections.remove(connectionId);
    QTRY_COMPARE(connRemoved.count(), 1);
    store.load();
    QTRY_COMPARE(loaded.count(), 1);
    const auto queries = loaded.at(0).at(1).value<QList<SavedQuery>>();
    QCOMPARE(queries.size(), 1); // the query survives
    QVERIFY(!queries.at(0).connectionId.has_value());
}

void TstSavedQueryStore::upgradesVersion2Database()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("quarry.db"));
    const QUuid id = QUuid::createUuid();
    // Migrations 1 and 2 as released, written by hand.
    const QByteArray sql = "CREATE TABLE connections(id TEXT PRIMARY KEY NOT NULL,"
                           " name TEXT NOT NULL, provider_id TEXT NOT NULL,"
                           " values_json TEXT NOT NULL DEFAULT '{}',"
                           " color_index INTEGER NOT NULL DEFAULT -1,"
                           " production INTEGER NOT NULL DEFAULT 0,"
                           " password_mode INTEGER NOT NULL DEFAULT 0,"
                           " created_at TEXT NOT NULL, updated_at TEXT NOT NULL,"
                           " last_used_at TEXT);"
                           "CREATE TABLE editor_tabs(id TEXT PRIMARY KEY NOT NULL,"
                           " connection_id TEXT REFERENCES connections(id) ON DELETE SET NULL,"
                           " title TEXT NOT NULL, text TEXT NOT NULL,"
                           " cursor_position INTEGER NOT NULL, position INTEGER NOT NULL,"
                           " updated_at TEXT NOT NULL);"
                           "INSERT INTO connections(id, name, provider_id, created_at,"
                           " updated_at) VALUES('"
        + id.toString(QUuid::WithoutBraces).toUtf8()
        + "','Old','sqlite','2024-01-01T00:00:00.000Z','2024-01-01T00:00:00.000Z');"
          "PRAGMA user_version=2";
    QVERIFY(execRaw(path, sql));

    SavedQueryStore store(path);
    QSignalSpy saved(&store, &SavedQueryStore::querySaved);
    auto query = makeQuery(QStringLiteral("Old friend"));
    query.connectionId = id;
    SavedQuery stored;
    QVERIFY(saveQuery(store, saved, query, stored));
    QCOMPARE(stored.connectionId, std::optional<QUuid>(id));
    QCOMPARE(scalar(path, "PRAGMA user_version"), 8);
}

void TstSavedQueryStore::futureVersionIsRefused()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("quarry.db"));
    QVERIFY(execRaw(path, "CREATE TABLE future(x); PRAGMA user_version=99"));

    SavedQueryStore store(path);
    QSignalSpy failed(&store, &SavedQueryStore::failed);
    QSignalSpy saved(&store, &SavedQueryStore::querySaved);
    store.load();
    store.saveQuery(makeQuery(QStringLiteral("x")));
    QTRY_COMPARE(failed.count(), 2);
    QCOMPARE(failed.at(0).at(0).value<Error>().kind, ErrorKind::Unsupported);
    QCOMPARE(saved.count(), 0);
    QCOMPARE(scalar(path, "PRAGMA user_version"), 99);
}

void TstSavedQueryStore::destructorFlushesQueuedSaves()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("quarry.db"));
    constexpr int kCount = 25;
    {
        SavedQueryStore store(path);
        for (int i = 0; i < kCount; ++i)
            store.saveQuery(makeQuery(QStringLiteral("Q%1").arg(i), i));
    } // destroyed right away
    SavedQueryStore again(path);
    QSignalSpy loaded(&again, &SavedQueryStore::loaded);
    again.load();
    QTRY_COMPARE(loaded.count(), 1);
    QCOMPARE(loaded.at(0).at(1).value<QList<SavedQuery>>().size(), kCount);
}

QTEST_MAIN(TstSavedQueryStore)
#include "tst_saved_query_store.moc"
