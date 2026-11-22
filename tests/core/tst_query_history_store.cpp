#include <quarry/connection_store.h>
#include <quarry/query_history_store.h>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <sqlite3.h>

using namespace quarry;

namespace {

const QDateTime kBase(QDate(2024, 1, 1), QTime(0, 0), QTimeZone::UTC);

HistoryEntry makeEntry(const QString &sql, int seconds = 0,
                       const std::optional<QUuid> &connectionId = std::nullopt, bool success = true)
{
    HistoryEntry e;
    e.connectionId = connectionId;
    e.sql = sql;
    e.executedAt = kBase.addSecs(seconds);
    e.durationMs = 12;
    e.rows = 3;
    e.success = success;
    if (!success)
        e.errorMessage = QStringLiteral("It failed.");
    return e;
}

struct Page
{
    quint64 requestId = 0;
    QList<HistoryEntry> entries;
    bool hasMore = false;
};

// Runs one search and waits for its answer (answers arrive in call order).
bool runSearch(QueryHistoryStore &store, QSignalSpy &searched, const HistoryQuery &query, Page &out)
{
    const qsizetype before = searched.count();
    store.search(query);
    if (!QTest::qWaitFor([&] { return searched.count() > before; }))
        return false;
    const QList<QVariant> args = searched.at(before);
    out.requestId = args.at(0).toULongLong();
    out.entries = args.at(1).value<QList<HistoryEntry>>();
    out.hasMore = args.at(2).toBool();
    return true;
}

QStringList sqlOf(const QList<HistoryEntry> &entries)
{
    QStringList list;
    list.reserve(entries.size());
    for (const HistoryEntry &e : entries)
        list.append(e.sql);
    return list;
}

QUuid addConnection(ConnectionStore &store, QSignalSpy &saved, const QString &name,
                    bool recordHistory = true)
{
    ConnectionProfile p;
    p.name = name;
    p.providerId = QStringLiteral("sqlite");
    p.recordHistory = recordHistory;
    const qsizetype before = saved.count();
    store.save(p);
    if (!QTest::qWaitFor([&] { return saved.count() > before; }))
        return {};
    return saved.at(before).at(0).value<ConnectionProfile>().id;
}

bool execRaw(const QString &path, const QByteArray &sql)
{
    sqlite3 *db = nullptr;
    bool ok = sqlite3_open_v2(path.toUtf8().constData(), &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr)
        == SQLITE_OK;
    if (ok) {
        sqlite3_busy_timeout(db, 3000);
        ok = sqlite3_exec(db, sql.constData(), nullptr, nullptr, nullptr) == SQLITE_OK;
    }
    sqlite3_close(db);
    return ok;
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

// Migration 1 and 2 as released, written by hand.
bool createVersion2Database(const QString &path, const QUuid &connectionId)
{
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
        + connectionId.toString(QUuid::WithoutBraces).toUtf8()
        + "','Old','sqlite','2024-01-01T00:00:00.000Z','2024-01-01T00:00:00.000Z');"
          "PRAGMA user_version=2";
    return execRaw(path, sql);
}

} // namespace

class TstQueryHistoryStore : public QObject
{
    Q_OBJECT

private slots:
    void recordAndSearchRoundtrip();
    void emptyTextOrdersNewestFirstThenById();
    void filtersByConnectionAndOnlyErrors();
    void ftsMatchesAllWordsAndLastAsPrefix();
    void ftsUserSyntaxIsLiteral();
    void symbolsOnlyTextMeansNoTextFilter();
    void paginationAndHasMore();
    void limitIsClamped();
    void trimsToMaxEntriesInOneTransaction();
    void recordHistoryFalseRecordsNothing();
    void emptySqlAndUnknownConnectionFail();
    void deletingConnectionDeletesItsHistory();
    void clearByConnectionAndGlobal();
    void removeAlsoUnknown();
    void updatedRowsStayInFtsIndex();
    void upgradesVersion2Database();
    void futureVersionIsRefused();
    void destructorFlushesQueuedRecords();
};

void TstQueryHistoryStore::recordAndSearchRoundtrip()
{
    QTemporaryDir dir;
    QueryHistoryStore store(dir.filePath(QStringLiteral("quarry.db")));
    QSignalSpy recorded(&store, &QueryHistoryStore::recorded);
    QSignalSpy searched(&store, &QueryHistoryStore::searched);
    QSignalSpy failed(&store, &QueryHistoryStore::failed);

    auto entry = makeEntry(QStringLiteral("SELECT 'é 日本 😀' FROM t"), 5, std::nullopt, false);
    entry.durationMs = 1234;
    entry.rows = -1;
    store.record(entry);
    QTRY_COMPARE(recorded.count(), 1);
    const auto stored = recorded.at(0).at(0).value<HistoryEntry>();
    QVERIFY(stored.id > 0);
    QCOMPARE(stored.sql, entry.sql);

    HistoryQuery q;
    q.requestId = 42;
    Page page;
    QVERIFY(runSearch(store, searched, q, page));
    QCOMPARE(page.requestId, quint64(42));
    QVERIFY(!page.hasMore);
    QCOMPARE(page.entries.size(), 1);
    const HistoryEntry &e = page.entries.at(0);
    QCOMPARE(e.id, stored.id);
    QVERIFY(!e.connectionId.has_value());
    QCOMPARE(e.sql, entry.sql);
    QCOMPARE(e.executedAt, entry.executedAt);
    QCOMPARE(e.executedAt.timeSpec(), Qt::UTC);
    QCOMPARE(e.durationMs, 1234);
    QCOMPARE(e.rows, qint64(-1));
    QVERIFY(!e.success);
    QCOMPARE(e.errorMessage, QStringLiteral("It failed."));
    QCOMPARE(failed.count(), 0);
}

void TstQueryHistoryStore::emptyTextOrdersNewestFirstThenById()
{
    QTemporaryDir dir;
    QueryHistoryStore store(dir.filePath(QStringLiteral("quarry.db")));
    QSignalSpy searched(&store, &QueryHistoryStore::searched);
    store.record(makeEntry(QStringLiteral("old"), 1));
    store.record(makeEntry(QStringLiteral("tie first"), 5));
    store.record(makeEntry(QStringLiteral("tie second"), 5));
    store.record(makeEntry(QStringLiteral("newest"), 9));
    store.record(makeEntry(QStringLiteral("oldest"), 0));

    Page page;
    QVERIFY(runSearch(store, searched, {}, page));
    QCOMPARE(sqlOf(page.entries),
             QStringList({ QStringLiteral("newest"), QStringLiteral("tie second"),
                           QStringLiteral("tie first"), QStringLiteral("old"),
                           QStringLiteral("oldest") }));
}

void TstQueryHistoryStore::filtersByConnectionAndOnlyErrors()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("quarry.db"));
    ConnectionStore connections(path);
    QueryHistoryStore store(path);
    QSignalSpy connSaved(&connections, &ConnectionStore::saved);
    QSignalSpy searched(&store, &QueryHistoryStore::searched);
    const QUuid a = addConnection(connections, connSaved, QStringLiteral("A"));
    const QUuid b = addConnection(connections, connSaved, QStringLiteral("B"));
    QVERIFY(!a.isNull() && !b.isNull());

    store.record(makeEntry(QStringLiteral("a ok"), 1, a));
    store.record(makeEntry(QStringLiteral("a bad"), 2, a, false));
    store.record(makeEntry(QStringLiteral("b ok"), 3, b));
    store.record(makeEntry(QStringLiteral("b bad"), 4, b, false));
    store.record(makeEntry(QStringLiteral("none bad"), 5, std::nullopt, false));

    Page page;
    HistoryQuery q;
    q.connectionId = a;
    QVERIFY(runSearch(store, searched, q, page));
    QCOMPARE(sqlOf(page.entries), QStringList({ QStringLiteral("a bad"), QStringLiteral("a ok") }));
    QCOMPARE(page.entries.at(0).connectionId, std::optional<QUuid>(a));

    q.onlyErrors = true;
    QVERIFY(runSearch(store, searched, q, page));
    QCOMPARE(sqlOf(page.entries), QStringList({ QStringLiteral("a bad") }));

    q.connectionId.reset();
    QVERIFY(runSearch(store, searched, q, page));
    QCOMPARE(sqlOf(page.entries),
             QStringList(
                 { QStringLiteral("none bad"), QStringLiteral("b bad"), QStringLiteral("a bad") }));

    q.text = QStringLiteral("b bad");
    QVERIFY(runSearch(store, searched, q, page)); // text + errors: "b bad" only
    QCOMPARE(sqlOf(page.entries), QStringList({ QStringLiteral("b bad") }));
}

