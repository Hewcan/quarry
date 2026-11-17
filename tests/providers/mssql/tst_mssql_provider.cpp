#include "mssql_support.h"
#include "odbc_state.h"
#include "odbc_support.h"
#include "provider_conformance.h"

#include <quarry/provider_registry.h>
#include <quarry/session.h>

#include <QSignalSpy>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <future>
#include <memory>

using namespace quarry;
using namespace quarry::conformance;

namespace {

// QUARRY_TEST_MSSQL_URL: mssql://user:password@host:port/database?trustservercertificate=true
// (also sqlserver://; the query takes trustservercertificate, encrypt and driver). The user needs
// the right to create and drop databases: the conformance suite makes quarry_conf_<pid>_<n> for
// each test and drops it when the test ends. The CI runs it against SQL Server 2022 with
// msodbcsql18.
const QString kUrlVariable = QStringLiteral("QUARRY_TEST_MSSQL_URL");

struct ServerUrl
{
    QString host;
    int port = 1433;
    QString user;
    QString password;
    QString database;
    bool trust = false;
    QString encrypt;
    QString driver;
    bool valid = false;

    static ServerUrl fromEnvironment()
    {
        ServerUrl out;
        const QUrl url(qEnvironmentVariable(qPrintable(kUrlVariable)));
        if (!url.isValid() || url.host().isEmpty())
            return out;
        out.host = url.host();
        out.port = url.port(1433);
        out.user = url.userName();
        out.password = url.password();
        out.database = url.path().mid(1);
        const QUrlQuery query(url);
        out.trust = QString::compare(query.queryItemValue(QStringLiteral("trustservercertificate")),
                                     QStringLiteral("true"), Qt::CaseInsensitive)
            == 0;
        out.encrypt = query.queryItemValue(QStringLiteral("encrypt"));
        out.driver = query.queryItemValue(QStringLiteral("driver"));
        out.valid = true;
        return out;
    }

    ConnectionConfig config(const QString &databaseName) const
    {
        ConnectionConfig c;
        c.values.insert(QStringLiteral("server"), host);
        c.values.insert(QStringLiteral("port"), port);
        c.values.insert(QStringLiteral("user"), user);
        c.values.insert(QStringLiteral("database"), databaseName);
        c.values.insert(QStringLiteral("trustservercertificate"), trust);
        if (!encrypt.isEmpty())
            c.values.insert(QStringLiteral("encrypt"), encrypt);
        if (!driver.isEmpty())
            c.values.insert(QStringLiteral("driver"), driver);
        return c;
    }
};

// Runs one statement to its end on a throwaway connection; empty string on success.
QString runOnce(IDatabaseProvider &provider, const ConnectionConfig &config, const QString &sql,
                const Secrets &secrets)
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

class MssqlFixture : public Fixture
{
public:
    MssqlFixture(IDatabaseProvider &provider, const ServerUrl &server, QString database)
        : m_provider(provider)
        , m_server(server)
        , m_database(std::move(database))
    { }

    // Fresh database per test: no collisions between tests or with other users of the server.
    QString create()
    {
        return runOnce(m_provider, m_server.config(m_server.database),
                       QStringLiteral("CREATE DATABASE [%1]").arg(m_database), secrets());
    }
    ~MssqlFixture() override
    {
        if (!m_created)
            return;
        // Nobody else is connected by now; the single-user switch only covers a straggler.
        const ConnectionConfig admin = m_server.config(m_server.database);
        runOnce(m_provider, admin,
                QStringLiteral("ALTER DATABASE [%1] SET SINGLE_USER WITH ROLLBACK IMMEDIATE")
                    .arg(m_database),
                secrets());
        runOnce(m_provider, admin, QStringLiteral("DROP DATABASE IF EXISTS [%1]").arg(m_database),
                secrets());
    }
    void markCreated() { m_created = true; }

    IDatabaseProvider &provider() override { return m_provider; }
    ConnectionConfig config() override { return m_server.config(m_database); }
    Secrets secrets() const override { return Secrets { m_server.password }; }
    QString sampleSchemaSql() const override
    {
        // CREATE VIEW has to be alone in its batch: every statement of the script runs apart.
        return QStringLiteral(
            "CREATE TABLE authors(id BIGINT PRIMARY KEY, name NVARCHAR(255) NOT NULL UNIQUE);\n"
            "CREATE TABLE books(id BIGINT PRIMARY KEY, title NVARCHAR(255) NOT NULL,\n"
            "  author_id BIGINT NOT NULL REFERENCES authors(id), price FLOAT NULL,\n"
            "  cover VARBINARY(MAX) NULL);\n"
            "CREATE INDEX idx_books_title ON books(title); -- a comment; with a semicolon\n"
            "GO\n"
            "CREATE VIEW v_titles AS SELECT id, title FROM books;\n");
    }
    QString slowQuerySql() const override { return QStringLiteral("WAITFOR DELAY '01:00:00'"); }
    QString seriesSql(int count) const override
    {
        return QStringLiteral("WITH s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < %1) "
                              "SELECT n FROM s ORDER BY n OPTION (MAXRECURSION 0)")
            .arg(count);
    }
    QString rowIdentifierName() const override { return {}; }
    bool hasRowIdentifier() const override { return false; }
    bool reportsErrorPosition() const override { return false; }
    ObjectPath booksPath() const override
    {
        return { { m_database, QStringLiteral("dbo"), QStringLiteral("books") },
                 ObjectKind::Table };
    }
    ObjectPath viewPath() const override
    {
        return { { m_database, QStringLiteral("dbo"), QStringLiteral("v_titles") },
                 ObjectKind::View };
    }
    ValueType fractionalLiteralType() const override { return ValueType::Decimal; }
    std::vector<SplitCase> extraSplitCases() const override
    {
        return {
            { QStringLiteral("SELECT 1\nGO\nSELECT 2"),
              { QStringLiteral("SELECT 1"), QStringLiteral("SELECT 2") } },
            { QStringLiteral("BEGIN\n  SELECT 1;\n  SELECT 2;\nEND;\nSELECT 3"),
              { QStringLiteral("BEGIN\n  SELECT 1;\n  SELECT 2;\nEND"),
                QStringLiteral("SELECT 3") } },
            { QStringLiteral("SELECT [a;b], [c]]d;e] FROM [t]; SELECT 2"),
              { QStringLiteral("SELECT [a;b], [c]]d;e] FROM [t]"), QStringLiteral("SELECT 2") } },
            { QStringLiteral("/* a /* nested ; */ still ; */ SELECT 1; SELECT 2"),
              { QStringLiteral("/* a /* nested ; */ still ; */ SELECT 1"),
                QStringLiteral("SELECT 2") } },
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
            QSKIP("QUARRY_TEST_MSSQL_URL is not set");                                             \
    } while (false)

// Text of one cell of a batch.
QString textAt(const ResultBatch &batch, size_t column, qsizetype row = 0)
{
    return QString::fromUtf8(batch.columns[column].bytesAt(row));
}

mssql::ConnSettings settingsFor(const QVariantMap &values, const QString &password = {})
{
    auto settings = mssql::buildConnSettings(values, password);
    return settings ? std::move(settings).value() : mssql::ConnSettings {};
}

QVariantMap form(std::initializer_list<std::pair<QString, QVariant>> items)
{
    QVariantMap map;
    for (const auto &item : items)
        map.insert(item.first, item.second);
    return map;
}

} // namespace

class TstMssqlProvider : public ProviderConformanceTest
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
                QTest::qSkip("QUARRY_TEST_MSSQL_URL is not set", __FILE__, __LINE__);
                return nullptr;
            }
            return std::make_unique<MssqlFixture>(*m_provider, m_server, QString());
        }
        auto fixture = std::make_unique<MssqlFixture>(*m_provider, m_server,
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
    void connectionStringIsBuilt();
    void connectionStringEscapes();
    void encryptionModes();
    void driverSelection();
    void typeNameMapping();
    void nativeTypeNames();
    void nativeErrorKinds();
    void dialectSpecifics();
    void splitStatementsTransactSql();
    void missingDriverNamesTheFix();
    void unknownDriverIsNamedInTheError();
    void connectionRefusedIsConnectionError();
    void connectionRefusedThroughAnInstalledDriver();
    void invalidFormIsReportedWhenOpening();

    // Server needed
    void typesRoundTrip();
    void longValuesRoundTrip();
    void longTextParameterRoundTrip();
    void topAndOffsetFetch();
    void goScriptRuns();
    void multipleResultSets();
    void printGoesToNotices();
    void affectedRowsOfBatch();
    void transactions();
    void cancelWaitforIsFast();
    void cancelledStatementLeavesConnectionUsable();
    void authenticationFailure();
    void unknownDatabase();
    void treeHidesSystemDatabases();
    void encryptOptionalConnects();
    void emptyAndCommentOnlyStatements();
    void droppedStreamKeepsConnectionUsable();
    void secondStatementWhileStreamOpenIsRefused();

private:
    // A connection to a fresh database; null after a recorded failure.
    std::unique_ptr<IConnection> openFresh() { return openConnection(false); }

    ProviderRegistry m_registry;
    IDatabaseProvider *m_provider = nullptr;
    ServerUrl m_server;
    int m_counter = 0;
};

void TstMssqlProvider::initTestCase()
{
    m_registry.scan(QStringLiteral(QUARRY_PLUGIN_DIR));
    QString error;
    m_provider = m_registry.provider(QStringLiteral("mssql"), &error);
    QVERIFY2(m_provider, qPrintable(error));
    m_server = ServerUrl::fromEnvironment();
}

