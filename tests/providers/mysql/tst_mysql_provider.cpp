#include "mysql_support.h"
#include "provider_conformance.h"

#include <quarry/provider_registry.h>
#include <quarry/session.h>

#include <QDir>
#include <QSignalSpy>
#include <QTimeZone>
#include <QUrl>
#include <QUuid>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <future>
#include <limits>
#include <mysql.h>

using namespace quarry;
using namespace quarry::conformance;

namespace {
// MYSQL_CLIENT_AUTHENTICATION_PLUGIN: its header drags in libmariadb's private ones.
constexpr int kAuthenticationPlugin = 2;
} // namespace

namespace {

const QString kUrlVariable = QStringLiteral("QUARRY_TEST_MYSQL_URL");

// Connection settings from QUARRY_TEST_MYSQL_URL (mysql://user:password@host:port/db).
struct ServerUrl
{
    QString host;
    int port = 3306;
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
        out.port = url.port(3306);
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

// Ten rows 0..9. Joined with itself it makes a source of 10^k rows without recursive CTEs (which
// MySQL limits to 1000 iterations) or the MariaDB-only sequence tables.
QString digitsSql()
{
    return QStringLiteral(
        "(SELECT 0 AS d UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL "
        "SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 "
        "UNION ALL SELECT 9)");
}

// FROM clause with `count` copies of digitsSql(): t0 .. t<count-1>.
QString digitsJoin(int count)
{
    QStringList parts;
    for (int i = 0; i < count; ++i)
        parts << QStringLiteral("%1 t%2").arg(digitsSql()).arg(i);
    return QStringLiteral(" FROM ") + parts.join(QLatin1String(", "));
}

class MysqlFixture : public Fixture
{
public:
    MysqlFixture(IDatabaseProvider &provider, const ServerUrl &server, QString database)
        : m_provider(provider)
        , m_server(server)
        , m_database(std::move(database))
    { }

    // Fresh database per test: no collisions between tests or with other users of the server.
    QString create()
    {
        return runOnce(
            m_provider, m_server.config(m_server.database),
            QStringLiteral("CREATE DATABASE `%1` CHARACTER SET utf8mb4").arg(m_database));
    }
    ~MysqlFixture() override
    {
        if (!m_created)
            return;
        runOnce(m_provider, m_server.config(m_server.database),
                QStringLiteral("DROP DATABASE IF EXISTS `%1`").arg(m_database));
    }
    void markCreated() { m_created = true; }