void TstQueryHistoryStore::ftsMatchesAllWordsAndLastAsPrefix()
{
    QTemporaryDir dir;
    QueryHistoryStore store(dir.filePath(QStringLiteral("quarry.db")));
    QSignalSpy searched(&store, &QueryHistoryStore::searched);
    store.record(makeEntry(QStringLiteral("SELECT name FROM customers WHERE id = 1"), 1));
    store.record(makeEntry(QStringLiteral("SELECT total FROM orders"), 2));
    store.record(makeEntry(QStringLiteral("UPDATE customers SET name = 'x'"), 3));

    HistoryQuery q;
    Page page;
    q.text = QStringLiteral("select customers");
    QVERIFY(runSearch(store, searched, q, page));
    QCOMPARE(sqlOf(page.entries),
             QStringList({ QStringLiteral("SELECT name FROM customers WHERE id = 1") }));

    q.text = QStringLiteral("cust"); // prefix of the last (only) word
    QVERIFY(runSearch(store, searched, q, page));
    QCOMPARE(page.entries.size(), 2);

    q.text = QStringLiteral("cust select"); // only the LAST word is a prefix: "cust" must be whole
    QVERIFY(runSearch(store, searched, q, page));
    QCOMPARE(page.entries.size(), 0);

    q.text = QStringLiteral("  FROM   ord  "); // case and whitespace do not matter
    QVERIFY(runSearch(store, searched, q, page));
    QCOMPARE(sqlOf(page.entries), QStringList({ QStringLiteral("SELECT total FROM orders") }));

    q.text = QStringLiteral("nothing here");
    QVERIFY(runSearch(store, searched, q, page));
    QVERIFY(page.entries.isEmpty());
}

