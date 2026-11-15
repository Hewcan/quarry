#include "postgres_support.h"
#include "provider_conformance.h"

#include <quarry/provider_registry.h>
#include <quarry/session.h>

#include <QSignalSpy>
#include <QUrl>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <future>
#include <limits>

using namespace quarry;
using namespace quarry::conformance;

namespace {

const QString kUrlVariable = QStringLiteral("QUARRY_TEST_POSTGRES_URL");

// Connection settings from QUARRY_TEST_POSTGRES_URL (postgresql://user:password@host:port/db).
struct ServerUrl
{
    QString host;
    int port = 5432;
    QString user;
    QString password;
    QString database;
    bool valid = false;

    static ServerUrl fromEnvironment()
    {
        ServerUrl out;
        const QUrl url(qEnvironmentVariable(qPrintable(kUrlVariable)));
        if (!url.isValid() || url.host().isEmpty())
            return out;
        out.host = url.host();
        out.port = url.port(5432);
        out.user = url.userName();
        out.password = url.password();
        out.database = url.path().mid(1);
        out.valid = true;
        return out;
    }

    ConnectionConfig config(const QString &databaseName) const
    {
        ConnectionConfig c;
        c.values.insert(QStringLiteral("host"), host);
        c.values.insert(QStringLiteral("port"), port);
        c.values.insert(QStringLiteral("user"), user);
        c.values.insert(QStringLiteral("database"), databaseName);
        return c;
    }
};

// Runs one statement to its end on a throwaway connection; empty string on success.
QString runOnce(IDatabaseProvider &provider, const ConnectionConfig &config, const QString &sql,
                const Secrets &secrets = {})
{
    auto connection = provider.createConnection(config, secrets);
    auto info = connection->open({});
    if (!info)
        return info.error().message + QStringLiteral(" | ") + info.error().detail;
    auto stream = connection->execute(Statement { sql, {}, 500 }, {});
    if (!stream)
        return stream.error().message + QStringLiteral(" | ") + stream.error().detail;
    while (!stream.value()->atEnd()) {
        auto batch = stream.value()->fetch(500, {});
        if (!batch)
            return batch.error().message;
    }
    return {};
}

class PostgresFixture : public Fixture
{
public:
    PostgresFixture(IDatabaseProvider &provider, const ServerUrl &server, QString database)
        : m_provider(provider)
        , m_server(server)
        , m_database(std::move(database))
    { }

    // Fresh database per test: no collisions between tests or with other users of the server.
    QString create()
    {
        return runOnce(m_provider, m_server.config(m_server.database),
                       QStringLiteral("CREATE DATABASE \"%1\"").arg(m_database));
    }
    ~PostgresFixture() override
    {
        if (!m_created)
            return;
        runOnce(m_provider, m_server.config(m_server.database),
                QStringLiteral("DROP DATABASE IF EXISTS \"%1\" WITH (FORCE)").arg(m_database));
    }
    void markCreated() { m_created = true; }

    IDatabaseProvider &provider() override { return m_provider; }
    ConnectionConfig config() override { return m_server.config(m_database); }
    QString sampleSchemaSql() const override
    {
        return QStringLiteral(
            "CREATE TABLE authors(id BIGINT PRIMARY KEY, name TEXT NOT NULL UNIQUE);\n"
            "CREATE TABLE books(id BIGINT PRIMARY KEY, title TEXT NOT NULL,\n"
            "  author_id BIGINT NOT NULL REFERENCES authors(id), price REAL, cover BYTEA);\n"
            "CREATE INDEX idx_books_title ON books(title); -- a comment; with a semicolon\n"
            "CREATE VIEW v_titles AS SELECT id, title FROM books;\n");
    }
    QString slowQuerySql() const override { return QStringLiteral("SELECT pg_sleep(3600)"); }
    QString seriesSql(int count) const override
    {
        return QStringLiteral("SELECT g AS n FROM generate_series(1, %1) AS g").arg(count);
    }
    QString placeholder(int index) const override { return QStringLiteral("$%1").arg(index); }
    QString rowIdentifierName() const override { return QStringLiteral("ctid"); }
    ObjectPath booksPath() const override
    {
        return { { m_database, QStringLiteral("public"), QStringLiteral("books") },
                 ObjectKind::Table };
    }
    ObjectPath viewPath() const override
    {
        return { { m_database, QStringLiteral("public"), QStringLiteral("v_titles") },
                 ObjectKind::View };
    }
    ValueType fractionalLiteralType() const override { return ValueType::Decimal; }
    std::vector<SplitCase> extraSplitCases() const override
    {
        return {
            { QStringLiteral("DO $$ BEGIN PERFORM 1; END $$; SELECT 2"),
              { QStringLiteral("DO $$ BEGIN PERFORM 1; END $$"), QStringLiteral("SELECT 2") } },
            { QStringLiteral("CREATE FUNCTION f() RETURNS int AS $fn$ SELECT 1; $$ ; $fn$ "
                             "LANGUAGE sql; SELECT 3"),
              { QStringLiteral("CREATE FUNCTION f() RETURNS int AS $fn$ SELECT 1; $$ ; $fn$ "
                               "LANGUAGE sql"),
                QStringLiteral("SELECT 3") } },
            { QStringLiteral("SELECT E'it\\'s; x'; SELECT 2"),
              { QStringLiteral("SELECT E'it\\'s; x'"), QStringLiteral("SELECT 2") } },
            { QStringLiteral("SELECT 'a\\'; SELECT 2"),
              { QStringLiteral("SELECT 'a\\'"), QStringLiteral("SELECT 2") } },
            { QStringLiteral("/* a /* ; */ ; */ SELECT 1; SELECT 2"),
              { QStringLiteral("/* a /* ; */ ; */ SELECT 1"), QStringLiteral("SELECT 2") } },
            { QStringLiteral("SELECT $1; SELECT 2"),
              { QStringLiteral("SELECT $1"), QStringLiteral("SELECT 2") } },
            { QStringLiteral("SELECT a$b$c; SELECT 2"),
              { QStringLiteral("SELECT a$b$c"), QStringLiteral("SELECT 2") } },
            { QStringLiteral("SELECT \"a\"\";b\"; SELECT 2"),
              { QStringLiteral("SELECT \"a\"\";b\""), QStringLiteral("SELECT 2") } },
            { QStringLiteral("SELECT $$unterminated; SELECT 2"),
              { QStringLiteral("SELECT $$unterminated; SELECT 2") } },
        };
    }

private:
    IDatabaseProvider &m_provider;
    ServerUrl m_server;
    QString m_database;
    bool m_created = false;
};

#define REQUIRE_SERVER()                                                                           \
    do {                                                                                           \
        if (!m_server.valid)                                                                       \
            QSKIP("QUARRY_TEST_POSTGRES_URL is not set");                                          \
    } while (false)

} // namespace

