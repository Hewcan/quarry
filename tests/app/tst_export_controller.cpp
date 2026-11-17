#include "ui/connection_controller.h"
#include "ui/editor_tabs_model.h"
#include "ui/export_controller.h"
#include "ui/query_controller.h"

#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace quarry;
using namespace quarry::ui;

namespace {

constexpr int kWaitMs = 10000;
constexpr int kSlowMs = 60000; // a debug build counts slowly; the cancel ends it at once anyway

constexpr int kBigRows = 2'000'000; // a debug build needs seconds to write this

// 3000 rows: more than the grid's first page (500) and than one fetch of the exporter's first
// round trip would hold in a row-by-row run.
const QString kManyRows
    = QStringLiteral("WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < 3000) "
                     "SELECT x, 'row ' || x AS label FROM c");

QByteArray readAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

} // namespace

// ExportController (H5, app side) over the real SQLite plugin, a real Exporter and files in a
// temporary directory: states, progress, cancelling, an existing file, what may be run again,
// path errors. The keychain is never used.
class TstExportController : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir; // databases and exported files
    QTemporaryDir m_settings;
    int m_files = 0;

    // Declared in this order so the ExportController dies before the plugins' registry.
    struct Env
    {
        std::unique_ptr<ConnectionController> controller;
        std::unique_ptr<EditorTabsModel> tabs;
        std::unique_ptr<QueryController> queries;
        std::unique_ptr<ExportController> exporter;
        QString connection;
    };

    QString outFile(const QString &name) const { return m_dir.filePath(name); }

    QString addConnection(ConnectionController &c, const QString &name)
    {
        const QString file = m_dir.filePath(QStringLiteral("data-%1.db").arg(++m_files));
        c.prepare(QString());
        c.form()->setValue(0, file);
        QSignalSpy saved(&c, &ConnectionController::connectionSaved);
        const bool ok = c.save({ { QStringLiteral("id"), QString() },
                                 { QStringLiteral("providerId"), QStringLiteral("sqlite") },
                                 { QStringLiteral("name"), name },
                                 { QStringLiteral("colorIndex"), 2 },
                                 { QStringLiteral("production"), false },
                                 { QStringLiteral("recordHistory"), true },
                                 { QStringLiteral("passwordMode"), 1 } });
        if (!ok || !saved.wait(kWaitMs))
            return {};
        return saved.at(0).at(0).toString();
    }

    Env start()
    {
        Env e;
        e.controller = std::make_unique<ConnectionController>();
        e.controller->setDatabasePath(
            m_dir.filePath(QStringLiteral("quarry-%1.db").arg(++m_files)));
        e.controller->setProviderDirectory(QStringLiteral(QUARRY_PLUGIN_DIR));
        e.controller->start();
        [&] { QTRY_VERIFY_WITH_TIMEOUT(!e.controller->model()->loading(), kWaitMs); }();
        e.connection = addConnection(*e.controller, QStringLiteral("db"));
        e.tabs = std::make_unique<EditorTabsModel>();
        e.tabs->setDatabasePath(m_dir.filePath(QStringLiteral("tabs-%1.db").arg(++m_files)));
        e.tabs->setConnections(e.controller.get());
        e.tabs->setSaveDelay(60000);
        e.tabs->start();
        [&] { QTRY_VERIFY_WITH_TIMEOUT(e.tabs->loaded(), kWaitMs); }();
        e.queries = std::make_unique<QueryController>();
        e.queries->setTabs(e.tabs.get());
        e.exporter = std::make_unique<ExportController>();
        e.exporter->setQueries(e.queries.get());
        return e;
    }

    // Opens a tab, runs `sql` in it and waits for the end. Returns the tab id.
    QString runTab(Env &e, const QString &sql, const QString &state = QStringLiteral("done"))
    {
        const int row = e.tabs->openTab(e.connection);
        const QString id = e.tabs->idOf(row);
        e.tabs->setText(row, sql);
        e.tabs->run(row, true, static_cast<int>(sql.size()));
        QueryState *s = e.queries->stateOf(id);
        [&] { QTRY_COMPARE_WITH_TIMEOUT(s->state(), state, kSlowMs); }();
        return id;
    }

    static QVariantMap request(const QString &tab, const QString &file,
                               const QString &scope = QStringLiteral("current"), int format = 0)
    {
        return { { QStringLiteral("tabId"), tab },
                 { QStringLiteral("scope"), scope },
                 { QStringLiteral("format"), format },
                 { QStringLiteral("filePath"), file },
                 { QStringLiteral("header"), true } };
    }

    static void waitState(ExportController &x, const QString &state)
    {
        QTRY_COMPARE_WITH_TIMEOUT(x.state(), state, kSlowMs);
    }