void TstQueryHistoryStore::ftsUserSyntaxIsLiteral()
{
    QTemporaryDir dir;
    QueryHistoryStore store(dir.filePath(QStringLiteral("quarry.db")));
    QSignalSpy searched(&store, &QueryHistoryStore::searched);
    QSignalSpy failed(&store, &QueryHistoryStore::failed);
    store.record(makeEntry(QStringLiteral("select foo from t"), 1));
    store.record(makeEntry(QStringLiteral("select bar from t"), 2));
    store.record(makeEntry(QStringLiteral("insert into foo or bar"), 3));
    store.record(makeEntry(QStringLiteral("say \"hello\" now"), 4));

    const auto find = [&](const QString &text) {
        HistoryQuery q;
        q.text = text;
        Page page;
        const bool ok = runSearch(store, searched, q, page);
        return ok ? sqlOf(page.entries) : QStringList({ QStringLiteral("<timeout>") });
    };

    // OR is a plain word: both words must be present, so "foo OR zzz" matches nothing.
    QCOMPARE(find(QStringLiteral("foo OR zzz")), QStringList());
    QCOMPARE(find(QStringLiteral("foo OR bar")),
             QStringList({ QStringLiteral("insert into foo or bar") }));
    // "-" is not NOT; "^" and "col:" are not column filters or anchors.
    QCOMPARE(find(QStringLiteral("-foo")).size(), 2);
    QCOMPARE(find(QStringLiteral("sql:foo")).size(),
             0); // one phrase "sql foo", not a column filter
    QCOMPARE(find(QStringLiteral("NEAR(foo bar)")).size(), 0);
    // A double quote is data: it must neither break the query nor open a phrase.
    QCOMPARE(find(QStringLiteral("\"hello")), QStringList({ QStringLiteral("say \"hello\" now") }));
    QCOMPARE(find(QStringLiteral("say \"")), QStringList({ QStringLiteral("say \"hello\" now") }));
    QCOMPARE(find(QStringLiteral("fo*")).size(), 2); // the star is not a second prefix operator
    QCOMPARE(find(QStringLiteral("\"")).size(), 4); // symbols only: no text filter
    QCOMPARE(failed.count(), 0);
}