class TstPostgresProvider : public ProviderConformanceTest
{
    Q_OBJECT

protected:
    std::unique_ptr<Fixture> createFixture() override
    {
        if (!m_server.valid) {
            // Shared tests need a server, except splitStatements. Our own tests never use the
            // fixture; they skip by themselves when they need a server.
            const QByteArray name = QTest::currentTestFunction();
            const bool shared
                = ProviderConformanceTest::staticMetaObject.indexOfSlot(name + "()") >= 0;
            if (shared && name != "splitStatements") {
                QTest::qSkip("QUARRY_TEST_POSTGRES_URL is not set", __FILE__, __LINE__);
                return nullptr;
            }
            return std::make_unique<PostgresFixture>(*m_provider, m_server, QString());
        }
        auto fixture
            = std::make_unique<PostgresFixture>(*m_provider, m_server,
                                                QStringLiteral("quarry_t_%1_%2")
                                                    .arg(QCoreApplication::applicationPid())
                                                    .arg(++m_counter));
        if (const QString error = fixture->create(); !error.isEmpty()) {
            QTest::qFail(qPrintable(QStringLiteral("CREATE DATABASE failed: ") + error), __FILE__,
                         __LINE__);
            return nullptr;
        }
        fixture->markCreated();
        return fixture;
    }

private slots:
    void initTestCase();
    void cleanupTestCase();

    // No server needed
    void pluginMetadata();
    void connParamsAreKeywordArrays();
    void connParamsRejectBadInput();
    void sqlStateMapping();
    void errorPositionConversion();
    void byteaHexDecoding();
    void oidMapping();
    void dialectSpecifics();
    void connectionRefusedIsConnectionError();

    // Server needed
    void typesRoundTrip();
    void parameterTypes();
    void fetchSizeIndependentOfChunk();
    void droppedStreamKeepsConnectionUsable();
    void cancelPgSleepIsFast();
    void cancelWhileFetchingChunks();
    void lateCancelNeverHitsNextStatement();
    void sessionCancelsPgSleep();
    void syntaxErrorPositionIsInCharacters();
    void authenticationFailure();
    void unknownDatabase();
    void sslModes();
    void treeHidesSystemSchemas();
    void noticesAreCollected();
    void dollarQuotedScriptRuns();
    void transactions();
    void multipleStatementsAreRejected();
    void emptyAndCommentOnlyStatements();

private:
    std::unique_ptr<IConnection> openFresh(QString *databaseName = nullptr);
    Drained run(IConnection &connection, const QString &sql) { return drain(connection, sql); }

    ProviderRegistry m_registry;
    IDatabaseProvider *m_provider = nullptr;
    ServerUrl m_server;
    int m_counter = 0;
    std::unique_ptr<PostgresFixture> m_scratch;
};

void TstPostgresProvider::initTestCase()
{
    m_registry.scan(QStringLiteral(QUARRY_PLUGIN_DIR));
    QString error;
    m_provider = m_registry.provider(QStringLiteral("postgres"), &error);
    QVERIFY2(m_provider, qPrintable(error));
    m_server = ServerUrl::fromEnvironment();
    // The conformance suite creates connections without secrets: libpq reads the password of
    // this process from the environment, as a psql user would.
    if (m_server.valid)
        qputenv("PGPASSWORD", m_server.password.toUtf8());
}

void TstPostgresProvider::cleanupTestCase()
{
    m_scratch.reset(); // drops the scratch database of the last test
    qunsetenv("PGPASSWORD");
}

std::unique_ptr<IConnection> TstPostgresProvider::openFresh(QString *databaseName)
{
    m_scratch = std::make_unique<PostgresFixture>(
        *m_provider, m_server,
        QStringLiteral("quarry_s_%1_%2").arg(QCoreApplication::applicationPid()).arg(++m_counter));
    if (const QString error = m_scratch->create(); !error.isEmpty()) {
        QTest::qFail(qPrintable(error), __FILE__, __LINE__);
        return nullptr;
    }
    m_scratch->markCreated();
    if (databaseName)
        *databaseName = m_scratch->config().values.value(QStringLiteral("database")).toString();
    auto connection = m_provider->createConnection(m_scratch->config(), {});
    auto info = connection->open({});
    if (!info) {
        QTest::qFail(qPrintable(info.error().detail), __FILE__, __LINE__);
        return nullptr;
    }
    return connection;
}

// ---- no server -----------------------------------------------------------------------------

void TstPostgresProvider::pluginMetadata()
{
    const auto &entries = m_registry.available();
    const auto entry = std::find_if(entries.begin(), entries.end(), [](const ProviderEntry &e) {
        return e.id == QLatin1String("postgres");
    });
    QVERIFY(entry != entries.end());
    QCOMPARE(entry->displayName, QStringLiteral("PostgreSQL"));
    QCOMPARE(entry->apiVersion, kProviderApiVersion);
    const ProviderInfo info = m_provider->info();
    QCOMPARE(info.id, QStringLiteral("postgres"));
    QVERIFY(info.caps.testFlag(Capability::Cancel));
    QVERIFY(info.caps.testFlag(Capability::Schemas));
    QVERIFY(!info.caps.testFlag(Capability::MultipleDatabases));

    const auto fields = m_provider->connectionFields();
    QCOMPARE(fields.size(), size_t(6));
    QCOMPARE(fields[0].key, QStringLiteral("host"));
    QCOMPARE(fields[0].kind, ConnectionField::Kind::Text);
    QCOMPARE(fields[0].defaultValue.toString(), QStringLiteral("localhost"));
    QCOMPARE(fields[1].key, QStringLiteral("port"));
    QCOMPARE(fields[1].kind, ConnectionField::Kind::Number);
    QCOMPARE(fields[1].defaultValue.toInt(), 5432);
    QCOMPARE(fields[2].key, QStringLiteral("database"));
    QVERIFY(fields[2].required);
    QCOMPARE(fields[3].key, QStringLiteral("user"));
    QVERIFY(fields[3].required);
    QCOMPARE(fields[4].key, QStringLiteral("sslmode"));
    QCOMPARE(fields[4].kind, ConnectionField::Kind::Enum);
    QCOMPARE(fields[4].enumValues,
             (QStringList { QStringLiteral("disable"), QStringLiteral("allow"),
                            QStringLiteral("prefer"), QStringLiteral("require"),
                            QStringLiteral("verify-ca"), QStringLiteral("verify-full") }));
    QCOMPARE(fields[4].defaultValue.toString(), QStringLiteral("prefer"));
    QCOMPARE(fields[5].key, QStringLiteral("sslrootcert"));
    QCOMPARE(fields[5].kind, ConnectionField::Kind::Path);
    QVERIFY(fields[5].advanced && !fields[5].required);
    for (const auto &field : fields)
        QVERIFY(field.kind != ConnectionField::Kind::Secret); // the password never is a field
}