    IDatabaseProvider &provider() override { return m_provider; }
    ConnectionConfig config() override { return m_server.config(m_database); }
    QString sampleSchemaSql() const override
    {
        return QStringLiteral(
            "CREATE TABLE authors(id BIGINT PRIMARY KEY, name VARCHAR(255) NOT NULL UNIQUE);\n"
            "CREATE TABLE books(id BIGINT PRIMARY KEY, title VARCHAR(255) NOT NULL,\n"
            "  author_id BIGINT NOT NULL, price DOUBLE NULL, cover BLOB NULL,\n"
            "  CONSTRAINT fk_books_author FOREIGN KEY (author_id) REFERENCES authors(id));\n"
            "CREATE INDEX idx_books_title ON books(title); -- a comment; with a semicolon\n"
            "CREATE VIEW v_titles AS SELECT id, title FROM books;\n");
    }
    QString slowQuerySql() const override { return QStringLiteral("SELECT SLEEP(3600)"); }
    QString seriesSql(int count) const override
    {
        // Up to 10 000 rows: four digit tables.
        return QStringLiteral("SELECT t0.d + 10 * t1.d + 100 * t2.d + 1000 * t3.d + 1 AS n")
            + digitsJoin(4) + QStringLiteral(" ORDER BY n LIMIT %1").arg(count);
    }
    QString rowIdentifierName() const override { return {}; }
    bool hasRowIdentifier() const override { return false; }
    bool reportsErrorPosition() const override { return false; }
    ObjectPath booksPath() const override
    {
        return { { m_database, QStringLiteral("books") }, ObjectKind::Table };
    }
    ObjectPath viewPath() const override
    {
        return { { m_database, QStringLiteral("v_titles") }, ObjectKind::View };
    }
    ValueType fractionalLiteralType() const override { return ValueType::Decimal; }
    std::vector<SplitCase> extraSplitCases() const override
    {
        return {
            { QStringLiteral("DELIMITER //\nCREATE PROCEDURE p() BEGIN SELECT 1; SELECT 2; END//\n"
                             "DELIMITER ;\nSELECT 3"),
              { QStringLiteral("CREATE PROCEDURE p() BEGIN SELECT 1; SELECT 2; END"),
                QStringLiteral("SELECT 3") } },
            { QStringLiteral("delimiter $$\nSELECT 1$$ SELECT 2$$\ndelimiter ;\nSELECT 3;"),
              { QStringLiteral("SELECT 1"), QStringLiteral("SELECT 2"),
                QStringLiteral("SELECT 3") } },
            { QStringLiteral("SELECT 1; DELIMITER //\nSELECT 2; SELECT 3//"),
              { QStringLiteral("SELECT 1"), QStringLiteral("DELIMITER //\nSELECT 2"),
                QStringLiteral("SELECT 3//") } },
            { QStringLiteral("# a comment; x\nSELECT 1; SELECT 2"),
              { QStringLiteral("# a comment; x\nSELECT 1"), QStringLiteral("SELECT 2") } },
            { QStringLiteral("SELECT 5--3; SELECT 2"),
              { QStringLiteral("SELECT 5--3"), QStringLiteral("SELECT 2") } },
            { QStringLiteral("SELECT 5-- 3; SELECT 2"),
              { QStringLiteral("SELECT 5-- 3; SELECT 2") } },
            { QStringLiteral("SELECT 'a\\'b;c'; SELECT 2"),
              { QStringLiteral("SELECT 'a\\'b;c'"), QStringLiteral("SELECT 2") } },
            { QStringLiteral("SELECT \"a\\\"b;c\", 'x''y;z'; SELECT 2"),
              { QStringLiteral("SELECT \"a\\\"b;c\", 'x''y;z'"), QStringLiteral("SELECT 2") } },
            { QStringLiteral("SELECT `a;b`, `c``;d`; SELECT 2"),
              { QStringLiteral("SELECT `a;b`, `c``;d`"), QStringLiteral("SELECT 2") } },
            { QStringLiteral("/*! SET x = 1; */; SELECT 2"),
              { QStringLiteral("/*! SET x = 1; */"), QStringLiteral("SELECT 2") } },
            { QStringLiteral("/*M!100100 SET x = 1; */; /* only a comment */; SELECT 2"),
              { QStringLiteral("/*M!100100 SET x = 1; */"), QStringLiteral("SELECT 2") } },
            { QStringLiteral("/* a /* ; */ SELECT 1; SELECT 2"),
              { QStringLiteral("/* a /* ; */ SELECT 1"), QStringLiteral("SELECT 2") } },
            { QStringLiteral("SELECT 'unterminated; SELECT 2"),
              { QStringLiteral("SELECT 'unterminated; SELECT 2") } },
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
            QSKIP("QUARRY_TEST_MYSQL_URL is not set");                                             \
    } while (false)

// Plain-text value of the first cell of a batch.
QString textAt(const ResultBatch &batch, size_t column, qsizetype row = 0)
{
    return QString::fromUtf8(batch.columns[column].bytesAt(row));
}

} // namespace

class TstMysqlProvider : public ProviderConformanceTest
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
                QTest::qSkip("QUARRY_TEST_MYSQL_URL is not set", __FILE__, __LINE__);
                return nullptr;
            }
            return std::make_unique<MysqlFixture>(*m_provider, m_server, QString());
        }
        auto fixture = std::make_unique<MysqlFixture>(*m_provider, m_server,
                                                      QStringLiteral("quarry_conf_%1_%2")
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
    void connSettingsFromForm();
    void connSettingsRejectBadInput();
    void tlsRetryDecision();
    void errnoMapping();
    void columnTypeMapping();
    void dataTypeMapping();
    void serverVersionParsing();
    void bitDecoding();
    void bindParametersLiterals();
    void bindParametersSkipsStringsAndComments();
    void bindParametersRejectsMismatch();
    void pluginDirectoryCalculation();
    void authPluginsLoadFromPluginDirectory();
    void dialectSpecifics();
    void explainStatements();
    void connectionRefusedIsConnectionError();

    // Server needed
    void explainRunsAgainstAServer();
    void typesRoundTrip();
    void parameterTypes();
    void decimalIsExact();
    void emojiRoundTrip();
    void blobIsBinary();
    void bitColumn();
    void noBackslashEscapesModeIsHonored();
    void fetchSizeIndependentOfBatch();
    void droppedStreamKeepsConnectionUsable();
    void cancelSleepIsFast();
    void cancelWhileFetchingRows();
    void lateCancelNeverHitsNextStatement();
    void sessionCancelsSleep();
    void errorKindsFromServer();
    void authenticationFailure();
    void cachingSha2PasswordLogin();
    void unknownDatabase();
    void sslModes();
    void treeHidesSystemDatabases();
    void noticesAreCollected();
    void delimiterScriptRuns();
    void callWithTwoResultSetsLeavesConnectionUsable();
    void transactions();
    void affectedRowsCountMatchedRows();
    void multipleStatementsAreRejected();
    void emptyAndCommentOnlyStatements();

private:
    std::unique_ptr<IConnection> openFresh(QString *databaseName = nullptr);
    Drained run(IConnection &connection, const QString &sql) { return drain(connection, sql); }
    bool isMariaDb() const { return m_product == QLatin1String("MariaDB"); }

    ProviderRegistry m_registry;
    IDatabaseProvider *m_provider = nullptr;
    ServerUrl m_server;
    int m_counter = 0;
    QString m_product; // "MariaDB" or "MySQL", from the first connection
    std::unique_ptr<MysqlFixture> m_scratch;
};

void TstMysqlProvider::initTestCase()
{
    m_registry.scan(QStringLiteral(QUARRY_PLUGIN_DIR));
    QString error;
    m_provider = m_registry.provider(QStringLiteral("mysql"), &error);
    QVERIFY2(m_provider, qPrintable(error));
    m_server = ServerUrl::fromEnvironment();
    // The conformance suite creates connections without secrets: libmariadb reads the password
    // of this process from the environment, as the mysql client does.
    if (m_server.valid)
        qputenv("MYSQL_PWD", m_server.password.toUtf8());
}

void TstMysqlProvider::cleanupTestCase()
{
    m_scratch.reset(); // drops the scratch database of the last test
    qunsetenv("MYSQL_PWD");
}

std::unique_ptr<IConnection> TstMysqlProvider::openFresh(QString *databaseName)
{
    m_scratch = std::make_unique<MysqlFixture>(
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
    m_product = info.value().product;
    return connection;
}

// ---- no server -----------------------------------------------------------------------------

void TstMysqlProvider::pluginMetadata()
{
    const auto &entries = m_registry.available();
    const auto entry = std::find_if(entries.begin(), entries.end(), [](const ProviderEntry &e) {
        return e.id == QLatin1String("mysql");
    });
    QVERIFY(entry != entries.end());
    QCOMPARE(entry->displayName, QStringLiteral("MySQL / MariaDB"));
    QCOMPARE(entry->apiVersion, kProviderApiVersion);
    // The authentication plugins live next to the executable: scan() must not have rejected
    // anything for them.
    QVERIFY(m_registry.rejected().empty());
    const ProviderInfo info = m_provider->info();
    QCOMPARE(info.id, QStringLiteral("mysql"));
    QVERIFY(info.caps.testFlag(Capability::Cancel));
    QVERIFY(info.caps.testFlag(Capability::MultipleDatabases));
    QVERIFY(info.caps.testFlag(Capability::Transactions));
    QVERIFY(!info.caps.testFlag(Capability::Schemas));

    const auto fields = m_provider->connectionFields();
    QCOMPARE(fields.size(), size_t(7));
    QCOMPARE(fields[0].key, QStringLiteral("host"));
    QCOMPARE(fields[0].kind, ConnectionField::Kind::Text);
    QCOMPARE(fields[0].defaultValue.toString(), QStringLiteral("localhost"));
    QCOMPARE(fields[1].key, QStringLiteral("port"));
    QCOMPARE(fields[1].kind, ConnectionField::Kind::Number);
    QCOMPARE(fields[1].defaultValue.toInt(), 3306);
    QCOMPARE(fields[2].key, QStringLiteral("database"));
    QVERIFY(!fields[2].required); // the default database is optional
    QCOMPARE(fields[3].key, QStringLiteral("user"));
    QVERIFY(fields[3].required);
    QCOMPARE(fields[4].key, QStringLiteral("sslmode"));
    QCOMPARE(fields[4].kind, ConnectionField::Kind::Enum);
    QCOMPARE(fields[4].enumValues,
             (QStringList { QStringLiteral("disable"), QStringLiteral("prefer"),
                            QStringLiteral("require"), QStringLiteral("verify-ca"),
                            QStringLiteral("verify-full") }));
    QCOMPARE(fields[4].defaultValue.toString(), QStringLiteral("prefer"));
    QCOMPARE(fields[5].key, QStringLiteral("sslrootcert"));
    QCOMPARE(fields[5].kind, ConnectionField::Kind::Path);
    QVERIFY(fields[5].advanced && !fields[5].required);
    // The password is the only Secret field: the dialog shows it in its own section.
    QCOMPARE(fields[6].key, QStringLiteral("password"));
    QCOMPARE(fields[6].kind, ConnectionField::Kind::Secret);
    for (size_t i = 0; i < 6; ++i)
        QVERIFY(fields[i].kind != ConnectionField::Kind::Secret);
}

void TstMysqlProvider::connSettingsFromForm()
{
    QVariantMap values;
    values.insert(QStringLiteral("host"), QStringLiteral("db host"));
    values.insert(QStringLiteral("port"), QStringLiteral("6543"));
    values.insert(QStringLiteral("database"), QStringLiteral("my db`x"));
    values.insert(QStringLiteral("user"), QStringLiteral("o'brien"));
    values.insert(QStringLiteral("sslmode"), QStringLiteral("verify-full"));
    values.insert(QStringLiteral("sslrootcert"), QStringLiteral("C:/certs/root ca.pem"));
    auto result = mysql::buildConnSettings(values, QStringLiteral("p w'\\ d"));
    QVERIFY(result);
    const mysql::ConnSettings &settings = result.value();
    QCOMPARE(settings.host, QByteArray("db host"));
    QCOMPARE(settings.port, 6543U);
    QCOMPARE(settings.database, QByteArray("my db`x"));
    QCOMPARE(settings.user, QByteArray("o'brien"));
    QCOMPARE(settings.password, QByteArray("p w'\\ d"));
    QCOMPARE(settings.sslMode, mysql::SslMode::VerifyFull);
    QCOMPARE(settings.rootCert, QByteArray("C:/certs/root ca.pem"));

    // Defaults, no password, numbers as they come back from quarry.db (double / qint64).
    QVariantMap minimal;
    minimal.insert(QStringLiteral("port"), 3306.0);
    auto defaults = mysql::buildConnSettings(minimal, {});
    QVERIFY(defaults);
    QCOMPARE(defaults.value().host, QByteArray("localhost"));
    QCOMPARE(defaults.value().port, 3306U);
    QVERIFY(defaults.value().password.isEmpty());
    QVERIFY(defaults.value().database.isEmpty());
    QCOMPARE(defaults.value().sslMode, mysql::SslMode::Prefer);
    QVERIFY(defaults.value().rootCert.isEmpty());

    const QPair<QString, mysql::SslMode> modes[]
        = { { QStringLiteral("disable"), mysql::SslMode::Disable },
            { QStringLiteral("prefer"), mysql::SslMode::Prefer },
            { QStringLiteral("require"), mysql::SslMode::Require },
            { QStringLiteral("verify-ca"), mysql::SslMode::VerifyCa },
            { QStringLiteral("verify-full"), mysql::SslMode::VerifyFull } };
    for (const auto &[name, mode] : modes) {
        QVariantMap one;
        one.insert(QStringLiteral("sslmode"), name);
        auto parsed = mysql::buildConnSettings(one, {});
        QVERIFY(parsed);
        QCOMPARE(parsed.value().sslMode, mode);
    }
}

void TstMysqlProvider::connSettingsRejectBadInput()
{
    QVariantMap values;
    values.insert(QStringLiteral("sslmode"), QStringLiteral("allow")); // a libpq mode, not ours
    auto badMode = mysql::buildConnSettings(values, {});
    QVERIFY(!badMode);
    QCOMPARE(std::move(badMode).error().kind, ErrorKind::Connection);

    for (const QString &port :
         { QStringLiteral("abc"), QStringLiteral("0"), QStringLiteral("70000") }) {
        QVariantMap bad;
        bad.insert(QStringLiteral("port"), port);
        QVERIFY2(!mysql::buildConnSettings(bad, {}), qPrintable(port));
    }
}

void TstMysqlProvider::tlsRetryDecision()
{
    // sslmode=prefer goes back to plain text only when TLS itself is what failed.
    QVERIFY(mysql::shouldRetryWithoutTls(2026)); // CR_SSL_CONNECTION_ERROR
    QVERIFY(mysql::shouldRetryWithoutTls(2013)); // server dropped the handshake
    QVERIFY(!mysql::shouldRetryWithoutTls(1045)); // wrong password: a second try cannot help
    QVERIFY(!mysql::shouldRetryWithoutTls(1049));
    QVERIFY(!mysql::shouldRetryWithoutTls(2003)); // nobody listens
    QVERIFY(!mysql::shouldRetryWithoutTls(0));
}

void TstMysqlProvider::errnoMapping()
{
    using mysql::kindForErrno;
    for (const unsigned code : { 1045U, 1698U })
        QCOMPARE(kindForErrno(code), ErrorKind::Authentication);
    for (const unsigned code : { 1044U, 1142U, 1143U, 1227U })
        QCOMPARE(kindForErrno(code), ErrorKind::Permission);
    for (const unsigned code : { 1062U, 1048U, 1216U, 1217U, 1451U, 1452U, 3819U, 4025U })
        QCOMPARE(kindForErrno(code), ErrorKind::Constraint);
    for (const unsigned code : { 1064U, 1149U })
        QCOMPARE(kindForErrno(code), ErrorKind::Syntax);
    for (const unsigned code : { 1049U, 1054U, 1146U, 1305U })
        QCOMPARE(kindForErrno(code), ErrorKind::NotFound);
    QCOMPARE(kindForErrno(1317), ErrorKind::Cancelled);
    for (const unsigned code : { 1205U, 1969U })
        QCOMPARE(kindForErrno(code), ErrorKind::Timeout);
    for (const unsigned code : { 2002U, 2003U, 2005U, 2006U, 2013U })
        QCOMPARE(kindForErrno(code), ErrorKind::Connection);
    QCOMPARE(kindForErrno(1365), ErrorKind::Driver); // division by zero
    QCOMPARE(kindForErrno(0), ErrorKind::Driver);
    for (const unsigned code : { 1045U, 1044U, 1062U, 1064U, 1146U, 1317U, 1205U, 2003U, 1365U })
        QVERIFY(!mysql::messageForErrno(code).isEmpty());
    QVERIFY(mysql::messageForErrno(1062) != mysql::messageForErrno(1048)); // plain-language detail
}

void TstMysqlProvider::columnTypeMapping()
{
    using mysql::valueTypeForColumn;
    const unsigned kBinary = 63;
    const unsigned kUtf8mb4 = 45;
    for (const unsigned type :
         { unsigned(MYSQL_TYPE_TINY), unsigned(MYSQL_TYPE_SHORT), unsigned(MYSQL_TYPE_INT24),
           unsigned(MYSQL_TYPE_LONG), unsigned(MYSQL_TYPE_LONGLONG), unsigned(MYSQL_TYPE_YEAR) }) {
        // Integers carry the binary charset 63 and must not turn into Bytes.
        QCOMPARE(valueTypeForColumn(type, kBinary), ValueType::Int64);
    }
    QCOMPARE(valueTypeForColumn(MYSQL_TYPE_FLOAT, kBinary), ValueType::Double);
    QCOMPARE(valueTypeForColumn(MYSQL_TYPE_DOUBLE, kBinary), ValueType::Double);
    QCOMPARE(valueTypeForColumn(MYSQL_TYPE_DECIMAL, kBinary), ValueType::Decimal);
    QCOMPARE(valueTypeForColumn(MYSQL_TYPE_NEWDECIMAL, kBinary), ValueType::Decimal);
    QCOMPARE(valueTypeForColumn(MYSQL_TYPE_DATE, kBinary), ValueType::Date);
    QCOMPARE(valueTypeForColumn(MYSQL_TYPE_TIME, kBinary), ValueType::Time);
    QCOMPARE(valueTypeForColumn(MYSQL_TYPE_DATETIME, kBinary), ValueType::DateTime);
    QCOMPARE(valueTypeForColumn(MYSQL_TYPE_TIMESTAMP, kBinary), ValueType::DateTime);
    QCOMPARE(valueTypeForColumn(MYSQL_TYPE_JSON, kUtf8mb4), ValueType::Json);
    QCOMPARE(valueTypeForColumn(MYSQL_TYPE_BIT, kBinary), ValueType::Int64);
    // The same wire type is text or bytes depending on the charset.
    for (const unsigned type : { unsigned(MYSQL_TYPE_BLOB), unsigned(MYSQL_TYPE_TINY_BLOB),
                                 unsigned(MYSQL_TYPE_MEDIUM_BLOB), unsigned(MYSQL_TYPE_LONG_BLOB),
                                 unsigned(MYSQL_TYPE_VARCHAR), unsigned(MYSQL_TYPE_VAR_STRING),
                                 unsigned(MYSQL_TYPE_STRING) }) {
        QCOMPARE(valueTypeForColumn(type, kBinary), ValueType::Bytes);
        QCOMPARE(valueTypeForColumn(type, kUtf8mb4), ValueType::Text);
    }
    QCOMPARE(valueTypeForColumn(MYSQL_TYPE_ENUM, kUtf8mb4), ValueType::Text);
    QCOMPARE(valueTypeForColumn(MYSQL_TYPE_SET, kUtf8mb4), ValueType::Text);
    QCOMPARE(valueTypeForColumn(MYSQL_TYPE_GEOMETRY, kBinary), ValueType::Other);
    QCOMPARE(valueTypeForColumn(MYSQL_TYPE_NULL, kBinary), ValueType::Other);

    QCOMPARE(mysql::nativeTypeName(MYSQL_TYPE_TINY, kBinary, 0), QStringLiteral("tinyint"));
    QCOMPARE(mysql::nativeTypeName(MYSQL_TYPE_LONGLONG, kBinary, UNSIGNED_FLAG),
             QStringLiteral("bigint unsigned"));
    QCOMPARE(mysql::nativeTypeName(MYSQL_TYPE_VAR_STRING, kBinary, 0), QStringLiteral("varbinary"));
    QCOMPARE(mysql::nativeTypeName(MYSQL_TYPE_VAR_STRING, kUtf8mb4, 0), QStringLiteral("varchar"));
    QCOMPARE(mysql::nativeTypeName(MYSQL_TYPE_BLOB, kUtf8mb4, 0), QStringLiteral("text"));
    QCOMPARE(mysql::nativeTypeName(MYSQL_TYPE_BLOB, kBinary, 0), QStringLiteral("blob"));
    QCOMPARE(mysql::nativeTypeName(MYSQL_TYPE_STRING, kUtf8mb4, ENUM_FLAG), QStringLiteral("enum"));
    QCOMPARE(mysql::nativeTypeName(MYSQL_TYPE_STRING, kUtf8mb4, SET_FLAG), QStringLiteral("set"));
    QCOMPARE(mysql::nativeTypeName(MYSQL_TYPE_NEWDECIMAL, kBinary, 0), QStringLiteral("decimal"));
}

void TstMysqlProvider::dataTypeMapping()
{
    using mysql::valueTypeForDataType;
    QCOMPARE(valueTypeForDataType(u"bigint"), ValueType::Int64);
    QCOMPARE(valueTypeForDataType(u"INT"), ValueType::Int64);
    QCOMPARE(valueTypeForDataType(u"tinyint"), ValueType::Int64); // tinyint(1) is not a Bool
    QCOMPARE(valueTypeForDataType(u"double"), ValueType::Double);
    QCOMPARE(valueTypeForDataType(u"decimal"), ValueType::Decimal);
    QCOMPARE(valueTypeForDataType(u"varchar"), ValueType::Text);
    QCOMPARE(valueTypeForDataType(u"longtext"), ValueType::Text);
    QCOMPARE(valueTypeForDataType(u"enum"), ValueType::Text);
    QCOMPARE(valueTypeForDataType(u"blob"), ValueType::Bytes);
    QCOMPARE(valueTypeForDataType(u"varbinary"), ValueType::Bytes);
    QCOMPARE(valueTypeForDataType(u"datetime"), ValueType::DateTime);
    QCOMPARE(valueTypeForDataType(u"timestamp"), ValueType::DateTime);
    QCOMPARE(valueTypeForDataType(u"json"), ValueType::Json);
    QCOMPARE(valueTypeForDataType(u"bit"), ValueType::Int64);
    QCOMPARE(valueTypeForDataType(u"geometry"), ValueType::Other);
}

void TstMysqlProvider::serverVersionParsing()
{
    QVERIFY(mysql::isMariaDb("11.8.10-MariaDB"));
    QVERIFY(mysql::isMariaDb("5.5.5-10.11.6-MariaDB-1:10.11.6+maria~ubu2204"));
    QVERIFY(!mysql::isMariaDb("8.4.3"));
    QVERIFY(!mysql::isMariaDb("8.0.36-0ubuntu0.22.04.1"));
    QCOMPARE(mysql::plainVersion("11.8.10-MariaDB"), QStringLiteral("11.8.10"));
    QCOMPARE(mysql::plainVersion("5.5.5-10.11.6-MariaDB-1"), QStringLiteral("10.11.6"));
    QCOMPARE(mysql::plainVersion("8.4.3"), QStringLiteral("8.4.3"));
    QCOMPARE(mysql::plainVersion("8.0.36-0ubuntu0.22.04.1"), QStringLiteral("8.0.36"));
    QCOMPARE(mysql::plainVersion(""), QString());
}

void TstMysqlProvider::bitDecoding()
{
    QCOMPARE(mysql::bitValue(QByteArrayView("\x05", 1)).value_or(-1), 5);
    QCOMPARE(mysql::bitValue(QByteArrayView("\x01\x02", 2)).value_or(-1), 258); // big endian
    QCOMPARE(mysql::bitValue(QByteArrayView("\xff\xff\xff\xff\xff\xff\xff\xff", 8)).value_or(0),
             static_cast<qint64>(-1)); // BIT(64) all ones: the 64 bits of the Int64
    QCOMPARE(mysql::bitValue(QByteArrayView()).value_or(-1), 0);
    QVERIFY(!mysql::bitValue(QByteArrayView("123456789", 9)).has_value());
}

namespace {

// Stand-in for mysql_real_escape_string with the default sql_mode.
QByteArray fakeEscape(const QByteArray &in)
{
    QByteArray out;
    for (const char c : in) {
        if (c == '\'' || c == '\\' || c == '"')
            out += '\\';
        out += c;
    }
    return out;
}

QString bound(const QString &sql, const QVariantList &parameters)
{
    auto result = mysql::bindParameters(sql, parameters, fakeEscape);
    return result ? QString::fromUtf8(result.value()) : QStringLiteral("<error>");
}

} // namespace

void TstMysqlProvider::bindParametersLiterals()
{
    QCOMPARE(bound(QStringLiteral("SELECT ?, ?, ?"),
                   { 1, QStringLiteral("it's \\ \"q\""), QVariant(QMetaType(QMetaType::QString)) }),
             QStringLiteral("SELECT 1, 'it\\'s \\\\ \\\"q\\\"', NULL"));
    QCOMPARE(bound(QStringLiteral("SELECT ?, ?"), { true, false }), QStringLiteral("SELECT 1, 0"));
    QCOMPARE(bound(QStringLiteral("SELECT ?"),
                   { QVariant::fromValue(std::numeric_limits<qint64>::min()) }),
             QStringLiteral("SELECT -9223372036854775808"));
    QCOMPARE(bound(QStringLiteral("SELECT ?"),
                   { QVariant::fromValue(std::numeric_limits<quint64>::max()) }),
             QStringLiteral("SELECT 18446744073709551615"));
    QCOMPARE(bound(QStringLiteral("SELECT ?"), { 0.1 }),
             QStringLiteral("SELECT 0.10000000000000001"));
    QCOMPARE(bound(QStringLiteral("SELECT ?"), { QByteArray("\x00\xff\x10", 3) }),
             QStringLiteral("SELECT _binary X'00ff10'"));
    QCOMPARE(bound(QStringLiteral("SELECT ?"), { QByteArray() }),
             QStringLiteral("SELECT _binary X''"));
    QCOMPARE(bound(QStringLiteral("SELECT ?"), { QDate(2026, 11, 15) }),
             QStringLiteral("SELECT '2026-11-15'"));
    QCOMPARE(bound(QStringLiteral("SELECT ?"),
                   { QDateTime(QDate(2026, 11, 15), QTime(1, 2, 3, 4), QTimeZone::UTC) }),
             QStringLiteral("SELECT '2026-11-15 01:02:03.004'"));
    QCOMPARE(bound(QStringLiteral("SELECT ?"),
                   { QUuid::fromString(QStringLiteral("123e4567-e89b-12d3-a456-426614174000")) }),
             QStringLiteral("SELECT '123e4567-e89b-12d3-a456-426614174000'"));
    // UTF-8 text around and inside the values stays intact.
    QCOMPARE(bound(QStringLiteral("SELECT 'é', ?, '😀'"), { QStringLiteral("ñ😀") }),
             QStringLiteral("SELECT 'é', 'ñ😀', '😀'"));
}

void TstMysqlProvider::bindParametersSkipsStringsAndComments()
{
    const QVariantList one { 7 };
    QCOMPARE(bound(QStringLiteral("SELECT '?', \"?\", `?`, ?"), one),
             QStringLiteral("SELECT '?', \"?\", `?`, 7"));
    QCOMPARE(bound(QStringLiteral("SELECT 'it''s ?', ?"), one),
             QStringLiteral("SELECT 'it''s ?', 7"));
    QCOMPARE(bound(QStringLiteral("SELECT 'a\\'?', ?"), one), QStringLiteral("SELECT 'a\\'?', 7"));
    QCOMPARE(bound(QStringLiteral("SELECT 1 -- ?\n, ?"), one),
             QStringLiteral("SELECT 1 -- ?\n, 7"));
    QCOMPARE(bound(QStringLiteral("SELECT 1 # ?\n, ?"), one), QStringLiteral("SELECT 1 # ?\n, 7"));
    QCOMPARE(bound(QStringLiteral("SELECT /* ? */ ?"), one), QStringLiteral("SELECT /* ? */ 7"));
    // "--" without a space is two minus signs, so the placeholder after it is live.
    QCOMPARE(bound(QStringLiteral("SELECT 5--?"), one), QStringLiteral("SELECT 5--7"));
}

void TstMysqlProvider::bindParametersRejectsMismatch()
{
    QCOMPARE(bound(QStringLiteral("SELECT ?"), {}), QStringLiteral("<error>"));
    QCOMPARE(bound(QStringLiteral("SELECT 1"), { 1 }), QStringLiteral("<error>"));
    QCOMPARE(bound(QStringLiteral("SELECT ?, ?"), { 1 }), QStringLiteral("<error>"));
    QCOMPARE(bound(QStringLiteral("SELECT ?"), { std::numeric_limits<double>::quiet_NaN() }),
             QStringLiteral("<error>"));
    QCOMPARE(bound(QStringLiteral("SELECT ?"), { std::numeric_limits<double>::infinity() }),
             QStringLiteral("<error>"));
    auto mismatch = mysql::bindParameters(QStringLiteral("SELECT ?"), {}, fakeEscape);
    QVERIFY(!mismatch);
    QVERIFY(!std::move(mismatch).error().message.isEmpty());
    // No parameters and no placeholder: the text goes through untouched.
    QCOMPARE(bound(QStringLiteral("SELECT 'x'"), {}), QStringLiteral("SELECT 'x'"));
}

void TstMysqlProvider::pluginDirectoryCalculation()
{
    QCOMPARE(mysql::pluginDirectory(QStringLiteral("C:/app")),
             QStringLiteral("C:/app/mariadb-plugins"));
    QCOMPARE(mysql::pluginDirectory(QStringLiteral("C:/app/")),
             QStringLiteral("C:/app/mariadb-plugins"));
    QCOMPARE(mysql::pluginDirectory(QStringLiteral("/opt/quarry/bin/../bin")),
             QStringLiteral("/opt/quarry/bin/mariadb-plugins"));
    // Inside a macOS bundle.
    QCOMPARE(mysql::pluginDirectory(QStringLiteral("/Applications/Quarry.app/Contents/MacOS")),
             QStringLiteral("/Applications/Quarry.app/Contents/PlugIns/mariadb"));
    QCOMPARE(mysql::pluginDirectory(QStringLiteral("/Applications/Quarry.app/Contents/MacOS/")),
             QStringLiteral("/Applications/Quarry.app/Contents/PlugIns/mariadb"));

#ifdef Q_OS_WIN
    // On Windows the vcpkg port builds the Connector/C plugins as DLLs: the build copies them
    // next to the test executable, never into the folder of the Quarry plugins (which would
    // reject them). On Linux and macOS they are built into libmariadb (see
    // authPluginsLoadFromPluginDirectory, which checks both cases).
    const QDir dir(mysql::pluginDirectory(QCoreApplication::applicationDirPath()));
    QVERIFY2(dir.exists(), qPrintable(dir.path()));
    const QStringList files = dir.entryList(QDir::Files);
    for (const char *name : { "caching_sha2_password", "sha256_password", "mysql_clear_password",
                              "client_ed25519", "dialog" }) {
        const bool found = std::any_of(files.begin(), files.end(), [name](const QString &file) {
            return file.startsWith(QLatin1String(name));
        });
        QVERIFY2(found, name);
    }
#endif
    const QDir quarryPlugins { QStringLiteral(QUARRY_PLUGIN_DIR) };
    QVERIFY(
        !QFileInfo::exists(quarryPlugins.filePath(QStringLiteral("caching_sha2_password.dll"))));
}

void TstMysqlProvider::authPluginsLoadFromPluginDirectory()
{
    // The authentication plugins are either built into libmariadb (Linux, macOS) or separate
    // libraries that it loads on demand from MYSQL_PLUGIN_DIR (Windows). Loading one registers it
    // for the whole process, so the check with a wrong directory has to come first: a plugin that
    // is not found there is a separate one and must load from the deployed directory. This is the
    // part of "caching_sha2_password works" that needs no server.
    const QStringList names { QStringLiteral("caching_sha2_password"),
                              QStringLiteral("sha256_password"),
                              QStringLiteral("mysql_clear_password"),
                              QStringLiteral("client_ed25519"), QStringLiteral("dialog") };
    const QString wrongDir = QDir::temp().filePath(QStringLiteral("quarry-no-such-plugin-dir"));
    const QByteArray wrongDirBytes = wrongDir.toUtf8();
    QStringList separate;
    for (const QString &name : names) {
        MYSQL *bare = mysql_init(nullptr);
        QVERIFY(bare);
        mysql_options(bare, MYSQL_PLUGIN_DIR, wrongDirBytes.constData());
        if (mysql_client_find_plugin(bare, qPrintable(name), kAuthenticationPlugin) == nullptr)
            separate << name;
        mysql_close(bare);
    }
#ifdef Q_OS_WIN
    // The port's Windows build has no built-in plugin: all five have to be deployed.
    QCOMPARE(separate, names);
#endif
    if (separate.isEmpty())
        qInfo("all authentication plugins are built into libmariadb on this platform");

    MYSQL *handle = mysql_init(nullptr);
    QVERIFY(handle);
    const QByteArray dir = mysql::pluginDirectory(QCoreApplication::applicationDirPath()).toUtf8();
    QCOMPARE(mysql_options(handle, MYSQL_PLUGIN_DIR, dir.constData()), 0);
    for (const QString &name : names) {
        const auto *plugin
            = mysql_client_find_plugin(handle, qPrintable(name), kAuthenticationPlugin);
        QVERIFY2(
            plugin != nullptr,
            qPrintable(QStringLiteral("%1: %2").arg(name, QString::fromUtf8(mysql_error(handle)))));
    }
    mysql_close(handle);
}

void TstMysqlProvider::explainStatements()
{
    const ISqlDialect &dialect = m_provider->dialect();
    const QString select = QStringLiteral("SELECT * FROM t");
    const QString mysql = QStringLiteral("MySQL");
    const QString mariadb = QStringLiteral("MariaDB");
    QCOMPARE(dialect.explainStatement(select, ExplainMode::Plan, mysql),
             QStringLiteral("EXPLAIN FORMAT=JSON SELECT * FROM t"));
    QCOMPARE(dialect.explainStatement(select, ExplainMode::Analyze, mysql),
             QStringLiteral("EXPLAIN ANALYZE SELECT * FROM t"));
    QCOMPARE(dialect.explainStatement(select, ExplainMode::Plan, mariadb),
             QStringLiteral("EXPLAIN FORMAT=JSON SELECT * FROM t"));
    QCOMPARE(dialect.explainStatement(select, ExplainMode::Analyze, mariadb),
             QStringLiteral("ANALYZE FORMAT=JSON SELECT * FROM t"));
    // The product is compared without case and as a substring.
    QCOMPARE(dialect.explainStatement(select, ExplainMode::Analyze, QStringLiteral("mariadb 11")),
             QStringLiteral("ANALYZE FORMAT=JSON SELECT * FROM t"));
    QCOMPARE(dialect.explainStatement(QStringLiteral(" SELECT 1;\n"), ExplainMode::Plan, {}),
             QStringLiteral("EXPLAIN FORMAT=JSON SELECT 1"));
    QVERIFY(dialect.explainStatement(QStringLiteral(" ; "), ExplainMode::Plan, mysql).isEmpty());

    // The product also picks the parser.
    const QString json = QStringLiteral(
        R"({"query_block": {"select_id": 1, "table": {"table_name": "t", "access_type": "ALL"}}})");
    const auto forMysql = dialect.parseExplain(json, ExplainMode::Plan, mysql);
    QVERIFY(forMysql);
    QCOMPARE(forMysql.value().engine, mysql);
    const auto forMariadb = dialect.parseExplain(json, ExplainMode::Plan, mariadb);
    QVERIFY(forMariadb);
    QCOMPARE(forMariadb.value().engine, mariadb);
}

void TstMysqlProvider::explainRunsAgainstAServer()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    const ISqlDialect &dialect = m_provider->dialect();
    QVERIFY(run(*connection, QStringLiteral("CREATE TABLE t(a INT PRIMARY KEY, b INT)")).ok);
    QVERIFY(run(*connection, QStringLiteral("INSERT INTO t VALUES (1, 1), (2, 2), (3, 1)")).ok);
    const QString select = QStringLiteral("SELECT * FROM t WHERE b = 1");

    const QString planText
        = drainAsText(*connection, dialect.explainStatement(select, ExplainMode::Plan, m_product));
    QVERIFY(!planText.isEmpty());
    const auto plan = dialect.parseExplain(planText, ExplainMode::Plan, m_product);
    QVERIFY2(plan, qPrintable(plan ? QString() : plan.error().detail + planText));
    QVERIFY(plan.value().nodeCount >= 1);
    QCOMPARE(plan.value().engine, m_product);

    // MySQL answers EXPLAIN ANALYZE with the indented text, MariaDB with JSON.
    const QString analyzeText = drainAsText(
        *connection, dialect.explainStatement(select, ExplainMode::Analyze, m_product));
    QVERIFY(!analyzeText.isEmpty());
    const auto analyzed = dialect.parseExplain(analyzeText, ExplainMode::Analyze, m_product);
    QVERIFY2(analyzed, qPrintable(analyzed ? QString() : analyzed.error().detail + analyzeText));
    QVERIFY(analyzed.value().nodeCount >= 1);
    QVERIFY(analyzed.value().analyzed);
    std::vector<const ExplainNode *> pending { &analyzed.value().root };
    bool anyActualRows = false;
    while (!pending.empty()) {
        const ExplainNode *node = pending.back();
        pending.pop_back();
        anyActualRows = anyActualRows || node->actualRows.has_value();
        for (const ExplainNode &child : node->children)
            pending.push_back(&child);
    }
    QVERIFY2(anyActualRows, qPrintable(analyzeText));
}

void TstMysqlProvider::dialectSpecifics()
{
    const ISqlDialect &dialect = m_provider->dialect();
    QCOMPARE(dialect.quoteIdentifier(u"a`b"), QStringLiteral("`a``b`"));
    QCOMPARE(dialect.quoteIdentifier(u"we\"ird"), QStringLiteral("`we\"ird`"));
    QCOMPARE(dialect.quoteLiteral(u"it's"), QStringLiteral("'it''s'"));
    QCOMPARE(dialect.quoteLiteral(u"a\\b'c"), QStringLiteral("'a\\\\b''c'"));
    QCOMPARE(dialect.highlightingDefinition(), QStringLiteral("SQL (MySQL)"));
    QCOMPARE(dialect.limitClause(10, 0), QStringLiteral("LIMIT 10"));
    QCOMPARE(dialect.limitClause(10, 20), QStringLiteral("LIMIT 10 OFFSET 20"));
    QVERIFY(dialect.limitClause(-1, 20).contains(QLatin1String("OFFSET 20"))); // needs a LIMIT
    QVERIFY(dialect.limitClause(-1, 20).startsWith(QLatin1String("LIMIT ")));
    QVERIFY(dialect.limitClause(-1, 0).isEmpty());
    QVERIFY(dialect.keywords().contains(QStringLiteral("SELECT")));
    QVERIFY(dialect.keywords().contains(QStringLiteral("DELIMITER")));
    QVERIFY(dialect.keywords().size() > 100);
    QVERIFY(dialect.builtinFunctions().contains(QStringLiteral("group_concat")));
    QVERIFY(dialect.builtinFunctions().contains(QStringLiteral("json_extract")));
    QCOMPARE(dialect.placeholder(1), QStringLiteral("?"));
    QCOMPARE(dialect.placeholder(12), QStringLiteral("?"));
    QCOMPARE(
        dialect.qualifiedName({ { QStringLiteral("db"), QStringLiteral("t") }, ObjectKind::Table }),
        QStringLiteral("`db`.`t`"));
    QVERIFY(!dialect.rowIdentifier().has_value());

    // Ranges are offsets into the original text, also with non-ASCII before them.
    const QString script = QStringLiteral("SELECT 'é;😀';\nDELIMITER $$\nSELECT 2; SELECT 3$$");
    const auto ranges = dialect.splitStatements(script);
    QCOMPARE(ranges.size(), size_t(2));
    QCOMPARE(script.mid(ranges[0].begin, ranges[0].end - ranges[0].begin),
             QStringLiteral("SELECT 'é;😀'"));
    QCOMPARE(script.mid(ranges[1].begin, ranges[1].end - ranges[1].begin),
             QStringLiteral("SELECT 2; SELECT 3"));
}

void TstMysqlProvider::connectionRefusedIsConnectionError()
{
    ConnectionConfig config;
    config.values.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
    config.values.insert(QStringLiteral("port"), 1); // nothing listens there
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

void TstMysqlProvider::typesRoundTrip()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    QVERIFY(run(*connection, QStringLiteral("SET time_zone = '+00:00'")).ok);
    const auto created = run(
        *connection,
        QStringLiteral(
            "CREATE TABLE t_types(c_tiny TINYINT, c_small SMALLINT, c_med MEDIUMINT, c_int INT, "
            "c_big BIGINT, c_ubig BIGINT UNSIGNED, c_float FLOAT, c_double DOUBLE, "
            "c_dec DECIMAL(30,10), c_date DATE, c_time TIME, c_dt DATETIME(3), c_ts TIMESTAMP "
            "NULL, "
            "c_year YEAR, c_vc VARCHAR(10), c_ch CHAR(4), c_bin BINARY(4), c_vbin VARBINARY(10), "
            "c_blob BLOB, c_text TEXT, c_enum ENUM('a','b'), c_set SET('x','y'), c_bit BIT(16), "
            "c_json JSON, c_null INT)"));
    QVERIFY2(created.ok, qPrintable(created.errorText()));
    const auto inserted = run(
        *connection,
        QStringLiteral(
            "INSERT INTO t_types VALUES (-5, 300, -70000, 123456, 9007199254740993, "
            "18446744073709551615, 1.5, 2.25, 12345678901234567890.1234567890, '2026-11-15', "
            "'10:20:30', '2026-11-15 10:20:30.500', '2026-11-15 10:20:30', 2026, 'vc', 'ch', 'ab', "
            "UNHEX('00FF'), UNHEX('00FF10'), 'txt', 'b', 'x,y', b'100000010', '{\"a\": 1}', "
            "NULL)"));
    QVERIFY2(inserted.ok, qPrintable(inserted.errorText()));
    const auto d = run(*connection, QStringLiteral("SELECT * FROM t_types"));
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    QCOMPARE(d.rows, 1);

    // MariaDB's JSON is LONGTEXT with an alias: the server sends it as text.
    const ValueType jsonType = isMariaDb() ? ValueType::Text : ValueType::Json;
    const ValueType expected[]
        = { ValueType::Int64,    ValueType::Int64, ValueType::Int64,  ValueType::Int64,
            ValueType::Int64,    ValueType::Int64, ValueType::Double, ValueType::Double,
            ValueType::Decimal,  ValueType::Date,  ValueType::Time,   ValueType::DateTime,
            ValueType::DateTime, ValueType::Int64, ValueType::Text,   ValueType::Text,
            ValueType::Bytes,    ValueType::Bytes, ValueType::Bytes,  ValueType::Text,
            ValueType::Text,     ValueType::Text,  ValueType::Int64,  jsonType,
            ValueType::Int64 };
    QCOMPARE(d.columns.size(), std::size(expected));
    for (size_t i = 0; i < std::size(expected); ++i)
        QVERIFY2(d.columns[i].type == expected[i],
                 qPrintable(QStringLiteral("column %1 (%2)").arg(i).arg(d.columns[i].nativeType)));
    QCOMPARE(d.columns[0].nativeType, QStringLiteral("tinyint"));
    QCOMPARE(d.columns[4].nativeType, QStringLiteral("bigint"));
    QCOMPARE(d.columns[5].nativeType, QStringLiteral("bigint unsigned"));
    QCOMPARE(d.columns[8].nativeType, QStringLiteral("decimal"));
    QCOMPARE(d.columns[18].nativeType, QStringLiteral("blob"));
    QCOMPARE(d.columns[19].nativeType, QStringLiteral("text"));
    QCOMPARE(d.columns[16].nativeType, QStringLiteral("binary"));

    const ResultBatch &b = d.batches.front();
    QCOMPARE(b.columns[0].int64At(0), -5);
    QCOMPARE(b.columns[1].int64At(0), 300);
    QCOMPARE(b.columns[2].int64At(0), -70000);
    QCOMPARE(b.columns[3].int64At(0), 123456);
    QCOMPARE(b.columns[4].int64At(0), 9'007'199'254'740'993LL); // beyond 2^53
    // Above 2^63 there is no Int64: the exact text is kept and the column becomes Other.
    QCOMPARE(b.columns[5].typeAt(0), ValueType::Other);
    QCOMPARE(textAt(b, 5), QStringLiteral("18446744073709551615"));
    QCOMPARE(b.columns[6].doubleAt(0), 1.5);
    QCOMPARE(b.columns[7].doubleAt(0), 2.25);
    QCOMPARE(textAt(b, 8), QStringLiteral("12345678901234567890.1234567890")); // exact
    QCOMPARE(textAt(b, 9), QStringLiteral("2026-11-15"));
    QCOMPARE(textAt(b, 10), QStringLiteral("10:20:30"));
    QCOMPARE(textAt(b, 11), QStringLiteral("2026-11-15 10:20:30.500"));
    QCOMPARE(textAt(b, 12), QStringLiteral("2026-11-15 10:20:30"));
    QCOMPARE(b.columns[13].int64At(0), 2026);
    QCOMPARE(textAt(b, 14), QStringLiteral("vc"));
    QCOMPARE(textAt(b, 15), QStringLiteral("ch"));
    QCOMPARE(QByteArray(b.columns[16].bytesAt(0)), QByteArray("ab\0\0", 4)); // BINARY pads
    QCOMPARE(QByteArray(b.columns[17].bytesAt(0)), QByteArray("\x00\xff", 2));
    QCOMPARE(QByteArray(b.columns[18].bytesAt(0)), QByteArray("\x00\xff\x10", 3));
    QCOMPARE(textAt(b, 19), QStringLiteral("txt"));
    QCOMPARE(textAt(b, 20), QStringLiteral("b"));
    QCOMPARE(textAt(b, 21), QStringLiteral("x,y"));
    QCOMPARE(b.columns[22].int64At(0), 258);
    QCOMPARE(textAt(b, 23), QStringLiteral("{\"a\": 1}"));
    QVERIFY(b.columns[24].isNull(0));

    // tinyint(1) is a number, not a Bool.
    const auto flag = run(*connection, QStringLiteral("SELECT CAST(1 AS UNSIGNED) = 1 AS t, TRUE"));
    QVERIFY(flag.ok);
    QCOMPARE(flag.columns[0].type, ValueType::Int64);
    QCOMPARE(flag.columns[1].type, ValueType::Int64);
}

void TstMysqlProvider::parameterTypes()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    QVERIFY(run(*connection, QStringLiteral("SET time_zone = '+00:00'")).ok);
    QVERIFY(run(*connection,
                QStringLiteral("CREATE TABLE p(i BIGINT, d DATE, u CHAR(36), f DOUBLE, "
                               "s VARCHAR(50), b BLOB, ts DATETIME(3), q BIGINT, n INT)"))
                .ok);
    const QDateTime when(QDate(2026, 11, 15), QTime(1, 2, 3, 4), QTimeZone::UTC);
    auto inserted
        = drain(*connection, QStringLiteral("INSERT INTO p VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"),
                { 1, QDate(2026, 11, 15),
                  QUuid::fromString(QStringLiteral("123e4567-e89b-12d3-a456-426614174000")), 0.1,
                  QStringLiteral("ñ😀"), QByteArray("\x00\x01", 2), when,
                  QVariant::fromValue(std::numeric_limits<qint64>::min()), QVariant() });
    QVERIFY2(inserted.ok, qPrintable(inserted.errorText()));
    QCOMPARE(inserted.affected.value_or(-1), 1);
    const auto d = run(*connection, QStringLiteral("SELECT * FROM p"));
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    const ResultBatch &b = d.batches.front();
    QCOMPARE(b.columns[0].int64At(0), 1);
    QCOMPARE(textAt(b, 1), QStringLiteral("2026-11-15"));
    QCOMPARE(textAt(b, 2), QStringLiteral("123e4567-e89b-12d3-a456-426614174000"));
    QCOMPARE(b.columns[3].doubleAt(0), 0.1); // 17 digits round trip
    QCOMPARE(textAt(b, 4), QStringLiteral("ñ😀"));
    QCOMPARE(QByteArray(b.columns[5].bytesAt(0)), QByteArray("\x00\x01", 2));
    QCOMPARE(textAt(b, 6), QStringLiteral("2026-11-15 01:02:03.004"));
    QCOMPARE(b.columns[7].int64At(0), std::numeric_limits<qint64>::min());
    QVERIFY(b.columns[8].isNull(0));

    // A '?' inside a string literal is text, not a placeholder.
    const auto literal = drain(*connection, QStringLiteral("SELECT '?' AS a, ? AS b"), { 5 });
    QVERIFY2(literal.ok, qPrintable(literal.errorText()));
    QCOMPARE(textAt(literal.batches.front(), 0), QStringLiteral("?"));
    QCOMPARE(literal.batches.front().columns[1].int64At(0), 5);
}

void TstMysqlProvider::decimalIsExact()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    const auto d = run(*connection,
                       QStringLiteral("SELECT CAST('12345678901234567890.123456789' AS "
                                      "DECIMAL(40,9)) AS a, 0.1 + 0.2 AS b, CAST('-0.000000001' "
                                      "AS DECIMAL(20,9)) AS c"));
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    QCOMPARE(d.columns[0].type, ValueType::Decimal);
    QCOMPARE(d.columns[1].type, ValueType::Decimal);
    QCOMPARE(textAt(d.batches.front(), 0), QStringLiteral("12345678901234567890.123456789"));
    QCOMPARE(textAt(d.batches.front(), 1), QStringLiteral("0.3")); // a double would give ...04
    QCOMPARE(textAt(d.batches.front(), 2), QStringLiteral("-0.000000001"));
}

