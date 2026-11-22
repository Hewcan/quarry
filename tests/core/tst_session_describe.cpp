#include <quarry/provider_registry.h>
#include <quarry/session.h>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <sqlite3.h>

using namespace quarry;

namespace {

ConnectionProfile sqliteProfile(const QString &file)
{
    ConnectionProfile p;
    p.id = QUuid::createUuid();
    p.name = QStringLiteral("test");
    p.providerId = QStringLiteral("sqlite");
    p.values = { { QStringLiteral("file"), file } };
    return p;
}

bool createDatabase(const QString &file)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open(file.toUtf8().constData(), &db) != SQLITE_OK)
        return false;
    const char *sql = "CREATE TABLE parent(id INTEGER PRIMARY KEY, name TEXT);"
                      "CREATE TABLE child(id INTEGER PRIMARY KEY, parent_id INTEGER NOT NULL "
                      "REFERENCES parent(id), code TEXT DEFAULT 'x');"
                      "CREATE UNIQUE INDEX child_code ON child(code);"
                      "CREATE VIEW child_view AS SELECT id, code FROM child;";
    const bool ok = sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(db);
    return ok;
}

ObjectPath path(const QString &name, ObjectKind kind = ObjectKind::Table)
{
    return ObjectPath { { QStringLiteral("main"), name }, kind };
}

// Registry must outlive the session: the connection's code lives in the plugin.
struct Fixture
{
    ProviderRegistry registry;
    QTemporaryDir dir;
    std::unique_ptr<Session> session;

    bool init()
    {
        registry.scan(QStringLiteral(QUARRY_PLUGIN_DIR));
        const QString file = dir.filePath(QStringLiteral("t.db"));
        if (!dir.isValid() || !createDatabase(file))
            return false;
        auto connection = registry.createConnection(sqliteProfile(file), {});
        if (!connection)
            return false;
        session = std::make_unique<Session>(std::move(connection).value());
        return true;
    }
};

Statement slowStatement()
{
    return Statement { QStringLiteral("WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM "
                                      "c WHERE x < 2000000000) SELECT count(*) FROM c"),
                       {},
                       500 };
}

} // namespace

class TstSessionDescribe : public QObject
{
    Q_OBJECT

private slots:
    void describesTableWithKeysAndIndex();
    void describesView();
    void missingTableFails();
    void withoutOpenFails();
    void cancelFailsPendingAndSessionStaysUsable();
    void closeFailsPending();
    void keepsOrderWithChildrenAndExecute();
    void consecutiveDescribesHaveDistinctIds();
};

void TstSessionDescribe::describesTableWithKeysAndIndex()
{
    Fixture f;
    QVERIFY(f.init());
    QSignalSpy described(f.session.get(), &Session::described);
    QSignalSpy failed(f.session.get(), &Session::describeFailed);
    f.session->open();
    const quint64 id = f.session->describe(path(QStringLiteral("child")));
    QTRY_COMPARE_WITH_TIMEOUT(described.count(), 1, 10000);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(described.at(0).at(0).toULongLong(), id);
    QCOMPARE(described.at(0).at(1).value<ObjectPath>().parts, path(QStringLiteral("child")).parts);
    const auto d = described.at(0).at(2).value<TableDetails>();
    QCOMPARE(d.columns.size(), size_t(3));
    QCOMPARE(d.columns[0].name, QStringLiteral("id"));
    QCOMPARE(d.columns[1].name, QStringLiteral("parent_id"));
    QVERIFY(!d.columns[1].nullable);
    QCOMPARE(d.columns[2].name, QStringLiteral("code"));
    QCOMPARE(d.primaryKey.value_or(QStringList {}), QStringList { QStringLiteral("id") });
    QCOMPARE(d.foreignKeys.size(), size_t(1));
    QCOMPARE(d.foreignKeys[0].columns, QStringList { QStringLiteral("parent_id") });
    QCOMPARE(d.foreignKeys[0].referencedTable.parts.value(1), QStringLiteral("parent"));
    QCOMPARE(d.foreignKeys[0].referencedColumns, QStringList { QStringLiteral("id") });
    const auto unique = std::find_if(d.indexes.begin(), d.indexes.end(), [](const IndexDef &i) {
        return i.name == QStringLiteral("child_code");
    });
    QVERIFY(unique != d.indexes.end());
    QVERIFY(unique->unique);
    QCOMPARE(unique->columns, QStringList { QStringLiteral("code") });
}

void TstSessionDescribe::describesView()
{
    Fixture f;
    QVERIFY(f.init());
    QSignalSpy described(f.session.get(), &Session::described);
    QSignalSpy failed(f.session.get(), &Session::describeFailed);
    f.session->open();
    f.session->describe(path(QStringLiteral("child_view"), ObjectKind::View));
    QTRY_COMPARE_WITH_TIMEOUT(described.count(), 1, 10000);
    QCOMPARE(failed.count(), 0);
    const auto d = described.at(0).at(2).value<TableDetails>();
    QCOMPARE(d.columns.size(), size_t(2));
    QVERIFY(d.foreignKeys.empty());
    QVERIFY(d.indexes.empty());
}