void TstPostgresProvider::connParamsAreKeywordArrays()
{
    QVariantMap values;
    values.insert(QStringLiteral("host"), QStringLiteral("db host"));
    values.insert(QStringLiteral("port"), QStringLiteral("6543"));
    values.insert(QStringLiteral("database"), QStringLiteral("my db'x"));
    values.insert(QStringLiteral("user"), QStringLiteral("o'brien"));
    auto result = postgres::buildConnParams(values, QStringLiteral("p w'\\ d"));
    QVERIFY(result);
    QMap<QByteArray, QByteArray> map;
    for (const auto &[key, value] : result.value())
        map.insert(key, value);
    // Values with spaces and quotes pass through untouched: no conninfo string is ever built.
    QCOMPARE(map.value("host"), QByteArray("db host"));
    QCOMPARE(map.value("port"), QByteArray("6543"));
    QCOMPARE(map.value("dbname"), QByteArray("my db'x"));
    QCOMPARE(map.value("user"), QByteArray("o'brien"));
    QCOMPARE(map.value("password"), QByteArray("p w'\\ d"));
    QCOMPARE(map.value("sslmode"), QByteArray("prefer"));
    QCOMPARE(map.value("connect_timeout"), QByteArray("15"));
    QCOMPARE(map.value("application_name"), QByteArray("Quarry"));
    QCOMPARE(map.value("client_encoding"), QByteArray("UTF8"));
    QVERIFY(map.value("options").contains("DateStyle=ISO"));
    QVERIFY(!map.contains("sslrootcert")); // optional field left empty

    // Defaults, no password, numbers as they come back from quarry.db (double / qint64).
    QVariantMap minimal;
    minimal.insert(QStringLiteral("port"), 5432.0);
    minimal.insert(QStringLiteral("sslmode"), QStringLiteral("verify-full"));
    minimal.insert(QStringLiteral("sslrootcert"), QStringLiteral("C:/certs/root ca.pem"));
    auto defaults = postgres::buildConnParams(minimal, {});
    QVERIFY(defaults);
    map.clear();
    for (const auto &[key, value] : defaults.value())
        map.insert(key, value);
    QCOMPARE(map.value("host"), QByteArray("localhost"));
    QCOMPARE(map.value("port"), QByteArray("5432"));
    QVERIFY(!map.contains("password"));
    QVERIFY(!map.contains("dbname"));
    QCOMPARE(map.value("sslmode"), QByteArray("verify-full"));
    QCOMPARE(map.value("sslrootcert"), QByteArray("C:/certs/root ca.pem"));
}

void TstPostgresProvider::connParamsRejectBadInput()
{
    QVariantMap values;
    values.insert(QStringLiteral("sslmode"), QStringLiteral("verify-all"));
    auto badMode = postgres::buildConnParams(values, {});
    QVERIFY(!badMode);
    QCOMPARE(std::move(badMode).error().kind, ErrorKind::Connection);

    for (const QString &port :
         { QStringLiteral("abc"), QStringLiteral("0"), QStringLiteral("70000") }) {
        QVariantMap bad;
        bad.insert(QStringLiteral("port"), port);
        QVERIFY2(!postgres::buildConnParams(bad, {}), qPrintable(port));
    }
}

void TstPostgresProvider::sqlStateMapping()
{
    using postgres::kindForSqlState;
    QCOMPARE(kindForSqlState("28P01"), ErrorKind::Authentication);
    QCOMPARE(kindForSqlState("28000"), ErrorKind::Authentication);
    QCOMPARE(kindForSqlState("42601"), ErrorKind::Syntax);
    QCOMPARE(kindForSqlState("42501"), ErrorKind::Permission);
    QCOMPARE(kindForSqlState("23505"), ErrorKind::Constraint);
    QCOMPARE(kindForSqlState("23503"), ErrorKind::Constraint);
    QCOMPARE(kindForSqlState("42P01"), ErrorKind::NotFound);
    QCOMPARE(kindForSqlState("42703"), ErrorKind::NotFound);
    QCOMPARE(kindForSqlState("57014"), ErrorKind::Cancelled);
    QCOMPARE(kindForSqlState("08006"), ErrorKind::Connection);
    QCOMPARE(kindForSqlState("22012"), ErrorKind::Driver);
    QCOMPARE(kindForSqlState(""), ErrorKind::Driver);
    QVERIFY(!postgres::messageForSqlState("23505").isEmpty());
}

void TstPostgresProvider::errorPositionConversion()
{
    using postgres::utf16OffsetFromCharPosition;
    QCOMPARE(utf16OffsetFromCharPosition(u"SELEC 1", 1), 0);
    QCOMPARE(utf16OffsetFromCharPosition(u"SELEC 1", 7), 6);
    QCOMPARE(utf16OffsetFromCharPosition(u"SELEC 1", 8), 7); // just past the end
    QCOMPARE(utf16OffsetFromCharPosition(u"SELEC 1", 9), -1);
    QCOMPARE(utf16OffsetFromCharPosition(u"x", 0), -1);
    // 'é' is two bytes in UTF-8 but one character; the emoji is one character and two UTF-16
    // units. Character 6 (1-based) is the "F" after "'é😀' ".
    const QString text = QStringLiteral("'é😀' F");
    QCOMPARE(utf16OffsetFromCharPosition(text, 6), 6);
    QCOMPARE(text.at(6), QChar('F'));
}

void TstPostgresProvider::byteaHexDecoding()
{
    QCOMPARE(postgres::decodeByteaHex("\\x00ff10").value_or(QByteArray("bad")),
             QByteArray("\x00\xff\x10", 3));
    QCOMPARE(postgres::decodeByteaHex("\\x").value_or(QByteArray("bad")), QByteArray());
    QVERIFY(!postgres::decodeByteaHex("\\x0").has_value());
    QVERIFY(!postgres::decodeByteaHex("\\xzz").has_value());
    QVERIFY(!postgres::decodeByteaHex("abc").has_value()); // escape format is not accepted
}

