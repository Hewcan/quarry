#include "ai_schema_adapter.h"

#include <quarry/ai/context_builder.h>
#include <quarry/provider_registry.h>

#include <QFile>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <sqlite3.h>

using namespace quarry;
using namespace quarry::ui;

namespace {

const QString kSentinel = QStringLiteral("SENTINEL_ROW_VALUE_42");
const QString kDefaultSentinel = QStringLiteral("SENTINEL_DEFAULT_77");

// Everything a context says, as one text: to check that nothing but structure is in it.
QString flatten(const ai::SchemaContext &context)
{
    QStringList out { context.dialect };
    for (const ai::AiTable &t : context.tables) {
        out << t.schema << t.name << t.comment << t.definition << t.checks.join(QLatin1Char(' '));
        for (const ai::AiColumn &c : t.columns)
            out << c.name << c.type << c.comment << c.defaultValue
                << c.enumValues.join(QLatin1Char(' '));
        for (const ai::AiForeignKey &f : t.foreignKeys)
            out << f.columns << f.refSchema << f.refTable << f.refColumns;
        for (const ai::AiIndex &i : t.indexes)
            out << i.name << i.columns;
    }
    return out.join(QLatin1Char('\n'));
}

const ai::AiTable *tableNamed(const ai::SchemaContext &context, const QString &name)
{
    for (const ai::AiTable &t : context.tables) {
        if (t.name == name)
            return &t;
    }
    return nullptr;
}

} // namespace

class TstAiSchemaAdapter : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    ProviderRegistry m_registry;
    QString m_file;

    ConnectionProfile profile() const
    {
        ConnectionProfile p;
        p.id = QUuid::createUuid();
        p.name = QStringLiteral("t");
        p.providerId = QStringLiteral("sqlite");
        p.values = { { QStringLiteral("file"), m_file } };
        return p;
    }

    struct Live
    {
        std::unique_ptr<Session> session;
    };

    Live openSession()
    {
        Live live;
        auto connection = m_registry.createConnection(profile(), {});
        if (!connection)
            return live;
        live.session = std::make_unique<Session>(std::move(connection).value());
        QSignalSpy opened(live.session.get(), &Session::opened);
        live.session->open();
        if (!opened.wait(10000))
            live.session.reset();
        return live;
    }

    static QString slurp(const QString &name)
    {
        QFile f(QStringLiteral(QUARRY_APP_UI_DIR "/") + name);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    }