void TstMssqlProvider::cleanupTestCase() { }

// ---- no server -----------------------------------------------------------------------------

void TstMssqlProvider::pluginMetadata()
{
    const auto &entries = m_registry.available();
    const auto entry = std::find_if(entries.begin(), entries.end(), [](const ProviderEntry &e) {
        return e.id == QLatin1String("mssql");
    });
    QVERIFY(entry != entries.end());
    QCOMPARE(entry->displayName, QStringLiteral("SQL Server"));
    QCOMPARE(entry->apiVersion, kProviderApiVersion);
    QVERIFY(m_registry.rejected().empty());

    const ProviderInfo info = m_provider->info();
    QCOMPARE(info.id, QStringLiteral("mssql"));
    QVERIFY(info.caps.testFlag(Capability::Cancel));
    QVERIFY(info.caps.testFlag(Capability::Transactions));
    QVERIFY(info.caps.testFlag(Capability::MultipleResultSets));
    QVERIFY(info.caps.testFlag(Capability::Schemas));
    QVERIFY(info.caps.testFlag(Capability::MultipleDatabases));
    QVERIFY(!info.caps.testFlag(Capability::Explain));

    const auto fields = m_provider->connectionFields();
    QCOMPARE(fields.size(), size_t(8));
    QCOMPARE(fields[0].key, QStringLiteral("server"));
    QCOMPARE(fields[0].defaultValue.toString(), QStringLiteral("localhost"));
    QCOMPARE(fields[1].key, QStringLiteral("port"));
    QCOMPARE(fields[1].kind, ConnectionField::Kind::Number);
    QCOMPARE(fields[1].defaultValue.toInt(), 1433);
    QCOMPARE(fields[2].key, QStringLiteral("database"));
    QVERIFY(!fields[2].required);
    QCOMPARE(fields[3].key, QStringLiteral("user"));
    QCOMPARE(fields[4].key, QStringLiteral("authentication"));
    QCOMPARE(fields[4].kind, ConnectionField::Kind::Enum);
    QCOMPARE(fields[4].enumValues,
             (QStringList { QStringLiteral("sql"), QStringLiteral("windows") }));
    QCOMPARE(fields[4].defaultValue.toString(), QStringLiteral("sql"));
    QCOMPARE(fields[5].key, QStringLiteral("encrypt"));
    QCOMPARE(fields[5].enumValues,
             (QStringList { QStringLiteral("mandatory"), QStringLiteral("optional"),
                            QStringLiteral("strict") }));
    QCOMPARE(fields[5].defaultValue.toString(), QStringLiteral("mandatory"));
    QCOMPARE(fields[6].key, QStringLiteral("trustservercertificate"));
    QCOMPARE(fields[6].kind, ConnectionField::Kind::Bool);
    QCOMPARE(fields[6].defaultValue.toBool(), false);
    QVERIFY(fields[6].advanced);
    QCOMPARE(fields[7].key, QStringLiteral("driver"));
    QVERIFY(fields[7].advanced);
    QVERIFY(!fields[7].required);
    for (const auto &field : fields) {
        QVERIFY(field.kind != ConnectionField::Kind::Secret); // the password never is a field
        QVERIFY(!field.label.isEmpty());
    }
}

void TstMssqlProvider::connSettingsFromForm()
{
    // Everything optional but the user (SQL authentication).
    auto minimal = mssql::buildConnSettings(
        form({ { QStringLiteral("user"), QStringLiteral("sa") } }), QStringLiteral("pw"));
    QVERIFY(minimal);
    const mssql::ConnSettings &m = minimal.value();
    QCOMPARE(m.host, QStringLiteral("localhost"));
    QCOMPARE(m.instance, QString());
    QCOMPARE(m.port, 1433);
    QCOMPARE(m.database, QString());
    QCOMPARE(m.user, QStringLiteral("sa"));
    QCOMPARE(m.password, QStringLiteral("pw"));
    QVERIFY(!m.windowsAuthentication);
    QVERIFY(m.encrypt == mssql::EncryptMode::Mandatory);
    QVERIFY(!m.trustServerCertificate);
    QCOMPARE(m.driver, QString());

    const auto full
        = settingsFor(form({ { QStringLiteral("server"), QStringLiteral("  db.example.com ") },
                             { QStringLiteral("port"), 14330 },
                             { QStringLiteral("database"), QStringLiteral("sales") },
                             { QStringLiteral("user"), QStringLiteral("ann") },
                             { QStringLiteral("authentication"), QStringLiteral("SQL") },
                             { QStringLiteral("encrypt"), QStringLiteral("Optional") },
                             { QStringLiteral("trustservercertificate"), true },
                             { QStringLiteral("driver"), QStringLiteral(" My Driver ") } }));
    QCOMPARE(full.host, QStringLiteral("db.example.com"));
    QCOMPARE(full.port, 14330);
    QCOMPARE(full.database, QStringLiteral("sales"));
    QVERIFY(full.encrypt == mssql::EncryptMode::Optional);
    QVERIFY(full.trustServerCertificate);
    QCOMPARE(full.driver, QStringLiteral("My Driver"));

    // The port may come as text; a named instance ignores it later, when the string is built.
    const auto instance
        = settingsFor(form({ { QStringLiteral("server"), QStringLiteral("host\\SQLEXPRESS") },
                             { QStringLiteral("port"), QStringLiteral("1500") },
                             { QStringLiteral("user"), QStringLiteral("sa") } }));
    QCOMPARE(instance.host, QStringLiteral("host"));
    QCOMPARE(instance.instance, QStringLiteral("SQLEXPRESS"));
    QCOMPARE(instance.port, 1500);
    QCOMPARE(mssql::serverSpec(instance), QStringLiteral("host\\SQLEXPRESS"));
    QCOMPARE(mssql::serverSpec(m), QStringLiteral("localhost,1433"));
    QCOMPARE(mssql::serverSpec(full), QStringLiteral("db.example.com,14330"));

    // Windows authentication needs no user.
    auto windows = mssql::buildConnSettings(
        form({ { QStringLiteral("authentication"), QStringLiteral("windows") } }), {});
    QVERIFY(windows);
    QVERIFY(windows.value().windowsAuthentication);
    QVERIFY(windows.value().user.isEmpty());

    // "false" as text is false.
    QVERIFY(!settingsFor(
                 form({ { QStringLiteral("user"), QStringLiteral("sa") },
                        { QStringLiteral("trustservercertificate"), QStringLiteral("false") } }))
                 .trustServerCertificate);
}

void TstMssqlProvider::connSettingsRejectBadInput()
{
    auto rejects = [](const QVariantMap &values) {
        auto settings = mssql::buildConnSettings(values, {});
        if (settings)
            return false;
        return std::move(settings).error().kind == ErrorKind::Connection;
    };
    const QPair<QString, QVariant> user { QStringLiteral("user"), QStringLiteral("sa") };
    QVERIFY(rejects(form({}))); // SQL authentication without a user
    QVERIFY(rejects(form({ { QStringLiteral("user"), QStringLiteral("  ") } })));
    for (const QVariant &port :
         { QVariant(0), QVariant(65536), QVariant(-1), QVariant(QStringLiteral("abc")),
           QVariant(QStringLiteral("14.5")) })
        QVERIFY2(rejects(form({ user, { QStringLiteral("port"), port } })),
                 qPrintable(port.toString()));
    QVERIFY(rejects(form({ user, { QStringLiteral("server"), QStringLiteral("host\\") } })));
    QVERIFY(rejects(form({ user, { QStringLiteral("server"), QStringLiteral("\\instance") } })));
    QVERIFY(
        rejects(form({ user, { QStringLiteral("authentication"), QStringLiteral("kerberos") } })));
    QVERIFY(rejects(form({ user, { QStringLiteral("encrypt"), QStringLiteral("always") } })));
}

void TstMssqlProvider::connectionStringIsBuilt()
{
    const QString driver = QStringLiteral("ODBC Driver 18 for SQL Server");
    auto basic = mssql::buildConnectionString(
        settingsFor(form({ { QStringLiteral("user"), QStringLiteral("sa") } }),
                    QStringLiteral("pw")),
        driver);
    QVERIFY(basic);
    QCOMPARE(basic.value(),
             QStringLiteral("Driver={ODBC Driver 18 for SQL Server};Server=localhost,1433;"
                            "UID=sa;PWD={pw};Encrypt=yes;TrustServerCertificate=no;"
                            "Application Name=Quarry;LoginTimeout=15;"));

    auto database = mssql::buildConnectionString(
        settingsFor(form({ { QStringLiteral("server"), QStringLiteral("db") },
                           { QStringLiteral("port"), 1444 },
                           { QStringLiteral("database"), QStringLiteral("sales") },
                           { QStringLiteral("user"), QStringLiteral("ann") },
                           { QStringLiteral("encrypt"), QStringLiteral("optional") },
                           { QStringLiteral("trustservercertificate"), true } }),
                    QStringLiteral("x")),
        QStringLiteral("ODBC Driver 17 for SQL Server"));
    QVERIFY(database);
    QCOMPARE(database.value(),
             QStringLiteral("Driver={ODBC Driver 17 for SQL Server};Server=db,1444;"
                            "Database=sales;UID=ann;PWD={x};Encrypt=no;"
                            "TrustServerCertificate=yes;Application Name=Quarry;LoginTimeout=15;"));

    // A named instance replaces the port.
    auto instance = mssql::buildConnectionString(
        settingsFor(form({ { QStringLiteral("server"), QStringLiteral("host\\inst") },
                           { QStringLiteral("port"), 1444 },
                           { QStringLiteral("user"), QStringLiteral("sa") } })),
        driver);
    QVERIFY(instance);
    QVERIFY2(instance.value().contains(QLatin1String(";Server=host\\inst;")),
             qPrintable(instance.value()));
    QVERIFY(!instance.value().contains(QLatin1String("1444")));

    // Windows authentication: a trusted connection, no user and no password.
    auto windows = mssql::buildConnectionString(
        settingsFor(form({ { QStringLiteral("authentication"), QStringLiteral("windows") },
                           { QStringLiteral("user"), QStringLiteral("ignored") } }),
                    QStringLiteral("ignored")),
        driver);
    QVERIFY(windows);
    QVERIFY(windows.value().contains(QLatin1String("Trusted_Connection=yes;")));
    QVERIFY(!windows.value().contains(QLatin1String("UID=")));
    QVERIFY(!windows.value().contains(QLatin1String("PWD=")));
    QVERIFY(!windows.value().contains(QLatin1String("ignored")));
}