void TstPostgresProvider::oidMapping()
{
    using postgres::valueTypeForOid;
    QCOMPARE(valueTypeForOid(21), ValueType::Int64);
    QCOMPARE(valueTypeForOid(23), ValueType::Int64);
    QCOMPARE(valueTypeForOid(20), ValueType::Int64);
    QCOMPARE(valueTypeForOid(700), ValueType::Double);
    QCOMPARE(valueTypeForOid(701), ValueType::Double);
    QCOMPARE(valueTypeForOid(1700), ValueType::Decimal);
    QCOMPARE(valueTypeForOid(16), ValueType::Bool);
    QCOMPARE(valueTypeForOid(25), ValueType::Text);
    QCOMPARE(valueTypeForOid(1043), ValueType::Text);
    QCOMPARE(valueTypeForOid(1042), ValueType::Text);
    QCOMPARE(valueTypeForOid(19), ValueType::Text);
    QCOMPARE(valueTypeForOid(17), ValueType::Bytes);
    QCOMPARE(valueTypeForOid(1082), ValueType::Date);
    QCOMPARE(valueTypeForOid(1083), ValueType::Time);
    QCOMPARE(valueTypeForOid(1114), ValueType::DateTime);
    QCOMPARE(valueTypeForOid(1184), ValueType::DateTimeTz);
    QCOMPARE(valueTypeForOid(2950), ValueType::Uuid);
    QCOMPARE(valueTypeForOid(114), ValueType::Json);
    QCOMPARE(valueTypeForOid(3802), ValueType::Json);
    QCOMPARE(valueTypeForOid(1007), ValueType::Other); // int4[]
    QCOMPARE(valueTypeForOid(1186), ValueType::Other); // interval
}

void TstPostgresProvider::dialectSpecifics()
{
    const ISqlDialect &dialect = m_provider->dialect();
    QCOMPARE(dialect.quoteIdentifier(u"a\"b"), QStringLiteral("\"a\"\"b\""));
    QCOMPARE(dialect.quoteLiteral(u"it's"), QStringLiteral("'it''s'"));
    // A backslash would be an escape with standard_conforming_strings=off: E'' is safe in both.
    QCOMPARE(dialect.quoteLiteral(u"a\\b'c"), QStringLiteral("E'a\\\\b''c'"));
    QCOMPARE(dialect.highlightingDefinition(), QStringLiteral("SQL (PostgreSQL)"));
    QCOMPARE(dialect.limitClause(10, 0), QStringLiteral("LIMIT 10"));
    QCOMPARE(dialect.limitClause(10, 20), QStringLiteral("LIMIT 10 OFFSET 20"));
    QCOMPARE(dialect.limitClause(-1, 20), QStringLiteral("OFFSET 20"));
    QCOMPARE(dialect.explainPrefix(ExplainMode::Plan), QStringLiteral("EXPLAIN "));
    QVERIFY(dialect.explainPrefix(ExplainMode::Analyze).contains(QLatin1String("ANALYZE")));
    QVERIFY(dialect.keywords().contains(QStringLiteral("SELECT")));
    QVERIFY(dialect.keywords().contains(QStringLiteral("RETURNING")));
    QVERIFY(dialect.keywords().size() > 100);

    // Ranges are offsets into the original text, also with non-ASCII before them.
    const QString script = QStringLiteral("SELECT 'é;😀'; DO $$ x; $$;");
    const auto ranges = dialect.splitStatements(script);
    QCOMPARE(ranges.size(), size_t(2));
    QCOMPARE(script.mid(ranges[1].begin, ranges[1].end - ranges[1].begin),
             QStringLiteral("DO $$ x; $$"));
}

void TstPostgresProvider::connectionRefusedIsConnectionError()
{
    ConnectionConfig config;
    config.values.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
    config.values.insert(QStringLiteral("port"), 1); // nothing listens there
    config.values.insert(QStringLiteral("database"), QStringLiteral("x"));
    config.values.insert(QStringLiteral("user"), QStringLiteral("x"));
    auto connection = m_provider->createConnection(config, {});
    QVERIFY(connection);
    QVERIFY(!connection->isOpen());
    auto info = connection->open({});
    QVERIFY(!info);
    const Error error = std::move(info).error();
    QVERIFY2(error.kind == ErrorKind::Connection || error.kind == ErrorKind::Timeout,
             qPrintable(error.detail));
    QVERIFY(!error.message.isEmpty() && !error.detail.isEmpty());
    QVERIFY(!connection->isOpen());
}

// ---- server --------------------------------------------------------------------------------

