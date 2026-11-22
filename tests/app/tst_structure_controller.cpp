#include "ui/connection_controller.h"
#include "ui/editor_tabs_model.h"
#include "ui/query_controller.h"
#include "ui/structure_controller.h"

#include <quarry/ddl_builder.h>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace quarry;
using namespace quarry::ui;

namespace {

constexpr int kWaitMs = 20000;

} // namespace

// The structure of a table (H11 phase 4) over the real SQLite plugin: what a data tab learns about
// its foreign keys (once, cached), the page of a structure tab (columns, indexes, foreign keys,
// DDL), failures and "Try again", and "Go to related row": the filter it builds (parameters,
// composite keys), a NULL key, a table that cannot be opened, a tab that is open already.
class TstStructureController : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    int m_files = 0;

    // Declared in this order so the controllers die before the plugins' registry.
    struct Env
    {
        std::unique_ptr<ConnectionController> controller;
        std::unique_ptr<EditorTabsModel> tabs;
        std::unique_ptr<QueryController> queries;
        std::unique_ptr<StructureController> structure;
        QString connection;
    };

    Env start()
    {
        Env e;
        e.controller = std::make_unique<ConnectionController>();
        e.controller->setDatabasePath(
            m_dir.filePath(QStringLiteral("quarry-%1.db").arg(++m_files)));
        e.controller->setProviderDirectory(QStringLiteral(QUARRY_PLUGIN_DIR));
        e.controller->start();
        [&] { QTRY_VERIFY_WITH_TIMEOUT(!e.controller->model()->loading(), kWaitMs); }();

        ConnectionProfile p;
        p.id = QUuid::createUuid();
        p.name = QStringLiteral("db");
        p.providerId = QStringLiteral("sqlite");
        p.values = { { QStringLiteral("file"),
                       m_dir.filePath(QStringLiteral("data-%1.db").arg(++m_files)) } };
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
        e.structure = std::make_unique<StructureController>();
        e.structure->setConnections(e.controller.get());
        e.structure->setTabs(e.tabs.get());
        e.structure->setQueries(e.queries.get());
        return e;
    }

    // Runs `sql` in a throwaway query tab, waits for the end and closes the tab.
    static void query(Env &e, const QString &sql)
    {
        const int row = e.tabs->openTab(e.connection);
        const QString id = e.tabs->idOf(row);
        e.tabs->setText(row, sql);
        e.tabs->run(row, true, static_cast<int>(sql.size()));
        QueryState *s = e.queries->stateOf(id);
        [&] { QTRY_COMPARE_WITH_TIMEOUT(s->state(), QStringLiteral("done"), kWaitMs); }();
        e.tabs->closeTab(row);
    }

    static QStringList table(const QString &name) { return { QStringLiteral("main"), name }; }

    // Opens a data tab and waits until its rows are in.
    static QString openData(Env &e, const QString &name, int rows)
    {
        const int row = e.tabs->openData(e.connection, table(name), QStringLiteral("table"));
        const QString id = e.tabs->idOf(row);
        QueryState *s = e.queries->stateOf(id);
        [&] {
            QTRY_COMPARE_WITH_TIMEOUT(s->state(), QStringLiteral("done"), kWaitMs);
            QTRY_COMPARE_WITH_TIMEOUT(s->results()->rows(), rows, kWaitMs);
        }();
        return id;
    }

    static void waitState(Env &e, const QString &id, const QString &state)
    {
        QTRY_COMPARE_WITH_TIMEOUT(e.structure->infoOf(id).value(QStringLiteral("state")).toString(),
                                  state, kWaitMs);
    }

    static QVariantMap filterOf(const QString &column, const QString &value)
    {
        return { { QStringLiteral("column"), column },
                 { QStringLiteral("op"), QStringLiteral("equals") },
                 { QStringLiteral("value"), value } };
    }

    static QString shop();

