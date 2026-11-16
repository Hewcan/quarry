#include "ui/completion_controller.h"
#include "ui/completion_model.h"
#include "ui/connection_controller.h"
#include "ui/editor_tabs_model.h"
#include "ui/query_controller.h"
#include "ui/schema_tree_model.h"

#include <quarry/sql_context.h>

#include <QAbstractItemModelTester>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace quarry;
using namespace quarry::ui;

namespace {

constexpr int kWaitMs = 20000;
const QString kUrlVariable = QStringLiteral("QUARRY_TEST_POSTGRES_URL");

} // namespace

// CompletionController and CompletionModel (H3b) over the real SQLite plugin and files in a
// temporary directory, and, when QUARRY_TEST_POSTGRES_URL is set, over PostgreSQL: the lazy load,
// the columns on demand, the invalidation after DDL, the "no password prompt" rule, the replace
// range, the palette's "@" search and the tree reveal.
class TstCompletionController : public QObject
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

    // Declared in this order so the controllers die before the plugins' registry.
    struct Env
    {
        std::unique_ptr<ConnectionController> controller;
        std::unique_ptr<EditorTabsModel> tabs;
        std::unique_ptr<QueryController> queries;
        std::unique_ptr<CompletionController> completion;
        QString connection;
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

    static bool havePostgres() { return qEnvironmentVariableIsSet(qPrintable(kUrlVariable)); }

    // `mode` None/Saved/AskEveryTime; with AskEveryTime the password is remembered for the run
    // unless `remember` is false (PostgreSQL always remembers: it needs it).
    Env start(const Engine &engine, PasswordMode mode = PasswordMode::None, bool remember = true)
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
        p.name = QStringLiteral("db");
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
            p.passwordMode = mode;
            if (mode == PasswordMode::AskEveryTime && remember)
                e.controller->rememberPassword(p.id, QString());
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
        e.completion = std::make_unique<CompletionController>();
        e.completion->setTabs(e.tabs.get());
        // What MainWindow.qml wires.
        connect(e.queries.get(), &QueryController::schemaChanged, e.completion.get(),
                &CompletionController::invalidate);
        return e;
    }

    QString table(Env &e, const QString &prefix)
    {
        const QString name = QStringLiteral("%1_%2_%3")
                                 .arg(prefix)
                                 .arg(QCoreApplication::applicationPid())
                                 .arg(++m_tables);
        e.created << name;
        return name;
    }

    // Runs `sql` in a query tab (a session of its own, not the tree's) and waits for the end.
    static void run(Env &e, const QString &sql)
    {
        const int row = e.tabs->openTab(e.connection);
        e.tabs->setText(row, sql);
        e.tabs->run(row, true, static_cast<int>(sql.size()));
        QueryState *s = e.queries->stateOf(e.tabs->idOf(row));
        [&] { QTRY_COMPARE_WITH_TIMEOUT(s->state(), QStringLiteral("done"), kWaitMs); }();
    }

    static void drop(Env &e)
    {
        for (const QString &name : std::as_const(e.created))
            run(e,
                QStringLiteral("DROP VIEW IF EXISTS \"v_%1\"; DROP TABLE IF EXISTS \"%1\";")
                    .arg(name));
    }

    // The query for the cursor at `|` in `text`.
    static QVariantMap ask(Env &e, int row, QString text)
    {
        const int cursor = static_cast<int>(text.indexOf(QLatin1Char('|')));
        text.remove(cursor, 1);
        e.tabs->setText(row, text);
        return e.completion->query(row, cursor);
    }

    static QStringList labels(Env &e)
    {
        QStringList out;
        const CompletionModel *m = e.completion->model();
        for (int i = 0; i < m->count(); ++i)
            out << m->labelAt(i);
        return out;
    }

    static QStringList kinds(Env &e)
    {
        QStringList out;
        const CompletionModel *m = e.completion->model();
        for (int i = 0; i < m->count(); ++i)
            out << m->data(m->index(i), CompletionModel::KindRole).toString();
        return out;
    }

    static bool has(const QStringList &list, const QString &name)
    {
        return std::ranges::any_of(
            list, [&](const QString &s) { return s.compare(name, Qt::CaseInsensitive) == 0; });
    }

    void lazyLoad(const Engine &engine)
    {
        Env e = start(engine);
        const QString t = table(e, QStringLiteral("lazy"));
        run(e, QStringLiteral("CREATE TABLE \"%1\"(id INTEGER PRIMARY KEY, name TEXT)").arg(t));
        const int row = e.tabs->openTab(e.connection);
        QAbstractItemModelTester tester(e.completion->model(),
                                        QAbstractItemModelTester::FailureReportingMode::Fatal);

        // Nothing is read before the first request: no cache, and the tree's session is closed.
        QCOMPARE(e.completion->cacheState(e.connection), QStringLiteral("none"));
        QVERIFY(!e.controller->tree()->isConnected(e.connection));

        QSignalSpy changed(e.completion.get(), &CompletionController::changed);
        const QVariantMap first = ask(e, row, QStringLiteral("SELECT * FROM |"));
        QCOMPARE(first.value(QStringLiteral("scope")).toInt(),
                 static_cast<int>(CompletionScope::Table));
        QVERIFY(first.value(QStringLiteral("loading")).toBool());
        QVERIFY(first.value(QStringLiteral("notice")).toString().isEmpty());

        // The session opens the way expanding the connection does, then the cache loads.
        QTRY_COMPARE_WITH_TIMEOUT(e.completion->cacheState(e.connection), QStringLiteral("ready"),
                                  kWaitMs);
        QVERIFY(changed.count() >= 1);
        QVERIFY(e.controller->tree()->isConnected(e.connection));

        const QVariantMap again = ask(e, row, QStringLiteral("SELECT * FROM |"));
        QVERIFY(!again.value(QStringLiteral("loading")).toBool());
        QVERIFY2(has(labels(e), t), qPrintable(labels(e).join(QLatin1Char(','))));
        const int at = static_cast<int>(labels(e).indexOf(t));
        QCOMPARE(e.completion->model()->kindTextAt(at), QStringLiteral("table"));
        QCOMPARE(again.value(QStringLiteral("replaceStart")).toInt(), 14);
        QCOMPARE(again.value(QStringLiteral("replaceEnd")).toInt(), 14);
        drop(e);
    }

    void columnsOnDemand(const Engine &engine)
    {
        Env e = start(engine);
        const QString a = table(e, QStringLiteral("cols_a"));
        const QString b = table(e, QStringLiteral("cols_b"));
        run(e,
            QStringLiteral("CREATE TABLE \"%1\"(id INTEGER PRIMARY KEY, alpha TEXT);"
                           "CREATE TABLE \"%2\"(id INTEGER PRIMARY KEY, beta TEXT);")
                .arg(a, b));
        const int row = e.tabs->openTab(e.connection);
        ask(e, row, QStringLiteral("SELECT | FROM \"%1\"").arg(a));
        QTRY_COMPARE_WITH_TIMEOUT(e.completion->cacheState(e.connection), QStringLiteral("ready"),
                                  kWaitMs);

        // The tables are known, their columns are not: the statement names one of them.
        const MetadataCache *cache = e.completion->cache(e.connection);
        const auto pathOf
            = [&](const QString &name) { return cache->resolve({ name }).value_or(ObjectPath {}); };
        QVERIFY(!pathOf(a).parts.isEmpty() && !pathOf(b).parts.isEmpty());
        QVERIFY(cache->columns(pathOf(b)) == nullptr);

        ask(e, row, QStringLiteral("SELECT | FROM \"%1\"").arg(a)); // asks for the columns of a
        QTRY_VERIFY_WITH_TIMEOUT(cache->columns(pathOf(a)) != nullptr, kWaitMs);
        const QVariantMap r = ask(e, row, QStringLiteral("SELECT | FROM \"%1\"").arg(a));
        QCOMPARE(r.value(QStringLiteral("scope")).toInt(),
                 static_cast<int>(CompletionScope::Column));
        QVERIFY2(has(labels(e), QStringLiteral("alpha")),
                 qPrintable(labels(e).join(QLatin1Char(','))));
        QVERIFY(!has(labels(e), QStringLiteral("beta")));
        QVERIFY(kinds(e).contains(QStringLiteral("column")));
        QVERIFY(cache->columns(pathOf(b)) == nullptr); // still not loaded
        drop(e);
    }

    void invalidationAfterDdl(const Engine &engine)
    {
        Env e = start(engine);
        const QString t = table(e, QStringLiteral("ddl"));
        const int row = e.tabs->openTab(e.connection);
        ask(e, row, QStringLiteral("SELECT * FROM |"));
        QTRY_COMPARE_WITH_TIMEOUT(e.completion->cacheState(e.connection), QStringLiteral("ready"),
                                  kWaitMs);
        ask(e, row, QStringLiteral("SELECT * FROM |"));
        QVERIFY(!has(labels(e), t));

        // A SELECT changes nothing.
        QSignalSpy schema(e.queries.get(), &QueryController::schemaChanged);
        run(e, QStringLiteral("SELECT 1"));
        QCOMPARE(schema.count(), 0);
        QCOMPARE(e.completion->cacheState(e.connection), QStringLiteral("ready"));

        // CREATE does: the cache is dropped and the next request reads it again.
        run(e, QStringLiteral("CREATE TABLE \"%1\"(id INTEGER)").arg(t));
        QCOMPARE(schema.count(), 1);
        QCOMPARE(schema.at(0).at(0).toString(), e.connection);
        QCOMPARE(e.completion->cacheState(e.connection), QStringLiteral("empty"));
        ask(e, row, QStringLiteral("SELECT * FROM |"));
        QTRY_COMPARE_WITH_TIMEOUT(e.completion->cacheState(e.connection), QStringLiteral("ready"),
                                  kWaitMs);
        ask(e, row, QStringLiteral("SELECT * FROM |"));
        QVERIFY2(has(labels(e), t), qPrintable(labels(e).join(QLatin1Char(','))));

        // And so does ALTER: the new column shows up.
        ask(e, row, QStringLiteral("SELECT | FROM \"%1\"").arg(t));
        const auto path = e.completion->cache(e.connection)->resolve({ t }).value_or(ObjectPath {});
        QTRY_VERIFY_WITH_TIMEOUT(e.completion->cache(e.connection)->columns(path) != nullptr,
                                 kWaitMs);
        run(e, QStringLiteral("ALTER TABLE \"%1\" ADD COLUMN extra_col INTEGER").arg(t));
        QCOMPARE(schema.count(), 2);
        QCOMPARE(e.completion->cacheState(e.connection), QStringLiteral("empty"));
        // The cache loads again, then the columns of the table the statement names.
        QTRY_VERIFY_WITH_TIMEOUT((ask(e, row, QStringLiteral("SELECT | FROM \"%1\"").arg(t)),
                                  has(labels(e), QStringLiteral("extra_col"))),
                                 kWaitMs);
        drop(e);
    }