void TstMssqlProvider::connectionStringEscapes()
{
    const QString driver = QStringLiteral("ODBC Driver 18 for SQL Server");
    // The password always goes in braces, with every } doubled; ; and = inside are harmless.
    auto text = mssql::buildConnectionString(
        settingsFor(form({ { QStringLiteral("user"), QStringLiteral("a;b") },
                           { QStringLiteral("database"), QStringLiteral("x}y") },
                           { QStringLiteral("server"), QStringLiteral("h;ost") } }),
                    QStringLiteral("p}w;d={x}")),
        driver);
    QVERIFY(text);
    QVERIFY2(text.value().contains(QLatin1String("Server={h;ost,1433};")),
             qPrintable(text.value()));
    QVERIFY2(text.value().contains(QLatin1String("Database={x}}y};")), qPrintable(text.value()));
    QVERIFY2(text.value().contains(QLatin1String("UID={a;b};")), qPrintable(text.value()));
    QVERIFY2(text.value().contains(QLatin1String("PWD={p}}w;d={x}}};")), qPrintable(text.value()));

    // A password that tries to add keys stays one value.
    auto hostile = mssql::buildConnectionString(
        settingsFor(form({ { QStringLiteral("user"), QStringLiteral("sa") } }),
                    QStringLiteral("x};TrustServerCertificate=yes;PWD={y")),
        driver);
    QVERIFY(hostile);
    QVERIFY2(
        hostile.value().contains(QLatin1String("PWD={x}};TrustServerCertificate=yes;PWD={y};")),
        qPrintable(hostile.value()));
    QVERIFY(hostile.value().contains(QLatin1String("TrustServerCertificate=no;")));
}

void TstMssqlProvider::encryptionModes()
{
    auto build = [](const QString &mode, const QString &driver) {
        return mssql::buildConnectionString(
            settingsFor(form({ { QStringLiteral("user"), QStringLiteral("sa") },
                               { QStringLiteral("encrypt"), mode } })),
            driver);
    };
    const QString d18 = QStringLiteral("ODBC Driver 18 for SQL Server");
    const QString d17 = QStringLiteral("ODBC Driver 17 for SQL Server");
    QVERIFY(
        build(QStringLiteral("mandatory"), d18).value().contains(QLatin1String("Encrypt=yes;")));
    QVERIFY(build(QStringLiteral("optional"), d18).value().contains(QLatin1String("Encrypt=no;")));
    QVERIFY(
        build(QStringLiteral("strict"), d18).value().contains(QLatin1String("Encrypt=strict;")));
    QVERIFY(
        build(QStringLiteral("mandatory"), d17).value().contains(QLatin1String("Encrypt=yes;")));
    QVERIFY(build(QStringLiteral("strict"), QStringLiteral("ODBC Driver 19 for SQL Server")));

    // strict only with Driver 18 or newer.
    auto strict17 = build(QStringLiteral("strict"), d17);
    QVERIFY(!strict17);
    QCOMPARE(std::move(strict17).error().kind, ErrorKind::Unsupported);
    QVERIFY(!build(QStringLiteral("strict"), QStringLiteral("SQL Server Native Client 11.0")));
    QVERIFY(!build(QStringLiteral("strict"), QStringLiteral("My Driver")));
    QVERIFY(mssql::supportsStrictEncryption(d18));
    QVERIFY(!mssql::supportsStrictEncryption(d17));
    QVERIFY(!mssql::supportsStrictEncryption(QString()));
}

void TstMssqlProvider::driverSelection()
{
    const QString d17 = QStringLiteral("ODBC Driver 17 for SQL Server");
    const QString d18 = QStringLiteral("ODBC Driver 18 for SQL Server");
    const QStringList other { QStringLiteral("SQL Server"), QStringLiteral("PostgreSQL Unicode") };
    // 18 before 17, whatever the order of the driver manager.
    QCOMPARE(mssql::pickDriver({}, { d17, d18 }).value_or(QString()), d18);
    QCOMPARE(mssql::pickDriver({}, other + QStringList { d17 }).value_or(QString()), d17);
    QCOMPARE(mssql::pickDriver({}, { QStringLiteral("odbc driver 18 for sql server") })
                 .value_or(QString()),
             QStringLiteral("odbc driver 18 for sql server")); // as the driver manager spells it
    // None of them: the old "SQL Server" driver is not an option (no TLS 1.2, no Encrypt).
    QVERIFY(!mssql::pickDriver({}, other).has_value());
    QVERIFY(!mssql::pickDriver({}, {}).has_value());
    // A requested driver wins, installed or not.
    QCOMPARE(mssql::pickDriver(QStringLiteral(" My Driver "), { d18 }).value_or(QString()),
             QStringLiteral("My Driver"));
}

void TstMssqlProvider::typeNameMapping()
{
    using mssql::valueTypeForTypeName;
    for (const QString &name : { QStringLiteral("tinyint"), QStringLiteral("smallint"),
                                 QStringLiteral("int"), QStringLiteral("bigint") })
        QCOMPARE(valueTypeForTypeName(name), ValueType::Int64);
    QCOMPARE(valueTypeForTypeName(u"real"), ValueType::Double);
    QCOMPARE(valueTypeForTypeName(u"float"), ValueType::Double);
    for (const QString &name : { QStringLiteral("decimal"), QStringLiteral("numeric"),
                                 QStringLiteral("money"), QStringLiteral("smallmoney") })
        QCOMPARE(valueTypeForTypeName(name), ValueType::Decimal);
    QCOMPARE(valueTypeForTypeName(u"bit"), ValueType::Bool);
    for (const QString &name :
         { QStringLiteral("char"), QStringLiteral("varchar"), QStringLiteral("text"),
           QStringLiteral("nchar"), QStringLiteral("nvarchar"), QStringLiteral("ntext"),
           QStringLiteral("sysname"), QStringLiteral("xml") })
        QCOMPARE(valueTypeForTypeName(name), ValueType::Text);
    for (const QString &name :
         { QStringLiteral("binary"), QStringLiteral("varbinary"), QStringLiteral("image"),
           QStringLiteral("timestamp"), QStringLiteral("rowversion") })
        QCOMPARE(valueTypeForTypeName(name), ValueType::Bytes);
    QCOMPARE(valueTypeForTypeName(u"date"), ValueType::Date);
    QCOMPARE(valueTypeForTypeName(u"time"), ValueType::Time);
    for (const QString &name : { QStringLiteral("datetime"), QStringLiteral("datetime2"),
                                 QStringLiteral("smalldatetime") })
        QCOMPARE(valueTypeForTypeName(name), ValueType::DateTime);
    QCOMPARE(valueTypeForTypeName(u"datetimeoffset"), ValueType::DateTimeTz);
    QCOMPARE(valueTypeForTypeName(u"uniqueidentifier"), ValueType::Uuid);
    QCOMPARE(valueTypeForTypeName(u"json"), ValueType::Json);
    for (const QString &name :
         { QStringLiteral("sql_variant"), QStringLiteral("geography"), QStringLiteral("geometry"),
           QStringLiteral("hierarchyid"), QStringLiteral("my_clr_type"), QString() })
        QCOMPARE(valueTypeForTypeName(name), ValueType::Other);
    QCOMPARE(valueTypeForTypeName(u"NVARCHAR"), ValueType::Text); // case does not matter
}

void TstMssqlProvider::nativeTypeNames()
{
    using mssql::nativeTypeName;
    // max_length is in bytes: half of it for the national types, -1 for max.
    QCOMPARE(nativeTypeName(QStringLiteral("nvarchar"), 510, 0, 0),
             QStringLiteral("nvarchar(255)"));
    QCOMPARE(nativeTypeName(QStringLiteral("nchar"), 20, 0, 0), QStringLiteral("nchar(10)"));
    QCOMPARE(nativeTypeName(QStringLiteral("nvarchar"), -1, 0, 0), QStringLiteral("nvarchar(max)"));
    QCOMPARE(nativeTypeName(QStringLiteral("varchar"), 50, 0, 0), QStringLiteral("varchar(50)"));
    QCOMPARE(nativeTypeName(QStringLiteral("varchar"), -1, 0, 0), QStringLiteral("varchar(max)"));
    QCOMPARE(nativeTypeName(QStringLiteral("varbinary"), -1, 0, 0),
             QStringLiteral("varbinary(max)"));
    QCOMPARE(nativeTypeName(QStringLiteral("binary"), 16, 0, 0), QStringLiteral("binary(16)"));
    QCOMPARE(nativeTypeName(QStringLiteral("decimal"), 9, 10, 2), QStringLiteral("decimal(10,2)"));
    QCOMPARE(nativeTypeName(QStringLiteral("numeric"), 9, 18, 0), QStringLiteral("numeric(18,0)"));
    QCOMPARE(nativeTypeName(QStringLiteral("datetime2"), 8, 27, 7), QStringLiteral("datetime2(7)"));
    QCOMPARE(nativeTypeName(QStringLiteral("time"), 5, 16, 3), QStringLiteral("time(3)"));
    QCOMPARE(nativeTypeName(QStringLiteral("datetimeoffset"), 10, 34, 7),
             QStringLiteral("datetimeoffset(7)"));
    QCOMPARE(nativeTypeName(QStringLiteral("bigint"), 8, 19, 0), QStringLiteral("bigint"));
    QCOMPARE(nativeTypeName(QStringLiteral("uniqueidentifier"), 16, 0, 0),
             QStringLiteral("uniqueidentifier"));
    QCOMPARE(nativeTypeName(QStringLiteral("MyAlias"), 4, 10, 0), QStringLiteral("MyAlias"));
}