void TstMysqlProvider::emojiRoundTrip()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    QVERIFY(run(*connection,
                QStringLiteral("CREATE TABLE e(id INT PRIMARY KEY, v VARCHAR(40)) "
                               "CHARACTER SET utf8mb4"))
                .ok);
    // Outside the BMP: 4 bytes in utf8mb4 (the 3-byte utf8mb3 would reject them), 2 UTF-16 units.
    const QString text = QStringLiteral("😀 𝄞 日本 é ✓ \U0001F9D1\u200D\U0001F4BB");
    auto viaParameter = drain(*connection, QStringLiteral("INSERT INTO e VALUES (1, ?)"), { text });
    QVERIFY2(viaParameter.ok, qPrintable(viaParameter.errorText()));
    const QString literal = m_provider->dialect().quoteLiteral(text);
    auto viaLiteral = run(*connection, QStringLiteral("INSERT INTO e VALUES (2, %1)").arg(literal));
    QVERIFY2(viaLiteral.ok, qPrintable(viaLiteral.errorText()));
    const auto d = run(*connection,
                       QStringLiteral("SELECT v, CHAR_LENGTH(v), LENGTH(v) FROM e ORDER BY id"));
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    QCOMPARE(d.rows, 2);
    for (qsizetype row = 0; row < 2; ++row) {
        QCOMPARE(textAt(d.batches.front(), 0, row), text);
        QCOMPARE(d.batches.front().columns[1].int64At(row), text.toUcs4().size());
        QCOMPARE(d.batches.front().columns[2].int64At(row), text.toUtf8().size());
    }
    // And inside a literal that the server evaluates on its own.
    const auto evaluated = run(*connection, QStringLiteral("SELECT '😀' AS v, CHAR_LENGTH('😀')"));
    QVERIFY2(evaluated.ok, qPrintable(evaluated.errorText()));
    QCOMPARE(textAt(evaluated.batches.front(), 0), QStringLiteral("😀"));
    QCOMPARE(evaluated.batches.front().columns[1].int64At(0), 1);
}