private slots:
    void initTestCase()
    {
        // The setting must not reach the user's real ones.
        QCoreApplication::setOrganizationName(QStringLiteral("QuarryTest"));
        QCoreApplication::setApplicationName(QStringLiteral("tst_completion_controller"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir.path());
    }

    void modelRolesAndKindWords()
    {
        CompletionModel model;
        QAbstractItemModelTester tester(&model,
                                        QAbstractItemModelTester::FailureReportingMode::Fatal);
        QSignalSpy counts(&model, &CompletionModel::countChanged);
        model.setSuggestions(
            { { QStringLiteral("SELECT"), QStringLiteral("SELECT"), SuggestionKind::Keyword, {} },
              { QStringLiteral("\"my table\""), QStringLiteral("my table"), SuggestionKind::Table,
                QStringLiteral("main") },
              { QStringLiteral("name"), QStringLiteral("name"), SuggestionKind::Column,
                QStringLiteral("t: TEXT") } });
        QCOMPARE(model.count(), 3);
        QCOMPARE(counts.count(), 1);
        QCOMPARE(model.labelAt(1), QStringLiteral("my table"));
        QCOMPARE(model.textAt(1), QStringLiteral("\"my table\""));
        QCOMPARE(model.kindTextAt(0), QStringLiteral("keyword"));
        QCOMPARE(model.kindTextAt(1), QStringLiteral("table"));
        QCOMPARE(model.kindTextAt(2), QStringLiteral("column"));
        QCOMPARE(model.data(model.index(2), CompletionModel::DetailRole).toString(),
                 QStringLiteral("t: TEXT"));
        QCOMPARE(model.data(model.index(1), CompletionModel::KindRole).toString(),
                 QStringLiteral("table"));
        QCOMPARE(model.textAt(9), QString());
        model.setSuggestions({});
        QCOMPARE(model.count(), 0);
        QCOMPARE(counts.count(), 2);
    }

    void autoOpenSettingIsPersisted()
    {
        {
            CompletionController c;
            QVERIFY(c.autoOpen()); // on by default
            QSignalSpy spy(&c, &CompletionController::autoOpenChanged);
            c.setAutoOpen(false);
            QCOMPARE(spy.count(), 1);
            c.setAutoOpen(false);
            QCOMPARE(spy.count(), 1);
        }
        CompletionController again;
        QVERIFY(!again.autoOpen());
        again.setAutoOpen(true);
        QVERIFY(CompletionController().autoOpen());
    }

    void lazyLoadSqlite() { lazyLoad({}); }
    void lazyLoadPostgres()
    {
        if (!havePostgres())
            QSKIP("QUARRY_TEST_POSTGRES_URL is not set");
        lazyLoad(postgres());
    }

    void columnsOnDemandSqlite() { columnsOnDemand({}); }
    void columnsOnDemandPostgres()
    {
        if (!havePostgres())
            QSKIP("QUARRY_TEST_POSTGRES_URL is not set");
        columnsOnDemand(postgres());
    }

    void invalidationAfterDdlSqlite() { invalidationAfterDdl({}); }
    void invalidationAfterDdlPostgres()
    {
        if (!havePostgres())
            QSKIP("QUARRY_TEST_POSTGRES_URL is not set");
        invalidationAfterDdl(postgres());
    }

    // "Refresh metadata": the cache is dropped and read again.
    void invalidateAndReload()
    {
        Env e = start({});
        const int row = e.tabs->openTab(e.connection);
        ask(e, row, QStringLiteral("SELECT * FROM |"));
        QTRY_COMPARE_WITH_TIMEOUT(e.completion->cacheState(e.connection), QStringLiteral("ready"),
                                  kWaitMs);
        QSignalSpy changed(e.completion.get(), &CompletionController::changed);
        e.completion->invalidate(e.connection);
        QCOMPARE(e.completion->cacheState(e.connection), QStringLiteral("empty"));
        QVERIFY(changed.count() >= 1);
        ask(e, row, QStringLiteral("SELECT * FROM |"));
        QTRY_COMPARE_WITH_TIMEOUT(e.completion->cacheState(e.connection), QStringLiteral("ready"),
                                  kWaitMs);
        // Disconnecting drops the cache with the session.
        e.controller->tree()->disconnectConnection(e.connection);
        QTRY_COMPARE_WITH_TIMEOUT(e.completion->cacheState(e.connection), QStringLiteral("empty"),
                                  kWaitMs);
    }

    // Typing must never ask for a password: "Ask every time" with none remembered gets keywords
    // and functions, and the notice.
    void noPasswordPromptWhileTyping()
    {
        Env e = start({}, PasswordMode::AskEveryTime, /*remember=*/false);
        const int row = e.tabs->openTab(e.connection);
        QSignalSpy prompt(e.controller->tree(), &SchemaTreeModel::passwordRequested);
        QSignalSpy queryPrompt(e.queries.get(), &QueryController::passwordRequested);
        for (const QString &text : { QStringLiteral("SEL|"), QStringLiteral("SELECT * FROM |"),
                                     QStringLiteral("SELECT a.|") }) {
            const QVariantMap r = ask(e, row, text);
            QCOMPARE(r.value(QStringLiteral("notice")).toString(),
                     QStringLiteral("Connect to suggest tables"));
            QVERIFY(!r.value(QStringLiteral("loading")).toBool());
        }
        const QVariantMap r = ask(e, row, QStringLiteral("SEL|"));
        QVERIFY(r.value(QStringLiteral("count")).toInt() > 0);
        QVERIFY2(has(labels(e), QStringLiteral("SELECT")),
                 qPrintable(labels(e).join(QLatin1Char(','))));
        QVERIFY(kinds(e).contains(QStringLiteral("keyword")));
        QTest::qWait(300);
        QCOMPARE(prompt.count(), 0);
        QCOMPARE(queryPrompt.count(), 0);
        QVERIFY(!e.controller->tree()->isConnected(e.connection));
        QVERIFY(e.controller->tree()->ensureSession(e.connection)
                == SchemaTreeModel::Access::NeedsPassword);

        // Functions are there too (built-ins of the engine).
        ask(e, row, QStringLiteral("SELECT coun|"));
        QVERIFY(has(labels(e), QStringLiteral("count")));
        QVERIFY(kinds(e).contains(QStringLiteral("function")));

        // A tab without a connection has no dialect: only the notice.
        const int bare = e.tabs->openTab(QString());
        e.tabs->setConnection(bare, QString());
        const QVariantMap none = ask(e, bare, QStringLiteral("SEL|"));
        QCOMPARE(none.value(QStringLiteral("count")).toInt(), 0);
        QCOMPARE(none.value(QStringLiteral("notice")).toString(),
                 QStringLiteral("Connect to suggest tables"));
    }

    // Strings and comments have no suggestions; the statement under the cursor is the one used.
    void scopeAndStatementOffsets()
    {
        Env e = start({});
        const QString t = table(e, QStringLiteral("off"));
        run(e, QStringLiteral("CREATE TABLE \"%1\"(id INTEGER)").arg(t));
        const int row = e.tabs->openTab(e.connection);
        QVERIFY(ask(e, row, QStringLiteral("SELECT 'ab|'")).value(QStringLiteral("scope")).toInt()
                == static_cast<int>(CompletionScope::None));
        QCOMPARE(e.completion->model()->count(), 0);
        QCOMPARE(ask(e, row, QStringLiteral("-- se|")).value(QStringLiteral("scope")).toInt(),
                 static_cast<int>(CompletionScope::None));

        ask(e, row, QStringLiteral("SELECT 1;|"));
        QTRY_COMPARE_WITH_TIMEOUT(e.completion->cacheState(e.connection), QStringLiteral("ready"),
                                  kWaitMs);
        // The second statement: offsets are of the whole document.
        const QString prefix = t.left(5);
        const QVariantMap r
            = ask(e, row, QStringLiteral("SELECT 1;\nSELECT * FROM %1|").arg(prefix));
        QCOMPARE(r.value(QStringLiteral("replaceStart")).toInt(), 24);
        QCOMPARE(r.value(QStringLiteral("replaceEnd")).toInt(),
                 24 + static_cast<int>(prefix.size()));
        QVERIFY2(has(labels(e), t), qPrintable(labels(e).join(QLatin1Char(','))));

        // Right after a semicolon a new statement starts: keywords, and the offset is the cursor.
        const QVariantMap fresh = ask(e, row, QStringLiteral("SELECT 1;|"));
        QCOMPARE(fresh.value(QStringLiteral("replaceStart")).toInt(), 9);
        QCOMPARE(fresh.value(QStringLiteral("scope")).toInt(),
                 static_cast<int>(CompletionScope::Keyword));
        drop(e);
    }

    // `"us|"`: the closing quote after the cursor is replaced with the word.
    void closingQuoteIsReplaced()
    {
        Env e = start({});
        const QString t = table(e, QStringLiteral("quo"));
        run(e, QStringLiteral("CREATE TABLE \"%1\"(id INTEGER)").arg(t));
        const int row = e.tabs->openTab(e.connection);
        ask(e, row, QStringLiteral("SELECT * FROM |"));
        QTRY_COMPARE_WITH_TIMEOUT(e.completion->cacheState(e.connection), QStringLiteral("ready"),
                                  kWaitMs);
        const QString head = QStringLiteral("SELECT * FROM \"");
        const QVariantMap r = ask(e, row, head + t.left(4) + QStringLiteral("|\" WHERE 1"));
        QCOMPARE(r.value(QStringLiteral("replaceStart")).toInt(),
                 static_cast<int>(head.size()) - 1);
        QCOMPARE(r.value(QStringLiteral("replaceEnd")).toInt(),
                 static_cast<int>(head.size()) + 4 + 1);
        QVERIFY2(has(labels(e), t), qPrintable(labels(e).join(QLatin1Char(','))));
        // Without a closing quote there is nothing extra to replace.
        const QVariantMap open = ask(e, row, head + t.left(4) + QStringLiteral("|"));
        QCOMPARE(open.value(QStringLiteral("replaceEnd")).toInt(),
                 static_cast<int>(head.size()) + 4);
        drop(e);
    }

    void searchObjectsForThePalette()
    {
        Env e = start({});
        const QString t = table(e, QStringLiteral("find"));
        run(e,
            QStringLiteral("CREATE TABLE \"%1\"(id INTEGER);"
                           "CREATE VIEW \"v_%1\" AS SELECT id FROM \"%1\";")
                .arg(t));
        QAbstractItemModelTester tester(e.completion->objects(),
                                        QAbstractItemModelTester::FailureReportingMode::Fatal);
        // No connection, then the first request opens the session and loads.
        QCOMPARE(e.completion->searchObjects(QString(), QStringLiteral("find"))
                     .value(QStringLiteral("state")),
                 QStringLiteral("none"));
        QCOMPARE(e.completion->searchObjects(e.connection, QStringLiteral("find"))
                     .value(QStringLiteral("state")),
                 QStringLiteral("loading"));
        QTRY_COMPARE_WITH_TIMEOUT(e.completion->cacheState(e.connection), QStringLiteral("ready"),
                                  kWaitMs);
        const QVariantMap r = e.completion->searchObjects(e.connection, QStringLiteral("  FIND "));
        QCOMPARE(r.value(QStringLiteral("state")).toString(), QStringLiteral("ready"));
        CompletionModel *m = e.completion->objects();
        QCOMPARE(m->count(), 2);
        // The table starts with the text, the view only contains it: the table is first.
        QCOMPARE(m->labelAt(0), t);
        QCOMPARE(m->kindTextAt(0), QStringLiteral("table"));
        QCOMPARE(m->labelAt(1), QStringLiteral("v_") + t);
        QCOMPARE(m->kindTextAt(1), QStringLiteral("view"));
        QCOMPARE(m->data(m->index(0), CompletionModel::DetailRole).toString(),
                 QStringLiteral("main"));
        QCOMPARE(m->partsAt(0), (QStringList { QStringLiteral("main"), t }));
        QCOMPARE(e.completion->searchObjects(e.connection, QStringLiteral("zzzz-no"))
                     .value(QStringLiteral("count"))
                     .toInt(),
                 0);
        QVERIFY(e.completion->searchObjects(e.connection, QString())
                    .value(QStringLiteral("count"))
                    .toInt()
                >= 2);
        drop(e);
    }

    void searchObjectsPostgres()
    {
        if (!havePostgres())
            QSKIP("QUARRY_TEST_POSTGRES_URL is not set");
        Env e = start(postgres());
        const QString t = table(e, QStringLiteral("find"));
        run(e, QStringLiteral("CREATE TABLE \"%1\"(id INTEGER)").arg(t));
        e.completion->searchObjects(e.connection, t);
        QTRY_COMPARE_WITH_TIMEOUT(e.completion->cacheState(e.connection), QStringLiteral("ready"),
                                  kWaitMs);
        e.completion->searchObjects(e.connection, t);
        CompletionModel *m = e.completion->objects();
        QCOMPARE(m->count(), 1);
        QCOMPARE(m->data(m->index(0), CompletionModel::DetailRole).toString(),
                 QStringLiteral("public"));
        QCOMPARE(m->partsAt(0), (QStringList { e.engine.database, QStringLiteral("public"), t }));
        drop(e);
    }

    // The palette's choice is found in the tree: the branches on the way are loaded and handed
    // over.
    void revealFindsTheNodeInTheTree()
    {
        Env e = start({});
        const QString t = table(e, QStringLiteral("rev"));
        run(e, QStringLiteral("CREATE TABLE \"%1\"(id INTEGER)").arg(t));
        SchemaTreeModel *tree = e.controller->tree();
        QSignalSpy announced(tree, &SchemaTreeModel::announcement);
        // Not connected: it never opens a session by itself.
        tree->reveal(e.connection, { QStringLiteral("main"), t });
        QCOMPARE(announced.count(), 1);
        QVERIFY(!tree->isConnected(e.connection));

        e.completion->searchObjects(e.connection, t);
        QTRY_COMPARE_WITH_TIMEOUT(e.completion->cacheState(e.connection), QStringLiteral("ready"),
                                  kWaitMs);
        QSignalSpy revealed(tree, &SchemaTreeModel::revealed);
        tree->reveal(e.connection, { QStringLiteral("main"), t });
        QTRY_COMPARE_WITH_TIMEOUT(revealed.count(), 1, kWaitMs);
        const QModelIndex target = revealed.at(0).at(0).toModelIndex();
        QVERIFY(target.isValid());
        QCOMPARE(tree->data(target, SchemaTreeModel::NameRole).toString(), t);
        QCOMPARE(tree->data(target, SchemaTreeModel::KindRole).toString(), QStringLiteral("table"));
        const QVariantList chain = revealed.at(0).at(1).toList();
        QCOMPARE(chain.size(), 3); // connection, database, the Tables folder
        QCOMPARE(tree->data(chain.at(0).toModelIndex(), SchemaTreeModel::KindRole).toString(),
                 QStringLiteral("connection"));
        QCOMPARE(tree->parent(target), chain.last().toModelIndex());

        // Something that is not there ends with a message, not a hang.
        const int before = static_cast<int>(announced.count());
        tree->reveal(e.connection, { QStringLiteral("main"), QStringLiteral("no_such_table") });
        QTRY_VERIFY_WITH_TIMEOUT(std::ranges::any_of(announced.mid(before),
                                                     [](const QList<QVariant> &args) {
                                                         return args.at(0).toString().contains(
                                                             QLatin1String("no_such_table"));
                                                     }),
                                 kWaitMs);
        drop(e);
    }
};

QTEST_MAIN(TstCompletionController)
#include "tst_completion_controller.moc"