void TstMssqlProvider::nativeErrorKinds()
{
    using odbc::kindForDiagnostic;
    QCOMPARE(kindForDiagnostic(u"28000", 18456), ErrorKind::Authentication); // login failed
    QCOMPARE(kindForDiagnostic(u"42000", 4060), ErrorKind::NotFound); // cannot open the database
    QCOMPARE(kindForDiagnostic(u"42000", 102), ErrorKind::Syntax); // incorrect syntax near
    QCOMPARE(kindForDiagnostic(u"42S02", 208), ErrorKind::NotFound); // invalid object name
    QCOMPARE(kindForDiagnostic(u"42S22", 207), ErrorKind::NotFound); // invalid column name
    QCOMPARE(kindForDiagnostic(u"42000", 229), ErrorKind::Permission); // permission denied
    QCOMPARE(kindForDiagnostic(u"23000", 2627), ErrorKind::Constraint); // primary key
    QCOMPARE(kindForDiagnostic(u"23000", 2601), ErrorKind::Constraint); // unique index
    QCOMPARE(kindForDiagnostic(u"23000", 515), ErrorKind::Constraint); // NULL in NOT NULL
    QCOMPARE(kindForDiagnostic(u"23000", 547), ErrorKind::Constraint); // foreign key
    QCOMPARE(kindForDiagnostic(u"HY008", 0), ErrorKind::Cancelled);
    QCOMPARE(kindForDiagnostic(u"HYT00", 0), ErrorKind::Timeout);
    QCOMPARE(kindForDiagnostic(u"08001", 0), ErrorKind::Connection);
    QCOMPARE(kindForDiagnostic(u"22018", 245), ErrorKind::Driver); // conversion failed
}

void TstMssqlProvider::dialectSpecifics()
{
    const ISqlDialect &d = m_provider->dialect();
    QCOMPARE(d.quoteIdentifier(u"a]b"), QStringLiteral("[a]]b]"));
    QCOMPARE(d.quoteIdentifier(u"my table"), QStringLiteral("[my table]"));
    QCOMPARE(d.quoteIdentifier(u"a\"b'c"), QStringLiteral("[a\"b'c]"));
    QCOMPARE(d.quoteLiteral(u"it's"), QStringLiteral("N'it''s'"));
    QCOMPARE(d.quoteLiteral(u"日本"), QStringLiteral("N'日本'"));
    QCOMPARE(d.quoteLiteral(u"a\\b"), QStringLiteral("N'a\\b'"));
    QCOMPARE(d.placeholder(1), QStringLiteral("?"));
    QCOMPARE(d.placeholder(3), QStringLiteral("?"));
    QVERIFY(!d.rowIdentifier().has_value());
    QCOMPARE(d.highlightingDefinition(), QStringLiteral("SQL (Transact-SQL)"));
    QCOMPARE(d.explainPrefix(ExplainMode::Plan), QString());
    QCOMPARE(d.explainPrefix(ExplainMode::Analyze), QString());
    for (const QString &word : { QStringLiteral("TOP"), QStringLiteral("SELECT"),
                                 QStringLiteral("DECLARE"), QStringLiteral("WAITFOR") })
        QVERIFY2(d.keywords().contains(word), qPrintable(word));
    for (const QString &name :
         { QStringLiteral("getdate"), QStringLiteral("isnull"), QStringLiteral("newid"),
           QStringLiteral("len"), QStringLiteral("dateadd") })
        QVERIFY2(d.builtinFunctions().contains(name), qPrintable(name));
    for (const QString &name : d.builtinFunctions())
        QCOMPARE(name, name.toLower());

    QCOMPARE(d.limitClause(-1, 0), QString());
    QCOMPARE(d.limitClause(10, 0), QStringLiteral("OFFSET 0 ROWS FETCH NEXT 10 ROWS ONLY"));
    QCOMPARE(d.limitClause(10, 20), QStringLiteral("OFFSET 20 ROWS FETCH NEXT 10 ROWS ONLY"));
    QCOMPARE(d.limitClause(-1, 5), QStringLiteral("OFFSET 5 ROWS"));
    QCOMPARE(d.limitClause(0, 0), QStringLiteral("OFFSET 0 ROWS FETCH NEXT 0 ROWS ONLY"));

    QCOMPARE(d.qualifiedName({ { QStringLiteral("db"), QStringLiteral("dbo"), QStringLiteral("t") },
                               ObjectKind::Table }),
             QStringLiteral("[db].[dbo].[t]"));
    QCOMPARE(
        d.qualifiedName({ { QStringLiteral("d]b"), QStringLiteral("s"), QStringLiteral("t u") },
                          ObjectKind::View }),
        QStringLiteral("[d]]b].[s].[t u]"));
}