void TstPostgresProvider::typesRoundTrip()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    QVERIFY(run(*connection, QStringLiteral("SET TIME ZONE 'UTC'")).ok);
    const auto d = run(
        *connection,
        QStringLiteral(
            "SELECT 1::int2 AS a, 2::int4 AS b, 3::int8 AS c, 1.5::float4 AS d, 2.25::float8 AS e, "
            "12345678901234567890.123456789::numeric AS f, true AS g, 'txt'::text AS h, "
            "'vc'::varchar(5) AS i, 'ch'::char(4) AS j, '\\x00ff10'::bytea AS k, "
            "DATE '2026-11-15' AS l, TIME '10:20:30' AS m, TIMESTAMP '2026-11-15 10:20:30.5' AS n, "
            "TIMESTAMPTZ '2026-11-15 10:20:30+00' AS o, "
            "'123e4567-e89b-12d3-a456-426614174000'::uuid AS p, '{\"a\": 1}'::json AS q, "
            "'{\"a\": 1}'::jsonb AS r, ARRAY[1,2,3] AS s, INTERVAL '1 day' AS t, NULL::int AS u, "
            "'NaN'::float8 AS v, 'Infinity'::float4 AS w, 'nm'::name AS x"));
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    QCOMPARE(d.rows, 1);
    const ValueType expected[]
        = { ValueType::Int64,  ValueType::Int64,    ValueType::Int64,      ValueType::Double,
            ValueType::Double, ValueType::Decimal,  ValueType::Bool,       ValueType::Text,
            ValueType::Text,   ValueType::Text,     ValueType::Bytes,      ValueType::Date,
            ValueType::Time,   ValueType::DateTime, ValueType::DateTimeTz, ValueType::Uuid,
            ValueType::Json,   ValueType::Json,     ValueType::Other,      ValueType::Other,
            ValueType::Int64,  ValueType::Double,   ValueType::Double,     ValueType::Text };
    QCOMPARE(d.columns.size(), std::size(expected));
    for (size_t i = 0; i < std::size(expected); ++i)
        QVERIFY2(d.columns[i].type == expected[i],
                 qPrintable(QStringLiteral("column %1 (%2)").arg(i).arg(d.columns[i].nativeType)));
    QCOMPARE(d.columns[5].nativeType, QStringLiteral("numeric"));
    QCOMPARE(d.columns[0].nativeType, QStringLiteral("int2"));

    const ResultBatch &b = d.batches.front();
    auto text = [&](size_t column) { return QString::fromUtf8(b.columns[column].bytesAt(0)); };
    QCOMPARE(b.columns[0].int64At(0), 1);
    QCOMPARE(b.columns[2].int64At(0), 3);
    QCOMPARE(b.columns[3].doubleAt(0), 1.5);
    QCOMPARE(b.columns[4].doubleAt(0), 2.25);
    QCOMPARE(text(5), QStringLiteral("12345678901234567890.123456789")); // exact, no double
    QCOMPARE(b.columns[6].typeAt(0), ValueType::Bool);
    QCOMPARE(b.columns[6].int64At(0), 1);
    QCOMPARE(text(7), QStringLiteral("txt"));
    QCOMPARE(text(9), QStringLiteral("ch  ")); // bpchar keeps its padding
    QCOMPARE(QByteArray(b.columns[10].bytesAt(0)), QByteArray("\x00\xff\x10", 3));
    QCOMPARE(text(11), QStringLiteral("2026-11-15"));
    QCOMPARE(text(12), QStringLiteral("10:20:30"));
    QCOMPARE(text(13), QStringLiteral("2026-11-15 10:20:30.5"));
    QCOMPARE(text(14), QStringLiteral("2026-11-15 10:20:30+00"));
    QCOMPARE(text(15), QStringLiteral("123e4567-e89b-12d3-a456-426614174000"));
    QCOMPARE(text(16), QStringLiteral("{\"a\": 1}"));
    QCOMPARE(text(17), QStringLiteral("{\"a\": 1}"));
    QCOMPARE(text(18), QStringLiteral("{1,2,3}"));
    QCOMPARE(text(19), QStringLiteral("1 day"));
    QVERIFY(b.columns[20].isNull(0));
    QVERIFY(std::isnan(b.columns[21].doubleAt(0)));
    QVERIFY(std::isinf(b.columns[22].doubleAt(0)));
    QCOMPARE(text(23), QStringLiteral("nm"));
    for (const auto &column : b.columns)
        QVERIFY(!column.isMixed());

    // A bytea of every byte value and a big int8 survive the text protocol.
    QByteArray all;
    for (int i = 0; i < 256; ++i)
        all.append(static_cast<char>(i));
    const auto bytes = run(*connection,
                           QStringLiteral("SELECT decode('%1', 'hex'), 9223372036854775807::int8, "
                                          "(-9223372036854775807 - 1)::int8")
                               .arg(QString::fromLatin1(all.toHex())));
    QVERIFY2(bytes.ok, qPrintable(bytes.errorText()));
    QCOMPARE(QByteArray(bytes.batches.front().columns[0].bytesAt(0)), all);
    QCOMPARE(bytes.batches.front().columns[1].int64At(0), std::numeric_limits<qint64>::max());
    QCOMPARE(bytes.batches.front().columns[2].int64At(0), std::numeric_limits<qint64>::min());
}

void TstPostgresProvider::parameterTypes()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    const auto d
        = drain(*connection,
                QStringLiteral("SELECT $1::bool, $2::date, $3::uuid, $4::float8, $5::text, "
                               "$6::bytea, $7::timestamp, $8::int8, $9::text"),
                { true, QDate(2026, 11, 15),
                  QUuid::fromString(QStringLiteral("123e4567-e89b-12d3-a456-426614174000")), 0.1,
                  QStringLiteral("ñ😀"), QByteArray("\x00\x01", 2),
                  QDateTime(QDate(2026, 11, 15), QTime(1, 2, 3, 4)),
                  QVariant::fromValue(std::numeric_limits<qint64>::min()), QVariant() });
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    const ResultBatch &b = d.batches.front();
    QCOMPARE(b.columns[0].int64At(0), 1);
    QCOMPARE(QByteArray(b.columns[1].bytesAt(0)), QByteArray("2026-11-15"));
    QCOMPARE(QByteArray(b.columns[2].bytesAt(0)),
             QByteArray("123e4567-e89b-12d3-a456-426614174000"));
    QCOMPARE(b.columns[3].doubleAt(0), 0.1); // 17 digits round trip
    QCOMPARE(QString::fromUtf8(b.columns[4].bytesAt(0)), QStringLiteral("ñ😀"));
    QCOMPARE(QByteArray(b.columns[5].bytesAt(0)), QByteArray("\x00\x01", 2));
    QVERIFY(QByteArray(b.columns[6].bytesAt(0)).startsWith("2026-11-15 01:02:03.004"));
    QCOMPARE(b.columns[7].int64At(0), std::numeric_limits<qint64>::min());
    QVERIFY(b.columns[8].isNull(0));
}

void TstPostgresProvider::fetchSizeIndependentOfChunk()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    // fetch() larger than the chunk size (100): rows from several chunks, none lost or repeated.
    auto stream = connection->execute(
        Statement { QStringLiteral("SELECT g FROM generate_series(1, 1000) g"), {}, 100 }, {});
    QVERIFY(stream);
    auto big = stream.value()->fetch(250, {});
    QVERIFY(big);
    QCOMPARE(big.value().rowCount, 250);
    QCOMPARE(big.value().columns[0].int64At(249), 250);
    // fetch() smaller than the chunk size: the rest of the chunk waits for the next call.
    auto small = stream.value()->fetch(30, {});
    QVERIFY(small);
    QCOMPARE(small.value().rowCount, 30);
    QCOMPARE(small.value().columns[0].int64At(0), 251);
    auto rest = stream.value()->fetch(5000, {});
    QVERIFY(rest);
    QCOMPARE(rest.value().rowCount, 720);
    QCOMPARE(rest.value().columns[0].int64At(719), 1000);
    QVERIFY(stream.value()->atEnd());
    QCOMPARE(stream.value()->affectedRows().has_value(), false);
}

void TstPostgresProvider::droppedStreamKeepsConnectionUsable()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    QElapsedTimer clock;
    clock.start();
    {
        auto stream = connection->execute(
            Statement { QStringLiteral("SELECT generate_series(1, 50000000)"), {}, 100 }, {});
        QVERIFY(stream);
        auto first = stream.value()->fetch(100, {});
        QVERIFY(first);
        QVERIFY(!stream.value()->atEnd());
        qInfo() << "first chunk after" << clock.elapsed() << "ms";
        clock.restart();
    } // destroyed with ~50M rows pending: must cancel and drain, not read them all
    const qint64 dropMs = clock.elapsed();
    const auto d = run(*connection, QStringLiteral("SELECT 1"));
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    qInfo() << "dropping a stream with 50M pending rows took" << dropMs << "ms";
    QVERIFY2(dropMs < 2000, qPrintable(QString::number(dropMs)));
}