void TstMysqlProvider::blobIsBinary()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    QVERIFY(run(*connection, QStringLiteral("CREATE TABLE bl(id INT PRIMARY KEY, v LONGBLOB)")).ok);
    QByteArray every(256, '\0');
    for (int i = 0; i < 256; ++i)
        every[i] = static_cast<char>(i);
    every += QByteArray("'\"\\\0\x1a", 5); // the bytes that break naive escaping
    auto inserted
        = drain(*connection, QStringLiteral("INSERT INTO bl VALUES (1, ?)"), { QVariant(every) });
    QVERIFY2(inserted.ok, qPrintable(inserted.errorText()));
    const auto d = run(*connection,
                       QStringLiteral("SELECT v, UNHEX('00FF10') AS u, '' AS e, "
                                      "CAST('abc' AS BINARY) AS c FROM bl"));
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    QCOMPARE(d.columns[0].type, ValueType::Bytes);
    QCOMPARE(d.columns[1].type, ValueType::Bytes);
    QCOMPARE(d.columns[2].type, ValueType::Text);
    QCOMPARE(d.columns[3].type, ValueType::Bytes);
    QCOMPARE(QByteArray(d.batches.front().columns[0].bytesAt(0)), every);
    QCOMPARE(QByteArray(d.batches.front().columns[1].bytesAt(0)), QByteArray("\x00\xff\x10", 3));
    QCOMPARE(QByteArray(d.batches.front().columns[3].bytesAt(0)), QByteArray("abc"));
}