void TstMssqlProvider::splitStatementsTransactSql()
{
    const ISqlDialect &d = m_provider->dialect();
    struct Case
    {
        QString script;
        QStringList expected;
        const char *name;
    };
    const QList<Case> cases {
        { QStringLiteral("SELECT 1\nGO\nSELECT 2"),
          { QStringLiteral("SELECT 1"), QStringLiteral("SELECT 2") },
          "GO between statements" },
        { QStringLiteral("SELECT 1\r\nGO\r\nSELECT 2"),
          { QStringLiteral("SELECT 1"), QStringLiteral("SELECT 2") },
          "GO with CRLF" },
        { QStringLiteral("SELECT 1\n  go  \nSELECT 2\nGO 5\n"),
          { QStringLiteral("SELECT 1"), QStringLiteral("SELECT 2") },
          "GO in lower case, indented, with a count" },
        { QStringLiteral("SELECT 1\nGO -- done\nSELECT 2"),
          { QStringLiteral("SELECT 1"), QStringLiteral("SELECT 2") },
          "GO with a comment" },
        { QStringLiteral("GO\nSELECT 1\nGO"), { QStringLiteral("SELECT 1") }, "GO around" },
        { QStringLiteral("GO"), {}, "only GO" },
        { QStringLiteral("SELECT 1\nGOTO label\nSELECT 2"),
          { QStringLiteral("SELECT 1\nGOTO label\nSELECT 2") },
          "GOTO is not GO" },
        { QStringLiteral("SELECT 1 -- go\nGO\nSELECT 2"),
          { QStringLiteral("SELECT 1 -- go"), QStringLiteral("SELECT 2") },
          "GO in a comment" },
        { QStringLiteral("SELECT 'a\nGO\nb'; SELECT 2"),
          { QStringLiteral("SELECT 'a\nGO\nb'"), QStringLiteral("SELECT 2") },
          "GO inside a string" },
        { QStringLiteral("/* x\nGO\ny */ SELECT 1"),
          { QStringLiteral("/* x\nGO\ny */ SELECT 1") },
          "GO inside a comment" },
        { QStringLiteral("SELECT 1 GO\nSELECT 2"),
          { QStringLiteral("SELECT 1 GO\nSELECT 2") },
          "GO after other text on its line" },
        { QStringLiteral("BEGIN\n  SELECT 1;\n  SELECT 2;\nEND;\nSELECT 3"),
          { QStringLiteral("BEGIN\n  SELECT 1;\n  SELECT 2;\nEND"), QStringLiteral("SELECT 3") },
          "BEGIN END block" },
        { QStringLiteral("IF 1 = 1 BEGIN SELECT 1; END ELSE BEGIN SELECT 2; END; SELECT 3"),
          { QStringLiteral("IF 1 = 1 BEGIN SELECT 1; END ELSE BEGIN SELECT 2; END"),
            QStringLiteral("SELECT 3") },
          "IF ELSE blocks" },
        { QStringLiteral("BEGIN\n BEGIN SELECT 1; END;\n SELECT 2;\nEND; SELECT 3"),
          { QStringLiteral("BEGIN\n BEGIN SELECT 1; END;\n SELECT 2;\nEND"),
            QStringLiteral("SELECT 3") },
          "nested blocks" },
        { QStringLiteral("BEGIN TRANSACTION; SELECT 1; COMMIT"),
          { QStringLiteral("BEGIN TRANSACTION"), QStringLiteral("SELECT 1"),
            QStringLiteral("COMMIT") },
          "BEGIN TRANSACTION" },
        { QStringLiteral("begin tran;\nUPDATE t SET a = 1;\ncommit tran;"),
          { QStringLiteral("begin tran"), QStringLiteral("UPDATE t SET a = 1"),
            QStringLiteral("commit tran") },
          "BEGIN TRAN in lower case" },
        { QStringLiteral("BEGIN -- a block\n TRAN; SELECT 1"),
          { QStringLiteral("BEGIN -- a block\n TRAN"), QStringLiteral("SELECT 1") },
          "BEGIN, a comment, TRAN" },
        { QStringLiteral("BEGIN DISTRIBUTED TRANSACTION; SELECT 1"),
          { QStringLiteral("BEGIN DISTRIBUTED TRANSACTION"), QStringLiteral("SELECT 1") },
          "BEGIN DISTRIBUTED" },
        { QStringLiteral("BEGIN CONVERSATION TIMER (@h) TIMEOUT = 5; SELECT 1"),
          { QStringLiteral("BEGIN CONVERSATION TIMER (@h) TIMEOUT = 5"),
            QStringLiteral("SELECT 1") },
          "BEGIN CONVERSATION" },
        { QStringLiteral("BEGIN DIALOG @h FROM SERVICE a TO SERVICE 'b'; SELECT 1"),
          { QStringLiteral("BEGIN DIALOG @h FROM SERVICE a TO SERVICE 'b'"),
            QStringLiteral("SELECT 1") },
          "BEGIN DIALOG" },
        { QStringLiteral("BEGIN TRY SELECT 1; END TRY BEGIN CATCH SELECT 2; END CATCH; SELECT 3"),
          { QStringLiteral("BEGIN TRY SELECT 1; END TRY BEGIN CATCH SELECT 2; END CATCH"),
            QStringLiteral("SELECT 3") },
          "TRY CATCH" },
        { QStringLiteral("END CONVERSATION @h; SELECT 2"),
          { QStringLiteral("END CONVERSATION @h"), QStringLiteral("SELECT 2") },
          "END CONVERSATION" },
        { QStringLiteral("SELECT CASE WHEN 1 = 1 THEN 'a;b' ELSE 'c' END AS x; SELECT 2"),
          { QStringLiteral("SELECT CASE WHEN 1 = 1 THEN 'a;b' ELSE 'c' END AS x"),
            QStringLiteral("SELECT 2") },
          "CASE expression" },
        { QStringLiteral(
              "IF 1 = 1 BEGIN SELECT CASE WHEN 1 = 1 THEN 1 END; SELECT 2; END; SELECT 3"),
          { QStringLiteral("IF 1 = 1 BEGIN SELECT CASE WHEN 1 = 1 THEN 1 END; SELECT 2; END"),
            QStringLiteral("SELECT 3") },
          "CASE inside a block" },
        { QStringLiteral("BEGIN\nSELECT 1\nEND\nGO\nSELECT 2;"),
          { QStringLiteral("BEGIN\nSELECT 1\nEND"), QStringLiteral("SELECT 2") },
          "GO after a block" },
        { QStringLiteral("BEGIN SELECT 1; SELECT 2"),
          { QStringLiteral("BEGIN SELECT 1; SELECT 2") },
          "unterminated block" },
        { QStringLiteral("SELECT @end, @begin, #case; SELECT 2"),
          { QStringLiteral("SELECT @end, @begin, #case"), QStringLiteral("SELECT 2") },
          "variables named like keywords" },
        { QStringLiteral("SELECT 1 AS [begin], [case], t.[end]; SELECT 2"),
          { QStringLiteral("SELECT 1 AS [begin], [case], t.[end]"), QStringLiteral("SELECT 2") },
          "bracketed keywords" },
        { QStringLiteral("SELECT t.begin, t.case FROM t; SELECT 2"),
          { QStringLiteral("SELECT t.begin, t.case FROM t"), QStringLiteral("SELECT 2") },
          "keywords after a dot" },
        { QStringLiteral("SELECT [a;b], [c]]d;e] FROM [t]; SELECT 2"),
          { QStringLiteral("SELECT [a;b], [c]]d;e] FROM [t]"), QStringLiteral("SELECT 2") },
          "bracketed identifiers" },
        { QStringLiteral("SELECT N'it''s;'; SELECT \"a;b\", \"c\"\";d\""),
          { QStringLiteral("SELECT N'it''s;'"), QStringLiteral("SELECT \"a;b\", \"c\"\";d\"") },
          "strings" },
        { QStringLiteral("/* a /* nested ; */ still ; */ SELECT 1; SELECT 2"),
          { QStringLiteral("/* a /* nested ; */ still ; */ SELECT 1"), QStringLiteral("SELECT 2") },
          "nested comments" },
        { QStringLiteral("SELECT 1 -- a; b\n; SELECT 2 /* c; d */;"),
          { QStringLiteral("SELECT 1 -- a; b"), QStringLiteral("SELECT 2 /* c; d */") },
          "comments with semicolons" },
        { QStringLiteral("SELECT 'unterminated; SELECT 2"),
          { QStringLiteral("SELECT 'unterminated; SELECT 2") },
          "unterminated string" },
        { QStringLiteral("-- only a comment\nGO\n/* and another */"), {}, "comments only" },
        { QStringLiteral("SELECT 1;;; ; SELECT 2;"),
          { QStringLiteral("SELECT 1"), QStringLiteral("SELECT 2") },
          "empty statements" },
    };
    for (const Case &c : cases) {
        QStringList actual;
        qsizetype previousEnd = 0;
        for (const StatementRange &r : d.splitStatements(c.script)) {
            QVERIFY2(r.begin >= previousEnd && r.begin < r.end && r.end <= c.script.size(), c.name);
            previousEnd = r.end;
            actual << c.script.mid(r.begin, r.end - r.begin);
        }
        QVERIFY2(actual == c.expected,
                 qPrintable(QStringLiteral("%1\nscript: %2\nactual: %3")
                                .arg(QLatin1String(c.name), c.script,
                                     actual.join(QLatin1String(" || ")))));
    }
}

void TstMssqlProvider::missingDriverNamesTheFix()
{
    const QStringList installed = odbc::installedDrivers();
    const bool hasMicrosoft = std::any_of(
        installed.begin(), installed.end(), [](const QString &name) {
            return name.compare(QLatin1String("ODBC Driver 18 for SQL Server"), Qt::CaseInsensitive)
                == 0
                || name.compare(QLatin1String("ODBC Driver 17 for SQL Server"), Qt::CaseInsensitive)
                == 0;
        });
    if (hasMicrosoft)
        QSKIP("a Microsoft ODBC driver for SQL Server is installed");
    ConnectionConfig config;
    config.values.insert(QStringLiteral("user"), QStringLiteral("sa"));
    auto connection = m_provider->createConnection(config, Secrets { QStringLiteral("x") });
    auto opened = connection->open({});
    QVERIFY(!opened);
    const Error error = std::move(opened).error();
    QCOMPARE(error.kind, ErrorKind::Connection);
    QCOMPARE(error.message, QStringLiteral("Install the Microsoft ODBC Driver 18 for SQL Server."));
    QVERIFY(!error.detail.isEmpty());
    QVERIFY(!connection->isOpen());
}

void TstMssqlProvider::unknownDriverIsNamedInTheError()
{
    ConnectionConfig config;
    config.values.insert(QStringLiteral("user"), QStringLiteral("sa"));
    config.values.insert(QStringLiteral("driver"), QStringLiteral("Quarry No Such Driver"));
    auto connection = m_provider->createConnection(config, Secrets { QStringLiteral("x") });
    auto opened = connection->open({});
    QVERIFY(!opened);
    const Error error = std::move(opened).error();
    QCOMPARE(error.kind, ErrorKind::Connection);
    QVERIFY2(error.message.contains(QLatin1String("Quarry No Such Driver")),
             qPrintable(error.message + QLatin1Char('|') + error.detail));
    QVERIFY(!error.detail.contains(QLatin1String("PWD"))); // no secrets in what is reported
}

void TstMssqlProvider::connectionRefusedIsConnectionError()
{
    ConnectionConfig config;
    config.values.insert(QStringLiteral("server"), QStringLiteral("127.0.0.1"));
    config.values.insert(QStringLiteral("port"), 1); // nothing listens there
    config.values.insert(QStringLiteral("user"), QStringLiteral("sa"));
    config.values.insert(QStringLiteral("trustservercertificate"), true);
    auto connection = m_provider->createConnection(config, Secrets { QStringLiteral("x") });
    auto opened = connection->open({});
    QVERIFY(!opened);
    const Error error = std::move(opened).error();
    // Connection when the port refuses (or no driver is installed), Timeout if it is filtered.
    QVERIFY2(error.kind == ErrorKind::Connection || error.kind == ErrorKind::Timeout,
             qPrintable(error.message + QStringLiteral(" | ") + error.detail));
    QVERIFY(!error.message.isEmpty());
    QVERIFY(!connection->isOpen());
}

void TstMssqlProvider::connectionRefusedThroughAnInstalledDriver()
{
    // The driver that ships with Windows ("SQL Server") is no good for the provider's own
    // selection, but it takes the connection string through the real driver manager.
    if (!odbc::installedDrivers().contains(QStringLiteral("SQL Server")))
        QSKIP("the SQL Server ODBC driver of Windows is not installed");
    ConnectionConfig config;
    config.values.insert(QStringLiteral("server"), QStringLiteral("quarry-no-such-host.invalid"));
    config.values.insert(QStringLiteral("user"), QStringLiteral("sa"));
    config.values.insert(QStringLiteral("driver"), QStringLiteral("SQL Server"));
    config.values.insert(QStringLiteral("encrypt"), QStringLiteral("optional"));
    auto connection = m_provider->createConnection(config, Secrets { QStringLiteral("x") });
    QElapsedTimer clock;
    clock.start();
    auto opened = connection->open({});
    QVERIFY(!opened);
    const Error error = std::move(opened).error();
    QVERIFY2(error.kind == ErrorKind::Connection || error.kind == ErrorKind::Timeout,
             qPrintable(error.message + QStringLiteral(" | ") + error.detail));
    QVERIFY2(error.detail.contains(QLatin1String("SQLSTATE")), qPrintable(error.detail));
    QVERIFY(!error.sqlState.isEmpty());
    QVERIFY(clock.elapsed() < 30000); // the login timeout is 15 s
    QVERIFY(!connection->isOpen());
}