void TstPostgresProvider::cancelPgSleepIsFast()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    QList<qint64> timings;
    QList<qint64> callTimings;
    for (int round = 0; round < 3; ++round) {
        std::atomic<bool> started { false };
        auto worker = std::async(std::launch::async, [&]() -> std::optional<ErrorKind> {
            started = true;
            auto stream = connection->execute(
                Statement { QStringLiteral("SELECT pg_sleep(60)"), {}, 500 }, {});
            if (!stream)
                return stream.error().kind;
            return std::nullopt;
        });
        QTRY_VERIFY_WITH_TIMEOUT(started.load(), 5000);
        QThread::msleep(150); // the statement is running on the server by now

        QElapsedTimer clock;
        clock.start();
        connection->requestCancel();
        const qint64 callUs = clock.nsecsElapsed() / 1000;
        callTimings << callUs;
        QVERIFY2(callUs < 5000,
                 qPrintable(QStringLiteral("requestCancel() took %1 us").arg(callUs)));
        QVERIFY2(worker.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                 "pg_sleep(60) did not stop");
        const qint64 elapsed = clock.elapsed();
        timings << elapsed;
        const auto kind = worker.get();
        QVERIFY(kind.has_value());
        QCOMPARE(kind.value_or(ErrorKind::Internal), ErrorKind::Cancelled);
        QVERIFY2(elapsed < 500, qPrintable(QStringLiteral("cancel took %1 ms").arg(elapsed)));

        const auto next = run(*connection, QStringLiteral("SELECT %1").arg(round + 1));
        QVERIFY2(next.ok, qPrintable(next.errorText()));
        QCOMPARE(next.batches.front().columns[0].int64At(0), round + 1);
    }
    qInfo() << "cancel of SELECT pg_sleep(60), cancel-to-return ms per round:" << timings
            << "requestCancel() call duration us:" << callTimings;
}

void TstPostgresProvider::cancelWhileFetchingChunks()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    // Row 20000 blocks the server for a minute. The server only flushes full 8 KB buffers, so
    // the padding makes sure the rows before it reach the client and the fetch loop gets stuck
    // inside a fetch(), not inside execute().
    auto stream = connection->execute(
        Statement { QStringLiteral("SELECT g, repeat('x', 100), CASE WHEN g = 20000 THEN "
                                   "(SELECT 1 FROM pg_sleep(60)) END "
                                   "FROM generate_series(1, 30000) g"),
                    {},
                    100 },
        {});
    QVERIFY2(stream, qPrintable(stream ? QString() : stream.error().detail));

    std::atomic<qint64> rows { 0 };
    auto worker = std::async(std::launch::async, [&]() -> std::optional<ErrorKind> {
        for (;;) {
            auto batch = stream.value()->fetch(100, {});
            if (!batch)
                return batch.error().kind;
            rows += batch.value().rowCount;
            if (stream.value()->atEnd())
                return std::nullopt;
        }
    });
    QTRY_VERIFY_WITH_TIMEOUT(rows.load() >= 19000, 5000);
    QThread::msleep(300); // stuck on row 20000 by now
    QVERIFY(rows.load() < 20000);

    QElapsedTimer clock;
    clock.start();
    connection->requestCancel();
    QVERIFY2(worker.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
             "fetch did not stop");
    qInfo() << "cancel of a blocked fetch took" << clock.elapsed() << "ms";
    QVERIFY2(clock.elapsed() < 500, qPrintable(QString::number(clock.elapsed())));
    QCOMPARE(worker.get().value_or(ErrorKind::Internal), ErrorKind::Cancelled);
    QVERIFY(stream.value()->atEnd());
    QVERIFY(run(*connection, QStringLiteral("SELECT 1")).ok);
}

void TstPostgresProvider::lateCancelNeverHitsNextStatement()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    for (int round = 0; round < 30; ++round) {
        auto worker = std::async(std::launch::async,
                                 [&] { return run(*connection, QStringLiteral("SELECT 1")); });
        QThread::usleep(static_cast<unsigned long>(round) * 150);
        connection->requestCancel(); // may land before, during or after the statement
        worker.wait();
        // Whatever happened to the first one, the next statement must run untouched.
        const auto next = run(*connection, QStringLiteral("SELECT %1").arg(round));
        QVERIFY2(next.ok, qPrintable(QStringLiteral("round %1: ").arg(round) + next.errorText()));
    }
}

void TstPostgresProvider::sessionCancelsPgSleep()
{
    REQUIRE_SERVER();
    QString database;
    auto probe = openFresh(&database);
    QVERIFY(probe);
    Session session(m_provider->createConnection(m_scratch->config(), {}));
    QSignalSpy started(&session, &Session::started);
    QSignalSpy cancelled(&session, &Session::cancelled);
    QSignalSpy failed(&session, &Session::failed);
    QSignalSpy finished(&session, &Session::finished);

    session.open();
    session.execute(Statement { QStringLiteral("SELECT pg_sleep(60)"), {}, 500 });
    QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 5000);
    QTest::qWait(200);

    QElapsedTimer clock;
    clock.start();
    session.cancel();
    const qint64 callUs = clock.nsecsElapsed() / 1000;
    QTRY_COMPARE_WITH_TIMEOUT(cancelled.count(), 1, 3000);
    qInfo() << "Session::cancel() call took" << callUs << "us; cancelled after" << clock.elapsed()
            << "ms";
    QVERIFY2(callUs < 5000, qPrintable(QString::number(callUs)));
    QVERIFY2(clock.elapsed() < 500, qPrintable(QString::number(clock.elapsed())));
    QCOMPARE(failed.count(), 0);

    session.execute(Statement { QStringLiteral("SELECT 41 + 1"), {}, 500 });
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
    const auto stats = finished.at(0).at(0).value<ExecutionStats>();
    QCOMPARE(stats.rowsFetched, 1);
    QVERIFY(stats.atEnd);
}