private slots:
    void initTestCase()
    {
        qRegisterMetaType<quarry::ai::SchemaContext>();
        m_registry.scan(QStringLiteral(QUARRY_PLUGIN_DIR));
        m_file = m_dir.filePath(QStringLiteral("shop.db"));
        sqlite3 *db = nullptr;
        QVERIFY(sqlite3_open(m_file.toUtf8().constData(), &db) == SQLITE_OK);
        // Tables with a foreign key, a unique index, a CHECK, a default and a view; and rows with a
        // marker that must never come out of the adapter.
        const QByteArray sql = "CREATE TABLE customers(id INTEGER PRIMARY KEY, name TEXT NOT NULL, "
                               "note TEXT DEFAULT '"
            + kDefaultSentinel.toUtf8()
            + "');"
              "CREATE UNIQUE INDEX customers_name ON customers(name);"
              "CREATE TABLE invoices(id INTEGER PRIMARY KEY, customer_id INTEGER NOT NULL "
              "REFERENCES customers(id), total REAL CHECK (total > 0));"
              "CREATE INDEX invoices_customer ON invoices(customer_id, total);"
              "CREATE TABLE items(id INTEGER PRIMARY KEY, invoice_id INTEGER);"
              "CREATE TABLE products(id INTEGER PRIMARY KEY, title TEXT);"
              "CREATE VIEW v_totals AS SELECT customer_id, sum(total) AS t FROM invoices "
              "GROUP BY customer_id;"
              "INSERT INTO customers(name, note) VALUES ('"
            + kSentinel.toUtf8()
            + "', 'x');"
              "INSERT INTO invoices(customer_id, total) VALUES (1, 99.5);"
              "INSERT INTO products(title) VALUES ('"
            + kSentinel.toUtf8() + "');";
        char *err = nullptr;
        const int rc = sqlite3_exec(db, sql.constData(), nullptr, nullptr, &err);
        const QString message = err ? QString::fromUtf8(err) : QString();
        sqlite3_free(err);
        sqlite3_close(db);
        QVERIFY2(rc == SQLITE_OK, qPrintable(message));
    }

    void dialectsFollowTheProvider()
    {
        QCOMPARE(AiSchemaAdapter::dialectOf(QStringLiteral("sqlite")), QStringLiteral("sqlite"));
        QCOMPARE(AiSchemaAdapter::dialectOf(QStringLiteral("postgres")),
                 QStringLiteral("postgresql"));
        QCOMPARE(AiSchemaAdapter::dialectOf(QStringLiteral("mysql")), QStringLiteral("mysql"));
        QCOMPARE(AiSchemaAdapter::dialectOf(QStringLiteral("mssql")), QStringLiteral("sqlserver"));
        QCOMPARE(AiSchemaAdapter::dialectOf(QStringLiteral("odbc")), QStringLiteral("sql"));
    }

    // The conversion of a provider's own description (IConnection::describe, real SQLite).
    void detailsOfTheProviderBecomeAnAiTable()
    {
        auto connection = m_registry.createConnection(profile(), {});
        QVERIFY(connection.has_value());
        std::unique_ptr<IConnection> conn = std::move(connection).value();
        QVERIFY(conn->open({}).has_value());
        auto details
            = conn->describe(ObjectPath { { QStringLiteral("main"), QStringLiteral("invoices") },
                                          ObjectKind::Table },
                             {});
        QVERIFY(details.has_value());
        const ai::AiTable t = AiSchemaAdapter::tableFromDetails({}, QStringLiteral("invoices"),
                                                                false, details.value());
        QCOMPARE(t.name, QStringLiteral("invoices"));
        QCOMPARE(t.columns.size(), size_t(3));
        QVERIFY(t.columns[0].primaryKey);
        QCOMPARE(t.columns[1].name, QStringLiteral("customer_id"));
        QVERIFY(!t.columns[1].nullable);
        QCOMPARE(t.foreignKeys.size(), size_t(1));
        QCOMPARE(t.foreignKeys[0].refTable, QStringLiteral("customers"));
        QCOMPARE(t.foreignKeys[0].refColumns, QStringList { QStringLiteral("id") });
        QCOMPARE(t.foreignKeys[0].columns, QStringList { QStringLiteral("customer_id") });
        QVERIFY(!t.indexes.empty());
        QVERIFY(!t.isView);
        // A default the provider reports travels in the context; ContextOptions decides later.
        auto cust
            = conn->describe(ObjectPath { { QStringLiteral("main"), QStringLiteral("customers") },
                                          ObjectKind::Table },
                             {});
        QVERIFY(cust.has_value());
        const ai::AiTable c = AiSchemaAdapter::tableFromDetails({}, QStringLiteral("customers"),
                                                                false, cust.value());
        bool sawDefault = false;
        for (const ai::AiColumn &col : c.columns)
            sawDefault = sawDefault || col.defaultValue.contains(kDefaultSentinel);
        QVERIFY(sawDefault);
        const ai::ContextOptions off; // sendDefaults is off by default: it never reaches the prompt
        ai::SchemaContext all;
        all.dialect = QStringLiteral("sqlite");
        all.tables.push_back(c);
        ai::ContextBuilder builder;
        builder.setSchema(all);
        QVERIFY(!builder.build(u"select * from customers", QStringView(), off)
                     .ddl.contains(kDefaultSentinel));
        conn->close();
    }

    void buildsTheStructureOfARealSqliteDatabase()
    {
        Live live = openSession();
        QVERIFY(live.session);
        AiSchemaAdapter adapter;
        QSignalSpy ready(&adapter, &AiSchemaAdapter::contextReady);
        QSignalSpy failed(&adapter, &AiSchemaAdapter::contextFailed);
        AiSchemaAdapter::Request request;
        request.session = live.session.get();
        request.providerId = QStringLiteral("sqlite");
        const quint64 id = adapter.contextFor(request);
        QVERIFY(id > 0);
        QCOMPARE(ready.size(), 0); // never inside the call
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 15000);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(ready.at(0).at(0).toULongLong(), id);
        const auto context = ready.at(0).at(1).value<ai::SchemaContext>();
        QVERIFY(!ready.at(0).at(2).toBool()); // not truncated
        QCOMPARE(context.dialect, QStringLiteral("sqlite"));
        QCOMPARE(context.tables.size(), size_t(5)); // 4 tables + the view

        const ai::AiTable *customers = tableNamed(context, QStringLiteral("customers"));
        const ai::AiTable *invoices = tableNamed(context, QStringLiteral("invoices"));
        const ai::AiTable *view = tableNamed(context, QStringLiteral("v_totals"));
        QVERIFY(customers && invoices && view);
        QVERIFY(view->isView);
        QVERIFY(!customers->isView);
        QCOMPARE(customers->columns.size(), size_t(3));
        QCOMPARE(customers->columns[0].name, QStringLiteral("id"));
        QVERIFY(customers->columns[0].primaryKey);
        QCOMPARE(customers->columns[1].name, QStringLiteral("name"));
        QVERIFY(!customers->columns[1].nullable);
        QCOMPARE(customers->columns[1].type, QStringLiteral("TEXT"));
        QVERIFY(customers->columns[2].nullable);
        QCOMPARE(view->columns.size(), size_t(2));

        // The foreign key and the unique index come from the same introspection as the tree.
        QCOMPARE(invoices->foreignKeys.size(), size_t(1));
        QCOMPARE(invoices->foreignKeys[0].refTable, QStringLiteral("customers"));
        QCOMPARE(invoices->foreignKeys[0].refColumns, QStringList { QStringLiteral("id") });
        // Both sides of the key (Session::describe): the local column too.
        QCOMPARE(invoices->foreignKeys[0].columns, QStringList { QStringLiteral("customer_id") });
        // The defaults of the provider come along, and ContextOptions decides what is sent.
        bool sawDefault = false;
        for (const ai::AiColumn &col : customers->columns)
            sawDefault = sawDefault || col.defaultValue.contains(kDefaultSentinel);
        QVERIFY(sawDefault);
        bool unique = false;
        for (const ai::AiIndex &i : customers->indexes) {
            if (i.name == QLatin1String("customers_name")) {
                unique = i.unique;
                QCOMPARE(i.columns, QStringList { QStringLiteral("name") });
            }
        }
        QVERIFY(unique);
        QVERIFY(!invoices->indexes.empty());
        bool compound = false;
        for (const ai::AiIndex &i : invoices->indexes)
            compound = compound || (i.columns.size() == 2 && !i.unique);
        QVERIFY(compound);

        // Structure only: no row value, and nothing the provider did not give us.
        const QString text = flatten(context);
        QVERIFY(!text.contains(kSentinel));
        QVERIFY(!text.contains(QStringLiteral("99.5")));
        QVERIFY(customers->comment.isEmpty() && customers->definition.isEmpty());
        QVERIFY(view->definition.isEmpty());

        // And the context goes into the prompt builder as it is.
        ai::ContextBuilder builder;
        builder.setSchema(context);
        const ai::SelectedContext selected
            = builder.build(u"select * from invoices", QStringView(), ai::ContextOptions {});
        QVERIFY(selected.tables.size() >= 2); // invoices and, one hop away, customers
        QVERIFY(!selected.ddl.contains(kSentinel));
        // The foreign key reads with both sides, and the options decide the rest: a default of
        // the provider and a comment are in the text only when their option is on.
        QVERIFY2(selected.ddl.contains(QStringLiteral("customer_id")), qPrintable(selected.ddl));
        QVERIFY(!selected.ddl.contains(kDefaultSentinel));
        ai::SchemaContext commented = context;
        for (ai::AiTable &t : commented.tables) {
            if (t.name == QLatin1String("customers"))
                t.comment = QStringLiteral("People who buy things");
        }
        builder.setSchema(commented);
        QVERIFY(!builder.build(u"select * from customers", QStringView(), ai::ContextOptions {})
                     .ddl.contains(QStringLiteral("People who buy things")));
        ai::ContextOptions all;
        all.sendComments = true;
        all.sendDefaults = true;
        const QString withAll = builder.build(u"select * from customers", QStringView(), all).ddl;
        QVERIFY2(withAll.contains(QStringLiteral("People who buy things")), qPrintable(withAll));
        QVERIFY2(withAll.contains(kDefaultSentinel), qPrintable(withAll));
        QVERIFY(!withAll.contains(kSentinel)); // a default is structure; a row never is
    }

    void respectsTheLimitAndTheNamesToPrioritize()
    {
        Live live = openSession();
        QVERIFY(live.session);
        AiSchemaAdapter adapter;
        QSignalSpy ready(&adapter, &AiSchemaAdapter::contextReady);
        AiSchemaAdapter::Request request;
        request.session = live.session.get();
        request.providerId = QStringLiteral("sqlite");
        request.limit = 2;
        adapter.contextFor(request);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 15000);
        auto context = ready.at(0).at(1).value<ai::SchemaContext>();
        QCOMPARE(context.tables.size(), size_t(2));
        QVERIFY(ready.at(0).at(2).toBool()); // truncated: the connection has more

        request.limit = 1;
        request.prioritize = { QStringLiteral("PRODUCTS") }; // case-insensitive
        adapter.contextFor(request);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 2, 15000);
        context = ready.at(1).at(1).value<ai::SchemaContext>();
        QCOMPARE(context.tables.size(), size_t(1));
        QCOMPARE(context.tables[0].name, QStringLiteral("products"));

        // The cap is never above kMaxTables, whatever is asked.
        request.limit = 100000;
        request.prioritize.clear();
        adapter.contextFor(request);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 3, 15000);
        QVERIFY(ready.at(2).at(1).value<ai::SchemaContext>().tables.size()
                <= size_t(AiSchemaAdapter::kMaxTables));
    }

    void excludedTablesAreNeverDescribed()
    {
        Live live = openSession();
        QVERIFY(live.session);
        AiSchemaAdapter adapter;
        QSignalSpy ready(&adapter, &AiSchemaAdapter::contextReady);
        AiSchemaAdapter::Request request;
        request.session = live.session.get();
        request.providerId = QStringLiteral("sqlite");
        request.options.excludedTables = { QStringLiteral("CUSTOMERS") };
        adapter.contextFor(request);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 15000);
        const auto context = ready.at(0).at(1).value<ai::SchemaContext>();
        QVERIFY(tableNamed(context, QStringLiteral("customers")) == nullptr);
        QVERIFY(tableNamed(context, QStringLiteral("invoices")) != nullptr);
    }

    void usesTheCacheOfTheConnectionWhenItIsReady()
    {
        Live live = openSession();
        QVERIFY(live.session);
        MetadataCache cache;
        cache.attach(live.session.get());
        cache.load();
        QTRY_VERIFY_WITH_TIMEOUT(cache.state() == MetadataCache::State::Ready, 15000);

        AiSchemaAdapter adapter;
        QSignalSpy ready(&adapter, &AiSchemaAdapter::contextReady);
        AiSchemaAdapter::Request request;
        request.session = live.session.get();
        request.providerId = QStringLiteral("sqlite");
        request.cache = &cache;
        adapter.contextFor(request);
        QCOMPARE(ready.size(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 15000);
        QCOMPARE(ready.at(0).at(1).value<ai::SchemaContext>().tables.size(), size_t(5));
    }

    void cancellingStopsTheRequest()
    {
        Live live = openSession();
        QVERIFY(live.session);
        AiSchemaAdapter adapter;
        QSignalSpy ready(&adapter, &AiSchemaAdapter::contextReady);
        QSignalSpy cancelled(&adapter, &AiSchemaAdapter::contextCancelled);
        AiSchemaAdapter::Request request;
        request.session = live.session.get();
        request.providerId = QStringLiteral("sqlite");
        const quint64 id = adapter.contextFor(request);
        QVERIFY(adapter.isRunning(id));
        adapter.cancel(id);
        QVERIFY(!adapter.isRunning(id));
        QCOMPARE(cancelled.size(), 1);
        QCOMPARE(cancelled.at(0).at(0).toULongLong(), id);
        QTest::qWait(500); // the answers still on their way are ignored
        QCOMPARE(ready.size(), 0);
        adapter.cancel(id); // unknown now: nothing
        QCOMPARE(cancelled.size(), 1);
    }

    void withoutASessionItFailsInPlainLanguage()
    {
        AiSchemaAdapter adapter;
        QSignalSpy failed(&adapter, &AiSchemaAdapter::contextFailed);
        AiSchemaAdapter::Request request;
        request.providerId = QStringLiteral("sqlite");
        adapter.contextFor(request);
        QCOMPARE(failed.size(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 5000);
        QVERIFY(!failed.at(0).at(1).toString().isEmpty());
    }

    // The adapter reads structure only: its sources cannot name a result, a row or a statement.
    void theAdapterNeverTouchesResultsOrExecution()
    {
        static const QRegularExpression kForbidden(QStringLiteral(
            "result_batch|column_buffer|\\b(ResultBatch|ColumnBuffer|IResultStream|"
            "ResultTableModel|QueryController|Statement|ExecutionStats|SqlSafety)\\b"
            "|\\b(execute|fetch|fetchMore|applyChanges|batchReady|columnsReady)\\s*\\("));
        for (const QString &name :
             { QStringLiteral("ai_schema_adapter.h"), QStringLiteral("ai_schema_adapter.cpp") }) {
            const QString text = slurp(name);
            QVERIFY2(!text.isEmpty(), qPrintable(name));
            const auto match = kForbidden.match(text);
            QVERIFY2(!match.hasMatch(),
                     qPrintable(name + QStringLiteral(" mentions ") + match.captured()));
        }
    }
};

QTEST_MAIN(TstAiSchemaAdapter)
#include "tst_ai_schema_adapter.moc"