void TstQueryHistoryStore::symbolsOnlyTextMeansNoTextFilter()
{
    QTemporaryDir dir;
    QueryHistoryStore store(dir.filePath(QStringLiteral("quarry.db")));
    QSignalSpy searched(&store, &QueryHistoryStore::searched);
    QSignalSpy failed(&store, &QueryHistoryStore::failed);
    store.record(makeEntry(QStringLiteral("one"), 1));
    store.record(makeEntry(QStringLiteral("two"), 2));
    for (const QString &text : { QStringLiteral("   "), QStringLiteral("*"),
                                 QStringLiteral("- + ( ) \" ' ^"), QStringLiteral("\t\n") }) {
        HistoryQuery q;
        q.text = text;
        Page page;
        QVERIFY(runSearch(store, searched, q, page));
        QVERIFY2(page.entries.size() == 2, qPrintable(text));
    }
    QCOMPARE(failed.count(), 0);
}

void TstQueryHistoryStore::paginationAndHasMore()
{
    QTemporaryDir dir;
    QueryHistoryStore store(dir.filePath(QStringLiteral("quarry.db")));
    QSignalSpy searched(&store, &QueryHistoryStore::searched);
    for (int i = 0; i < 5; ++i)
        store.record(makeEntry(QStringLiteral("item %1").arg(i), i));

    HistoryQuery q;
    q.limit = 2;
    Page page;
    QVERIFY(runSearch(store, searched, q, page));
    QCOMPARE(sqlOf(page.entries),
             QStringList({ QStringLiteral("item 4"), QStringLiteral("item 3") }));
    QVERIFY(page.hasMore);

    q.offset = 2;
    QVERIFY(runSearch(store, searched, q, page));
    QCOMPARE(sqlOf(page.entries),
             QStringList({ QStringLiteral("item 2"), QStringLiteral("item 1") }));
    QVERIFY(page.hasMore);

    q.offset = 4;
    QVERIFY(runSearch(store, searched, q, page));
    QCOMPARE(sqlOf(page.entries), QStringList({ QStringLiteral("item 0") }));
    QVERIFY(!page.hasMore);

    q.offset = 0;
    q.limit = 5; // exactly the total: nothing more
    QVERIFY(runSearch(store, searched, q, page));
    QCOMPARE(page.entries.size(), 5);
    QVERIFY(!page.hasMore);

    q.text = QStringLiteral("item"); // same paging through the FTS path
    q.limit = 3;
    QVERIFY(runSearch(store, searched, q, page));
    QCOMPARE(page.entries.size(), 3);
    QVERIFY(page.hasMore);
}

void TstQueryHistoryStore::limitIsClamped()
{
    QTemporaryDir dir;
    QueryHistoryStore store(dir.filePath(QStringLiteral("quarry.db")));
    QSignalSpy searched(&store, &QueryHistoryStore::searched);
    QSignalSpy failed(&store, &QueryHistoryStore::failed);
    store.record(makeEntry(QStringLiteral("a"), 1));
    store.record(makeEntry(QStringLiteral("b"), 2));

    HistoryQuery q;
    Page page;
    q.limit = 0; // up to 1
    QVERIFY(runSearch(store, searched, q, page));
    QCOMPARE(page.entries.size(), 1);
    QVERIFY(page.hasMore);
    q.limit = -7;
    QVERIFY(runSearch(store, searched, q, page));
    QCOMPARE(page.entries.size(), 1);
    q.limit = 100000; // down to 1000
    q.offset = -3; // up to 0
    QVERIFY(runSearch(store, searched, q, page));
    QCOMPARE(page.entries.size(), 2);
    QVERIFY(!page.hasMore);
    QCOMPARE(failed.count(), 0);
}