void TstPostgresProvider::syntaxErrorPositionIsInCharacters()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);

    // ASCII first: the position is the plain offset.
    const QString plain = QStringLiteral("SELECT 1 FROM FROM t");
    auto a = connection->execute(Statement { plain, {}, 500 }, {});
    QVERIFY(!a);
    const Error plainError = std::move(a).error();
    QCOMPARE(plainError.kind, ErrorKind::Syntax);
    QCOMPARE(plainError.sqlState, QStringLiteral("42601"));
    QCOMPARE(plainError.position.value_or(-1), plain.indexOf(QLatin1String("FROM"), 14));
    QVERIFY(plainError.detail.contains(QLatin1String("syntax error")));

    // 'é' = 2 UTF-8 bytes, the emoji = 4 bytes and 2 UTF-16 units, both 1 character for the
    // server. The offset must be in UTF-16 units: neither the byte nor the character count.
    const QString sql = QStringLiteral("SELECT 'é日本😀', 1 FROM FROM t");
    auto stream = connection->execute(Statement { sql, {}, 500 }, {});
    QVERIFY(!stream);
    const Error error = std::move(stream).error();
    QCOMPARE(error.kind, ErrorKind::Syntax);
    const int expected = sql.lastIndexOf(QLatin1String("FROM t"));
    qInfo() << "syntax error position:" << error.position.value_or(-1) << "expected" << expected
            << "(UTF-16 units); characters would be" << (expected - 1) << ", bytes"
            << sql.left(expected).toUtf8().size();
    QCOMPARE(error.position.value_or(-1), expected);
    QCOMPARE(sql.mid(error.position.value_or(0), 4), QStringLiteral("FROM"));

    // A runtime error has no statement position.
    auto zero = connection->execute(Statement { QStringLiteral("SELECT 1/0"), {}, 500 }, {});
    QVERIFY(!zero);
    const Error zeroError = std::move(zero).error();
    QCOMPARE(zeroError.kind, ErrorKind::Driver);
    QVERIFY(!zeroError.position.has_value());
    QVERIFY(run(*connection, QStringLiteral("SELECT 1")).ok);
}

void TstPostgresProvider::authenticationFailure()
{
    REQUIRE_SERVER();
    ConnectionConfig config = m_server.config(m_server.database);
    auto connection
        = m_provider->createConnection(config, Secrets { QStringLiteral("not the password") });
    auto info = connection->open({});
    if (info)
        QSKIP("the server accepts any password (trust authentication)");
    const Error error = std::move(info).error();
    QCOMPARE(error.kind, ErrorKind::Authentication);
    QVERIFY(!error.detail.isEmpty());
    QVERIFY(!connection->isOpen());

    // The right password through Secrets works even when the environment has a wrong one.
    qputenv("PGPASSWORD", "wrong");
    auto good = m_provider->createConnection(config, Secrets { m_server.password });
    auto goodInfo = good->open({});
    qputenv("PGPASSWORD", m_server.password.toUtf8());
    QVERIFY2(goodInfo, qPrintable(goodInfo ? QString() : goodInfo.error().detail));
}

void TstPostgresProvider::unknownDatabase()
{
    REQUIRE_SERVER();
    auto connection
        = m_provider->createConnection(m_server.config(QStringLiteral("no_such_db_quarry")), {});
    auto info = connection->open({});
    QVERIFY(!info);
    QCOMPARE(std::move(info).error().kind, ErrorKind::NotFound);
}

void TstPostgresProvider::sslModes()
{
    REQUIRE_SERVER();
    // disable and prefer always work; require needs a server with SSL, verify-* also a root cert.
    for (const QString &mode :
         { QStringLiteral("disable"), QStringLiteral("prefer"), QStringLiteral("allow") }) {
        ConnectionConfig config = m_server.config(m_server.database);
        config.values.insert(QStringLiteral("sslmode"), mode);
        auto connection = m_provider->createConnection(config, {});
        auto info = connection->open({});
        QVERIFY2(info, qPrintable(mode + (info ? QString() : info.error().detail)));
    }
    ConnectionConfig strict = m_server.config(m_server.database);
    strict.values.insert(QStringLiteral("sslmode"), QStringLiteral("verify-full"));
    strict.values.insert(QStringLiteral("sslrootcert"),
                         QDir::temp().filePath(QStringLiteral("quarry-missing-root.crt")));
    auto connection = m_provider->createConnection(strict, {});
    auto info = connection->open({});
    QVERIFY(!info); // missing root certificate, or no SSL on the server: never a silent downgrade
    QCOMPARE(std::move(info).error().kind, ErrorKind::Connection);

    strict.values.insert(QStringLiteral("sslmode"), QStringLiteral("require"));
    strict.values.remove(QStringLiteral("sslrootcert"));
    auto required = m_provider->createConnection(strict, {});
    auto requiredInfo = required->open({});
    if (requiredInfo) // the server has SSL: fine, the mode was honored
        QSKIP("server supports SSL: require connected");
    QCOMPARE(std::move(requiredInfo).error().kind, ErrorKind::Connection);
}

void TstPostgresProvider::treeHidesSystemSchemas()
{
    REQUIRE_SERVER();
    QString database;
    auto connection = openFresh(&database);
    QVERIFY(connection);
    QVERIFY(run(*connection, QStringLiteral("CREATE TEMP TABLE tmp_t(a int)")).ok); // pg_temp_N
    QVERIFY(run(*connection, QStringLiteral("CREATE SCHEMA app")).ok);
    QVERIFY(run(*connection, QStringLiteral("CREATE TABLE app.t(id int PRIMARY KEY, v text)")).ok);
    QVERIFY(run(*connection, QStringLiteral("CREATE VIEW app.v AS SELECT id FROM app.t")).ok);
    QVERIFY(
        run(*connection, QStringLiteral("CREATE MATERIALIZED VIEW app.mv AS SELECT id FROM app.t"))
            .ok);

    auto root = connection->children(ObjectPath { {}, ObjectKind::Database }, {});
    QVERIFY(root);
    QCOMPARE(root.value().size(), size_t(1)); // only the connected database
    QCOMPARE(root.value().front().path.kind, ObjectKind::Database);
    QCOMPARE(root.value().front().name, database);
    QVERIFY(root.value().front().hasChildren);

    auto schemas = connection->children(root.value().front().path, {});
    QVERIFY(schemas);
    QStringList names;
    for (const SchemaNode &schema : schemas.value()) {
        QCOMPARE(schema.path.kind, ObjectKind::Schema);
        QCOMPARE(schema.path.parts, (QStringList { database, schema.name }));
        names << schema.name;
    }
    QCOMPARE(names, (QStringList { QStringLiteral("app"), QStringLiteral("public") }));

    // The temp schema really exists in the catalog: it is hidden by the provider, not absent.
    const auto catalog = run(*connection,
                             QStringLiteral("SELECT count(*) FROM pg_namespace WHERE nspname "
                                            "LIKE 'pg\\_temp\\_%' OR nspname LIKE 'pg\\_toast%'"));
    QVERIFY(catalog.ok);
    QVERIFY(catalog.batches.front().columns[0].int64At(0) >= 2);

    auto items = connection->children(schemas.value().front().path, {});
    QVERIFY(items);
    QCOMPARE(items.value().size(), size_t(3));
    QCOMPARE(items.value()[0].name, QStringLiteral("t"));
    QCOMPARE(items.value()[0].path.kind, ObjectKind::Table);
    QCOMPARE(items.value()[1].name, QStringLiteral("v"));
    QCOMPARE(items.value()[1].path.kind, ObjectKind::View);
    QCOMPARE(items.value()[2].name, QStringLiteral("mv"));
    QCOMPARE(items.value()[2].path.kind, ObjectKind::MaterializedView);
    QCOMPARE(items.value()[0].path.parts,
             (QStringList { database, QStringLiteral("app"), QStringLiteral("t") }));

    auto folders = connection->children(items.value()[0].path, {});
    QVERIFY(folders);
    QCOMPARE(folders.value().size(), size_t(3)); // Columns, Indexes, Foreign keys
    auto columns = connection->children(folders.value()[0].path, {});
    QVERIFY(columns);
    QCOMPARE(columns.value().size(), size_t(2));
    QCOMPARE(columns.value()[0].name, QStringLiteral("id"));
    QCOMPARE(columns.value()[0].detail, QStringLiteral("integer NOT NULL PK"));
    QCOMPARE(columns.value()[1].detail, QStringLiteral("text"));
    QCOMPARE(columns.value()[0].path.parts.size(), 4);

    auto mvFolders = connection->children(items.value()[2].path, {});
    QVERIFY(mvFolders);
    QVERIFY(!mvFolders.value().empty());

    // Tables of the temp schema are not reachable either.
    auto publicItems = connection->children(schemas.value().back().path, {});
    QVERIFY(publicItems);
    QVERIFY(publicItems.value().empty());

    auto details = connection->describe(items.value()[0].path, {});
    QVERIFY(details);
    QCOMPARE(details.value().primaryKey.value_or(QStringList()),
             QStringList { QStringLiteral("id") });
}