private slots:
    // Nothing is read until a tab asks, and a tab asks once: the answer is cached.
    void aDataTabLearnsItsForeignKeysOnce()
    {
        Env e = start();
        query(e, shop());
        const QString id = openData(e, QStringLiteral("orders"), 3);
        QCOMPARE(e.structure->infoOf(id).value(QStringLiteral("state")).toString(),
                 QStringLiteral("idle"));
        QVERIFY(e.structure->linkColumns(id).isEmpty());

        QSignalSpy changed(e.structure.get(), &StructureController::changed);
        e.structure->load(id);
        waitState(e, id, QStringLiteral("ready"));
        QVERIFY(changed.size() >= 2); // loading, ready
        QCOMPARE(changed.at(0).at(0).toString(), id);
        const QVariantMap links = e.structure->linkColumns(id);
        QCOMPARE(links.size(), 1);
        QCOMPARE(links.value(QStringLiteral("customer_id")).toString(),
                 QStringLiteral("Foreign key to customers.id"));
        QCOMPARE(e.structure->linkText(id, QStringLiteral("customer_id")),
                 QStringLiteral("Foreign key to customers.id"));
        QCOMPARE(e.structure->linkText(id, QStringLiteral("note")), QString());

        // Cached: asking again changes nothing and sends nothing.
        const qsizetype n = changed.size();
        e.structure->load(id);
        e.structure->load(id);
        QCOMPARE(changed.size(), n);
        // "Try again" reads it again.
        e.structure->reload(id);
        waitState(e, id, QStringLiteral("ready"));
        QVERIFY(changed.size() > n);

        // The cache goes with the tab.
        e.tabs->closeTab(e.tabs->currentIndex());
        QCOMPARE(e.structure->infoOf(id).value(QStringLiteral("state")).toString(),
                 QStringLiteral("idle"));
    }

    void theStructureTabShowsColumnsIndexesForeignKeysAndTheDdl()
    {
        Env e = start();
        query(e, shop());
        const int row = e.tabs->openStructure(e.connection, table(QStringLiteral("orders")),
                                              QStringLiteral("table"));
        const QString id = e.tabs->idOf(row);
        e.structure->load(id);
        waitState(e, id, QStringLiteral("ready"));
        const QVariantMap info = e.structure->infoOf(id);
        QVERIFY(!info.value(QStringLiteral("isView")).toBool());
        QVERIFY(!info.value(QStringLiteral("empty")).toBool());

        const QVariantList columns = info.value(QStringLiteral("columns")).toList();
        QCOMPARE(columns.size(), 3);
        const QVariantMap idCol = columns.at(0).toMap();
        QCOMPARE(idCol.value(QStringLiteral("name")).toString(), QStringLiteral("id"));
        QVERIFY(idCol.value(QStringLiteral("primaryKey")).toBool());
        QCOMPARE(idCol.value(QStringLiteral("marks")).toString(), QStringLiteral("Primary key"));
        const QVariantMap fkCol = columns.at(1).toMap();
        QCOMPARE(fkCol.value(QStringLiteral("name")).toString(), QStringLiteral("customer_id"));
        QCOMPARE(fkCol.value(QStringLiteral("marks")).toString(),
                 QStringLiteral("Foreign key to customers.id"));
        QCOMPARE(fkCol.value(QStringLiteral("type")).toString(), QStringLiteral("INTEGER"));
        const QVariantMap noteCol = columns.at(2).toMap();
        QCOMPARE(noteCol.value(QStringLiteral("nullableText")).toString(),
                 QStringLiteral("nullable"));
        QCOMPARE(noteCol.value(QStringLiteral("defaultText")).toString(), QStringLiteral("'none'"));
        QCOMPARE(noteCol.value(QStringLiteral("marks")).toString(), QString());

        const QVariantList indexes = info.value(QStringLiteral("indexes")).toList();
        QCOMPARE(indexes.size(), 1);
        QCOMPARE(indexes.at(0).toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("orders_note"));
        QCOMPARE(indexes.at(0).toMap().value(QStringLiteral("uniqueText")).toString(),
                 QStringLiteral("unique"));
        QCOMPARE(indexes.at(0).toMap().value(QStringLiteral("columns")).toString(),
                 QStringLiteral("note"));

        const QVariantList keys = info.value(QStringLiteral("foreignKeys")).toList();
        QCOMPARE(keys.size(), 1);
        const QVariantMap key = keys.at(0).toMap();
        QCOMPARE(key.value(QStringLiteral("table")).toString(), QStringLiteral("customers"));
        QCOMPARE(key.value(QStringLiteral("columns")).toString(), QStringLiteral("customer_id"));
        QCOMPARE(key.value(QStringLiteral("refColumns")).toString(), QStringLiteral("id"));
        QCOMPARE(key.value(QStringLiteral("text")).toString(),
                 QStringLiteral("customer_id references customers (id)"));
        QCOMPARE(key.value(QStringLiteral("kind")).toString(), QStringLiteral("table"));
        QVERIFY(key.value(QStringLiteral("openable")).toBool());

        // The DDL starts with the header that says it was rebuilt.
        const QString ddl = info.value(QStringLiteral("ddl")).toString();
        QVERIFY2(ddl.startsWith(DdlBuilder::header()), qPrintable(ddl));
        QVERIFY(ddl.contains(QLatin1String("CREATE TABLE")));
        QVERIFY(ddl.contains(QLatin1String("FOREIGN KEY")));
        QVERIFY(ddl.contains(QLatin1String("CREATE UNIQUE INDEX")));
    }

    void aViewHasItsColumnsAndNoDefinition()
    {
        Env e = start();
        query(e, shop());
        const int row = e.tabs->openStructure(e.connection, table(QStringLiteral("order_notes")),
                                              QStringLiteral("view"));
        const QString id = e.tabs->idOf(row);
        e.structure->load(id);
        waitState(e, id, QStringLiteral("ready"));
        const QVariantMap info = e.structure->infoOf(id);
        QVERIFY(info.value(QStringLiteral("isView")).toBool());
        QCOMPARE(info.value(QStringLiteral("columns")).toList().size(), 1);
        const QString ddl = info.value(QStringLiteral("ddl")).toString();
        QVERIFY2(ddl.startsWith(DdlBuilder::header()), qPrintable(ddl));
        QVERIFY(!ddl.contains(QLatin1String("CREATE TABLE")));
        QVERIFY(ddl.contains(QLatin1String("definition of a view is not available")));
    }

    void aMissingTableFailsAndTryAgainReadsItAgain()
    {
        Env e = start();
        query(e, QStringLiteral("SELECT 1"));
        const int row = e.tabs->openStructure(e.connection, table(QStringLiteral("later")),
                                              QStringLiteral("table"));
        const QString id = e.tabs->idOf(row);
        e.structure->load(id);
        waitState(e, id, QStringLiteral("failed"));
        const QVariantMap failed = e.structure->infoOf(id);
        QVERIFY(
            failed.value(QStringLiteral("message")).toString().contains(QLatin1String("later")));
        QVERIFY(failed.value(QStringLiteral("columns")).toList().isEmpty());
        QVERIFY(failed.value(QStringLiteral("ddl")).toString().isEmpty());

        query(e, QStringLiteral("CREATE TABLE later(a INTEGER)"));
        e.structure->reload(id);
        waitState(e, id, QStringLiteral("ready"));
        QCOMPARE(e.structure->infoOf(id).value(QStringLiteral("columns")).toList().size(), 1);
    }

    // The tab of an unknown connection says so instead of waiting for ever.
    void aTabWithoutAConnectionFails()
    {
        Env e = start();
        const int row = e.tabs->openTab(QString());
        e.structure->load(e.tabs->idOf(row)); // a query tab has no table
        waitState(e, e.tabs->idOf(row), QStringLiteral("failed"));
    }

    void goToRelatedOpensTheReferencedRowWithTheKeyAsAParameter()
    {
        Env e = start();
        query(e, shop());
        const QString id = openData(e, QStringLiteral("orders"), 3);
        QSignalSpy said(e.structure.get(), &StructureController::announcement);
        QSignalSpy opened(e.structure.get(), &StructureController::relatedOpened);

        // Before the structure is read there is nothing to follow.
        QVariantMap result = e.structure->goToRelated(id, 1, QStringLiteral("customer_id"));
        QVERIFY(!result.value(QStringLiteral("started")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QLatin1String("loading")));
        e.structure->load(id);
        waitState(e, id, QStringLiteral("ready"));

        // A column that is not a foreign key.
        result = e.structure->goToRelated(id, 1, QStringLiteral("note"));
        QVERIFY(!result.value(QStringLiteral("started")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QLatin1String("not a foreign key")));

        // A NULL key opens nothing and says so.
        const int tabs = e.tabs->count();
        said.clear();
        result = e.structure->goToRelated(id, 2, QStringLiteral("customer_id"));
        QVERIFY(!result.value(QStringLiteral("started")).toBool());
        QCOMPARE(result.value(QStringLiteral("message")).toString(),
                 QStringLiteral("This row has no related row"));
        QCOMPARE(e.tabs->count(), tabs);
        QCOMPARE(said.size(), 1);
        QVERIFY(said.at(0).at(1).toBool()); // assertive
        QCOMPARE(opened.size(), 0);

        // Row 1 points to customer 2.
        result = e.structure->goToRelated(id, 1, QStringLiteral("customer_id"));
        QVERIFY(result.value(QStringLiteral("started")).toBool());
        QTRY_COMPARE_WITH_TIMEOUT(opened.size(), 1, kWaitMs);
        const QString target = opened.at(0).at(1).toString();
        QCOMPARE(opened.at(0).at(0).toString(), id);
        QCOMPARE(e.tabs->count(), tabs + 1);
        QCOMPARE(e.tabs->idOf(e.tabs->currentIndex()), target);
        QVERIFY(e.tabs->isData(e.tabs->currentIndex()));
        QCOMPARE(e.tabs->titleOf(e.tabs->currentIndex()), QStringLiteral("customers"));
        QueryState *ts = e.queries->stateOf(target);
        QTRY_COMPARE_WITH_TIMEOUT(ts->state(), QStringLiteral("done"), kWaitMs);
        QTRY_COMPARE_WITH_TIMEOUT(ts->results()->rows(), 1,
                                  kWaitMs); // the filter ran, not the whole table
        QCOMPARE(ts->results()->cellText(0, 1), QStringLiteral("Luis"));
        QCOMPARE(ts->filterOrigin(), QStringLiteral("Filtered from orders.customer_id = 2"));
        const QVariantMap view = e.queries->dataViewOf(target);
        const QVariantList filters = view.value(QStringLiteral("filters")).toList();
        QCOMPARE(filters.size(), 1);
        QCOMPARE(filters.at(0).toMap(), filterOf(QStringLiteral("id"), QStringLiteral("2")));
        QVERIFY(view.value(QStringLiteral("where")).toString().isEmpty());
        QVERIFY(e.queries->hasDataView(target));
        // The new tab already knows its structure: it does not read it again.
        QCOMPARE(e.structure->infoOf(target).value(QStringLiteral("state")).toString(),
                 QStringLiteral("ready"));
        QVERIFY(said.last().at(0).toString().contains(QLatin1String("Filtered from")));

        // The same row again: the tab is shown, not opened twice.
        e.tabs->setCurrentIndex(e.tabs->dataRowOf(e.connection, table(QStringLiteral("orders")),
                                                  QStringLiteral("table")));
        result = e.structure->goToRelated(id, 1, QStringLiteral("customer_id"));
        QVERIFY(result.value(QStringLiteral("started")).toBool());
        QCOMPARE(opened.size(), 2);
        QCOMPARE(e.tabs->count(), tabs + 1);
        QCOMPARE(e.tabs->idOf(e.tabs->currentIndex()), target);

        // Another row: the same tab, with the new key.
        e.tabs->setCurrentIndex(e.tabs->dataRowOf(e.connection, table(QStringLiteral("orders")),
                                                  QStringLiteral("table")));
        result = e.structure->goToRelated(id, 0, QStringLiteral("customer_id"));
        QVERIFY(result.value(QStringLiteral("started")).toBool());
        QCOMPARE(e.tabs->count(), tabs + 1);
        QCOMPARE(ts->filterOrigin(), QStringLiteral("Filtered from orders.customer_id = 1"));
        QTRY_COMPARE_WITH_TIMEOUT(ts->results()->cellText(0, 1), QStringLiteral("Ana"), kWaitMs);

        // Any other filter drops the sentence: it described the old one.
        QVERIFY(e.queries->setDataView(target, {}));
        QCOMPARE(ts->filterOrigin(), QString());

        // With edits waiting in the other tab the filter is refused, and nothing moves.
        QTRY_COMPARE_WITH_TIMEOUT(ts->state(), QStringLiteral("done"), kWaitMs);
        QTRY_COMPARE_WITH_TIMEOUT(ts->results()->rows(), 2, kWaitMs);
        ts->edits()->setCell(0, 1, QStringLiteral("changed"));
        e.tabs->setCurrentIndex(e.tabs->dataRowOf(e.connection, table(QStringLiteral("orders")),
                                                  QStringLiteral("table")));
        result = e.structure->goToRelated(id, 0, QStringLiteral("customer_id"));
        QVERIFY(!result.value(QStringLiteral("started")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QLatin1String("pending changes")));
        QCOMPARE(e.tabs->idOf(e.tabs->currentIndex()),
                 e.tabs->idOf(e.tabs->dataRowOf(e.connection, table(QStringLiteral("orders")),
                                                QStringLiteral("table"))));
        ts->edits()->discardAll();
    }

    // A key of two columns: one filter per column, values with quotes only as parameters.
    void aCompositeKeyFiltersEveryColumn()
    {
        Env e = start();
        query(e,
              QStringLiteral("CREATE TABLE parent(a TEXT, b TEXT, v TEXT, PRIMARY KEY(a, b));"
                             "CREATE TABLE child(id INTEGER PRIMARY KEY, a TEXT, b TEXT,"
                             " FOREIGN KEY(a, b) REFERENCES parent(a, b));"
                             "INSERT INTO parent VALUES "
                             "('x','it''s','one'),('x','y','two'),('z','it''s','three');"
                             "INSERT INTO child VALUES (1,'x','it''s')"));
        const QString id = openData(e, QStringLiteral("child"), 1);
        e.structure->load(id);
        waitState(e, id, QStringLiteral("ready"));
        QSignalSpy opened(e.structure.get(), &StructureController::relatedOpened);
        // Either column of the key goes to the same row.
        const auto result = e.structure->goToRelated(id, 0, QStringLiteral("b"));
        QVERIFY(result.value(QStringLiteral("started")).toBool());
        QTRY_COMPARE_WITH_TIMEOUT(opened.size(), 1, kWaitMs);
        const QString target = opened.at(0).at(1).toString();
        QueryState *ts = e.queries->stateOf(target);
        QTRY_COMPARE_WITH_TIMEOUT(ts->state(), QStringLiteral("done"), kWaitMs);
        QTRY_COMPARE_WITH_TIMEOUT(ts->results()->rows(), 1, kWaitMs);
        QCOMPARE(ts->results()->cellText(0, 2), QStringLiteral("one"));
        const QVariantList filters
            = e.queries->dataViewOf(target).value(QStringLiteral("filters")).toList();
        QCOMPARE(filters.size(), 2);
        QCOMPARE(filters.at(0).toMap(), filterOf(QStringLiteral("a"), QStringLiteral("x")));
        QCOMPARE(filters.at(1).toMap(), filterOf(QStringLiteral("b"), QStringLiteral("it's")));
        QCOMPARE(ts->filterOrigin(), QStringLiteral("Filtered from child.(a, b) = (x, it's)"));
    }

    // A foreign key to a table that is not there: an error in words and no tab.
    void aTableThatCannotBeOpenedIsNeverAnEmptyTab()
    {
        Env e = start();
        query(e,
              QStringLiteral(
                  "PRAGMA foreign_keys = OFF;" // SQLite checks the key on INSERT otherwise
                  "CREATE TABLE lost(id INTEGER PRIMARY KEY, g INTEGER REFERENCES ghost(id));"
                  "INSERT INTO lost VALUES (1, 5)"));
        const QString id = openData(e, QStringLiteral("lost"), 1);
        e.structure->load(id);
        waitState(e, id, QStringLiteral("ready"));
        const int tabs = e.tabs->count();
        QSignalSpy failed(e.structure.get(), &StructureController::relatedFailed);
        QSignalSpy opened(e.structure.get(), &StructureController::relatedOpened);
        const auto result = e.structure->goToRelated(id, 0, QStringLiteral("g"));
        QVERIFY(result.value(QStringLiteral("started")).toBool());
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, kWaitMs);
        QCOMPARE(failed.at(0).at(0).toString(), id);
        const QString message = failed.at(0).at(1).toString();
        QVERIFY2(message.contains(QLatin1String("ghost")), qPrintable(message));
        QVERIFY2(message.contains(QLatin1String("cannot be opened")), qPrintable(message));
        QCOMPARE(opened.size(), 0);
        QCOMPARE(e.tabs->count(), tabs);
        QCOMPARE(e.tabs->idOf(e.tabs->currentIndex()), id);
    }

    // Nothing is left behind by a tab that is closed while its structure is being read.
    void closingATabWhileItReadsIsHarmless()
    {
        Env e = start();
        query(e, shop());
        const int row = e.tabs->openStructure(e.connection, table(QStringLiteral("orders")),
                                              QStringLiteral("table"));
        const QString id = e.tabs->idOf(row);
        e.structure->load(id);
        e.tabs->closeTab(row);
        QCOMPARE(e.structure->infoOf(id).value(QStringLiteral("state")).toString(),
                 QStringLiteral("idle"));
        QTest::qWait(300); // the late answer is dropped
        QCOMPARE(e.structure->infoOf(id).value(QStringLiteral("state")).toString(),
                 QStringLiteral("idle"));
    }
};

QString TstStructureController::shop()
{
    return QStringLiteral(
        "CREATE TABLE customers(id INTEGER PRIMARY KEY, name TEXT);"
        "CREATE TABLE orders(id INTEGER PRIMARY KEY, customer_id INTEGER REFERENCES customers(id),"
        " note TEXT DEFAULT 'none');"
        "CREATE UNIQUE INDEX orders_note ON orders(note);"
        "CREATE VIEW order_notes AS SELECT note FROM orders;"
        "INSERT INTO customers VALUES (1,'Ana'),(2,'Luis');"
        "INSERT INTO orders VALUES (10,1,'a'),(11,2,'b'),(12,NULL,'c')");
}

QTEST_MAIN(TstStructureController)
#include "tst_structure_controller.moc"