void TstMssqlProvider::invalidFormIsReportedWhenOpening()
{
    ConnectionConfig config; // SQL authentication without a user
    auto connection = m_provider->createConnection(config, {});
    auto opened = connection->open({});
    QVERIFY(!opened);
    QCOMPARE(std::move(opened).error().kind, ErrorKind::Connection);
    auto stream = connection->execute(Statement { QStringLiteral("SELECT 1"), {}, 500 }, {});
    QVERIFY(!stream);
    QCOMPARE(std::move(stream).error().kind, ErrorKind::Connection);
    QVERIFY(!connection->children(ObjectPath { {}, ObjectKind::Database }, {}));
}

// ---- server needed -------------------------------------------------------------------------

void TstMssqlProvider::typesRoundTrip()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    const auto d = drain(
        *connection,
        QStringLiteral("SELECT CAST(1 AS BIT) AS b, CAST(12.50 AS DECIMAL(10,2)) AS dec, "
                       "CAST('2026-11-17' AS DATE) AS d, CAST('12:34:56.1234567' AS TIME(7)) AS t, "
                       "CAST('2026-11-17T12:34:56.1234567' AS DATETIME2(7)) AS dt2, "
                       "CAST('2026-11-17T12:34:56.1234567+01:00' AS DATETIMEOFFSET(7)) AS dto, "
                       "CAST('6F9619FF-8B86-D011-B42D-00C04FC964FF' AS UNIQUEIDENTIFIER) AS g, "
                       "CAST(N'x' AS NVARCHAR(MAX)) AS nm, CAST(0x0102 AS VARBINARY(MAX)) AS vb, "
                       "CAST(7 AS TINYINT) AS ti, CAST(1.5 AS REAL) AS r, CAST(9007199254740993 AS "
                       "BIGINT) AS bi, "
                       "CAST(NULL AS NVARCHAR(10)) AS nul"));
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    QCOMPARE(d.rows, 1);
    QCOMPARE(d.columns.size(), size_t(13));
    const ValueType expected[]
        = { ValueType::Bool,     ValueType::Decimal,    ValueType::Date,   ValueType::Time,
            ValueType::DateTime, ValueType::DateTimeTz, ValueType::Uuid,   ValueType::Text,
            ValueType::Bytes,    ValueType::Int64,      ValueType::Double, ValueType::Int64,
            ValueType::Text };
    for (size_t i = 0; i < d.columns.size(); ++i) {
        QCOMPARE(d.columns[i].type, expected[i]);
        QVERIFY2(!d.columns[i].nativeType.isEmpty(), qPrintable(d.columns[i].name));
    }
    const ResultBatch &b = d.batches.front();
    QCOMPARE(b.columns[0].int64At(0), 1);
    QCOMPARE(textAt(b, 1), QStringLiteral("12.50"));
    QCOMPARE(textAt(b, 2), QStringLiteral("2026-11-17"));
    QVERIFY2(textAt(b, 3).startsWith(QLatin1String("12:34:56.1234567")), qPrintable(textAt(b, 3)));
    QVERIFY2(textAt(b, 4).startsWith(QLatin1String("2026-11-17 12:34:56.1234567")),
             qPrintable(textAt(b, 4)));
    QVERIFY2(textAt(b, 5).contains(QLatin1String("12:34:56.1234567"))
                 && textAt(b, 5).contains(QLatin1String("+01:00")),
             qPrintable(textAt(b, 5)));
    QCOMPARE(textAt(b, 6).toUpper(), QStringLiteral("6F9619FF-8B86-D011-B42D-00C04FC964FF"));
    QCOMPARE(textAt(b, 7), QStringLiteral("x"));
    QCOMPARE(QByteArray(b.columns[8].bytesAt(0)), QByteArray("\x01\x02", 2));
    QCOMPARE(b.columns[9].int64At(0), 7);
    QCOMPARE(b.columns[10].doubleAt(0), 1.5);
    QCOMPARE(b.columns[11].int64At(0), 9'007'199'254'740'993LL);
    QVERIFY(b.columns[12].isNull(0));
}

void TstMssqlProvider::longValuesRoundTrip()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    // NVARCHAR(MAX) and VARBINARY(MAX) report no size: they are read in pieces with SQLGetData.
    // 300 000 characters (with a surrogate pair and an accent) and 2 MiB of bytes cross many
    // pieces of the reader.
    const auto d = drain(
        *connection,
        QStringLiteral("SELECT REPLICATE(CAST(N'é\U0001F600x' AS NVARCHAR(MAX)), 100000) AS t, "
                       "REPLICATE(CAST(0x41 AS VARBINARY(MAX)), 2097152) AS b, "
                       "REPLICATE(CAST('abc' AS VARCHAR(MAX)), 50000) AS v, "
                       "CAST(N'' AS NVARCHAR(MAX)) AS empty, CAST(NULL AS VARBINARY(MAX)) AS nul, "
                       "CAST('<a>1</a>' AS XML) AS x"));
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    QCOMPARE(d.rows, 1);
    QCOMPARE(d.columns[0].type, ValueType::Text);
    QCOMPARE(d.columns[1].type, ValueType::Bytes);
    QCOMPARE(d.columns[5].type, ValueType::Text);
    const ResultBatch &b = d.batches.front();
    const QString text = textAt(b, 0);
    QCOMPARE(text.size(), qsizetype(100000 * 4)); // 1 + 2 + 1 UTF-16 units each
    QVERIFY(text.startsWith(QStringLiteral("é\U0001F600x")));
    QVERIFY(text.endsWith(QStringLiteral("é\U0001F600x")));
    QCOMPARE(b.columns[1].bytesAt(0).size(), qsizetype(2097152));
    QCOMPARE(b.columns[1].bytesAt(0).front(), 'A');
    QCOMPARE(b.columns[1].bytesAt(0).back(), 'A');
    QCOMPARE(textAt(b, 2).size(), qsizetype(150000));
    QVERIFY(textAt(b, 3).isEmpty());
    QVERIFY(!b.columns[3].isNull(0)); // empty is not NULL
    QVERIFY(b.columns[4].isNull(0));
    QVERIFY(textAt(b, 5).contains(QLatin1String("<a>1</a>")));

    // Several rows of long values and a bounded column after them: the columns after the first
    // unbounded one are read with SQLGetData too, in order.
    const auto rows
        = drain(*connection,
                QStringLiteral("SELECT n, REPLICATE(CAST(N'z' AS NVARCHAR(MAX)), n * 1000) "
                               "AS big, CAST(n AS INT) AS after "
                               "FROM (VALUES (1), (2), (3)) AS v(n) ORDER BY n"));
    QVERIFY2(rows.ok, qPrintable(rows.errorText()));
    QCOMPARE(rows.rows, 3);
    const ResultBatch &r = rows.batches.front();
    for (qsizetype i = 0; i < 3; ++i) {
        QCOMPARE(r.columns[0].int64At(i), i + 1);
        QCOMPARE(textAt(r, 1, i).size(), qsizetype((i + 1) * 1000));
        QCOMPARE(r.columns[2].int64At(i), i + 1);
    }
}