void TstPostgresProvider::noticesAreCollected()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    auto stream = connection->execute(
        Statement { QStringLiteral("DO $$ BEGIN RAISE NOTICE 'hello %', 42; END $$"), {}, 500 },
        {});
    QVERIFY2(stream, qPrintable(stream ? QString() : stream.error().detail));
    QVERIFY(stream.value()->atEnd());
    const QStringList notices = stream.value()->notices();
    QCOMPARE(notices.size(), 1);
    QCOMPARE(notices.front(), QStringLiteral("NOTICE: hello 42"));
    QVERIFY(!stream.value()->affectedRows().has_value());

    // Notices that arrive while rows stream are collected too, and do not leak into the next one.
    auto rows = connection->execute(
        Statement {
            QStringLiteral("SELECT g, (SELECT f_notice()) FROM generate_series(1, 2) g"), {}, 1 },
        {});
    QVERIFY(!rows); // f_notice does not exist yet
    QVERIFY(run(*connection,
                QStringLiteral("CREATE FUNCTION f_notice() RETURNS int LANGUAGE plpgsql AS $$ "
                               "BEGIN RAISE WARNING 'careful'; RETURN 1; END $$"))
                .ok);
    auto second
        = connection->execute(Statement { QStringLiteral("SELECT f_notice()"), {}, 500 }, {});
    QVERIFY(second);
    auto batch = second.value()->fetch(500, {});
    QVERIFY(batch);
    QCOMPARE(second.value()->notices(), QStringList { QStringLiteral("WARNING: careful") });
}

void TstPostgresProvider::dollarQuotedScriptRuns()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    const QString script = QStringLiteral(
        "CREATE TABLE t(v text);\n"
        "CREATE FUNCTION add_row(x text) RETURNS void LANGUAGE plpgsql AS $body$\n"
        "BEGIN\n  INSERT INTO t VALUES (x); -- a ; inside\n  INSERT INTO t VALUES ($$;$$);\nEND\n"
        "$body$;\n"
        "SELECT add_row('a;b');\n"
        "SELECT v FROM t ORDER BY v;");
    const auto ranges = m_provider->dialect().splitStatements(script);
    QCOMPARE(ranges.size(), size_t(4));
    Drained last;
    for (const StatementRange &range : ranges) {
        last = run(*connection, script.mid(range.begin, range.end - range.begin));
        QVERIFY2(last.ok, qPrintable(last.errorText()));
    }
    QCOMPARE(last.rows, 2);
    QCOMPARE(QString::fromUtf8(last.batches.front().columns[0].bytesAt(0)), QStringLiteral(";"));
    QCOMPARE(QString::fromUtf8(last.batches.front().columns[0].bytesAt(1)), QStringLiteral("a;b"));
}

void TstPostgresProvider::transactions()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    QVERIFY(run(*connection, QStringLiteral("CREATE TABLE t(a int)")).ok);
    QVERIFY(connection->begin());
    QVERIFY(run(*connection, QStringLiteral("INSERT INTO t VALUES (1)")).ok);
    QVERIFY(connection->rollback());
    QCOMPARE(run(*connection, QStringLiteral("SELECT * FROM t")).rows, 0);
    QVERIFY(connection->begin());
    const auto inserted = run(*connection, QStringLiteral("INSERT INTO t VALUES (1), (2)"));
    QVERIFY(inserted.ok);
    QCOMPARE(inserted.affected.value_or(-1), 2);
    QVERIFY(connection->commit());
    QCOMPARE(run(*connection, QStringLiteral("SELECT * FROM t")).rows, 2);

    // INSERT ... RETURNING streams rows and still reports the count.
    auto returning = connection->execute(
        Statement { QStringLiteral("INSERT INTO t VALUES (3), (4), (5) RETURNING a"), {}, 2 }, {});
    QVERIFY(returning);
    qint64 rows = 0;
    while (!returning.value()->atEnd()) {
        auto batch = returning.value()->fetch(2, {});
        QVERIFY(batch);
        rows += batch.value().rowCount;
    }
    QCOMPARE(rows, 3);
    QCOMPARE(returning.value()->affectedRows().value_or(-1), 3);
}

void TstPostgresProvider::multipleStatementsAreRejected()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    const auto d
        = run(*connection, QStringLiteral("CREATE TABLE t(a int); INSERT INTO t VALUES (1)"));
    QVERIFY(!d.ok);
    QCOMPARE(d.error.kind, ErrorKind::Unsupported);
    QVERIFY(!run(*connection, QStringLiteral("SELECT * FROM t")).ok); // nothing ran
    QVERIFY(run(*connection, QStringLiteral("SELECT 1")).ok);
}

void TstPostgresProvider::emptyAndCommentOnlyStatements()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    for (const QString &sql :
         { QString(), QStringLiteral("  "), QStringLiteral("-- nothing"), QStringLiteral(";") }) {
        const auto d = run(*connection, sql);
        QVERIFY2(d.ok, qPrintable(sql + d.errorText()));
        QCOMPARE(d.rows, 0);
        QVERIFY(d.columns.empty());
    }
    QVERIFY(run(*connection, QStringLiteral("SELECT 1")).ok);
}

QTEST_GUILESS_MAIN(TstPostgresProvider)
#include "tst_postgres_provider.moc"