void TstMysqlProvider::bitColumn()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    QVERIFY(run(*connection,
                QStringLiteral("CREATE TABLE bits(b1 BIT(1), b8 BIT(8), b64 BIT(64), "
                               "bn BIT(8) NULL)"))
                .ok);
    QVERIFY(
        run(*connection,
            QStringLiteral("INSERT INTO bits VALUES (b'1', b'10000001', "
                           "b'1111111111111111111111111111111111111111111111111111111111111110',"
                           " NULL)"))
            .ok);
    const auto d = run(*connection, QStringLiteral("SELECT * FROM bits"));
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    for (size_t i = 0; i < 4; ++i) {
        QCOMPARE(d.columns[i].type, ValueType::Int64);
        QCOMPARE(d.columns[i].nativeType, QStringLiteral("bit"));
    }
    const ResultBatch &b = d.batches.front();
    QCOMPARE(b.columns[0].int64At(0), 1);
    QCOMPARE(b.columns[1].int64At(0), 129);
    QCOMPARE(b.columns[2].int64At(0), static_cast<qint64>(-2)); // the 64 bits of the BIT(64)
    QVERIFY(b.columns[3].isNull(0));
}

void TstMysqlProvider::noBackslashEscapesModeIsHonored()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    // With NO_BACKSLASH_ESCAPES a backslash is an ordinary character and only '' escapes a
    // quote: parameters must be escaped by the library, which knows the mode.
    QVERIFY(run(*connection, QStringLiteral("SET SESSION sql_mode = 'NO_BACKSLASH_ESCAPES'")).ok);
    const QString hostile = QStringLiteral("a\\' OR '1'='1 \\");
    const auto d = drain(*connection, QStringLiteral("SELECT ? AS v"), { hostile });
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    QCOMPARE(textAt(d.batches.front(), 0), hostile);
}