void TstMssqlProvider::longTextParameterRoundTrip()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    QVERIFY(drain(*connection,
                  QStringLiteral("CREATE TABLE big(id INT PRIMARY KEY, t NVARCHAR(MAX), "
                                 "b VARBINARY(MAX))"))
                .ok);
    const QString text = QString(9000, QLatin1Char('q')) + QStringLiteral("é\U0001F600");
    const QByteArray bytes(20000, '\x7f');
    const auto inserted
        = drain(*connection, QStringLiteral("INSERT INTO big(id, t, b) VALUES (?, ?, ?)"),
                { 1, text, QVariant(bytes) });
    QVERIFY2(inserted.ok, qPrintable(inserted.errorText()));
    QCOMPARE(inserted.affected.value_or(-1), 1);
    const auto d = drain(*connection, QStringLiteral("SELECT t, b FROM big WHERE id = ?"), { 1 });
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    QCOMPARE(textAt(d.batches.front(), 0), text);
    QCOMPARE(QByteArray(d.batches.front().columns[1].bytesAt(0)), bytes);

    // An empty string and an empty byte array are values, not NULL.
    QVERIFY(drain(*connection, QStringLiteral("INSERT INTO big(id, t, b) VALUES (?, ?, ?)"),
                  { 2, QString(), QVariant(QByteArray()) })
                .ok);
    const auto e = drain(*connection, QStringLiteral("SELECT t, b FROM big WHERE id = 2"));
    QVERIFY2(e.ok, qPrintable(e.errorText()));
    QVERIFY(!e.batches.front().columns[0].isNull(0));
    QVERIFY(!e.batches.front().columns[1].isNull(0));
    QVERIFY(textAt(e.batches.front(), 0).isEmpty());

    // NULL for a binary column goes through the type the server reports for the parameter.
    QVERIFY(drain(*connection, QStringLiteral("INSERT INTO big(id, t, b) VALUES (?, ?, ?)"),
                  { 3, QVariant(), QVariant() })
                .ok);
    const auto f = drain(*connection, QStringLiteral("SELECT t, b FROM big WHERE id = 3"));
    QVERIFY2(f.ok, qPrintable(f.errorText()));
    QVERIFY(f.batches.front().columns[0].isNull(0));
    QVERIFY(f.batches.front().columns[1].isNull(0));

    // Other parameter types.
    const auto types
        = drain(*connection,
                QStringLiteral("SELECT CAST(? AS BIT), CAST(? AS FLOAT), "
                               "CAST(? AS UNIQUEIDENTIFIER), CAST(? AS BIGINT)"),
                { true, 2.5, QUuid(QStringLiteral("6f9619ff-8b86-d011-b42d-00c04fc964ff")),
                  QVariant::fromValue<qint64>(9'007'199'254'740'993LL) });
    QVERIFY2(types.ok, qPrintable(types.errorText()));
    const ResultBatch &tb = types.batches.front();
    QCOMPARE(tb.columns[0].int64At(0), 1);
    QCOMPARE(tb.columns[1].doubleAt(0), 2.5);
    QCOMPARE(textAt(tb, 2).toLower(), QStringLiteral("6f9619ff-8b86-d011-b42d-00c04fc964ff"));
    QCOMPARE(tb.columns[3].int64At(0), 9'007'199'254'740'993LL);
}

void TstMssqlProvider::topAndOffsetFetch()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    const auto top = drain(*connection,
                           QStringLiteral("SELECT TOP 3 n FROM (VALUES (1), (2), (3), "
                                          "(4), (5)) AS v(n) ORDER BY n"));
    QVERIFY2(top.ok, qPrintable(top.errorText()));
    QCOMPARE(top.rows, 3);

    // The clause of the dialect runs as written, after an ORDER BY.
    const QString clause = m_provider->dialect().limitClause(2, 1);
    const auto page = drain(*connection,
                            QStringLiteral("SELECT n FROM (VALUES (1), (2), (3), (4), (5)) AS v(n) "
                                           "ORDER BY n %1")
                                .arg(clause));
    QVERIFY2(page.ok, qPrintable(page.errorText()));
    QCOMPARE(page.rows, 2);
    QCOMPARE(page.batches.front().columns[0].int64At(0), 2);
    QCOMPARE(page.batches.front().columns[0].int64At(1), 3);

    // Without ORDER BY SQL Server refuses: the error is a syntax error, with the driver's text.
    const auto bad = drain(
        *connection, QStringLiteral("SELECT n FROM (VALUES (1), (2)) AS v(n) %1").arg(clause));
    QVERIFY(!bad.ok);
    QCOMPARE(bad.error.kind, ErrorKind::Syntax);
    QVERIFY(bad.error.detail.contains(QLatin1String("42000")));
    QCOMPARE(bad.error.sqlState, QStringLiteral("42000"));
    QVERIFY(!bad.error.position.has_value());
}

void TstMssqlProvider::goScriptRuns()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    const QString script
        = QStringLiteral("CREATE TABLE s(a INT);\nGO\nINSERT INTO s VALUES (1), (2)\nGO 2\n"
                         "CREATE VIEW v_s AS SELECT a FROM s\nGO\nSELECT COUNT(*) AS c FROM v_s");
    const auto ranges = m_provider->dialect().splitStatements(script);
    QCOMPARE(ranges.size(), size_t(4));
    qint64 count = -1;
    for (const StatementRange &range : ranges) {
        const auto d = drain(*connection, script.mid(range.begin, range.end - range.begin));
        QVERIFY2(d.ok, qPrintable(d.errorText()));
        if (!d.batches.empty() && d.batches.front().rowCount > 0)
            count = d.batches.front().columns[0].int64At(0);
    }
    QCOMPARE(count, 2);
}

void TstMssqlProvider::multipleResultSets()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    auto stream = connection->execute(
        Statement { QStringLiteral("SELECT 1 AS a; SELECT N'x' AS b, 2 AS c"), {}, 500 }, {});
    QVERIFY2(stream, qPrintable(stream ? QString() : stream.error().detail));
    IResultStream &s = *stream.value();
    QCOMPARE(s.columns().size(), size_t(1));
    QCOMPARE(s.columns()[0].name, QStringLiteral("a"));
    auto first = s.fetch(500, {});
    QVERIFY(first);
    QCOMPARE(first.value().rowCount, 1);
    QCOMPARE(first.value().columns[0].int64At(0), 1);
    QVERIFY(s.atEnd()); // the first result is over; the second one is waiting

    auto more = s.nextResult({});
    QVERIFY2(more, qPrintable(more ? QString() : more.error().detail));
    QVERIFY(more.value());
    QVERIFY(!s.atEnd());
    QCOMPARE(s.columns().size(), size_t(2));
    QCOMPARE(s.columns()[0].name, QStringLiteral("b"));
    QCOMPARE(s.columns()[0].type, ValueType::Text);
    QCOMPARE(s.columns()[1].type, ValueType::Int64);
    auto second = s.fetch(500, {});
    QVERIFY(second);
    QCOMPARE(second.value().rowCount, 1);
    QCOMPARE(textAt(second.value(), 0), QStringLiteral("x"));
    QCOMPARE(second.value().columns[1].int64At(0), 2);
    QVERIFY(s.atEnd());
    auto none = s.nextResult({});
    QVERIFY(none);
    QVERIFY(!none.value());

    // The connection is free again.
    QVERIFY(drain(*connection, QStringLiteral("SELECT 1")).ok);

    // A statement that only counts rows, then a result: the counts are stepped over.
    auto mixed = connection->execute(
        Statement { QStringLiteral("CREATE TABLE m(a INT); INSERT INTO m VALUES (1), (2); "
                                   "SELECT a FROM m ORDER BY a"),
                    {},
                    500 },
        {});
    QVERIFY2(mixed, qPrintable(mixed ? QString() : mixed.error().detail));
    QCOMPARE(mixed.value()->columns().size(), size_t(1));
    auto rows = mixed.value()->fetch(500, {});
    QVERIFY(rows);
    QCOMPARE(rows.value().rowCount, 2);
}

void TstMssqlProvider::printGoesToNotices()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    auto stream = connection->execute(
        Statement { QStringLiteral("PRINT 'hello from quarry'; SELECT 1 AS a; "
                                   "RAISERROR('a warning', 10, 1)"),
                    {},
                    500 },
        {});
    QVERIFY2(stream, qPrintable(stream ? QString() : stream.error().detail));
    while (!stream.value()->atEnd()) {
        auto batch = stream.value()->fetch(500, {});
        QVERIFY(batch);
    }
    while (true) {
        auto more = stream.value()->nextResult({});
        QVERIFY(more);
        if (!more.value())
            break;
    }
    const QString notices = stream.value()->notices().join(QLatin1Char('\n'));
    QVERIFY2(notices.contains(QLatin1String("hello from quarry")), qPrintable(notices));
    QVERIFY2(notices.contains(QLatin1String("a warning")), qPrintable(notices));
    // The text of the driver is not in front of the message.
    QVERIFY2(!notices.contains(QLatin1String("[Microsoft]")), qPrintable(notices));
}

void TstMssqlProvider::affectedRowsOfBatch()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    QVERIFY(drain(*connection, QStringLiteral("CREATE TABLE a(n INT)")).ok);
    const auto insert = drain(*connection, QStringLiteral("INSERT INTO a VALUES (1), (2), (3)"));
    QVERIFY2(insert.ok, qPrintable(insert.errorText()));
    QCOMPARE(insert.affected.value_or(-1), 3);
    QVERIFY(insert.columns.empty());
    // No row matches: SQLExecDirect answers SQL_NO_DATA, which is a result of 0 rows.
    const auto none = drain(*connection, QStringLiteral("UPDATE a SET n = 9 WHERE n = 99"));
    QVERIFY2(none.ok, qPrintable(none.errorText()));
    QCOMPARE(none.affected.value_or(-1), 0);
    const auto del = drain(*connection, QStringLiteral("DELETE FROM a WHERE n < 3"));
    QCOMPARE(del.affected.value_or(-1), 2);
    // DDL has no count.
    const auto ddl = drain(*connection, QStringLiteral("CREATE TABLE b(n INT)"));
    QVERIFY2(ddl.ok, qPrintable(ddl.errorText()));
    QVERIFY(!ddl.affected.has_value());
    // OUTPUT gives rows, not a count.
    const auto output
        = drain(*connection, QStringLiteral("INSERT INTO a OUTPUT INSERTED.n VALUES (7)"));
    QVERIFY2(output.ok, qPrintable(output.errorText()));
    QCOMPARE(output.rows, 1);
}

