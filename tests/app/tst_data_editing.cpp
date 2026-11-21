#include "ui/connection_controller.h"
#include "ui/editor_tabs_model.h"
#include "ui/query_controller.h"
#include "ui/schema_tree_model.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include <memory>

using namespace quarry;
using namespace quarry::ui;

namespace {

constexpr int kWaitMs = 20000;
const QString kUrlVariable = QStringLiteral("QUARRY_TEST_POSTGRES_URL");

} // namespace

// Editing table data (H2) through QueryController, end to end over the real SQLite plugin and, when
// QUARRY_TEST_POSTGRES_URL is set, over PostgreSQL: open a table, edit, preview, apply, the key
// choices, rollback on a failure, and the production confirmation. Tables are created and checked
// through ordinary query tabs.
class TstDataEditing : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    int m_files = 0;
    int m_tables = 0;

    struct Engine
    {
        bool pg = false;
        QUrl url;
        QString database;
    };

    // Declared in this order so the QueryController dies before the plugins' registry.
    struct Env
    {
        std::unique_ptr<ConnectionController> controller;
        std::unique_ptr<EditorTabsModel> tabs;
        std::unique_ptr<QueryController> queries;
        QString connection;
        QString name = QStringLiteral("db");
        Engine engine;
        QStringList created;
    };

    static Engine postgres()
    {
        Engine e;
        e.pg = true;
        e.url = QUrl(qEnvironmentVariable(qPrintable(kUrlVariable)));
        e.database = e.url.path().mid(1);
        return e;
    }

    Env start(const Engine &engine, bool production = false)
    {
        Env e;
        e.engine = engine;
        e.controller = std::make_unique<ConnectionController>();
        e.controller->setDatabasePath(
            m_dir.filePath(QStringLiteral("quarry-%1.db").arg(++m_files)));
        e.controller->setProviderDirectory(QStringLiteral(QUARRY_PLUGIN_DIR));
        e.controller->start();
        [&] { QTRY_VERIFY_WITH_TIMEOUT(!e.controller->model()->loading(), kWaitMs); }();

        ConnectionProfile p;
        p.id = QUuid::createUuid();
        p.name = e.name;
        p.production = production;
        if (engine.pg) {
            p.providerId = QStringLiteral("postgres");
            p.values = { { QStringLiteral("host"), engine.url.host() },
                         { QStringLiteral("port"), engine.url.port(5432) },
                         { QStringLiteral("user"), engine.url.userName() },
                         { QStringLiteral("database"), engine.database } };
            p.passwordMode = PasswordMode::AskEveryTime;
            e.controller->rememberPassword(p.id, engine.url.password());
        } else {
            p.providerId = QStringLiteral("sqlite");
            p.values = { { QStringLiteral("file"),
                           m_dir.filePath(QStringLiteral("data-%1.db").arg(++m_files)) } };
        }
        e.controller->model()->store()->save(p);
        e.connection = p.id.toString(QUuid::WithoutBraces);
        [&] {
            QTRY_VERIFY_WITH_TIMEOUT(e.controller->model()->indexOf(e.connection) >= 0, kWaitMs);
        }();

        e.tabs = std::make_unique<EditorTabsModel>();
        e.tabs->setDatabasePath(m_dir.filePath(QStringLiteral("tabs-%1.db").arg(++m_files)));
        e.tabs->setConnections(e.controller.get());
        e.tabs->setSaveDelay(60000);
        e.tabs->start();
        [&] { QTRY_VERIFY_WITH_TIMEOUT(e.tabs->loaded(), kWaitMs); }();
        e.queries = std::make_unique<QueryController>();
        e.queries->setTabs(e.tabs.get());
        return e;
    }

    // A fresh table name (PostgreSQL keeps tables between runs).
    QString table(Env &e, const QString &prefix)
    {
        const QString name = QStringLiteral("%1_%2_%3")
                                 .arg(prefix)
                                 .arg(QCoreApplication::applicationPid())
                                 .arg(++m_tables);
        e.created << name;
        return name;
    }

    // Runs `sql` in a query tab and waits for the end; returns the tab's state.
    static QueryState *query(Env &e, const QString &sql)
    {
        const int row = e.tabs->openTab(e.connection);
        e.tabs->setText(row, sql);
        e.tabs->run(row, true, static_cast<int>(sql.size()));
        QueryState *s = e.queries->stateOf(e.tabs->idOf(row));
        [&] { QTRY_COMPARE_WITH_TIMEOUT(s->state(), QStringLiteral("done"), kWaitMs); }();
        return s;
    }

    static QStringList rowsOf(Env &e, const QString &sql)
    {
        QueryState *s = query(e, sql);
        QStringList out;
        for (int r = 0; r < s->results()->rows(); ++r) {
            QStringList cells;
            for (int c = 0; c < s->results()->columns(); ++c)
                cells << s->results()->cellText(r, c);
            out << cells.join(QLatin1Char('|'));
        }
        return out;
    }

    // Opens the data tab of a table and waits until its rows are in.
    static QString openData(Env &e, const QString &name, int rows,
                            const QString &kind = QStringLiteral("table"))
    {
        const QStringList parts = e.engine.pg
            ? QStringList { e.engine.database, QStringLiteral("public"), name }
            : QStringList { QStringLiteral("main"), name };
        const int row = e.tabs->openData(e.connection, parts, kind);
        const QString id = e.tabs->idOf(row);
        QueryState *s = e.queries->stateOf(id);
        [&] {
            QTRY_COMPARE_WITH_TIMEOUT(s->state(), QStringLiteral("done"), kWaitMs);
            QTRY_COMPARE_WITH_TIMEOUT(s->results()->rows(), rows, kWaitMs);
        }();
        return id;
    }

    static void drop(Env &e)
    {
        for (const QString &name : std::as_const(e.created)) {
            const QString what
                = QStringLiteral("DROP VIEW IF EXISTS \"v_%1\"; DROP TABLE IF EXISTS \"%1\";")
                      .arg(name);
            query(e, what);
        }
    }

    // The five-row table most tests start from.
    QString makePeople(Env &e, const QString &extra = {})
    {
        const QString t = table(e, QStringLiteral("people"));
        query(e,
              QStringLiteral("CREATE TABLE \"%1\"(id INTEGER PRIMARY KEY, name TEXT%2, note TEXT);"
                             "INSERT INTO \"%1\" VALUES (1,'a','x'),(2,'b','y'),(3,'c','z');")
                  .arg(t, extra));
        return t;
    }

    void endToEnd(const Engine &engine)
    {
        Env e = start(engine);
        const QString t = makePeople(e);
        const QString id = openData(e, t, 3);
        QueryState *s = e.queries->stateOf(id);
        TableEditModel *edits = s->edits();
        QVERIFY(edits->active());
        QVERIFY(edits->editable());
        QVERIFY(edits->keyWarning().isEmpty()); // it has a primary key
        QCOMPARE(edits->columnCount(), 3);

        QVERIFY(edits->setCell(0, 1, QStringLiteral("Zed")));
        QVERIFY(edits->setNull(1, 2));
        const int created = edits->insertRow();
        QCOMPARE(created, 3);
        QVERIFY(edits->setCell(created, 0, QStringLiteral("10")));
        QVERIFY(edits->setCell(created, 1, QStringLiteral("New")));
        QCOMPARE(edits->deleteRows(2, 2), 1);
        QCOMPARE(edits->pendingCount(), 4);

        // The preview is what the session receives.
        const QVariantList preview = e.queries->previewStatements(id);
        QCOMPARE(preview.size(), 4);
        QVERIFY(preview[0].toMap()[QStringLiteral("sql")].toString().startsWith(
            QLatin1String("UPDATE")));
        QVERIFY(preview[2].toMap()[QStringLiteral("sql")].toString().startsWith(
            QLatin1String("DELETE")));
        QVERIFY(preview[3].toMap()[QStringLiteral("sql")].toString().startsWith(
            QLatin1String("INSERT")));
        const QVariantList firstParams = preview[0].toMap()[QStringLiteral("parameters")].toList();
        QCOMPARE(firstParams.size(), 2);
        QCOMPARE(firstParams[0].toMap()[QStringLiteral("value")].toString(), QStringLiteral("Zed"));
        QVERIFY(preview[1]
                    .toMap()[QStringLiteral("parameters")]
                    .toList()[0]
                    .toMap()[QStringLiteral("isNull")]
                    .toBool());

        QSignalSpy sent(e.queries.get(), &QueryController::changesSent);
        QSignalSpy saved(e.queries.get(), &QueryController::changesSaved);
        QVERIFY(e.queries->apply(id));
        QVERIFY(saved.wait(kWaitMs));
        QCOMPARE(sent.count(), 1);
        QCOMPARE(sent.at(0).at(1).toList(), preview);
        QCOMPARE(saved.at(0).at(1).toInt(), 4);

        // Pending changes are gone and the table was read again.
        QCOMPARE(edits->pendingCount(), 0);
        [&] {
            QTRY_COMPARE_WITH_TIMEOUT(s->state(), QStringLiteral("done"), kWaitMs);
            QTRY_COMPARE_WITH_TIMEOUT(s->results()->rows(), 3, kWaitMs);
        }();
        const QStringList expected { QStringLiteral("1|Zed|x"), QStringLiteral("2|b|NULL"),
                                     QStringLiteral("10|New|NULL") };
        QCOMPARE(rowsOf(e, QStringLiteral("SELECT id, name, note FROM \"%1\" ORDER BY id").arg(t)),
                 expected);
        drop(e);
    }

    // The second statement breaks a UNIQUE constraint: the first one must not stay.
    void rollsBack(const Engine &engine)
    {
        Env e = start(engine);
        const QString t = makePeople(e);
        query(e, QStringLiteral("CREATE UNIQUE INDEX \"u_%1\" ON \"%1\"(name)").arg(t));
        const QString id = openData(e, t, 3);
        TableEditModel *edits = e.queries->stateOf(id)->edits();
        QVERIFY(edits->setCell(0, 1, QStringLiteral("zz")));
        QVERIFY(edits->setCell(1, 1, QStringLiteral("c"))); // duplicate of row 3
        QSignalSpy failed(e.queries.get(), &QueryController::planFailed);
        QSignalSpy focus(edits, &TableEditModel::focusRowRequested);
        QSignalSpy said(e.queries.get(), &QueryController::announcement);
        QVERIFY(e.queries->apply(id));
        QVERIFY(failed.wait(kWaitMs));
        QCOMPARE(edits->pendingCount(), 2); // still pending
        QVERIFY(edits->lastError().startsWith(QLatin1String("Row 2:")));
        QCOMPARE(focus.count(), 1);
        QCOMPARE(focus.at(0).at(0).toInt(), 1);
        QVERIFY(!said.isEmpty());
        QVERIFY(said.last().at(0).toString().startsWith(QLatin1String("Row 2:")));
        QVERIFY(said.last().at(1).toBool()); // assertive
        QCOMPARE(rowsOf(e, QStringLiteral("SELECT name FROM \"%1\" ORDER BY id").arg(t)),
                 (QStringList { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") }));
        // Fix the duplicate and save again.
        QVERIFY(edits->setCell(1, 1, QStringLiteral("d")));
        QSignalSpy saved(e.queries.get(), &QueryController::changesSaved);
        QVERIFY(e.queries->apply(id));
        QVERIFY(saved.wait(kWaitMs));
        QCOMPARE(rowsOf(e, QStringLiteral("SELECT name FROM \"%1\" ORDER BY id").arg(t)),
                 (QStringList { QStringLiteral("zz"), QStringLiteral("d"), QStringLiteral("c") }));
        drop(e);
    }

    void changedByAnother(const Engine &engine)
    {
        Env e = start(engine);
        const QString t = makePeople(e);
        const QString id = openData(e, t, 3);
        TableEditModel *edits = e.queries->stateOf(id)->edits();
        QVERIFY(edits->setCell(0, 1, QStringLiteral("mine")));
        QVERIFY(edits->setCell(1, 1, QStringLiteral("mine too")));
        query(e, QStringLiteral("DELETE FROM \"%1\" WHERE id = 2").arg(t)); // someone else
        QSignalSpy failed(e.queries.get(), &QueryController::planFailed);
        QVERIFY(e.queries->apply(id));
        QVERIFY(failed.wait(kWaitMs));
        QVERIFY(edits->lastError().startsWith(QLatin1String("Row 2:")));
        QVERIFY(edits->lastError().contains(QLatin1String("not found")));
        QCOMPARE(edits->pendingCount(), 2);
        QCOMPARE(rowsOf(e, QStringLiteral("SELECT name FROM \"%1\" ORDER BY id").arg(t)),
                 (QStringList { QStringLiteral("a"), QStringLiteral("c") })); // nothing saved
        drop(e);
    }

    void noPrimaryKey(const Engine &engine)
    {
        Env e = start(engine);
        const QString t = table(e, QStringLiteral("nokey"));
        query(e,
              QStringLiteral("CREATE TABLE \"%1\"(a INTEGER, b TEXT);"
                             "INSERT INTO \"%1\" VALUES (1,'x'),(2,'y');")
                  .arg(t));
        const QString id = openData(e, t, 2);
        TableEditModel *edits = e.queries->stateOf(id)->edits();
        QVERIFY(edits->editable());
        const QString identifier = engine.pg ? QStringLiteral("ctid") : QStringLiteral("rowid");
        QVERIFY2(edits->keyWarning().contains(identifier), qPrintable(edits->keyWarning()));
        QCOMPARE(edits->columnCount(), 2); // the identifier column is hidden
        QVERIFY(edits->setCell(1, 1, QStringLiteral("changed")));
        QSignalSpy saved(e.queries.get(), &QueryController::changesSaved);
        QVERIFY(e.queries->apply(id));
        QVERIFY(saved.wait(kWaitMs));
        QCOMPARE(rowsOf(e, QStringLiteral("SELECT a, b FROM \"%1\" ORDER BY a").arg(t)),
                 (QStringList { QStringLiteral("1|x"), QStringLiteral("2|changed") }));
        drop(e);
    }

    // H11: the order and the filters reach the server as SQL, the values as parameters.
    void sortAndFilter(const Engine &engine)
    {
        Env e = start(engine);
        const QString t = makePeople(e);
        const QString id = openData(e, t, 3);
        QueryState *s = e.queries->stateOf(id);
        QCOMPARE(s->results()->pageRows(), ResultTableModel::kDataPageRows);
        const auto waitRows = [&](int rows) {
            QTRY_COMPARE_WITH_TIMEOUT(s->state(), QStringLiteral("done"), kWaitMs);
            QTRY_COMPARE_WITH_TIMEOUT(s->results()->rows(), rows, kWaitMs);
        };

        QVariantMap sorted;
        sorted.insert(QStringLiteral("sortColumn"), QStringLiteral("name"));
        sorted.insert(QStringLiteral("descending"), true);
        QVERIFY(e.queries->setDataView(id, sorted));
        waitRows(3);
        QCOMPARE(s->results()->cellText(0, 1), QStringLiteral("c"));
        QCOMPARE(s->results()->cellText(2, 1), QStringLiteral("a"));
        const QVariantMap active = e.queries->dataViewOf(id);
        QCOMPARE(active.value(QStringLiteral("sortColumn")).toString(), QStringLiteral("name"));

        // A value with a quote travels as a parameter, so it is just text.
        const auto filtered = [](const QString &op, const QString &value) {
            QVariantMap f;
            f.insert(QStringLiteral("column"), QStringLiteral("note"));
            f.insert(QStringLiteral("op"), op);
            f.insert(QStringLiteral("value"), value);
            QVariantMap view;
            view.insert(QStringLiteral("filters"), QVariantList { f });
            return view;
        };
        const auto apply = [&](const QString &op, const QString &value) {
            return e.queries->setDataView(id, filtered(op, value));
        };
        QVERIFY(apply(QStringLiteral("notEquals"), QStringLiteral("y'; --")));
        waitRows(3);
        QVERIFY(apply(QStringLiteral("equals"), QStringLiteral("y")));
        waitRows(1);
        QCOMPARE(s->results()->cellText(0, 1), QStringLiteral("b"));
        QVERIFY(apply(QStringLiteral("isNull"), {}));
        waitRows(0);

        // The view survives a refresh, and the rows stay editable.
        QVERIFY(apply(QStringLiteral("startsWith"), QStringLiteral("z")));
        waitRows(1);
        e.queries->reload(id);
        waitRows(1);
        TableEditModel *edits = s->edits();
        QVERIFY(edits->editable());

        // Pending changes block a new view; a bad operator is refused.
        edits->setCell(0, 1, QStringLiteral("changed"));
        QVERIFY(!e.queries->setDataView(id, {}));
        edits->discardAll();
        QVERIFY(!apply(QStringLiteral("nope"), {}));
        QVERIFY(e.queries->setDataView(id, {}));
        waitRows(3);
        drop(e);
    }

    // The filter box: one SQL condition typed by the user (`where`), checked before it is sent.
    void whereCondition(const Engine &engine)
    {
        Env e = start(engine);
        const QString t = makePeople(e);
        const QString id = openData(e, t, 3);
        QueryState *s = e.queries->stateOf(id);
        const auto waitRows = [&](int rows) {
            QTRY_COMPARE_WITH_TIMEOUT(s->state(), QStringLiteral("done"), kWaitMs);
            QTRY_COMPARE_WITH_TIMEOUT(s->results()->rows(), rows, kWaitMs);
        };
        const auto viewWith = [](const QString &where) {
            QVariantMap view;
            view.insert(QStringLiteral("where"), where);
            return view;
        };
        QSignalSpy failed(e.queries.get(), &QueryController::viewFailed);

        // A valid condition filters and is kept by dataViewOf and by a refresh.
        const QString condition = QStringLiteral("id > 1 AND (name = 'b' OR note = 'z')");
        QVERIFY(e.queries->setDataView(id, viewWith(condition)));
        waitRows(2);
        QCOMPARE(e.queries->dataViewOf(id).value(QStringLiteral("where")).toString(), condition);
        e.queries->reload(id);
        waitRows(2);
        QCOMPARE(e.queries->dataViewOf(id).value(QStringLiteral("where")).toString(), condition);
        QVERIFY(!s->results()->moreAvailable());
        QVERIFY(s->edits()->editable());
        QVERIFY(failed.isEmpty());

        // `;`, a comment or an unbalanced parenthesis: refused, said, and the view stays.
        for (const QString &bad : { QStringLiteral("id = 1; DROP TABLE x"),
                                    QStringLiteral("id = 1 -- all"), QStringLiteral("(id = 1") }) {
            failed.clear();
            QVERIFY2(!e.queries->setDataView(id, viewWith(bad)), qPrintable(bad));
            QCOMPARE(failed.size(), 1);
            QCOMPARE(failed.first().at(0).toString(), id);
            QVERIFY(!failed.first().at(1).toString().isEmpty());
            QCOMPARE(e.queries->dataViewOf(id).value(QStringLiteral("where")).toString(),
                     condition);
            QCOMPARE(s->results()->rows(), 2);
            QCOMPARE(s->state(), QStringLiteral("done"));
        }

        // A condition the server rejects (a column that is not there): the error is said for the
        // filter box and is in Messages; the table is empty, which is the documented limit.
        failed.clear();
        QVERIFY(e.queries->setDataView(id, viewWith(QStringLiteral("nosuchcolumn = 1"))));
        QTRY_COMPARE_WITH_TIMEOUT(s->state(), QStringLiteral("failed"), kWaitMs);
        QCOMPARE(failed.size(), 1);
        QVERIFY(!failed.first().at(1).toString().isEmpty());
        QVERIFY(!s->edits()->lastError().isEmpty());
        QVERIFY(s->messages()->count() > 0);

        // Clearing restores every row; the other view fields survive.
        QVERIFY(e.queries->setDataView(id, viewWith({})));
        waitRows(3);
        QCOMPARE(e.queries->dataViewOf(id).value(QStringLiteral("where")).toString(), QString());
        // Pending changes block a new condition, as they block a new order.
        s->edits()->setCell(0, 1, QStringLiteral("changed"));
        QVERIFY(!e.queries->setDataView(id, viewWith(QStringLiteral("id = 1"))));
        s->edits()->discardAll();
        drop(e);
    }

private slots:
    void whereConditionSqlite() { whereCondition({}); }
    void whereConditionPostgres()
    {
        if (!qEnvironmentVariableIsSet(qPrintable(kUrlVariable)))
            QSKIP("QUARRY_TEST_POSTGRES_URL is not set");
        whereCondition(postgres());
    }

    // The rows a table loads are a setting; "Count rows" gives the total of the view on the
    // tree's session and leaves the rows of the tab alone.
    void rowsToLoadAndCount(const Engine &engine)
    {
        Env e = start(engine);
        QCOMPARE(e.queries->dataPageRows(), ResultTableModel::kDataPageRows);
        e.queries->setDataPageRows(0);
        QCOMPARE(e.queries->dataPageRows(), QueryController::kMinDataPageRows);
        e.queries->setDataPageRows(99999);
        QCOMPARE(e.queries->dataPageRows(), QueryController::kMaxDataPageRows);
        QCOMPARE(QueryController::clampDataPageRows(-3), 1);
        QCOMPARE(QueryController::clampDataPageRows(250), 250);

        const QString t = table(e, QStringLiteral("many"));
        query(e,
              QStringLiteral("CREATE TABLE \"%1\"(id INTEGER PRIMARY KEY, name TEXT);"
                             "INSERT INTO \"%1\" VALUES (1,'a'),(2,'b'),(3,'c'),(4,'d'),(5,'e'),"
                             "(6,'f'),(7,'g'),(8,'h');")
                  .arg(t));
        e.queries->setDataPageRows(5);
        const QString id = openData(e, t, 5);
        QueryState *s = e.queries->stateOf(id);
        QCOMPARE(s->results()->pageRows(), 5);
        QVERIFY(s->results()->moreAvailable());
        QCOMPARE(s->countedTotal(), qint64 { -1 });
        QVERIFY(!s->counting());
        const auto waitRows = [&](int rows) {
            QTRY_COMPARE_WITH_TIMEOUT(s->state(), QStringLiteral("done"), kWaitMs);
            QTRY_COMPARE_WITH_TIMEOUT(s->results()->rows(), rows, kWaitMs);
        };

        // The count runs on the session of the tree: it has to be open.
        SchemaTreeModel *tree = e.controller->tree();
        [&] {
            QTRY_VERIFY_WITH_TIMEOUT(
                tree->ensureSession(e.connection) == SchemaTreeModel::Access::Connected, kWaitMs);
        }();
        QSignalSpy changes(s, &QueryState::countChanged);
        e.queries->countRows(id);
        QVERIFY(s->counting());
        QTRY_VERIFY_WITH_TIMEOUT(!s->counting(), kWaitMs);
        QCOMPARE(s->countedTotal(), qint64 { 8 });
        QCOMPARE(s->countError(), QString());
        QVERIFY(changes.count() >= 2);
        // The grid and its stream are untouched.
        QCOMPARE(s->results()->rows(), 5);
        QVERIFY(s->results()->moreAvailable());
        QCOMPARE(s->state(), QStringLiteral("done"));

        // The view's filter is in the count; changing the view forgets the old one.
        QVariantMap view;
        view.insert(QStringLiteral("where"), QStringLiteral("id > 2"));
        QVERIFY(e.queries->setDataView(id, view));
        waitRows(5);
        QCOMPARE(s->countedTotal(), qint64 { -1 });
        e.queries->countRows(id);
        QTRY_VERIFY_WITH_TIMEOUT(!s->counting(), kWaitMs);
        QCOMPARE(s->countedTotal(), qint64 { 6 });
        QCOMPARE(s->results()->rows(), 5);
        // A refresh forgets it as well.
        e.queries->reload(id);
        waitRows(5);
        QCOMPARE(s->countedTotal(), qint64 { -1 });

        // Cancelled at once: no total and no error, and it can be counted again.
        e.queries->countRows(id);
        QVERIFY(s->counting());
        e.queries->cancelCount(id);
        QVERIFY(!s->counting());
        QCOMPARE(s->countedTotal(), qint64 { -1 });
        QCOMPARE(s->countError(), QString());
        e.queries->countRows(id);
        QTRY_VERIFY_WITH_TIMEOUT(!s->counting() && s->countedTotal() == 6, kWaitMs);

        // A table that is gone (the tab's stream is read to its end first: an open cursor would
        // keep the table locked): the failure is the tab's count error and a message.
        s->results()->fetchAll();
        QTRY_VERIFY_WITH_TIMEOUT(!s->results()->moreAvailable() && !s->results()->fetchingAll(),
                                 kWaitMs);
        query(e, QStringLiteral("DROP TABLE \"%1\";").arg(t));
        const int messages = s->messages()->rowCount();
        e.queries->countRows(id);
        QTRY_VERIFY_WITH_TIMEOUT(!s->counting() && !s->countError().isEmpty(), kWaitMs);
        QCOMPARE(s->countedTotal(), qint64 { -1 });
        QVERIFY(s->messages()->rowCount() > messages);
        // The rows that were loaded (the 6 of the view) are still there.
        QCOMPARE(s->results()->rows(), 6);
        drop(e);
    }

    void sortAndFilterSqlite() { sortAndFilter({}); }
    void sortAndFilterPostgres()
    {
        if (!qEnvironmentVariableIsSet(qPrintable(kUrlVariable)))
            QSKIP("QUARRY_TEST_POSTGRES_URL is not set");
        sortAndFilter(postgres());
    }

    void rowsToLoadAndCountSqlite() { rowsToLoadAndCount({}); }
    void rowsToLoadAndCountPostgres()
    {
        if (!qEnvironmentVariableIsSet(qPrintable(kUrlVariable)))
            QSKIP("QUARRY_TEST_POSTGRES_URL is not set");
        rowsToLoadAndCount(postgres());
    }

    void endToEndSqlite() { endToEnd({}); }
    void endToEndPostgres()
    {
        if (!qEnvironmentVariableIsSet(qPrintable(kUrlVariable)))
            QSKIP("QUARRY_TEST_POSTGRES_URL is not set");
        endToEnd(postgres());
    }

    void failureRollsBackSqlite() { rollsBack({}); }
    void failureRollsBackPostgres()
    {
        if (!qEnvironmentVariableIsSet(qPrintable(kUrlVariable)))
            QSKIP("QUARRY_TEST_POSTGRES_URL is not set");
        rollsBack(postgres());
    }

    void rowChangedByAnotherSqlite() { changedByAnother({}); }
    void rowChangedByAnotherPostgres()
    {
        if (!qEnvironmentVariableIsSet(qPrintable(kUrlVariable)))
            QSKIP("QUARRY_TEST_POSTGRES_URL is not set");
        changedByAnother(postgres());
    }

    void tableWithoutPrimaryKeyUsesTheRowIdentifierSqlite() { noPrimaryKey({}); }
    void tableWithoutPrimaryKeyUsesTheRowIdentifierPostgres()
    {
        if (!qEnvironmentVariableIsSet(qPrintable(kUrlVariable)))
            QSKIP("QUARRY_TEST_POSTGRES_URL is not set");
        noPrimaryKey(postgres());
    }

    // SQLite: a real column called rowid hides the alias, so _rowid_ is used.
    void shadowedRowidUsesUnderscoreRowid()
    {
        Env e = start({});
        const QString t = table(e, QStringLiteral("shadow"));
        query(e,
              QStringLiteral("CREATE TABLE \"%1\"(rowid TEXT, v TEXT);"
                             "INSERT INTO \"%1\" VALUES ('r1','a'),('r2','b');")
                  .arg(t));
        const QString id = openData(e, t, 2);
        TableEditModel *edits = e.queries->stateOf(id)->edits();
        QVERIFY(edits->editable());
        QVERIFY2(edits->keyWarning().contains(QLatin1String("_rowid_")),
                 qPrintable(edits->keyWarning()));
        QCOMPARE(edits->target().key.columns, QStringList { QStringLiteral("_rowid_") });
        QVERIFY(edits->setCell(1, 1, QStringLiteral("B")));
        QSignalSpy saved(e.queries.get(), &QueryController::changesSaved);
        QVERIFY(e.queries->apply(id));
        QVERIFY(saved.wait(kWaitMs));
        QCOMPARE(rowsOf(e, QStringLiteral("SELECT rowid, v FROM \"%1\" ORDER BY rowid").arg(t)),
                 (QStringList { QStringLiteral("r1|a"), QStringLiteral("r2|B") }));
    }

    // All three names taken: no way to identify a row, so the tab is read-only.
    void noUsableIdentifierIsReadOnly()
    {
        Env e = start({});
        const QString t = table(e, QStringLiteral("allnames"));
        query(e,
              QStringLiteral("CREATE TABLE \"%1\"(rowid, _rowid_, oid);"
                             "INSERT INTO \"%1\" VALUES (1,2,3);")
                  .arg(t));
        const QString id = openData(e, t, 1);
        TableEditModel *edits = e.queries->stateOf(id)->edits();
        QVERIFY(!edits->editable());
        QVERIFY(edits->readOnlyReason().contains(QLatin1String("no primary key")));
    }

    void viewsAreReadOnly()
    {
        Env e = start({});
        const QString t = makePeople(e);
        query(e, QStringLiteral("CREATE VIEW \"v_%1\" AS SELECT * FROM \"%1\"").arg(t));
        const QString id = openData(e, QStringLiteral("v_%1").arg(t), 3, QStringLiteral("view"));
        TableEditModel *edits = e.queries->stateOf(id)->edits();
        QVERIFY(edits->active());
        QVERIFY(!edits->editable());
        QVERIFY(edits->readOnlyReason().contains(QLatin1String("view")));
        QVERIFY(!edits->setCell(0, 1, QStringLiteral("x")));
        QVERIFY(!e.queries->apply(id));
    }

    void blobCellsAreReadOnly()
    {
        Env e = start({});
        const QString t = table(e, QStringLiteral("blobs"));
        query(e,
              QStringLiteral("CREATE TABLE \"%1\"(id INTEGER PRIMARY KEY, data BLOB, name TEXT);"
                             "INSERT INTO \"%1\" VALUES (1, x'0102', 'n');")
                  .arg(t));
        const QString id = openData(e, t, 1);
        TableEditModel *edits = e.queries->stateOf(id)->edits();
        QVERIFY(!edits->blockReason(0, 1).isEmpty());
        QVERIFY(edits->blockReason(0, 2).isEmpty());
    }

    void emptyStringIsSentAsAnEmptyStringNotNull()
    {
        Env e = start({});
        const QString t = makePeople(e);
        const QString id = openData(e, t, 3);
        TableEditModel *edits = e.queries->stateOf(id)->edits();
        QVERIFY(edits->setCell(0, 1, QString()));
        const QVariantList preview = e.queries->previewStatements(id);
        QCOMPARE(preview.size(), 1);
        const QVariantMap p = preview[0].toMap()[QStringLiteral("parameters")].toList()[0].toMap();
        QVERIFY(!p[QStringLiteral("isNull")].toBool());
        QCOMPARE(p[QStringLiteral("text")].toString(), QStringLiteral("''"));
    }

    void productionNeedsTheConnectionNameTyped()
    {
        Env e = start({}, true);
        const QString t = makePeople(e);
        const QString id = openData(e, t, 3);
        TableEditModel *edits = e.queries->stateOf(id)->edits();
        QVERIFY(edits->setCell(0, 1, QStringLiteral("prod")));
        QSignalSpy asked(e.queries.get(), &QueryController::applyConfirmationRequested);
        QSignalSpy sent(e.queries.get(), &QueryController::changesSent);
        QSignalSpy saved(e.queries.get(), &QueryController::changesSaved);
        QVERIFY(e.queries->apply(id));
        QCOMPARE(asked.count(), 1);
        QCOMPARE(asked.at(0).at(0).toMap()[QStringLiteral("connectionName")].toString(), e.name);
        QCOMPARE(sent.count(), 0); // nothing goes out without the name
        QVERIFY(!e.queries->apply(id)); // already waiting
        QVERIFY(!e.queries->confirmApply(id, QString()));
        QVERIFY(!e.queries->confirmApply(id, QStringLiteral("DB")));
        QVERIFY(!e.queries->confirmApply(id, QStringLiteral("db ")));
        QCOMPARE(sent.count(), 0);
        // Declining leaves the changes pending and sends nothing.
        e.queries->declineApply(id);
        QCOMPARE(edits->pendingCount(), 1);
        QVERIFY(!e.queries->confirmApply(id, e.name));
        QCOMPARE(sent.count(), 0);
        // Ask again and type the name.
        QVERIFY(e.queries->apply(id));
        QVERIFY(e.queries->confirmApply(id, e.name));
        QVERIFY(saved.wait(kWaitMs));
        QCOMPARE(sent.count(), 1);
        QCOMPARE(rowsOf(e, QStringLiteral("SELECT name FROM \"%1\" WHERE id = 1").arg(t)),
                 QStringList { QStringLiteral("prod") });
    }

    // applyChanges is queued to the session thread: right after apply() the answer cannot be in.
    void nothingChangesWhileASaveIsInFlight()
    {
        Env e = start({});
        const QString t = makePeople(e);
        const QString id = openData(e, t, 3);
        TableEditModel *edits = e.queries->stateOf(id)->edits();
        QVERIFY(edits->setCell(0, 1, QStringLiteral("saved")));
        QSignalSpy saved(e.queries.get(), &QueryController::changesSaved);
        QVERIFY(e.queries->apply(id));
        QVERIFY(edits->saving());
        QVERIFY(!edits->setCell(1, 1, QStringLiteral("late")));
        QVERIFY(!edits->setNull(1, 1));
        QCOMPARE(edits->insertRow(), -1);
        QCOMPARE(edits->deleteRows(0, 0), -1);
        QCOMPARE(edits->undo(), QString());
        edits->discardAll();
        QCOMPARE(edits->pendingCount(), 1);
        e.queries->reload(id); // refused: the transaction is still running
        QCOMPARE(edits->pendingCount(), 1);
        QVERIFY(!e.queries->apply(id));
        QVERIFY(saved.wait(kWaitMs));
        QCOMPARE(saved.count(), 1);
        QVERIFY(!edits->saving());
        QCOMPARE(edits->pendingCount(), 0);
        // Editing works again once the answer is in.
        QTRY_COMPARE_WITH_TIMEOUT(e.queries->stateOf(id)->results()->rows(), 3, kWaitMs);
        QVERIFY(edits->setCell(1, 1, QStringLiteral("after")));
        QCOMPARE(
            rowsOf(e, QStringLiteral("SELECT name FROM \"%1\" ORDER BY id").arg(t)),
            (QStringList { QStringLiteral("saved"), QStringLiteral("b"), QStringLiteral("c") }));
    }

    void pendingChangesAreCountedAndReloadDropsThem()
    {
        Env e = start({});
        const QString t = makePeople(e);
        const QString id = openData(e, t, 3);
        TableEditModel *edits = e.queries->stateOf(id)->edits();
        QCOMPARE(e.queries->pendingChanges(id), 0);
        QCOMPARE(e.queries->pendingTotal(), 0);
        edits->setCell(0, 1, QStringLiteral("x"));
        edits->insertRow();
        QCOMPARE(e.queries->pendingChanges(id), 2);
        QCOMPARE(e.queries->pendingTotal(), 2);
        e.queries->reload(id);
        QCOMPARE(e.queries->pendingChanges(id), 0);
        QueryState *s = e.queries->stateOf(id);
        QTRY_COMPARE_WITH_TIMEOUT(s->state(), QStringLiteral("done"), kWaitMs);
        QTRY_COMPARE_WITH_TIMEOUT(s->results()->rows(), 3, kWaitMs);
        QCOMPARE(edits->rowCount(), 3);
    }

    void aDataTabIsOpenedOnceAndNeverStored()
    {
        Env e = start({});
        const QString t = makePeople(e);
        const QString id = openData(e, t, 3);
        const int before = e.tabs->count();
        const int again = e.tabs->openData(e.connection, { QStringLiteral("main"), t },
                                           QStringLiteral("table"));
        QCOMPARE(e.tabs->count(), before); // the same tab is shown
        QCOMPARE(e.tabs->idOf(again), id);
        QVERIFY(e.tabs->isData(again));
        QCOMPARE(e.tabs->titleOf(again), t);
        QVERIFY(!e.tabs->run(again, true, 0).value(QStringLiteral("ok")).toBool());
        QCOMPARE(
            e.tabs->openData(e.connection, { QStringLiteral("main"), t }, QStringLiteral("column")),
            -1);
        e.tabs->flush();
        // A second model on the same store sees only the query tabs.
        EditorTabsModel other;
        other.setDatabasePath(e.tabs->databasePath());
        other.setConnections(e.controller.get());
        other.start();
        QTRY_VERIFY_WITH_TIMEOUT(other.loaded(), kWaitMs);
        for (int i = 0; i < other.count(); ++i)
            QVERIFY(!other.isData(i));
        QVERIFY(other.count() < e.tabs->count());
    }
};

QTEST_MAIN(TstDataEditing)
#include "tst_data_editing.moc"