void TstMysqlProvider::fetchSizeIndependentOfBatch()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    auto stream = connection->execute(Statement { m_scratch->seriesSql(1000), {}, 100 }, {});
    QVERIFY2(stream, qPrintable(stream ? QString() : stream.error().detail));
    auto big = stream.value()->fetch(250, {});
    QVERIFY(big);
    QCOMPARE(big.value().rowCount, 250);
    QCOMPARE(big.value().columns[0].int64At(249), 250);
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

void TstMysqlProvider::droppedStreamKeepsConnectionUsable()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    QElapsedTimer clock;
    clock.start();
    {
        // 10^8 rows, produced as they are sent.
        auto stream = connection->execute(
            Statement { QStringLiteral("SELECT t0.d, t7.d") + digitsJoin(8), {}, 100 }, {});
        QVERIFY2(stream, qPrintable(stream ? QString() : stream.error().detail));
        auto first = stream.value()->fetch(100, {});
        QVERIFY(first);
        QVERIFY(!stream.value()->atEnd());
        qInfo() << "first batch after" << clock.elapsed() << "ms";
        clock.restart();
    } // destroyed with ~100M rows pending: must KILL QUERY and drain, not read them all
    const qint64 dropMs = clock.elapsed();
    const auto d = run(*connection, QStringLiteral("SELECT 1"));
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    qInfo() << "dropping a stream with 100M pending rows took" << dropMs << "ms";
    QVERIFY2(dropMs < 2000, qPrintable(QString::number(dropMs)));
}

void TstMysqlProvider::cancelSleepIsFast()
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
                Statement { QStringLiteral("SELECT SLEEP(30)"), {}, 500 }, {});
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
                 "SLEEP(30) did not stop");
        const qint64 elapsed = clock.elapsed();
        timings << elapsed;
        const auto kind = worker.get();
        QVERIFY(kind.has_value());
        // SLEEP returns 1 instead of failing when it is killed: still a cancellation.
        QCOMPARE(kind.value_or(ErrorKind::Internal), ErrorKind::Cancelled);
        QVERIFY2(elapsed < 500, qPrintable(QStringLiteral("cancel took %1 ms").arg(elapsed)));

        const auto next = run(*connection, QStringLiteral("SELECT %1").arg(round + 1));
        QVERIFY2(next.ok, qPrintable(next.errorText()));
        QCOMPARE(next.batches.front().columns[0].int64At(0), round + 1);
    }
    qInfo() << "cancel of SELECT SLEEP(30), cancel-to-return ms per round:" << timings
            << "requestCancel() call duration us:" << callTimings;
}