void TstMssqlProvider::transactions()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    QVERIFY(drain(*connection, QStringLiteral("CREATE TABLE tx(a INT)")).ok);
    auto count = [&] {
        const auto d = drain(*connection, QStringLiteral("SELECT COUNT(*) FROM tx"));
        return d.ok ? d.batches.front().columns[0].int64At(0) : qint64(-1);
    };
    QVERIFY(connection->begin());
    QVERIFY(drain(*connection, QStringLiteral("INSERT INTO tx VALUES (1)")).ok);
    QCOMPARE(count(), 1);
    QVERIFY(connection->rollback());
    QCOMPARE(count(), 0);

    QVERIFY(connection->begin());
    QVERIFY(drain(*connection, QStringLiteral("INSERT INTO tx VALUES (2)")).ok);
    QVERIFY(connection->commit());
    QCOMPARE(count(), 1);

    // Autocommit is back: a statement outside a transaction is permanent, and a rollback that
    // follows has nothing to undo.
    QVERIFY(drain(*connection, QStringLiteral("INSERT INTO tx VALUES (3)")).ok);
    QCOMPARE(count(), 2);

    // A failed statement inside a transaction leaves the connection in it until the rollback.
    QVERIFY(connection->begin());
    QVERIFY(drain(*connection, QStringLiteral("INSERT INTO tx VALUES (4)")).ok);
    QVERIFY(!drain(*connection, QStringLiteral("INSERT INTO no_such_table VALUES (1)")).ok);
    QVERIFY(connection->rollback());
    QCOMPARE(count(), 2);

    // A connection closed in the middle of a transaction rolls it back.
    QVERIFY(connection->begin());
    QVERIFY(drain(*connection, QStringLiteral("INSERT INTO tx VALUES (5)")).ok);
    connection->close();
    QVERIFY(!connection->isOpen());
    QVERIFY(connection->open({}));
    QCOMPARE(count(), 2);
}

void TstMssqlProvider::cancelWaitforIsFast()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    QList<qint64> timings;
    for (int round = 0; round < 3; ++round) {
        std::atomic<bool> started { false };
        auto worker = std::async(std::launch::async, [&]() -> std::optional<ErrorKind> {
            started = true;
            auto stream = connection->execute(
                Statement { QStringLiteral("WAITFOR DELAY '00:00:30'"), {}, 500 }, {});
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
        QVERIFY2(callUs < 50'000,
                 qPrintable(QStringLiteral("requestCancel() took %1 us").arg(callUs)));
        QVERIFY2(worker.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                 "WAITFOR did not stop");
        timings << clock.elapsed();
        QCOMPARE(worker.get().value_or(ErrorKind::Internal), ErrorKind::Cancelled);
        QVERIFY2(timings.last() < 1000,
                 qPrintable(QStringLiteral("cancel took %1 ms").arg(timings.last())));

        const auto next = drain(*connection, QStringLiteral("SELECT %1").arg(round + 1));
        QVERIFY2(next.ok, qPrintable(next.errorText()));
        QCOMPARE(next.batches.front().columns[0].int64At(0), round + 1);
    }
    qInfo() << "cancel of WAITFOR DELAY, cancel-to-return ms per round:" << timings;
}

void TstMssqlProvider::cancelledStatementLeavesConnectionUsable()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    // Cancel while rows are being produced: a long result that the client is reading.
    std::atomic<bool> started { false };
    auto worker = std::async(std::launch::async, [&]() -> std::optional<ErrorKind> {
        auto stream = connection->execute(
            Statement {
                QStringLiteral("SELECT a.object_id FROM sys.all_objects a, sys.all_objects b, "
                               "sys.all_objects c"),
                {},
                100 },
            {});
        if (!stream)
            return stream.error().kind;
        started = true;
        for (int i = 0; i < 100000; ++i) {
            auto batch = stream.value()->fetch(100, {});
            if (!batch)
                return batch.error().kind;
            if (stream.value()->atEnd())
                break;
        }
        return std::nullopt;
    });
    QTRY_VERIFY_WITH_TIMEOUT(started.load(), 10000);
    QThread::msleep(100);
    connection->requestCancel();
    QVERIFY2(worker.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
             "the read did not stop");
    QCOMPARE(worker.get().value_or(ErrorKind::Internal), ErrorKind::Cancelled);
    const auto next = drain(*connection, QStringLiteral("SELECT 42"));
    QVERIFY2(next.ok, qPrintable(next.errorText()));
    QCOMPARE(next.batches.front().columns[0].int64At(0), 42);
}

void TstMssqlProvider::authenticationFailure()
{
    REQUIRE_SERVER();
    auto connection = m_provider->createConnection(m_server.config(m_server.database),
                                                   Secrets { QStringLiteral("not the password") });
    auto opened = connection->open({});
    QVERIFY(!opened);
    const Error error = std::move(opened).error();
    QCOMPARE(error.kind, ErrorKind::Authentication);
    QVERIFY(!error.message.isEmpty());
    QVERIFY(error.detail.contains(QLatin1String("18456")));
    QVERIFY(!error.detail.contains(QLatin1String("not the password")));
    QVERIFY(!connection->isOpen());
}

void TstMssqlProvider::unknownDatabase()
{
    REQUIRE_SERVER();
    auto connection = m_provider->createConnection(
        m_server.config(QStringLiteral("quarry_no_such_db")), Secrets { m_server.password });
    auto opened = connection->open({});
    QVERIFY(!opened);
    QCOMPARE(std::move(opened).error().kind, ErrorKind::NotFound);
}

void TstMssqlProvider::treeHidesSystemDatabases()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    auto roots = connection->children(ObjectPath { {}, ObjectKind::Database }, {});
    QVERIFY2(roots, qPrintable(roots ? QString() : roots.error().detail));
    QStringList names;
    for (const SchemaNode &node : roots.value()) {
        QCOMPARE(node.path.kind, ObjectKind::Database);
        QVERIFY(node.hasChildren);
        names << node.name;
    }
    for (const QString &system : { QStringLiteral("master"), QStringLiteral("tempdb"),
                                   QStringLiteral("model"), QStringLiteral("msdb") })
        QVERIFY2(!names.contains(system), qPrintable(system));
    const QString mine = fixture().booksPath().parts.front();
    QVERIFY2(names.contains(mine), qPrintable(names.join(QLatin1String(", "))));

    // Schemas: dbo yes, the system ones no.
    auto schemas = connection->children(ObjectPath { { mine }, ObjectKind::Database }, {});
    QVERIFY2(schemas, qPrintable(schemas ? QString() : schemas.error().detail));
    QStringList schemaNames;
    for (const SchemaNode &node : schemas.value()) {
        QCOMPARE(node.path.kind, ObjectKind::Schema);
        schemaNames << node.name;
    }
    QVERIFY(schemaNames.contains(QStringLiteral("dbo")));
    for (const QString &system :
         { QStringLiteral("sys"), QStringLiteral("INFORMATION_SCHEMA"), QStringLiteral("guest"),
           QStringLiteral("db_owner"), QStringLiteral("db_datareader") })
        QVERIFY2(!schemaNames.contains(system), qPrintable(system));

    // A database name with a bracket is quoted, not interpreted.
    auto odd = connection->children(
        ObjectPath { { QStringLiteral("x]; DROP TABLE y; --") }, ObjectKind::Database }, {});
    QVERIFY(!odd);
}

void TstMssqlProvider::encryptOptionalConnects()
{
    REQUIRE_SERVER();
    ConnectionConfig config = m_server.config(m_server.database);
    config.values.insert(QStringLiteral("encrypt"), QStringLiteral("optional"));
    config.values.insert(QStringLiteral("trustservercertificate"), true);
    auto connection = m_provider->createConnection(config, Secrets { m_server.password });
    auto info = connection->open({});
    QVERIFY2(info, qPrintable(info ? QString() : info.error().detail));
    QCOMPARE(info.value().product, QStringLiteral("SQL Server"));
    QVERIFY(!info.value().version.isEmpty());
}

void TstMssqlProvider::emptyAndCommentOnlyStatements()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    for (const QString &sql : { QStringLiteral("-- nothing"), QStringLiteral("  \n"),
                                QStringLiteral("/* nothing */"), QStringLiteral("GO") }) {
        auto stream = connection->execute(Statement { sql, {}, 500 }, {});
        QVERIFY2(stream, qPrintable(sql));
        QVERIFY(stream.value()->atEnd());
        QVERIFY(stream.value()->columns().empty());
        auto batch = stream.value()->fetch(500, {});
        QVERIFY(batch);
        QCOMPARE(batch.value().rowCount, 0);
    }
}

void TstMssqlProvider::droppedStreamKeepsConnectionUsable()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    // A stream dropped long before its end: the server may still be producing rows, and the
    // connection (one cursor, no MARS) has to take the next statement at once.
    for (int round = 0; round < 3; ++round) {
        {
            auto stream
                = connection->execute(Statement { fixture().seriesSql(100000), {}, 10 }, {});
            QVERIFY2(stream, qPrintable(stream ? QString() : stream.error().detail));
            auto batch = stream.value()->fetch(10, {});
            QVERIFY(batch);
            QCOMPARE(batch.value().rowCount, 10);
            QVERIFY(!stream.value()->atEnd());
        }
        const auto next = drain(*connection, QStringLiteral("SELECT %1").arg(round + 1));
        QVERIFY2(next.ok, qPrintable(next.errorText()));
        QCOMPARE(next.batches.front().columns[0].int64At(0), round + 1);
    }
}

void TstMssqlProvider::secondStatementWhileStreamOpenIsRefused()
{
    REQUIRE_SERVER();
    auto connection = openFresh();
    QVERIFY(connection);
    auto stream = connection->execute(Statement { fixture().seriesSql(1000), {}, 10 }, {});
    QVERIFY(stream);
    auto busy = connection->execute(Statement { QStringLiteral("SELECT 1"), {}, 10 }, {});
    QVERIFY(!busy);
    QCOMPARE(std::move(busy).error().kind, ErrorKind::Internal);
    // The tree has its own session: here it is refused too, not mixed into the open cursor.
    QVERIFY(!connection->children(ObjectPath { {}, ObjectKind::Database }, {}));
    // The stream is intact.
    auto batch = stream.value()->fetch(10, {});
    QVERIFY(batch);
    QCOMPARE(batch.value().columns[0].int64At(0), 1);
}

QTEST_GUILESS_MAIN(TstMssqlProvider)
#include "tst_mssql_provider.moc"