void TstQueryHistoryStore::trimsToMaxEntriesInOneTransaction()
{
    // No seam needed: seed the table at the cap with one raw transaction, then record through the
    // store and check that exactly the oldest row goes.
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("quarry.db"));
    QueryHistoryStore store(path);
    QSignalSpy searched(&store, &QueryHistoryStore::searched);
    QSignalSpy recorded(&store, &QueryHistoryStore::recorded);
    QSignalSpy failed(&store, &QueryHistoryStore::failed);
    Page page;
    QVERIFY(runSearch(store, searched, {}, page)); // opens and migrates the file

    QVERIFY(execRaw(path,
                    "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<10000)"
                    " INSERT INTO query_history(connection_id, sql, executed_at, duration_ms, rows,"
                    " success, error_message) SELECT NULL, 'seed ' || i,"
                    " strftime('%Y-%m-%dT%H:%M:%S.000Z', '2000-01-01', '+' || i || ' seconds'),"
                    " 1, 1, 1, '' FROM n"));
    QCOMPARE(scalar(path, "SELECT count(*) FROM query_history"),
             qint64(QueryHistoryStore::kMaxEntries));

    store.record(makeEntry(QStringLiteral("brand new"), 0));
    QTRY_COMPARE(recorded.count(), 1);
    QCOMPARE(scalar(path, "SELECT count(*) FROM query_history"),
             qint64(QueryHistoryStore::kMaxEntries));
    QCOMPARE(scalar(path, "SELECT count(*) FROM query_history WHERE sql='seed 1'"), 0);
    QCOMPARE(scalar(path, "SELECT count(*) FROM query_history WHERE sql='seed 2'"), 1);
    QCOMPARE(scalar(path, "SELECT count(*) FROM query_history WHERE sql='brand new'"), 1);
    // The FTS index followed the delete: the token "1" only existed in 'seed 1'.
    QCOMPARE(scalar(path,
                    "SELECT count(*) FROM query_history_fts WHERE query_history_fts MATCH '\"1\"'"),
             0);
    QCOMPARE(failed.count(), 0);
}

void TstQueryHistoryStore::recordHistoryFalseRecordsNothing()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("quarry.db"));
    ConnectionStore connections(path);
    QueryHistoryStore store(path);
    QSignalSpy connSaved(&connections, &ConnectionStore::saved);
    QSignalSpy recorded(&store, &QueryHistoryStore::recorded);
    QSignalSpy searched(&store, &QueryHistoryStore::searched);
    QSignalSpy failed(&store, &QueryHistoryStore::failed);
    const QUuid quiet = addConnection(connections, connSaved, QStringLiteral("Quiet"), false);
    const QUuid loud = addConnection(connections, connSaved, QStringLiteral("Loud"));
    QVERIFY(!quiet.isNull() && !loud.isNull());

    store.record(makeEntry(QStringLiteral("secret"), 1, quiet));
    store.record(makeEntry(QStringLiteral("public"), 2, loud));
    Page page;
    QVERIFY(runSearch(store, searched, {}, page)); // answers come in call order
    QCOMPARE(recorded.count(), 1);
    QCOMPARE(sqlOf(page.entries), QStringList({ QStringLiteral("public") }));
    QCOMPARE(failed.count(), 0);
}

void TstQueryHistoryStore::emptySqlAndUnknownConnectionFail()
{
    QTemporaryDir dir;
    QueryHistoryStore store(dir.filePath(QStringLiteral("quarry.db")));
    QSignalSpy recorded(&store, &QueryHistoryStore::recorded);
    QSignalSpy failed(&store, &QueryHistoryStore::failed);
    QSignalSpy searched(&store, &QueryHistoryStore::searched);

    store.record(makeEntry(QStringLiteral("  \n")));
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(failed.at(0).at(0).value<Error>().kind, ErrorKind::Constraint);
    QVERIFY(!failed.at(0).at(0).value<Error>().message.isEmpty());

    store.record(makeEntry(QStringLiteral("SELECT 1"), 0, QUuid::createUuid()));
    QTRY_COMPARE(failed.count(), 2);
    const auto error = failed.at(1).at(0).value<Error>();
    QCOMPARE(error.kind, ErrorKind::Constraint);
    QVERIFY(!error.message.isEmpty());
    QVERIFY(!error.detail.isEmpty());
    QCOMPARE(recorded.count(), 0);

    Page page;
    QVERIFY(runSearch(store, searched, {}, page));
    QVERIFY(page.entries.isEmpty());
}

