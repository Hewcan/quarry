#include "ui/connection_controller.h"
#include "ui/editor_tabs_model.h"
#include "ui/history_model.h"
#include "ui/plan_model.h"
#include "ui/query_controller.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest>

#include <memory>

using namespace quarry;
using namespace quarry::ui;

namespace {

constexpr int kWaitMs = 10000;

QString currentThread()
{
    return QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId()));
}

} // namespace

// Explain and Explain analyze in QueryController (H9): the availability rules, the confirmation
// before ANALYZE, the parse (on the UI thread for a small plan, off it for a big one) and the
// states the Plan tab shows. FakeDB (tests/app/fake_explain) is an engine that can analyze; the
// real SQLite plugin covers the whole path once, end to end.
class TstExplainController : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    int m_files = 0;

    // Declared in this order so the QueryController dies before the plugins' registry.
    struct Env
    {
        std::unique_ptr<ConnectionController> controller;
        std::unique_ptr<EditorTabsModel> tabs;
        std::unique_ptr<QueryController> queries;
        QString connection;
    };

    QString addFake(ConnectionController &c, const QString &name)
    {
        ConnectionProfile p;
        p.id = QUuid::createUuid();
        p.name = name;
        p.providerId = QStringLiteral("fakeexplain");
        p.passwordMode = PasswordMode::None;
        c.model()->store()->save(p);
        const QString id = p.id.toString(QUuid::WithoutBraces);
        (void)QTest::qWaitFor([&] { return c.model()->indexOf(id) >= 0; }, kWaitMs);
        return id;
    }

    QString addSqlite(ConnectionController &c)
    {
        const QString file = m_dir.filePath(QStringLiteral("data-%1.db").arg(++m_files));
        c.prepare(QString());
        c.form()->setValue(0, file);
        QSignalSpy saved(&c, &ConnectionController::connectionSaved);
        const bool ok = c.save({ { QStringLiteral("id"), QString() },
                                 { QStringLiteral("providerId"), QStringLiteral("sqlite") },
                                 { QStringLiteral("name"), QStringLiteral("db") },
                                 { QStringLiteral("colorIndex"), 2 },
                                 { QStringLiteral("production"), false },
                                 { QStringLiteral("recordHistory"), true },
                                 { QStringLiteral("passwordMode"), 1 } });
        if (!ok || !saved.wait(kWaitMs))
            return {};
        return saved.at(0).at(0).toString();
    }

    Env start(bool fake)
    {
        Env e;
        e.controller = std::make_unique<ConnectionController>();
        e.controller->setDatabasePath(
            m_dir.filePath(QStringLiteral("quarry-%1.db").arg(++m_files)));
        e.controller->setProviderDirectory(
            QString::fromUtf8(fake ? QUARRY_FAKE_PLUGIN_DIR : QUARRY_PLUGIN_DIR));
        e.controller->start();
        [&] { QTRY_VERIFY_WITH_TIMEOUT(!e.controller->model()->loading(), kWaitMs); }();
        e.connection
            = fake ? addFake(*e.controller, QStringLiteral("fake")) : addSqlite(*e.controller);
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

    static int openTab(Env &e, const QString &connection = {})
    {
        return e.tabs->openTab(connection.isEmpty() ? e.connection : connection);
    }

    static QueryState *stateOf(Env &e, int row) { return e.queries->stateOf(e.tabs->idOf(row)); }

    // Sets the text and asks for the plan of the statement at the end of it (or at `cursor`).
    static QVariantMap explain(Env &e, int row, const QString &sql, bool analyze = false,
                               int cursor = -1)
    {
        e.tabs->setText(row, sql);
        return e.queries->explain(e.tabs->idOf(row), analyze,
                                  cursor < 0 ? static_cast<int>(sql.size()) : cursor);
    }

    static void waitPlan(PlanModel *plan, const QString &state)
    {
        QTRY_COMPARE_WITH_TIMEOUT(plan->state(), state, kWaitMs);
    }

    static QString threadOfFirstStep(PlanModel &plan)
    {
        const QVariantList details = plan.data(plan.index(0, 0), PlanModel::DetailsRole).toList();
        return details.isEmpty() ? QString()
                                 : details.at(0).toMap().value(QStringLiteral("value")).toString();
    }

private slots:
    void supportsExplainFollowsTheEngine()
    {
        Env fake = start(true);
        Env sqlite = start(false);
        QVERIFY(fake.queries->supportsExplain(fake.connection));
        QVERIFY(sqlite.queries->supportsExplain(sqlite.connection));
        QVERIFY(!fake.queries->supportsExplain(QString()));
        QVERIFY(!fake.queries->supportsExplain(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    }

    // The whole path with the real engine: the statement the SQLite dialect writes, the rows,
    // the parser, the model.
    void sqlitePlanEndToEnd()
    {
        Env e = start(false);
        const int row = openTab(e);
        QueryState *s = stateOf(e, row);
        PlanModel *plan = s->plan();
        QVERIFY(!plan->requested());
        e.tabs->setText(row, QStringLiteral("CREATE TABLE t(a INTEGER); CREATE INDEX ti ON t(a)"));
        e.tabs->run(row, true, 0);
        QTRY_COMPARE_WITH_TIMEOUT(s->state(), QStringLiteral("done"), kWaitMs);

        QSignalSpy requested(e.queries.get(), &QueryController::planRequested);
        QSignalSpy said(e.queries.get(), &QueryController::announcement);
        QSignalSpy failed(e.queries.get(), &QueryController::planFailed);
        const QVariantMap r = explain(e, row, QStringLiteral("SELECT * FROM t WHERE a = 1"));
        QVERIFY(r.value(QStringLiteral("ok")).toBool());
        QCOMPARE(r.value(QStringLiteral("ranges")).toList().size(), 1);
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.at(0).at(0).toString(), e.tabs->idOf(row));
        QVERIFY(plan->requested());
        QCOMPARE(plan->state(), QStringLiteral("running")); // at once, never blocking
        QCOMPARE(s->state(), QStringLiteral("running"));
        waitPlan(plan, QStringLiteral("ready"));
        QTRY_COMPARE_WITH_TIMEOUT(s->state(), QStringLiteral("done"), kWaitMs);
        QCOMPARE(plan->engine(), QStringLiteral("SQLite"));
        QVERIFY(!plan->analyzed());
        QVERIFY(plan->nodeCount() >= 1);
        QVERIFY(plan->rowCount() >= 1);
        QVERIFY(plan->rawText().contains(QStringLiteral("t")));
        QVERIFY(!plan->hasBars()); // SQLite gives no cost: nothing is invented
        QVERIFY(failed.isEmpty());
        QVERIFY(said.at(0).at(0).toString().contains(QStringLiteral("plan")));
        bool ready = false;
        for (const QList<QVariant> &args : said)
            ready = ready || args.at(0).toString().startsWith(QStringLiteral("Query plan ready, "));
        QVERIFY(ready);
        QCOMPARE(s->messages()->count(), 1);
        QCOMPARE(e.queries->sessionCount(), 1);
    }

    void sqliteCannotAnalyze()
    {
        Env e = start(false);
        const int row = openTab(e);
        PlanModel *plan = stateOf(e, row)->plan();
        QSignalSpy asked(e.queries.get(), &QueryController::analyzeConfirmationRequested);
        QSignalSpy requested(e.queries.get(), &QueryController::planRequested);
        const QVariantMap r = explain(e, row, QStringLiteral("SELECT 1"), true);
        QVERIFY(!r.value(QStringLiteral("ok")).toBool());
        QVERIFY(r.value(QStringLiteral("message")).toString().contains(QStringLiteral("SQLite")));
        QVERIFY(r.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("Explain analyze")));
        QCOMPARE(plan->notice(), r.value(QStringLiteral("message")).toString());
        QVERIFY(plan->requested()); // the Plan tab says why
        QCOMPARE(plan->state(), QStringLiteral("idle"));
        QCOMPARE(requested.count(), 1);
        QVERIFY(asked.isEmpty());
        QCOMPARE(e.queries->sessionCount(), 0); // nothing was opened, nothing ran
    }

    void anEngineWithoutAPlanForTheStatementIsUnavailable()
    {
        Env e = start(true);
        const int row = openTab(e);
        PlanModel *plan = stateOf(e, row)->plan();
        const QVariantMap r = explain(e, row, QStringLiteral("SELECT NOPLAN"));
        QVERIFY(!r.value(QStringLiteral("ok")).toBool());
        QVERIFY(r.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("can't show a plan")));
        QCOMPARE(plan->notice(), r.value(QStringLiteral("message")).toString());
        QCOMPARE(e.queries->sessionCount(), 0);
        QCOMPARE(stateOf(e, row)->state(), QStringLiteral("idle"));
    }

    void thereIsNothingToExplainWithoutAStatementOrAConnection()
    {
        Env e = start(true);
        QVERIFY(!e.queries->explain(QStringLiteral("nope"), false, 0)
                     .value(QStringLiteral("ok"))
                     .toBool());
        const int row = e.tabs->openTab(QString()); // no connection
        QVariantMap r = explain(e, row, QStringLiteral("SELECT 1"));
        QVERIFY(!r.value(QStringLiteral("ok")).toBool());
        QCOMPARE(r.value(QStringLiteral("message")).toString(),
                 QStringLiteral("Choose a connection first."));
        QVERIFY(!stateOf(e, row)->plan()->requested());
        e.tabs->setConnection(row, e.connection);
        r = explain(e, row, QStringLiteral("   "));
        QVERIFY(!r.value(QStringLiteral("ok")).toBool());
        QCOMPARE(r.value(QStringLiteral("message")).toString(), QStringLiteral("Nothing to run."));
        QCOMPARE(e.queries->sessionCount(), 0);
    }

    void explainRunsTheEnginesStatementOnTheTabsSession()
    {
        Env e = start(true);
        auto history = std::make_unique<HistoryModel>();
        history->setDatabasePath(e.controller->databasePath());
        history->setConnections(e.controller.get());
        e.queries->setHistory(history.get());
        const int row = openTab(e);
        QueryState *s = stateOf(e, row);
        PlanModel *plan = s->plan();
        QSignalSpy asked(e.queries.get(), &QueryController::analyzeConfirmationRequested);
        const QVariantMap r = explain(e, row, QStringLiteral("SELECT * FROM t;"));
        QVERIFY(r.value(QStringLiteral("ok")).toBool());
        QVERIFY(asked.isEmpty()); // a plan runs nothing: no confirmation
        waitPlan(plan, QStringLiteral("ready"));
        QTRY_COMPARE_WITH_TIMEOUT(s->state(), QStringLiteral("done"), kWaitMs);
        // The first line echoes what the engine received: the product comes from the session.
        QVERIFY2(
            plan->rawText().startsWith(QStringLiteral("Echo: EXPLAIN [FakeDB] SELECT * FROM t")),
            qPrintable(plan->rawText()));
        QCOMPARE(plan->engine(), QStringLiteral("FakeDB"));
        QCOMPARE(plan->nodeCount(), 3);
        QVERIFY(!plan->analyzed());
        QCOMPARE(plan->barBasis(), QStringLiteral("cost"));
        QCOMPARE(e.queries->sessionCount(), 1);

        // The history keeps the EXPLAIN that really ran, like any other statement.
        QSignalSpy finished(history.get(), &HistoryModel::searchFinished);
        history->refresh();
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, kWaitMs);
        QCOMPARE(history->count(), 1);
        QCOMPARE(history->data(history->index(0), HistoryModel::SqlRole).toString(),
                 QStringLiteral("EXPLAIN [FakeDB] SELECT * FROM t"));
        QVERIFY(history->data(history->index(0), HistoryModel::SuccessRole).toBool());
    }

    void theStatementUnderTheCursorIsTheOneExplained()
    {
        Env e = start(true);
        const int row = openTab(e);
        PlanModel *plan = stateOf(e, row)->plan();
        const QString sql = QStringLiteral("SELECT 1;\nSELECT 2;\nSELECT 3");
        explain(e, row, sql, false, static_cast<int>(sql.indexOf(QStringLiteral("SELECT 2"))) + 3);
        waitPlan(plan, QStringLiteral("ready"));
        QVERIFY2(plan->rawText().startsWith(QStringLiteral("Echo: EXPLAIN [FakeDB] SELECT 2")),
                 qPrintable(plan->rawText()));
    }

    void analyzeOfAWriteIsRefusedAndRunsNothing()
    {
        Env e = start(true);
        const int row = openTab(e);
        QueryState *s = stateOf(e, row);
        QSignalSpy asked(e.queries.get(), &QueryController::analyzeConfirmationRequested);
        const QStringList writes {
            QStringLiteral("INSERT INTO t VALUES (1)"),
            QStringLiteral("UPDATE t SET a = 1 WHERE a = 2"),
            QStringLiteral("DELETE FROM t WHERE a = 1"),
            QStringLiteral("CREATE TABLE u(a)"),
            QStringLiteral("DROP TABLE u"),
            QStringLiteral("  -- a comment\n  insert into t values (1)"),
            QStringLiteral("WITH d AS (DELETE FROM t RETURNING *) SELECT * FROM d"),
            QStringLiteral("SELECT * INTO u FROM t"),
            QStringLiteral("EXPLAIN SELECT 1"),
        };
        for (const QString &sql : writes) {
            const QVariantMap r = explain(e, row, sql, true);
            QVERIFY2(!r.value(QStringLiteral("ok")).toBool(), qPrintable(sql));
            QVERIFY2(r.value(QStringLiteral("message"))
                         .toString()
                         .contains(QStringLiteral("only takes SELECT")),
                     qPrintable(sql));
        }
        QVERIFY(asked.isEmpty());
        QCOMPARE(e.queries->sessionCount(), 0);
        QCOMPARE(s->state(), QStringLiteral("idle"));
        // The words of a write inside a string, a quoted name or a comment are not writes.
        for (const QString &sql :
             { QStringLiteral("SELECT 'delete from t' AS \"insert\" /* update */ FROM t"),
               QStringLiteral("WITH c AS (SELECT 1) SELECT * FROM c"),
               QStringLiteral("select replace(a, 'x', 'y') from t -- into u") }) {
            asked.clear();
            QVERIFY2(explain(e, row, sql, true).value(QStringLiteral("ok")).toBool(),
                     qPrintable(sql));
            QCOMPARE(asked.count(), 1);
            e.queries->decline(e.tabs->idOf(row));
        }
        // Their plan, without running them, is fine.
        const QVariantMap plain
            = explain(e, row, QStringLiteral("INSERT INTO t VALUES (1)"), false);
        QVERIFY(plain.value(QStringLiteral("ok")).toBool());
        QTRY_COMPARE_WITH_TIMEOUT(s->plan()->state(), QStringLiteral("ready"), kWaitMs);
    }

    void analyzeAsksFirstAndRunsOnlyAfterConfirm()
    {
        Env e = start(true);
        const int row = openTab(e, e.connection);
        const QString tab = e.tabs->idOf(row);
        QueryState *s = stateOf(e, row);
        PlanModel *plan = s->plan();
        QSignalSpy asked(e.queries.get(), &QueryController::analyzeConfirmationRequested);
        QSignalSpy requested(e.queries.get(), &QueryController::planRequested);
        const QVariantMap r = explain(e, row, QStringLiteral("SELECT * FROM big_table"), true);
        QVERIFY(r.value(QStringLiteral("ok")).toBool());
        QCOMPARE(asked.count(), 1);
        const QVariantMap request = asked.at(0).at(0).toMap();
        QCOMPARE(request.value(QStringLiteral("tabId")).toString(), tab);
        QCOMPARE(request.value(QStringLiteral("connectionName")).toString(),
                 QStringLiteral("fake"));
        QVERIFY(!request.value(QStringLiteral("production")).toBool());
        QCOMPARE(request.value(QStringLiteral("sql")).toString(),
                 QStringLiteral("SELECT * FROM big_table"));
        // Nothing runs until it is answered: no session, no running state, no Plan tab yet.
        QTest::qWait(300);
        QCOMPARE(e.queries->sessionCount(), 0);
        QCOMPARE(s->state(), QStringLiteral("idle"));
        QVERIFY(!plan->requested());
        QVERIFY(requested.isEmpty());

        e.queries->confirm(tab);
        QCOMPARE(s->state(), QStringLiteral("running"));
        QVERIFY(plan->requested());
        QVERIFY(plan->analyzed());
        waitPlan(plan, QStringLiteral("ready"));
        QVERIFY(plan->analyzed());
        QVERIFY2(plan->rawText().startsWith(
                     QStringLiteral("Echo: EXPLAIN ANALYZE [FakeDB] SELECT * FROM big_table")),
                 qPrintable(plan->rawText()));
        QCOMPARE(plan->executionTime(), 6.0);
        QCOMPARE(plan->barBasis(), QStringLiteral("time"));
        QCOMPARE(requested.count(), 1);
    }

    void decliningTheConfirmationRunsNothing()
    {
        Env e = start(true);
        const int row = openTab(e);
        const QString tab = e.tabs->idOf(row);
        QueryState *s = stateOf(e, row);
        QSignalSpy asked(e.queries.get(), &QueryController::analyzeConfirmationRequested);
        QSignalSpy said(e.queries.get(), &QueryController::announcement);
        explain(e, row, QStringLiteral("SELECT 1"), true);
        QCOMPARE(asked.count(), 1);
        e.queries->decline(tab);
        QTest::qWait(300);
        QCOMPARE(e.queries->sessionCount(), 0);
        QCOMPARE(s->state(), QStringLiteral("idle"));
        QVERIFY(!s->plan()->requested());
        QCOMPARE(said.count(), 1);
        QCOMPARE(said.at(0).at(0).toString(), QStringLiteral("Cancelled. Nothing was run."));
        e.queries->confirm(tab); // nothing is pending any more
        QTest::qWait(100);
        QCOMPARE(s->state(), QStringLiteral("idle"));
    }

    void aPlanThatCannotBeReadShowsTheEnginesText()
    {
        Env e = start(true);
        const int row = openTab(e);
        QueryState *s = stateOf(e, row);
        PlanModel *plan = s->plan();
        QSignalSpy failed(e.queries.get(), &QueryController::planFailed);
        QSignalSpy said(e.queries.get(), &QueryController::announcement);
        explain(e, row, QStringLiteral("SELECT BAD"));
        waitPlan(plan, QStringLiteral("failed"));
        QCOMPARE(plan->errorMessage(), QStringLiteral("The plan could not be read."));
        QCOMPARE(plan->errorDetail(), QStringLiteral("unexpected token near BAD"));
        // Never an empty tab: the output as the engine returned it.
        QVERIFY2(plan->rawText().startsWith(QStringLiteral("Echo: EXPLAIN [FakeDB] SELECT BAD")),
                 qPrintable(plan->rawText()));
        QCOMPARE(plan->rowCount(), 0);
        QCOMPARE(s->state(), QStringLiteral("failed"));
        QVERIFY(failed.isEmpty()); // the Plan tab stays in front
        QCOMPARE(s->errorOffset(), -1); // the position of an EXPLAIN means nothing in the text
        QCOMPARE(s->messages()->count(), 1);
        bool announced = false;
        for (const QList<QVariant> &args : said)
            announced = announced
                || (args.at(0).toString() == plan->errorMessage() && args.at(1).toBool());
        QVERIFY(announced);
    }

    void anErrorOfTheExplainItselfIsInThePlanTab()
    {
        Env e = start(true);
        const int row = openTab(e);
        QueryState *s = stateOf(e, row);
        PlanModel *plan = s->plan();
        QSignalSpy failed(e.queries.get(), &QueryController::planFailed);
        explain(e, row, QStringLiteral("SELECT FAILEXEC"));
        waitPlan(plan, QStringLiteral("failed"));
        QCOMPARE(plan->errorMessage(), QStringLiteral("The query has an error."));
        QVERIFY(plan->rawText().isEmpty());
        QVERIFY(failed.isEmpty());
        QCOMPARE(s->errorOffset(), -1);
        QCOMPARE(s->state(), QStringLiteral("failed"));
    }

    void aSmallPlanIsParsedOnTheUiThread()
    {
        Env e = start(true);
        const int row = openTab(e);
        PlanModel *plan = stateOf(e, row)->plan();
        explain(e, row, QStringLiteral("SELECT 1"));
        waitPlan(plan, QStringLiteral("ready"));
        QCOMPARE(threadOfFirstStep(*plan), currentThread());
    }

    void aBigPlanIsParsedOffTheUiThread()
    {
        Env e = start(true);
        const int row = openTab(e);
        QueryState *s = stateOf(e, row);
        PlanModel *plan = s->plan();
        // 300 KiB of output and a parser that takes 300 ms: the UI must keep turning meanwhile.
        int ticks = 0;
        QTimer ticker;
        ticker.setInterval(5);
        connect(&ticker, &QTimer::timeout, this, [&ticks] { ++ticks; });
        ticker.start();
        QSignalSpy said(e.queries.get(), &QueryController::announcement);
        explain(e, row, QStringLiteral("SELECT BIG SLOWPARSE"));
        waitPlan(plan, QStringLiteral("ready"));
        ticker.stop();
        QVERIFY(plan->rawText().size() > qsizetype { 256 } * 1024);
        const QString parsedOn = threadOfFirstStep(*plan);
        QVERIFY(!parsedOn.isEmpty());
        QVERIFY2(parsedOn != currentThread(), "parsed on the UI thread");
        // 300 ms at 5 ms a tick: a blocked loop would have counted a handful at most.
        QVERIFY2(ticks >= 15, qPrintable(QString::number(ticks)));
        QCOMPARE(s->state(), QStringLiteral("done"));
        QCOMPARE(plan->nodeCount(), 2);
    }

    void cancellingBeforeTheParseDropsThePlan()
    {
        Env e = start(true);
        const int row = openTab(e);
        QueryState *s = stateOf(e, row);
        PlanModel *plan = s->plan();
        explain(e, row, QStringLiteral("SELECT BIG SLOWPARSE"));
        QCOMPARE(s->state(), QStringLiteral("running"));
        e.queries->cancel(e.tabs->idOf(row));
        QTRY_COMPARE_WITH_TIMEOUT(s->state(), QStringLiteral("cancelled"), kWaitMs);
        QCOMPARE(plan->state(), QStringLiteral("cancelled"));
        // What the parse thread had in flight arrives and is ignored.
        QTest::qWait(800);
        QCOMPARE(plan->state(), QStringLiteral("cancelled"));
        QCOMPARE(plan->rowCount(), 0);
        QCOMPARE(s->state(), QStringLiteral("cancelled"));
    }

    void aNormalRunKeepsThePlan()
    {
        Env e = start(true);
        const int row = openTab(e);
        QueryState *s = stateOf(e, row);
        PlanModel *plan = s->plan();
        explain(e, row, QStringLiteral("SELECT 1"));
        waitPlan(plan, QStringLiteral("ready"));
        QTRY_COMPARE_WITH_TIMEOUT(s->state(), QStringLiteral("done"), kWaitMs);
        QSignalSpy started(e.queries.get(), &QueryController::runStarted);
        e.tabs->setText(row, QStringLiteral("SELECT 7"));
        e.tabs->run(row, true, 0);
        QCOMPARE(started.count(), 1);
        QCOMPARE(started.at(0).at(0).toString(), e.tabs->idOf(row));
        QTRY_COMPARE_WITH_TIMEOUT(s->state(), QStringLiteral("done"), kWaitMs);
        QCOMPARE(s->results()->columns(), 1); // the results are the query's
        QVERIFY(plan->requested()); // and the plan stays until the next Explain
        QCOMPARE(plan->state(), QStringLiteral("ready"));
        QCOMPARE(plan->nodeCount(), 3);
    }

    void anExplainKeepsTheLastResult()
    {
        Env e = start(true);
        const int row = openTab(e);
        QueryState *s = stateOf(e, row);
        e.tabs->setText(row, QStringLiteral("SELECT 7"));
        e.tabs->run(row, true, 0);
        QTRY_COMPARE_WITH_TIMEOUT(s->state(), QStringLiteral("done"), kWaitMs);
        QCOMPARE(s->results()->columns(), 1);
        explain(e, row, QStringLiteral("SELECT 1"));
        waitPlan(s->plan(), QStringLiteral("ready"));
        QTRY_COMPARE_WITH_TIMEOUT(s->state(), QStringLiteral("done"), kWaitMs);
        QCOMPARE(s->results()->columns(), 1);
    }

    void changingTheConnectionDropsThePlan()
    {
        Env e = start(true);
        const QString other = addFake(*e.controller, QStringLiteral("other"));
        const int row = openTab(e);
        QueryState *s = stateOf(e, row);
        explain(e, row, QStringLiteral("SELECT 1"));
        waitPlan(s->plan(), QStringLiteral("ready"));
        QVERIFY(s->plan()->requested());
        e.tabs->setConnection(row, other);
        QTRY_VERIFY_WITH_TIMEOUT(!s->plan()->requested(), kWaitMs);
        QCOMPARE(s->plan()->rowCount(), 0);
        QCOMPARE(s->plan()->state(), QStringLiteral("idle"));
    }

    void aNewRunReplacesAPlanThatIsStillBeingMade()
    {
        Env e = start(true);
        const int row = openTab(e);
        QueryState *s = stateOf(e, row);
        explain(e, row, QStringLiteral("SELECT 1"));
        QCOMPARE(s->plan()->state(), QStringLiteral("running"));
        e.tabs->setText(row, QStringLiteral("SELECT 2"));
        e.tabs->run(row, true, 0);
        QCOMPARE(s->plan()->state(), QStringLiteral("cancelled")); // never "running" forever
        QTRY_COMPARE_WITH_TIMEOUT(s->state(), QStringLiteral("done"), kWaitMs);
        QCOMPARE(s->plan()->state(), QStringLiteral("cancelled"));
    }

    void aDataTabHasNoQueryToExplain()
    {
        Env e = start(true);
        const int row
            = e.tabs->openData(e.connection, { QStringLiteral("t") }, QStringLiteral("table"));
        QVERIFY(row >= 0);
        const QVariantMap r = e.queries->explain(e.tabs->idOf(row), false, 0);
        QVERIFY(!r.value(QStringLiteral("ok")).toBool());
        QVERIFY(!r.value(QStringLiteral("message")).toString().isEmpty());
    }
};

QTEST_MAIN(TstExplainController)
#include "tst_explain_controller.moc"