void TstMysqlProvider::cancelWhileFetchingRows()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    QVERIFY(run(*connection, QStringLiteral("SET @n = 0")).ok);
    // Row 20000 blocks the server for a minute. The padding fills the network buffer, so the
    // rows before it reach the client and the fetch loop gets stuck inside a fetch(), not inside
    // execute().
    auto stream = connection->execute(
        Statement { QStringLiteral("SELECT (@n := @n + 1) AS n, REPEAT('x', 100) AS pad, "
                                   "IF(@n = 20000, SLEEP(60), 0) AS s")
                        + digitsJoin(5) + QStringLiteral(" LIMIT 30000"),
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
    QTRY_VERIFY_WITH_TIMEOUT(rows.load() >= 19000, 10000);
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

void TstMysqlProvider::lateCancelNeverHitsNextStatement()
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

void TstMysqlProvider::sessionCancelsSleep()
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
    session.execute(Statement { QStringLiteral("SELECT SLEEP(30)"), {}, 500 });
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

void TstMysqlProvider::errorKindsFromServer()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    QVERIFY(run(*connection,
                QStringLiteral("CREATE TABLE k(id INT PRIMARY KEY, "
                               "v INT NOT NULL CHECK (v > 0))"))
                .ok);
    QVERIFY(run(*connection, QStringLiteral("INSERT INTO k VALUES (1, 1)")).ok);

    struct Case
    {
        QString sql;
        ErrorKind kind;
        unsigned code;
    };
    const Case cases[]
        = { { QStringLiteral("SELEC 1"), ErrorKind::Syntax, 1064 },
            { QStringLiteral("SELECT zzz FROM k"), ErrorKind::NotFound, 1054 },
            { QStringLiteral("SELECT * FROM no_such_table"), ErrorKind::NotFound, 1146 },
            { QStringLiteral("INSERT INTO k VALUES (1, 1)"), ErrorKind::Constraint, 1062 },
            { QStringLiteral("INSERT INTO k VALUES (2, NULL)"), ErrorKind::Constraint, 1048 },
            { QStringLiteral("INSERT INTO k VALUES (3, -1)"), ErrorKind::Constraint, 4025 } };
    for (const Case &c : cases) {
        auto stream = connection->execute(Statement { c.sql, {}, 500 }, {});
        QVERIFY2(!stream, qPrintable(c.sql));
        const Error error = std::move(stream).error();
        if (c.code == 4025) { // MariaDB 4025, MySQL 3819: both are CHECK violations
            QCOMPARE(error.kind, ErrorKind::Constraint);
        } else {
            QCOMPARE(error.kind, c.kind);
            QVERIFY2(error.detail.contains(QStringLiteral("error %1").arg(c.code)),
                     qPrintable(error.detail));
        }
        QVERIFY(!error.message.isEmpty());
        QVERIFY2(!error.detail.isEmpty(), qPrintable(c.sql));
        QVERIFY2(!error.sqlState.isEmpty(), qPrintable(c.sql));
        QVERIFY(!error.position.has_value()); // MySQL gives no position
        QVERIFY(run(*connection, QStringLiteral("SELECT 1")).ok); // still usable
    }
    // A runtime error in the middle of a stream surfaces from fetch(), not from execute().
    QVERIFY(run(*connection,
                QStringLiteral("SET SESSION sql_mode = 'STRICT_ALL_TABLES,"
                               "ERROR_FOR_DIVISION_BY_ZERO'"))
                .ok);
    QVERIFY(run(*connection, QStringLiteral("CREATE TABLE z(a INT)")).ok);
    QVERIFY(run(*connection, QStringLiteral("INSERT INTO z VALUES (1), (2)")).ok);
    const auto updated = run(*connection, QStringLiteral("UPDATE z SET a = 1 / (a - 1)"));
    QVERIFY(!updated.ok);
    QCOMPARE(updated.error.kind, ErrorKind::Driver);
    QVERIFY(updated.error.detail.contains(QLatin1String("1365")));
}

void TstMysqlProvider::authenticationFailure()
{
    REQUIRE_SERVER();
    ConnectionConfig config = m_server.config(m_server.database);
    auto connection
        = m_provider->createConnection(config, Secrets { QStringLiteral("not the password") });
    auto info = connection->open({});
    if (info)
        QSKIP("the server accepts any password");
    const Error error = std::move(info).error();
    QCOMPARE(error.kind, ErrorKind::Authentication);
    QVERIFY(!error.detail.isEmpty());
    QVERIFY(error.detail.contains(QLatin1String("1045")));
    QVERIFY(!connection->isOpen());

    // The right password through Secrets works even when the environment has a wrong one.
    qputenv("MYSQL_PWD", "wrong");
    auto good = m_provider->createConnection(config, Secrets { m_server.password });
    auto goodInfo = good->open({});
    qputenv("MYSQL_PWD", m_server.password.toUtf8());
    QVERIFY2(goodInfo, qPrintable(goodInfo ? QString() : goodInfo.error().detail));
    QVERIFY(!goodInfo.value().version.isEmpty());
}

void TstMysqlProvider::cachingSha2PasswordLogin()
{
    REQUIRE_SERVER();
    // The default login method of MySQL 8.4 (MariaDB has it since 11.6). libmariadb has to load
    // caching_sha2_password.dll from MYSQL_PLUGIN_DIR for this to work at all.
    const QString user = QStringLiteral("quarry_sha2_%1").arg(QCoreApplication::applicationPid());
    const QString password = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto admin = m_provider->createConnection(m_server.config(m_server.database), {});
    auto adminInfo = admin->open({});
    QVERIFY2(adminInfo, qPrintable(adminInfo ? QString() : adminInfo.error().detail));
    const QString account = QStringLiteral("`%1`@`%`").arg(user);
    const QString create = adminInfo.value().product == QLatin1String("MariaDB")
        ? QStringLiteral("CREATE USER %1 IDENTIFIED VIA caching_sha2_password USING PASSWORD('%2')")
        : QStringLiteral("CREATE USER %1 IDENTIFIED WITH caching_sha2_password BY '%2'");
    const auto created = run(*admin, create.arg(account, password));
    if (!created.ok)
        QSKIP(qPrintable(QStringLiteral("the server cannot create a caching_sha2_password user: ")
                         + created.errorText()));

    ConnectionConfig config = m_server.config(QString());
    config.values.insert(QStringLiteral("user"), user);
    for (const QString &mode : { QStringLiteral("prefer"), QStringLiteral("disable") }) {
        config.values.insert(QStringLiteral("sslmode"), mode);
        auto connection = m_provider->createConnection(config, Secrets { password });
        auto info = connection->open({});
        // Without TLS the server asks for the RSA public key: MySQL 8.4 allows that only when
        // the client asks for it, so a refusal there is a server policy, not a plugin problem.
        if (!info && mode == QLatin1String("disable")) {
            qInfo() << "caching_sha2_password without TLS:" << info.error().detail;
            continue;
        }
        QVERIFY2(info,
                 qPrintable(mode + QLatin1String(": ") + (info ? QString() : info.error().detail)));
        const auto d = run(*connection, QStringLiteral("SELECT CURRENT_USER()"));
        QVERIFY2(d.ok, qPrintable(d.errorText()));
        QVERIFY(textAt(d.batches.front(), 0).startsWith(user));
    }

    config.values.insert(QStringLiteral("sslmode"), QStringLiteral("prefer"));
    auto wrong = m_provider->createConnection(config, Secrets { QStringLiteral("nope") });
    auto wrongInfo = wrong->open({});
    QVERIFY(!wrongInfo);
    QCOMPARE(std::move(wrongInfo).error().kind, ErrorKind::Authentication);

    const auto dropped = run(*admin, QStringLiteral("DROP USER %1").arg(account));
    QVERIFY2(dropped.ok, qPrintable(dropped.errorText()));
}

void TstMysqlProvider::unknownDatabase()
{
    REQUIRE_SERVER();
    auto connection
        = m_provider->createConnection(m_server.config(QStringLiteral("no_such_db_quarry")), {});
    auto info = connection->open({});
    QVERIFY(!info);
    const Error error = std::move(info).error();
    QCOMPARE(error.kind, ErrorKind::NotFound);
    QVERIFY(error.detail.contains(QLatin1String("1049")));
}

void TstMysqlProvider::sslModes()
{
    REQUIRE_SERVER();
    auto cipherOf = [&](const QString &mode) -> QString {
        ConnectionConfig config = m_server.config(m_server.database);
        config.values.insert(QStringLiteral("sslmode"), mode);
        auto connection = m_provider->createConnection(config, {});
        auto info = connection->open({});
        if (!info)
            return QStringLiteral("<failed: ") + info.error().detail + QLatin1Char('>');
        const auto d = drain(*connection, QStringLiteral("SHOW SESSION STATUS LIKE 'Ssl_cipher'"));
        if (!d.ok || d.rows != 1)
            return QStringLiteral("<no status>");
        return textAt(d.batches.front(), 1);
    };

    const QString plain = cipherOf(QStringLiteral("disable"));
    QVERIFY2(!plain.startsWith(QLatin1Char('<')), qPrintable(plain));
    QVERIFY2(plain.isEmpty(), qPrintable(QStringLiteral("disable used TLS: ") + plain));

    const QString preferred = cipherOf(QStringLiteral("prefer"));
    QVERIFY2(!preferred.startsWith(QLatin1Char('<')), qPrintable(preferred)); // never fails
    const QString required = cipherOf(QStringLiteral("require"));
    if (preferred.isEmpty()) {
        // A server without TLS: prefer must have fallen back to plain text, and require must
        // refuse instead of silently downgrading.
        QVERIFY2(required.startsWith(QLatin1Char('<')), qPrintable(required));
        qInfo() << "server without TLS: prefer fell back to plain text, require refused";
    } else {
        QVERIFY2(!required.isEmpty() && !required.startsWith(QLatin1Char('<')),
                 qPrintable(required));
        qInfo() << "server with TLS: prefer and require both negotiated" << preferred;
    }

    ConnectionConfig strict = m_server.config(m_server.database);
    strict.values.insert(QStringLiteral("sslmode"), QStringLiteral("verify-full"));
    strict.values.insert(QStringLiteral("sslrootcert"),
                         QDir::temp().filePath(QStringLiteral("quarry-missing-root.crt")));
    auto connection = m_provider->createConnection(strict, {});
    auto info = connection->open({});
    QVERIFY(!info); // missing root certificate, or no TLS on the server: never a silent downgrade
    QCOMPARE(std::move(info).error().kind, ErrorKind::Connection);

    strict.values.insert(QStringLiteral("sslmode"), QStringLiteral("verify-ca"));
    auto verifyCa = m_provider->createConnection(strict, {});
    auto verifyInfo = verifyCa->open({});
    QVERIFY(!verifyInfo);
    QCOMPARE(std::move(verifyInfo).error().kind, ErrorKind::Connection);
}

void TstMysqlProvider::treeHidesSystemDatabases()
{
    REQUIRE_SERVER();
    QString database;
    auto connection = openFresh(&database);
    QVERIFY(connection);
    QVERIFY(run(*connection,
                QStringLiteral("CREATE TABLE t(id INT PRIMARY KEY, v TEXT, "
                               "w VARCHAR(5) NOT NULL, UNIQUE KEY uq_w (w), "
                               "KEY idx_vw (id, w))"))
                .ok);
    QVERIFY(run(*connection,
                QStringLiteral("CREATE TABLE child(id INT PRIMARY KEY, t_id INT, "
                               "CONSTRAINT fk_child_t FOREIGN KEY (t_id) "
                               "REFERENCES t(id))"))
                .ok);
    QVERIFY(run(*connection, QStringLiteral("CREATE VIEW vw AS SELECT id FROM t")).ok);

    auto root = connection->children(ObjectPath { {}, ObjectKind::Database }, {});
    QVERIFY2(root, qPrintable(root ? QString() : root.error().detail));
    QStringList names;
    for (const SchemaNode &db : root.value()) {
        QCOMPARE(db.path.kind, ObjectKind::Database);
        QCOMPARE(db.path.parts, QStringList { db.name });
        QVERIFY(db.hasChildren);
        names << db.name;
    }
    QVERIFY(names.contains(database));
    for (const QString &hidden :
         { QStringLiteral("mysql"), QStringLiteral("sys"), QStringLiteral("information_schema"),
           QStringLiteral("performance_schema") })
        QVERIFY2(!names.contains(hidden), qPrintable(hidden));
    // They really exist: hidden by the provider, not absent.
    const auto catalog
        = run(*connection,
              QStringLiteral("SELECT COUNT(*) FROM information_schema.SCHEMATA "
                             "WHERE SCHEMA_NAME IN ('mysql', 'information_schema')"));
    QVERIFY(catalog.ok);
    QCOMPARE(catalog.batches.front().columns[0].int64At(0), 2);
    // The system tables are not reachable through the tree either.
    auto systemTables = connection->children(
        ObjectPath { { QStringLiteral("mysql") }, ObjectKind::Database }, {});
    QVERIFY(systemTables); // asking for them directly is allowed, the root just never offers them

    const auto &dbNode = std::find_if(root.value().begin(), root.value().end(),
                                      [&](const SchemaNode &n) { return n.name == database; });
    QVERIFY(dbNode != root.value().end());
    auto items = connection->children(dbNode->path, {});
    QVERIFY(items);
    QCOMPARE(items.value().size(), size_t(3));
    QCOMPARE(items.value()[0].name, QStringLiteral("child"));
    QCOMPARE(items.value()[0].path.kind, ObjectKind::Table);
    QCOMPARE(items.value()[1].name, QStringLiteral("t"));
    QCOMPARE(items.value()[2].name, QStringLiteral("vw"));
    QCOMPARE(items.value()[2].path.kind, ObjectKind::View); // views after tables
    QCOMPARE(items.value()[1].path.parts, (QStringList { database, QStringLiteral("t") }));

    auto folders = connection->children(items.value()[1].path, {});
    QVERIFY(folders);
    QCOMPARE(folders.value().size(), size_t(3)); // Columns, Indexes, Foreign keys
    auto viewFolders = connection->children(items.value()[2].path, {});
    QVERIFY(viewFolders);
    QCOMPARE(viewFolders.value().size(), size_t(1)); // a view only has columns

    auto columns = connection->children(folders.value()[0].path, {});
    QVERIFY(columns);
    QCOMPARE(columns.value().size(), size_t(3));
    QCOMPARE(columns.value()[0].name, QStringLiteral("id"));
    QVERIFY2(columns.value()[0].detail.startsWith(QLatin1String("int"))
                 && columns.value()[0].detail.endsWith(QLatin1String("NOT NULL PK")),
             qPrintable(columns.value()[0].detail));
    QCOMPARE(columns.value()[1].detail, QStringLiteral("text"));
    QCOMPARE(columns.value()[2].detail, QStringLiteral("varchar(5) NOT NULL"));
    QCOMPARE(columns.value()[0].path.parts.size(), 3);

    auto indexes = connection->children(folders.value()[1].path, {});
    QVERIFY(indexes);
    QStringList indexNames;
    for (const SchemaNode &index : indexes.value()) {
        QCOMPARE(index.path.kind, ObjectKind::Index);
        indexNames << index.name;
        if (index.name == QLatin1String("uq_w"))
            QCOMPARE(index.detail, QStringLiteral("UNIQUE"));
    }
    QCOMPARE(indexNames, (QStringList { QStringLiteral("idx_vw"), QStringLiteral("uq_w") }));

    auto childFolders = connection->children(items.value()[0].path, {});
    QVERIFY(childFolders);
    auto fks = connection->children(childFolders.value()[2].path, {});
    QVERIFY(fks);
    QCOMPARE(fks.value().size(), size_t(1));
    QCOMPARE(fks.value()[0].name, QStringLiteral("fk_child_t"));
    QCOMPARE(fks.value()[0].path.kind, ObjectKind::ForeignKey);
    QCOMPARE(fks.value()[0].detail, QStringLiteral("-> %1.t(id)").arg(database));

    auto details = connection->describe(items.value()[1].path, {});
    QVERIFY(details);
    QCOMPARE(details.value().primaryKey.value_or(QStringList()),
             QStringList { QStringLiteral("id") });
    QCOMPARE(details.value().indexes.size(), size_t(2));
    const auto composite
        = std::find_if(details.value().indexes.begin(), details.value().indexes.end(),
                       [](const IndexDef &i) { return i.name == QLatin1String("idx_vw"); });
    QVERIFY(composite != details.value().indexes.end());
    QCOMPARE(composite->columns, (QStringList { QStringLiteral("id"), QStringLiteral("w") }));
    QVERIFY(!composite->unique);
    QCOMPARE(details.value().columns[1].type, ValueType::Text);
    QVERIFY(!details.value().columns[2].nullable);
    auto childDetails = connection->describe(items.value()[0].path, {});
    QVERIFY(childDetails);
    QCOMPARE(childDetails.value().foreignKeys.size(), size_t(1));
    QCOMPARE(childDetails.value().foreignKeys[0].referencedTable.parts,
             (QStringList { database, QStringLiteral("t") }));
}

void TstMysqlProvider::noticesAreCollected()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    // A statement without rows: the warning is there as soon as execute() returns.
    auto stream = connection->execute(
        Statement { QStringLiteral("DROP TABLE IF EXISTS no_such_table_quarry"), {}, 500 }, {});
    QVERIFY2(stream, qPrintable(stream ? QString() : stream.error().detail));
    QVERIFY(stream.value()->atEnd());
    QCOMPARE(stream.value()->notices().size(), 1);
    QVERIFY2(stream.value()->notices().front().startsWith(QLatin1String("Note (1051): ")),
             qPrintable(stream.value()->notices().front()));
    QVERIFY(stream.value()->affectedRows().has_value());

    // Warnings of a SELECT arrive when its rows have been read, and do not leak into the next.
    auto rows = connection->execute(
        Statement { QStringLiteral("SELECT CAST('12abc' AS SIGNED) AS v"), {}, 500 }, {});
    QVERIFY(rows);
    QVERIFY(rows.value()->notices().isEmpty());
    auto batch = rows.value()->fetch(500, {});
    QVERIFY(batch);
    QVERIFY(rows.value()->atEnd());
    QCOMPARE(rows.value()->notices().size(), 1);
    QVERIFY2(rows.value()->notices().front().startsWith(QLatin1String("Warning (1292): ")),
             qPrintable(rows.value()->notices().front()));
    rows.value().reset();
    auto clean = run(*connection, QStringLiteral("SELECT 1"));
    QVERIFY(clean.ok);
    auto second = connection->execute(Statement { QStringLiteral("SELECT 2"), {}, 500 }, {});
    QVERIFY(second);
    QVERIFY(second.value()->fetch(500, {}));
    QVERIFY(second.value()->notices().isEmpty());
}