void TstQueryHistoryStore::deletingConnectionDeletesItsHistory()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("quarry.db"));
    ConnectionStore connections(path);
    QueryHistoryStore store(path);
    QSignalSpy connSaved(&connections, &ConnectionStore::saved);
    QSignalSpy connRemoved(&connections, &ConnectionStore::removed);
    QSignalSpy searched(&store, &QueryHistoryStore::searched);
    const QUuid a = addConnection(connections, connSaved, QStringLiteral("A"));
    const QUuid b = addConnection(connections, connSaved, QStringLiteral("B"));

    store.record(makeEntry(QStringLiteral("uniqueword from a"), 1, a));
    store.record(makeEntry(QStringLiteral("from b"), 2, b));
    store.record(makeEntry(QStringLiteral("from nobody"), 3));
    Page page;
    QVERIFY(runSearch(store, searched, {}, page));
    QCOMPARE(page.entries.size(), 3);

    connections.remove(a);
    QTRY_COMPARE(connRemoved.count(), 1);
    QVERIFY(runSearch(store, searched, {}, page));
    QCOMPARE(sqlOf(page.entries),
             QStringList({ QStringLiteral("from nobody"), QStringLiteral("from b") }));
    HistoryQuery q;
    q.text = QStringLiteral("uniqueword"); // the FTS index lost it too
    QVERIFY(runSearch(store, searched, q, page));
    QVERIFY(page.entries.isEmpty());
}

void TstQueryHistoryStore::clearByConnectionAndGlobal()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("quarry.db"));
    ConnectionStore connections(path);
    QueryHistoryStore store(path);
    QSignalSpy connSaved(&connections, &ConnectionStore::saved);
    QSignalSpy cleared(&store, &QueryHistoryStore::cleared);
    QSignalSpy searched(&store, &QueryHistoryStore::searched);
    QSignalSpy failed(&store, &QueryHistoryStore::failed);
    const QUuid a = addConnection(connections, connSaved, QStringLiteral("A"));
    const QUuid b = addConnection(connections, connSaved, QStringLiteral("B"));

    store.record(makeEntry(QStringLiteral("a1"), 1, a));
    store.record(makeEntry(QStringLiteral("a2"), 2, a));
    store.record(makeEntry(QStringLiteral("b1"), 3, b));
    store.record(makeEntry(QStringLiteral("n1"), 4));

    store.clear(a);
    QTRY_COMPARE(cleared.count(), 1);
    QCOMPARE(cleared.at(0).at(0).value<std::optional<QUuid>>(), std::optional<QUuid>(a));
    QCOMPARE(cleared.at(0).at(1).toLongLong(), qint64(2));
    Page page;
    QVERIFY(runSearch(store, searched, {}, page));
    QCOMPARE(sqlOf(page.entries), QStringList({ QStringLiteral("n1"), QStringLiteral("b1") }));

    store.clear(a); // nothing left for it
    QTRY_COMPARE(cleared.count(), 2);
    QCOMPARE(cleared.at(1).at(1).toLongLong(), qint64(0));

    store.clear(std::nullopt);
    QTRY_COMPARE(cleared.count(), 3);
    QVERIFY(!cleared.at(2).at(0).value<std::optional<QUuid>>().has_value());
    QCOMPARE(cleared.at(2).at(1).toLongLong(), qint64(2));
    QVERIFY(runSearch(store, searched, {}, page));
    QVERIFY(page.entries.isEmpty());
    QCOMPARE(scalar(path,
                    "SELECT count(*) FROM query_history_fts WHERE query_history_fts MATCH "
                    "'\"b1\"'"),
             0);
    QCOMPARE(failed.count(), 0);
}