private slots:
    void initTestCase()
    {
        QCoreApplication::setApplicationName(QStringLiteral("tst_export_controller"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
        QVERIFY(m_dir.isValid());
    }

    void init() { QSettings().clear(); }

    void returnsRowsClassifiesStatements_data()
    {
        QTest::addColumn<QString>("sql");
        QTest::addColumn<bool>("rows");
        const auto row = [](const char *name, const char *sql, bool rows) {
            QTest::newRow(name) << QString::fromUtf8(sql) << rows;
        };
        row("select", "SELECT 1", true);
        row("lowercase", "  select * from t where a = 1;", true);
        row("comments first", "-- note\n/* more */ SELECT 1", true);
        row("parenthesised", "(SELECT 1) UNION (SELECT 2)", true);
        row("with select", "WITH x AS (SELECT 1) SELECT * FROM x", true);
        row("with recursive",
            "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c "
            "WHERE n < 3) SELECT n FROM c",
            true);
        row("table", "TABLE t", true);
        row("show", "SHOW TABLES", true);
        row("explain select", "EXPLAIN SELECT 1", true);
        row("explain query plan", "EXPLAIN QUERY PLAN SELECT * FROM t", true);
        row("word inside a string", "SELECT 'insert into x' AS a, \"delete\" FROM t", true);
        row("word inside a comment", "SELECT 1 -- DELETE everything\n", true);
        row("for update only locks", "SELECT * FROM t FOR UPDATE", true);
        row("insert", "INSERT INTO t VALUES (1)", false);
        row("update", "UPDATE t SET a = 1", false);
        row("delete", "DELETE FROM t", false);
        row("create", "CREATE TABLE t(a)", false);
        row("drop", "DROP TABLE t", false);
        row("pragma", "PRAGMA table_info(t)", false);
        row("select into", "SELECT * INTO copy FROM t", false);
        row("with insert", "WITH x AS (SELECT 1) INSERT INTO t SELECT * FROM x", false);
        row("with delete in the cte", "WITH gone AS (DELETE FROM t RETURNING *) SELECT * FROM gone",
            false);
        row("explain analyze delete", "EXPLAIN ANALYZE DELETE FROM t", false);
        row("empty", "", false);
        row("only a comment", "-- nothing", false);
        row("a word that starts like select", "SELECTED 1", false);
    }

    void returnsRowsClassifiesStatements()
    {
        QFETCH(QString, sql);
        QFETCH(bool, rows);
        QCOMPARE(ExportController::returnsRows(sql), rows);
    }

    void infoWithoutResult()
    {
        Env e = start();
        QVERIFY(!e.exporter->info().value(QStringLiteral("available")).toBool());
        QVERIFY(!e.exporter->info().value(QStringLiteral("reason")).toString().isEmpty());

        const int row = e.tabs->openTab(e.connection);
        const QString id = e.tabs->idOf(row);
        const QVariantMap info = e.exporter->infoFor(id);
        QVERIFY(!info.value(QStringLiteral("available")).toBool());
        QVERIFY(!info.value(QStringLiteral("currentAllowed")).toBool());
        QVERIFY(!info.value(QStringLiteral("queryAllowed")).toBool());
        QVERIFY(info.value(QStringLiteral("reason")).toString().contains(QStringLiteral("result")));
        // Nothing to export: start() refuses and nothing changes.
        QSignalSpy states(e.exporter.get(), &ExportController::stateChanged);
        const QString why = e.exporter->start(request(id, outFile(QStringLiteral("none.csv"))));
        QVERIFY(!why.isEmpty());
        QCOMPARE(e.exporter->state(), QStringLiteral("idle"));
        QCOMPARE(states.count(), 0);
        QVERIFY(!QFileInfo::exists(outFile(QStringLiteral("none.csv"))));
    }

    void infoFollowsTheTab()
    {
        Env e = start();
        QSignalSpy changed(e.exporter.get(), &ExportController::infoChanged);
        const QString id = runTab(e, QStringLiteral("SELECT 1 AS a, 2 AS b"));
        QVERIFY(changed.count() > 0);
        QVERIFY(e.exporter->info().value(QStringLiteral("available")).toBool());
        const QVariantMap info = e.exporter->infoFor(id);
        QVERIFY(info.value(QStringLiteral("currentAllowed")).toBool());
        QVERIFY(info.value(QStringLiteral("queryAllowed")).toBool());
        QCOMPARE(info.value(QStringLiteral("loadedRows")).toInt(), 1);
        QVERIFY(!info.value(QStringLiteral("moreAvailable")).toBool());
        QCOMPARE(info.value(QStringLiteral("connectionName")).toString(), QStringLiteral("db"));
        QCOMPARE(info.value(QStringLiteral("defaultTable")).toString(),
                 QStringLiteral("export_table"));
        QCOMPARE(info.value(QStringLiteral("baseName")).toString(),
                 e.tabs->titleOf(e.tabs->currentIndex()));
    }

    // The statement of the grid is not a row-returning one: only the loaded rows can be exported.
    void nonSelectCannotRunAgain()
    {
        Env e = start();
        const QString id = runTab(e,
                                  QStringLiteral("CREATE TABLE t(a); INSERT INTO t VALUES (1);"
                                                 " PRAGMA table_info(t)"));
        const QVariantMap info = e.exporter->infoFor(id);
        QVERIFY(info.value(QStringLiteral("currentAllowed")).toBool());
        QVERIFY(!info.value(QStringLiteral("queryAllowed")).toBool());
        QVERIFY(info.value(QStringLiteral("queryReason"))
                    .toString()
                    .contains(QStringLiteral("SELECT")));
        QVERIFY(info.value(QStringLiteral("available")).toBool());

        const QString file = outFile(QStringLiteral("again.csv"));
        const QString why = e.exporter->start(request(id, file, QStringLiteral("query")));
        QCOMPARE(why, info.value(QStringLiteral("queryReason")).toString());
        QCOMPARE(e.exporter->state(), QStringLiteral("idle"));
        QVERIFY(!QFileInfo::exists(file));
        // The current result still goes out.
        QVERIFY(e.exporter->start(request(id, file)).isEmpty());
        waitState(*e.exporter, QStringLiteral("done"));
        QVERIFY(readAll(file).startsWith("cid,name,type,notnull,dflt_value,pk\n0,a,"));
    }

    void exportsTheCurrentResult()
    {
        Env e = start();
        const QString id = runTab(
            e,
            QStringLiteral("CREATE TABLE t(a, b); INSERT INTO t VALUES (1,'x'),(2,'y,z'),(3,NULL);"
                           " SELECT a, b FROM t"));
        const QString file = outFile(QStringLiteral("current.csv"));
        QSignalSpy states(e.exporter.get(), &ExportController::stateChanged);
        QSignalSpy said(e.exporter.get(), &ExportController::announcement);

        const QString why = e.exporter->start(request(id, file));
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QCOMPARE(e.exporter->state(), QStringLiteral("running")); // at once, never blocking
        QVERIFY(e.exporter->busy());
        QVERIFY(!e.exporter->cancelling());
        QCOMPARE(e.exporter->start(request(id, file)),
                 QStringLiteral("An export is already running."));
        waitState(*e.exporter, QStringLiteral("done"));
        QVERIFY(!e.exporter->busy());

        QCOMPARE(readAll(file), QByteArray("a,b\n1,x\n2,\"y,z\"\n3,\n"));
        QCOMPARE(e.exporter->rows(), 3);
        QVERIFY(e.exporter->bytes() > 0);
        QCOMPARE(QDir::fromNativeSeparators(e.exporter->filePath()),
                 QDir::fromNativeSeparators(file));
        QCOMPARE(e.exporter->progressText(),
                 QStringLiteral("%1 rows written").arg(QLocale().toString(qint64(3))));
        QVERIFY(e.exporter->resultText().startsWith(
            QStringLiteral("Exported %1 rows (").arg(QLocale().toString(qint64(3)))));
        QVERIFY(e.exporter->resultText().contains(QDir::toNativeSeparators(file)));
        QVERIFY(e.exporter->errorMessage().isEmpty());

        // Only the start and the end are announced; the end is polite.
        QCOMPARE(said.count(), 2);
        QCOMPARE(said.at(0).at(0).toString(), QStringLiteral("Exporting to current.csv"));
        QCOMPARE(said.at(1).at(0).toString(), e.exporter->resultText());
        QVERIFY(!said.at(1).at(1).toBool());
        QCOMPARE(states.count(), 2); // running, done

        e.exporter->reset();
        QCOMPARE(e.exporter->state(), QStringLiteral("idle"));
        QCOMPARE(e.exporter->rows(), 0);
        QVERIFY(e.exporter->resultText().isEmpty());
    }

    // The grid has the first page; the streaming scope writes every row without touching the tab.
    void streamsTheWholeQuery()
    {
        Env e = start();
        const QString id = runTab(e, kManyRows);
        QueryState *tab = e.queries->stateOf(id);
        const int loaded = tab->results()->rows();
        QVERIFY(loaded < 3000);
        QVERIFY(e.exporter->infoFor(id).value(QStringLiteral("moreAvailable")).toBool());
        QCOMPARE(e.queries->sessionCount(), 1);

        const QString file = outFile(QStringLiteral("stream.jsonl"));
        QSignalSpy progress(e.exporter.get(), &ExportController::progressChanged);
        const QString why = e.exporter->start(request(id, file, QStringLiteral("query"), 3));
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QVERIFY(e.exporter->state() == QLatin1String("preparing")
                || e.exporter->state() == QLatin1String("running"));
        waitState(*e.exporter, QStringLiteral("done"));
        QVERIFY(progress.count() >= 1);
        QCOMPARE(e.exporter->rows(), 3000);
        const QList<QByteArray> lines = readAll(file).trimmed().split('\n');
        QCOMPARE(lines.size(), 3000);
        QCOMPARE(lines.first(), QByteArray("{\"x\":1,\"label\":\"row 1\"}"));
        QCOMPARE(lines.last(), QByteArray("{\"x\":3000,\"label\":\"row 3000\"}"));
        // The tab and its grid were not touched, and the extra session is gone.
        QCOMPARE(tab->results()->rows(), loaded);
        QCOMPARE(tab->state(), QStringLiteral("done"));
        QCOMPARE(e.queries->sessionCount(), 1);

        // The same for the rows that are loaded.
        const QString part = outFile(QStringLiteral("part.csv"));
        e.exporter->reset();
        QVERIFY(e.exporter->start(request(id, part)).isEmpty());
        waitState(*e.exporter, QStringLiteral("done"));
        QCOMPARE(e.exporter->rows(), loaded);
    }

    // A table of `rows` rows, then a SELECT of all of it: the grid keeps the first page and the
    // export has far more to write than it can finish before the test cancels it.
    static QString bigSql(int rows)
    {
        return QStringLiteral("CREATE TABLE n AS WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT "
                              "x+1 FROM c WHERE x < %1) SELECT x FROM c; SELECT x FROM n")
            .arg(rows);
    }

    void cancelsARunningStreamAndKeepsTheOldFile()
    {
        Env e = start();
        const QString id = runTab(e, bigSql(kBigRows));
        const QString file = outFile(QStringLiteral("old.csv"));
        QFile old(file);
        QVERIFY(old.open(QIODevice::WriteOnly));
        old.write("keep me");
        old.close();
        QVERIFY(e.exporter->fileExists(file));

        QVERIFY(e.exporter->start(request(id, file, QStringLiteral("query"))).isEmpty());
        waitState(*e.exporter, QStringLiteral("running"));
        QSignalSpy said(e.exporter.get(), &ExportController::announcement);
        e.exporter->cancel();
        QVERIFY(e.exporter->cancelling());
        QVERIFY(e.exporter->busy()); // until the writer confirms
        e.exporter->cancel(); // Esc twice is harmless
        waitState(*e.exporter, QStringLiteral("cancelled"));
        QVERIFY(!e.exporter->busy());
        QVERIFY(!e.exporter->cancelling());
        QCOMPARE(readAll(file), QByteArray("keep me"));
        QCOMPARE(QDir(m_dir.path()).entryList({ QStringLiteral("old.csv*") }).size(), 1);
        QCOMPARE(said.count(), 1);
        QCOMPARE(said.at(0).at(0).toString(), QStringLiteral("Export cancelled"));
        QVERIFY(!said.at(0).at(1).toBool());
        QVERIFY(e.exporter->errorMessage().isEmpty());
        QCOMPARE(e.queries->sessionCount(), 1); // only the tab's own

        // The next export starts normally and replaces the file.
        e.exporter->reset();
        QVERIFY(e.exporter->start(request(id, file)).isEmpty());
        waitState(*e.exporter, QStringLiteral("done"));
        QVERIFY(readAll(file).startsWith("x\n1\n2\n"));
    }

    // Cancelling while the session is still opening (or the password is being read).
    void cancelsWhilePreparing()
    {
        Env e = start();
        const QString id = runTab(e, bigSql(1000));
        const QString file = outFile(QStringLiteral("prep.csv"));
        QVERIFY(e.exporter->start(request(id, file, QStringLiteral("query"))).isEmpty());
        QCOMPARE(e.exporter->state(), QStringLiteral("preparing"));
        QVERIFY(e.exporter->busy());
        QSignalSpy said(e.exporter.get(), &ExportController::announcement);
        e.exporter->cancel();
        QCOMPARE(e.exporter->state(), QStringLiteral("cancelled")); // nothing to wait for
        QVERIFY(!e.exporter->busy());
        QCOMPARE(said.count(), 1);
        // Give the dropped session time to close: nothing arrives afterwards.
        QTest::qWait(300);
        QCOMPARE(e.exporter->state(), QStringLiteral("cancelled"));
        QVERIFY(!QFileInfo::exists(file));
        e.exporter->reset();
        QVERIFY(e.exporter->start(request(id, file, QStringLiteral("query"))).isEmpty());
        waitState(*e.exporter, QStringLiteral("done"));
        QCOMPARE(e.exporter->rows(), 1000);
    }

    void pathErrors()
    {
        Env e = start();
        const QString id = runTab(e, QStringLiteral("SELECT 1 AS a"));
        QSignalSpy states(e.exporter.get(), &ExportController::stateChanged);
        QVERIFY(!e.exporter->start(request(id, QString())).isEmpty());
        QVERIFY(!e.exporter->start(request(id, QStringLiteral("   "))).isEmpty());
        const QString folder = m_dir.path();
        QVERIFY(e.exporter->start(request(id, folder)).contains(QStringLiteral("folder")));
        const QString missing = outFile(QStringLiteral("no/such/folder/x.csv"));
        QVERIFY(e.exporter->start(request(id, missing)).contains(QStringLiteral("does not exist")));
        QVERIFY(!e.exporter
                     ->start(request(id, outFile(QStringLiteral("x.csv")),
                                     QStringLiteral("current"), 9))
                     .isEmpty()); // no such format
        QCOMPARE(e.exporter->state(), QStringLiteral("idle"));
        QCOMPARE(states.count(), 0);
        QVERIFY(!e.exporter->fileExists(folder)); // a folder is not a file to replace
        QVERIFY(!e.exporter->fileExists(QString()));
    }

    // A destination the exporter cannot write ends in "failed" with the plain message and the
    // driver's detail, and leaves nothing behind.
    void diskErrorFails()
    {
        Env e = start();
        const QString id = runTab(e, QStringLiteral("SELECT 1 AS a"));
        const QString file = outFile(QStringLiteral("locked.csv"));
        QFile locked(file);
        QVERIFY(locked.open(QIODevice::WriteOnly));
        locked.write("old");
        locked.close();
        QVERIFY(locked.setPermissions(QFileDevice::ReadOwner));
        QSignalSpy said(e.exporter.get(), &ExportController::announcement);
        QVERIFY(e.exporter->start(request(id, file)).isEmpty());
        waitState(*e.exporter, QStringLiteral("failed"));
        QVERIFY(!e.exporter->errorMessage().isEmpty());
        QCOMPARE(said.last().at(0).toString(), e.exporter->errorMessage());
        QVERIFY(said.last().at(1).toBool()); // errors are assertive
        QVERIFY(!e.exporter->busy());
        QVERIFY(locked.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QCOMPARE(readAll(file), QByteArray("old"));
        QCOMPARE(QDir(m_dir.path()).entryList({ QStringLiteral("locked.csv*") }).size(), 1);
        e.exporter->reset();
        QCOMPARE(e.exporter->state(), QStringLiteral("idle"));
        QVERIFY(e.exporter->errorMessage().isEmpty());
    }

    // A data tab: the table name is the default one, and the whole table is read, not the grid.
    void dataTabExportsTheTable()
    {
        Env e = start();
        const QString setup
            = runTab(e,
                     QStringLiteral("CREATE TABLE people(id INTEGER PRIMARY KEY, name TEXT);"
                                    " INSERT INTO people(name) VALUES ('Ana'),('Luis'),(NULL)"));
        Q_UNUSED(setup)
        const int row
            = e.tabs->openData(e.connection, { QStringLiteral("people") }, QStringLiteral("table"));
        QVERIFY(row >= 0);
        const QString id = e.tabs->idOf(row);
        QueryState *tab = e.queries->stateOf(id);
        [&] { QTRY_COMPARE_WITH_TIMEOUT(tab->state(), QStringLiteral("done"), kWaitMs); }();
        const QVariantMap info = e.exporter->infoFor(id);
        QVERIFY(info.value(QStringLiteral("available")).toBool());
        QVERIFY(info.value(QStringLiteral("queryAllowed")).toBool());
        QCOMPARE(info.value(QStringLiteral("defaultTable")).toString(), QStringLiteral("people"));
        QCOMPARE(info.value(QStringLiteral("baseName")).toString(), QStringLiteral("people"));

        QVariantMap r
            = request(id, outFile(QStringLiteral("people.sql")), QStringLiteral("query"), 4);
        r.insert(QStringLiteral("tableName"), QStringLiteral("   ")); // blank: the default
        r.insert(QStringLiteral("rowsPerInsert"), 2);
        QVERIFY(e.exporter->start(r).isEmpty());
        waitState(*e.exporter, QStringLiteral("done"));
        QCOMPARE(e.exporter->rows(), 3);
        const QByteArray sql = readAll(outFile(QStringLiteral("people.sql")));
        QVERIFY2(sql.contains("INSERT INTO \"people\""), sql.constData());
        QVERIFY(sql.contains("'Ana'"));
        QVERIFY(sql.contains("NULL"));

        // The same table, as the rows of the grid (a rowid-less key: nothing hidden here).
        e.exporter->reset();
        r = request(id, outFile(QStringLiteral("people.md")), QStringLiteral("current"), 5);
        QVERIFY(e.exporter->start(r).isEmpty());
        waitState(*e.exporter, QStringLiteral("done"));
        QVERIFY(readAll(outFile(QStringLiteral("people.md"))).startsWith("| id | name |"));
    }

    // "Ask every time" and no password in memory: no export, and the text says what to do.
    void askEveryTimeNeedsThePasswordInMemory()
    {
        Env e = start();
        ConnectionProfile p;
        p.id = QUuid::createUuid();
        p.name = QStringLiteral("asks");
        p.providerId = QStringLiteral("sqlite");
        p.values = { { QStringLiteral("file"),
                       m_dir.filePath(QStringLiteral("data-%1.db").arg(++m_files)) } };
        p.passwordMode = PasswordMode::AskEveryTime;
        e.controller->model()->store()->save(p);
        const QString cid = p.id.toString(QUuid::WithoutBraces);
        QTRY_VERIFY_WITH_TIMEOUT(e.controller->model()->indexOf(cid) >= 0, kWaitMs);
        e.controller->rememberPassword(p.id, QStringLiteral("secret"));
        e.connection = cid;
        const QString id = runTab(e, QStringLiteral("SELECT 1 AS a"));
        QVERIFY(e.exporter->infoFor(id).value(QStringLiteral("queryAllowed")).toBool());

        const QString file = outFile(QStringLiteral("asks.csv"));
        // With the password in memory it runs, and never asks for it.
        QVERIFY(e.exporter->start(request(id, file, QStringLiteral("query"))).isEmpty());
        waitState(*e.exporter, QStringLiteral("done"));
        QCOMPARE(e.exporter->rows(), 1);
        e.exporter->reset();

        e.controller->forgetPassword(p.id);
        QVERIFY(!e.controller->sessionPassword(p.id).has_value());
        const QString why = e.exporter->start(request(id, file, QStringLiteral("query")));
        QVERIFY2(why.contains(QStringLiteral("Connect first")), qPrintable(why));
        QCOMPARE(e.exporter->state(), QStringLiteral("idle"));
        // The current result needs no connection at all.
        QVERIFY(e.exporter->start(request(id, file)).isEmpty());
        waitState(*e.exporter, QStringLiteral("done"));
    }

    void retargetsTheExtension()
    {
        Env e = start();
        const auto formats = e.exporter->formats();
        QCOMPARE(formats.size(), 6);
        QCOMPARE(formats.at(3).toMap().value(QStringLiteral("extension")).toString(),
                 QStringLiteral("jsonl"));
        QCOMPARE(formats.at(4).toMap().value(QStringLiteral("filter")).toString(),
                 QStringLiteral("SQL INSERT (*.sql)"));
        QCOMPARE(e.exporter->retarget(QStringLiteral("C:/out/data.csv"), 2),
                 QStringLiteral("C:/out/data.json"));
        QCOMPARE(e.exporter->retarget(QStringLiteral("C:/out/DATA.JSON"), 5),
                 QStringLiteral("C:/out/DATA.md"));
        // Not one of ours: what the user typed stays.
        QCOMPARE(e.exporter->retarget(QStringLiteral("C:/out/data.txt"), 2),
                 QStringLiteral("C:/out/data.txt"));
        QCOMPARE(e.exporter->retarget(QStringLiteral("C:/out/data"), 2),
                 QStringLiteral("C:/out/data"));
    }

    // The folder of the last export is the next suggestion.
    void remembersTheFolder()
    {
        Env e = start();
        const QString id = runTab(e, QStringLiteral("SELECT 1 AS a"));
        QDir(m_dir.path()).mkdir(QStringLiteral("remembered"));
        const QString file = outFile(QStringLiteral("remembered/first.csv"));
        QVERIFY(e.exporter->start(request(id, file)).isEmpty());
        waitState(*e.exporter, QStringLiteral("done"));
        const QString suggested = e.exporter->defaultPath(id, 2);
        QCOMPARE(QFileInfo(suggested).absolutePath(),
                 QFileInfo(outFile(QStringLiteral("remembered"))).absoluteFilePath());
        QVERIFY(suggested.endsWith(QStringLiteral(".json")));
    }

    // Closing the window while an export runs cancels it and does not hang.
    void destroyingCancels()
    {
        QString file;
        QElapsedTimer timer;
        {
            Env e = start();
            const QString id = runTab(e, bigSql(kBigRows));
            file = outFile(QStringLiteral("gone.csv"));
            QVERIFY(e.exporter->start(request(id, file, QStringLiteral("query"))).isEmpty());
            waitState(*e.exporter, QStringLiteral("running"));
            timer.start();
        }
        QVERIFY2(timer.elapsed() < 20000, "the destructor must not wait for the whole export");
        QVERIFY(!QFileInfo::exists(file));
        QCOMPARE(QDir(m_dir.path()).entryList({ QStringLiteral("gone.csv*") }).size(), 0);
    }
};

QTEST_MAIN(TstExportController)
#include "tst_export_controller.moc"