void TstMysqlProvider::delimiterScriptRuns()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    const QString script = QStringLiteral(
        "CREATE TABLE t(v VARCHAR(20));\n"
        "DELIMITER //\n"
        "CREATE PROCEDURE add_row(IN x VARCHAR(20))\n"
        "BEGIN\n  INSERT INTO t VALUES (x); -- a ; inside\n  INSERT INTO t VALUES (';');\nEND//\n"
        "DELIMITER ;\n"
        "CALL add_row('a;b');\n"
        "SELECT v FROM t ORDER BY v;");
    const auto ranges = m_provider->dialect().splitStatements(script);
    QCOMPARE(ranges.size(), size_t(4));
    Drained last;
    for (const StatementRange &range : ranges) {
        last = run(*connection, script.mid(range.begin, range.end - range.begin));
        QVERIFY2(last.ok,
                 qPrintable(script.mid(range.begin, range.end - range.begin) + " | "
                            + last.errorText()));
    }
    QCOMPARE(last.rows, 2);
    QCOMPARE(textAt(last.batches.front(), 0, 0), QStringLiteral(";"));
    QCOMPARE(textAt(last.batches.front(), 0, 1), QStringLiteral("a;b"));
}

void TstMysqlProvider::callWithTwoResultSetsLeavesConnectionUsable()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    QVERIFY(run(*connection,
                QStringLiteral("CREATE PROCEDURE two() BEGIN SELECT 1 AS a; "
                               "SELECT 2 AS b, 3 AS c; END"))
                .ok);
    {
        auto stream = connection->execute(Statement { QStringLiteral("CALL two()"), {}, 500 }, {});
        QVERIFY2(stream, qPrintable(stream ? QString() : stream.error().detail));
        QCOMPARE(stream.value()->columns().size(), size_t(1));
        auto batch = stream.value()->fetch(500, {});
        QVERIFY(batch);
        QCOMPARE(batch.value().columns[0].int64At(0), 1);
        auto more = stream.value()->nextResult({});
        QVERIFY(more);
        QVERIFY(!more.value()); // shortcut: v1 reads the first result set only
    }
    // The remaining results were drained: no "commands out of sync".
    const auto d = run(*connection, QStringLiteral("SELECT 7"));
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    QCOMPARE(d.batches.front().columns[0].int64At(0), 7);

    // Same when the stream is dropped before it was read at all.
    {
        auto dropped = connection->execute(Statement { QStringLiteral("CALL two()"), {}, 500 }, {});
        QVERIFY(dropped);
    }
    QVERIFY(run(*connection, QStringLiteral("SELECT 8")).ok);
}

void TstMysqlProvider::transactions()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    QVERIFY(run(*connection, QStringLiteral("CREATE TABLE t(a INT) ENGINE=InnoDB")).ok);
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

    // Another connection sees only what was committed.
    auto other = m_provider->createConnection(m_scratch->config(), {});
    QVERIFY(other->open({}));
    QVERIFY(connection->begin());
    QVERIFY(run(*connection, QStringLiteral("INSERT INTO t VALUES (3)")).ok);
    QCOMPARE(run(*other, QStringLiteral("SELECT * FROM t")).rows, 2);
    QVERIFY(connection->commit());
    QCOMPARE(run(*other, QStringLiteral("SELECT * FROM t")).rows, 3);
}

void TstMysqlProvider::affectedRowsCountMatchedRows()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    QVERIFY(run(*connection, QStringLiteral("CREATE TABLE t(id INT PRIMARY KEY, v INT)")).ok);
    QVERIFY(run(*connection, QStringLiteral("INSERT INTO t VALUES (1, 5), (2, 6)")).ok);
    // MySQL counts changed rows by default: a no-op UPDATE would look like a lost update to the
    // data editor. The connection asks for matched rows instead (CLIENT_FOUND_ROWS).
    const auto same = run(*connection, QStringLiteral("UPDATE t SET v = 5 WHERE id = 1"));
    QVERIFY2(same.ok, qPrintable(same.errorText()));
    QCOMPARE(same.affected.value_or(-1), 1);
    const auto none = run(*connection, QStringLiteral("UPDATE t SET v = 5 WHERE id = 99"));
    QCOMPARE(none.affected.value_or(-1), 0);
}

void TstMysqlProvider::multipleStatementsAreRejected()
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
    // One statement with a trailing semicolon is fine.
    QVERIFY(run(*connection, QStringLiteral("SELECT 1;")).ok);
}

void TstMysqlProvider::emptyAndCommentOnlyStatements()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    for (const QString &sql :
         { QString(), QStringLiteral("  "), QStringLiteral("-- nothing"), QStringLiteral(";"),
           QStringLiteral("# nothing"), QStringLiteral("/* nothing */") }) {
        const auto d = run(*connection, sql);
        QVERIFY2(d.ok, qPrintable(sql + d.errorText()));
        QCOMPARE(d.rows, 0);
        QVERIFY(d.columns.empty());
    }
    QVERIFY(run(*connection, QStringLiteral("SELECT 1")).ok);
}

QTEST_GUILESS_MAIN(TstMysqlProvider)
#include "tst_mysql_provider.moc"