void TstSessionDescribe::missingTableFails()
{
    Fixture f;
    QVERIFY(f.init());
    QSignalSpy described(f.session.get(), &Session::described);
    QSignalSpy failed(f.session.get(), &Session::describeFailed);
    f.session->open();
    const quint64 id = f.session->describe(path(QStringLiteral("nope")));
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
    QCOMPARE(failed.at(0).at(0).toULongLong(), id);
    QCOMPARE(failed.at(0).at(2).value<Error>().kind, ErrorKind::NotFound);
    QCOMPARE(described.count(), 0);
}

void TstSessionDescribe::withoutOpenFails()
{
    Fixture f;
    QVERIFY(f.init());
    QSignalSpy failed(f.session.get(), &Session::describeFailed);
    f.session->describe(path(QStringLiteral("child")));
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
    QCOMPARE(failed.at(0).at(2).value<Error>().message,
             QStringLiteral("The connection is not open."));
}

void TstSessionDescribe::cancelFailsPendingAndSessionStaysUsable()
{
    Fixture f;
    QVERIFY(f.init());
    QSignalSpy described(f.session.get(), &Session::described);
    QSignalSpy failed(f.session.get(), &Session::describeFailed);
    QSignalSpy executeCancelled(f.session.get(), &Session::cancelled);
    f.session->open();
    f.session->execute(slowStatement()); // keeps the describes pending on the worker
    std::vector<quint64> ids;
    ids.reserve(3);
    for (int i = 0; i < 3; ++i)
        ids.push_back(f.session->describe(path(QStringLiteral("child"))));
    QTest::qWait(100);
    QCOMPARE(failed.count(), 0);

    f.session->cancel();
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 3, 10000);
    QTRY_COMPARE(executeCancelled.count(), 1);
    for (int i = 0; i < 3; ++i) {
        QCOMPARE(failed.at(i).at(0).toULongLong(), ids[size_t(i)]);
        QCOMPARE(failed.at(i).at(2).value<Error>().kind, ErrorKind::Cancelled);
    }
    QTest::qWait(50);
    QCOMPARE(failed.count(), 3);
    QCOMPARE(described.count(), 0);

    f.session->describe(path(QStringLiteral("child")));
    QTRY_COMPARE_WITH_TIMEOUT(described.count(), 1, 10000);
    QCOMPARE(failed.count(), 3);
}

void TstSessionDescribe::closeFailsPending()
{
    Fixture f;
    QVERIFY(f.init());
    QSignalSpy described(f.session.get(), &Session::described);
    QSignalSpy failed(f.session.get(), &Session::describeFailed);
    QSignalSpy closed(f.session.get(), &Session::closed);
    f.session->open();
    f.session->execute(slowStatement());
    for (int i = 0; i < 2; ++i)
        f.session->describe(path(QStringLiteral("child")));
    QTest::qWait(100);
    f.session->close();
    QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
    QTRY_COMPARE(failed.count(), 2);
    QTest::qWait(50);
    QCOMPARE(failed.count(), 2);
    QCOMPARE(described.count(), 0);
}

void TstSessionDescribe::keepsOrderWithChildrenAndExecute()
{
    Fixture f;
    QVERIFY(f.init());
    QStringList log;
    connect(f.session.get(), &Session::finished, this, [&] { log << QStringLiteral("execute"); });
    connect(f.session.get(), &Session::childrenReady, this,
            [&](quint64 id) { log << QStringLiteral("children%1").arg(id); });
    connect(f.session.get(), &Session::described, this,
            [&](quint64 id) { log << QStringLiteral("describe%1").arg(id); });
    f.session->open();
    // A second execute() cancels one still pending, so the executes are queued one at a time.
    f.session->execute(Statement { QStringLiteral("select 1"), {}, 500 });
    const quint64 c = f.session->children(ObjectPath { {}, ObjectKind::Database });
    const quint64 d = f.session->describe(path(QStringLiteral("parent")));
    const quint64 c2 = f.session->children(ObjectPath { {}, ObjectKind::Database });
    QTRY_COMPARE_WITH_TIMEOUT(log.size(), 4, 10000);
    QCOMPARE(log,
             (QStringList { QStringLiteral("execute"), QStringLiteral("children%1").arg(c),
                            QStringLiteral("describe%1").arg(d),
                            QStringLiteral("children%1").arg(c2) }));
    log.clear();
    f.session->execute(Statement { QStringLiteral("select 2"), {}, 500 });
    const quint64 d2 = f.session->describe(path(QStringLiteral("child")));
    QTRY_COMPARE_WITH_TIMEOUT(log.size(), 2, 10000);
    QCOMPARE(log,
             (QStringList { QStringLiteral("execute"), QStringLiteral("describe%1").arg(d2) }));
}

void TstSessionDescribe::consecutiveDescribesHaveDistinctIds()
{
    Fixture f;
    QVERIFY(f.init());
    QSignalSpy described(f.session.get(), &Session::described);
    f.session->open();
    const quint64 a = f.session->describe(path(QStringLiteral("parent")));
    const quint64 b = f.session->describe(path(QStringLiteral("parent")));
    QVERIFY(a > 0);
    QVERIFY(b > 0);
    QVERIFY(a != b);
    QTRY_COMPARE_WITH_TIMEOUT(described.count(), 2, 10000);
}

QTEST_GUILESS_MAIN(TstSessionDescribe)
#include "tst_session_describe.moc"