void TstQueryHistoryStore::removeAlsoUnknown()
{
    QTemporaryDir dir;
    QueryHistoryStore store(dir.filePath(QStringLiteral("quarry.db")));
    QSignalSpy recorded(&store, &QueryHistoryStore::recorded);
    QSignalSpy removed(&store, &QueryHistoryStore::removed);
    QSignalSpy searched(&store, &QueryHistoryStore::searched);
    QSignalSpy failed(&store, &QueryHistoryStore::failed);
    store.record(makeEntry(QStringLiteral("keep"), 1));
    store.record(makeEntry(QStringLiteral("drop"), 2));
    QTRY_COMPARE(recorded.count(), 2);
    const qint64 dropId = recorded.at(1).at(0).value<HistoryEntry>().id;

    store.remove(dropId);
    QTRY_COMPARE(removed.count(), 1);
    QCOMPARE(removed.at(0).at(0).toLongLong(), dropId);
    store.remove(987654321);
    QTRY_COMPARE(removed.count(), 2);
    Page page;
    HistoryQuery q;
    q.text = QStringLiteral("drop");
    QVERIFY(runSearch(store, searched, q, page));
    QVERIFY(page.entries.isEmpty());
    QVERIFY(runSearch(store, searched, {}, page));
    QCOMPARE(sqlOf(page.entries), QStringList({ QStringLiteral("keep") }));
    QCOMPARE(failed.count(), 0);
}

void TstQueryHistoryStore::updatedRowsStayInFtsIndex()
{
    // Guards the UPDATE trigger: the store never updates rows, but a future change might.
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("quarry.db"));
    QueryHistoryStore store(path);
    QSignalSpy searched(&store, &QueryHistoryStore::searched);
    store.record(makeEntry(QStringLiteral("alpha query"), 1));
    Page page;
    QVERIFY(runSearch(store, searched, {}, page));
    QVERIFY(execRaw(path, "UPDATE query_history SET sql='omega query'"));
    HistoryQuery q;
    q.text = QStringLiteral("alpha");
    QVERIFY(runSearch(store, searched, q, page));
    QVERIFY(page.entries.isEmpty());
    q.text = QStringLiteral("omega");
    QVERIFY(runSearch(store, searched, q, page));
    QCOMPARE(page.entries.size(), 1);
}

void TstQueryHistoryStore::upgradesVersion2Database()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("quarry.db"));
    const QUuid id = QUuid::createUuid();
    QVERIFY(createVersion2Database(path, id));
    QCOMPARE(scalar(path, "PRAGMA user_version"), 2);

    QueryHistoryStore store(path);
    QSignalSpy recorded(&store, &QueryHistoryStore::recorded);
    QSignalSpy searched(&store, &QueryHistoryStore::searched);
    QSignalSpy failed(&store, &QueryHistoryStore::failed);
    store.record(makeEntry(QStringLiteral("SELECT 1"), 1, id)); // old connection: history on
    QTRY_COMPARE(recorded.count(), 1);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(scalar(path, "PRAGMA user_version"), 8);
    QCOMPARE(scalar(path, "SELECT record_history FROM connections"), 1);
    Page page;
    HistoryQuery q;
    q.text = QStringLiteral("sel");
    QVERIFY(runSearch(store, searched, q, page));
    QCOMPARE(page.entries.size(), 1);
}

void TstQueryHistoryStore::futureVersionIsRefused()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("quarry.db"));
    QVERIFY(execRaw(path, "CREATE TABLE future(x); PRAGMA user_version=99"));

    QueryHistoryStore store(path);
    QSignalSpy recorded(&store, &QueryHistoryStore::recorded);
    QSignalSpy failed(&store, &QueryHistoryStore::failed);
    store.record(makeEntry(QStringLiteral("SELECT 1")));
    store.search({});
    QTRY_COMPARE(failed.count(), 2);
    QCOMPARE(failed.at(0).at(0).value<Error>().kind, ErrorKind::Unsupported);
    QCOMPARE(recorded.count(), 0);
    QCOMPARE(scalar(path, "PRAGMA user_version"), 99);
}

void TstQueryHistoryStore::destructorFlushesQueuedRecords()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("quarry.db"));
    constexpr int kCount = 25;
    {
        QueryHistoryStore store(path);
        for (int i = 0; i < kCount; ++i)
            store.record(makeEntry(QStringLiteral("SELECT %1").arg(i), i));
    } // destroyed right away
    QueryHistoryStore again(path);
    QSignalSpy searched(&again, &QueryHistoryStore::searched);
    Page page;
    QVERIFY(runSearch(again, searched, {}, page));
    QCOMPARE(page.entries.size(), kCount);
}

QTEST_MAIN(TstQueryHistoryStore)
#include "tst_query_history_store.moc"
