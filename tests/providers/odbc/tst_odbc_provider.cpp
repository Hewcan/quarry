#include "generic_settings.h"
#include "odbc_state.h"
#include "odbc_support.h"
#include "provider_conformance.h"

#include <quarry/provider_registry.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <map>
#include <memory>

using namespace quarry;
using namespace quarry::conformance;

namespace {

// QUARRY_TEST_ODBC_URL is a complete ODBC connection string for a server that speaks the
// PostgreSQL dialect (the CI uses psqlodbc against postgres:17), for example
//   Driver={PostgreSQL Unicode};Server=localhost;Port=5432;Database=postgres;Uid=postgres;Pwd=x;
// Every conformance test gets its own schema: the fixture runs CREATE SCHEMA quarry_conf_<pid>_<n>
// on a first connection, points the others at it with ConnSettings=SET search_path=<schema> (a
// psqlodbc option; the schema is the first one of the search path, so unqualified tables land in
// it), and DROP SCHEMA ... CASCADE when the test ends. UseServerSidePrepare=0 makes psqlodbc put
// the parameters into the text of the statement: the grid sends every edited value as text, and a
// server-side prepared statement would reject text for a numeric column.
const QString kUrlVariable = QStringLiteral("QUARRY_TEST_ODBC_URL");
const QString kTextDriver = QStringLiteral("Microsoft Access Text Driver (*.txt, *.csv)");

ConnectionConfig configFor(const QString &connectionString)
{
    ConnectionConfig c;
    c.values.insert(QStringLiteral("connectionString"), connectionString);
    return c;
}

// Runs one statement to its end on a throwaway connection; empty string on success.
QString runOnce(IDatabaseProvider &provider, const ConnectionConfig &config, const QString &sql)
{
    auto connection = provider.createConnection(config, {});
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

bool containsDriver(const QString &name)
{
    const QStringList drivers = odbc::installedDrivers();
    return std::any_of(drivers.begin(), drivers.end(), [&name](const QString &driver) {
        return driver.compare(name, Qt::CaseInsensitive) == 0;
    });
}

// Text of one cell of a batch.
QString textAt(const ResultBatch &batch, size_t column, qsizetype row = 0)
{
    return QString::fromUtf8(batch.columns[column].bytesAt(row));
}

void collectNodes(IConnection &connection, const ObjectPath &parent, int depth,
                  std::vector<SchemaNode> &out)
{
    if (depth > 6)
        return;
    auto children = connection.children(parent, {});
    if (!children)
        return;
    for (const SchemaNode &node : children.value()) {
        out.push_back(node);
        if (node.hasChildren)
            collectNodes(connection, node.path, depth + 1, out);
    }
}

class OdbcFixture : public Fixture
{
public:
    OdbcFixture(IDatabaseProvider &provider, QString baseConnectionString, QString schema)
        : m_provider(provider)
        , m_base(std::move(baseConnectionString))
        , m_schema(std::move(schema))
    { }

    QString create()
    {
        return runOnce(m_provider, configFor(m_base),
                       QStringLiteral("CREATE SCHEMA %1").arg(m_schema));
    }
    ~OdbcFixture() override
    {
        if (!m_created)
            return;
        runOnce(m_provider, configFor(m_base),
                QStringLiteral("DROP SCHEMA IF EXISTS %1 CASCADE").arg(m_schema));
    }
    void markCreated() { m_created = true; }

    IDatabaseProvider &provider() override { return m_provider; }
    ConnectionConfig config() override { return makeConfig(); }
    ConnectionConfig makeConfig() const
    {
        QString text = m_base;
        if (!text.endsWith(QLatin1Char(';')))
            text += QLatin1Char(';');
        text += QStringLiteral("ConnSettings=SET search_path=%1;").arg(m_schema);
        if (!text.contains(QLatin1String("UseServerSidePrepare"), Qt::CaseInsensitive))
            text += QStringLiteral("UseServerSidePrepare=0;");
        return configFor(text);
    }
    QString sampleSchemaSql() const override
    {
        return QStringLiteral(
            "CREATE TABLE authors(id BIGINT PRIMARY KEY, name VARCHAR(255) NOT NULL UNIQUE);\n"
            "CREATE TABLE books(id BIGINT PRIMARY KEY, title VARCHAR(255) NOT NULL,\n"
            "  author_id BIGINT NOT NULL REFERENCES authors(id), price DOUBLE PRECISION NULL,\n"
            "  cover BYTEA NULL);\n"
            "CREATE INDEX idx_books_title ON books(title); -- a comment; with a semicolon\n"
            "CREATE VIEW v_titles AS SELECT id, title FROM books;\n");
    }
    QString slowQuerySql() const override { return QStringLiteral("SELECT pg_sleep(3600)"); }
    QString seriesSql(int count) const override
    {
        return QStringLiteral("SELECT generate_series(1, %1) AS n").arg(count);
    }
    QString rowIdentifierName() const override { return {}; }
    bool hasRowIdentifier() const override { return false; }
    bool reportsErrorPosition() const override { return false; }
    // Where the tree of the provider puts the tables depends on what the driver reports as
    // catalog and schema: ask the tree instead of assuming it.
    ObjectPath booksPath() const override
    {
        return find(QStringLiteral("books"), ObjectKind::Table);
    }
    ObjectPath viewPath() const override
    {
        return find(QStringLiteral("v_titles"), ObjectKind::View);
    }
    ValueType fractionalLiteralType() const override { return ValueType::Decimal; }

private:
    ObjectPath find(const QString &name, ObjectKind kind) const
    {
        ObjectPath fallback { { QString(), m_schema, name }, kind };
        if (const auto cached = m_found.find(name); cached != m_found.end())
            return cached->second;
        auto connection = m_provider.createConnection(makeConfig(), {});
        if (!connection || !connection->open({}))
            return fallback;
        std::vector<SchemaNode> nodes;
        collectNodes(*connection, ObjectPath { {}, ObjectKind::Database }, 0, nodes);
        for (const SchemaNode &node : nodes) {
            if (node.path.kind == kind && node.path.parts.size() == 3
                && node.path.parts.at(1) == m_schema && node.name == name) {
                m_found.emplace(name, node.path);
                return node.path;
            }
        }
        return fallback;
    }

    IDatabaseProvider &m_provider;
    QString m_base;
    QString m_schema;
    bool m_created = false;
    mutable std::map<QString, ObjectPath> m_found;
};

} // namespace

class TstOdbcProvider : public ProviderConformanceTest
{
    Q_OBJECT

protected:
    std::unique_ptr<Fixture> createFixture() override
    {
        if (m_url.isEmpty()) {
            // Shared tests need a server, except splitStatements. Our own tests never use the
            // fixture; they skip by themselves when they need one.
            const QByteArray name = QTest::currentTestFunction();
            const bool shared
                = ProviderConformanceTest::staticMetaObject.indexOfSlot(name + "()") >= 0;
            if (shared && name != "splitStatements") {
                QTest::qSkip("QUARRY_TEST_ODBC_URL is not set", __FILE__, __LINE__);
                return nullptr;
            }
            return std::make_unique<OdbcFixture>(*m_provider, QString(), QStringLiteral("none"));
        }
        auto fixture = std::make_unique<OdbcFixture>(*m_provider, m_url,
                                                     QStringLiteral("quarry_conf_%1_%2")
                                                         .arg(QCoreApplication::applicationPid())
                                                         .arg(++m_counter));
        if (const QString error = fixture->create(); !error.isEmpty()) {
            QTest::qFail(qPrintable(QStringLiteral("CREATE SCHEMA failed: ") + error), __FILE__,
                         __LINE__);
            return nullptr;
        }
        fixture->markCreated();
        return fixture;
    }

private slots:
    void initTestCase();

    // No server needed
    void pluginMetadata();
    void settingsFromForm();
    void settingsRejectEmptyForm();
    void connectionStringIsBuiltWithEscapes();
    void connectionStringValues();
    void diagnosticKinds();
    void sqlTypeMapping();
    void columnPlans();
    void driverMessagesAndPatterns();
    void wideStringConversion();
    void inlineNullParametersSkipsQuotedText();
    void dialectSpecifics();
    void splitStatementsAnsi();
    void unknownDriverIsNamedInTheError();
    void unknownDataSourceIsNamedInTheError();
    void executeBeforeOpenFails();

    // Microsoft Access Text Driver over CSV files in a temporary folder (Windows only)
    void textDriverConnectsAndSelects();
    void textDriverTree();
    void textDriverDescribe();
    void textDriverTypes();
    void textDriverParameters();
    void textDriverMissingTable();
    void textDriverEmptyResult();
    void textDriverStreamsInBlocks();
    void textDriverFixedColumnsAreReadInBlocks();

private:
    // Opens a connection on the folder of CSV files, or skips the test.
    std::unique_ptr<IConnection> openTextDriver();
    bool prepareTextFiles();

    ProviderRegistry m_registry;
    IDatabaseProvider *m_provider = nullptr;
    QString m_url;
    int m_counter = 0;
    QTemporaryDir m_folder;
};

void TstOdbcProvider::initTestCase()
{
    m_registry.scan(QStringLiteral(QUARRY_PLUGIN_DIR));
    QString error;
    m_provider = m_registry.provider(QStringLiteral("odbc"), &error);
    QVERIFY2(m_provider, qPrintable(error));
    m_url = qEnvironmentVariable(qPrintable(kUrlVariable)).trimmed();
}

// ---- no server -----------------------------------------------------------------------------

void TstOdbcProvider::pluginMetadata()
{
    const auto &entries = m_registry.available();
    const auto entry = std::find_if(entries.begin(), entries.end(), [](const ProviderEntry &e) {
        return e.id == QLatin1String("odbc");
    });
    QVERIFY(entry != entries.end());
    QCOMPARE(entry->displayName, QStringLiteral("ODBC (generic)"));
    QCOMPARE(entry->apiVersion, kProviderApiVersion);
    QVERIFY(m_registry.rejected().empty());

    const ProviderInfo info = m_provider->info();
    QCOMPARE(info.id, QStringLiteral("odbc"));
    QVERIFY(info.caps.testFlag(Capability::Cancel));
    QVERIFY(info.caps.testFlag(Capability::Transactions));
    QVERIFY(info.caps.testFlag(Capability::MultipleResultSets));
    QVERIFY(!info.caps.testFlag(Capability::Schemas));
    QVERIFY(!info.caps.testFlag(Capability::MultipleDatabases));

    const auto fields = m_provider->connectionFields();
    QCOMPARE(fields.size(), size_t(4));
    QCOMPARE(fields[0].key, QStringLiteral("dsn"));
    QCOMPARE(fields[1].key, QStringLiteral("driver"));
    QCOMPARE(fields[2].key, QStringLiteral("connectionString"));
    QVERIFY(fields[2].advanced);
    QCOMPARE(fields[3].key, QStringLiteral("user"));
    for (const auto &field : fields) {
        QCOMPARE(field.kind, ConnectionField::Kind::Text); // the password never is a field
        QVERIFY(!field.required); // one of three is needed: checked when connecting
        QVERIFY(!field.label.isEmpty());
    }
}

void TstOdbcProvider::settingsFromForm()
{
    QVariantMap values;
    values.insert(QStringLiteral("dsn"), QStringLiteral("  Sales  "));
    values.insert(QStringLiteral("user"), QStringLiteral("ann"));
    auto settings = odbc::buildGenericSettings(values, QStringLiteral("secret"));
    QVERIFY(settings);
    QCOMPARE(settings.value().dsn, QStringLiteral("Sales"));
    QCOMPARE(settings.value().user, QStringLiteral("ann"));
    QCOMPARE(settings.value().password, QStringLiteral("secret"));
    QCOMPARE(odbc::genericTarget(settings.value()), QStringLiteral("Sales"));

    // Any one of the three is enough.
    for (const char *key : { "driver", "connectionString" }) {
        QVariantMap one;
        one.insert(QString::fromLatin1(key), QStringLiteral("x"));
        QVERIFY(odbc::buildGenericSettings(one, {}));
    }
    QVariantMap driver;
    driver.insert(QStringLiteral("driver"), QStringLiteral("PostgreSQL Unicode"));
    QCOMPARE(odbc::genericTarget(odbc::buildGenericSettings(driver, {}).value()),
             QStringLiteral("PostgreSQL Unicode"));
}

void TstOdbcProvider::settingsRejectEmptyForm()
{
    QVariantMap values;
    values.insert(QStringLiteral("dsn"), QStringLiteral("   "));
    values.insert(QStringLiteral("user"), QStringLiteral("ann"));
    auto settings = odbc::buildGenericSettings(values, {});
    QVERIFY(!settings);
    QCOMPARE(std::move(settings).error().kind, ErrorKind::Connection);

    // The same through the provider: the connection reports it when it opens.
    ConnectionConfig config;
    config.values = values;
    auto connection = m_provider->createConnection(config, {});
    auto opened = connection->open({});
    QVERIFY(!opened);
    QCOMPARE(std::move(opened).error().kind, ErrorKind::Connection);
    QVERIFY(!connection->isOpen());
}

void TstOdbcProvider::connectionStringIsBuiltWithEscapes()
{
    odbc::GenericSettings s;
    s.dsn = QStringLiteral("Sales");
    s.user = QStringLiteral("ann");
    s.password = QStringLiteral("p}w;d={x}");
    QCOMPARE(odbc::buildGenericConnectionString(s),
             QStringLiteral("DSN=Sales;UID=ann;PWD={p}}w;d={x}}};"));

    // A driver goes in braces whatever it looks like; the extra text keeps its own ;.
    odbc::GenericSettings d;
    d.driver = QStringLiteral("C:\\Program Files\\odbc}.dll");
    d.extra = QStringLiteral("Server=localhost;Database=x");
    QCOMPARE(odbc::buildGenericConnectionString(d),
             QStringLiteral("Driver={C:\\Program Files\\odbc}}.dll};Server=localhost;Database=x;"));

    // No password: nothing is appended, so the one of the data source or the text stays.
    odbc::GenericSettings n;
    n.extra = QStringLiteral("Driver={x};Pwd=inline;");
    n.user = QStringLiteral("a b");
    QCOMPARE(odbc::buildGenericConnectionString(n),
             QStringLiteral("Driver={x};Pwd=inline;UID=a b;"));

    odbc::GenericSettings e;
    e.dsn = QStringLiteral("Sales");
    e.password = QStringLiteral("}");
    QCOMPARE(odbc::buildGenericConnectionString(e), QStringLiteral("DSN=Sales;PWD={}}};"));
}

void TstOdbcProvider::connectionStringValues()
{
    QCOMPARE(odbc::bracedValue(QStringLiteral("a}b")), QStringLiteral("{a}}b}"));
    QCOMPARE(odbc::bracedValue(QString()), QStringLiteral("{}"));
    QCOMPARE(odbc::connectionStringValue(QStringLiteral("plain")), QStringLiteral("plain"));
    QCOMPARE(odbc::connectionStringValue(QStringLiteral("a;b")), QStringLiteral("{a;b}"));
    QCOMPARE(odbc::connectionStringValue(QStringLiteral("a=b")), QStringLiteral("{a=b}"));
    QCOMPARE(odbc::connectionStringValue(QStringLiteral(" lead")), QStringLiteral("{ lead}"));
    QCOMPARE(odbc::connectionStringValue(QStringLiteral("{x")), QStringLiteral("{{x}"));
    QCOMPARE(odbc::connectionStringPair(QStringLiteral("Server"), QStringLiteral("h,1433")),
             QStringLiteral("Server=h,1433;"));
    QCOMPARE(
        odbc::connectionStringBracedPair(QStringLiteral("Driver"), QStringLiteral("My Driver")),
        QStringLiteral("Driver={My Driver};"));
}

void TstOdbcProvider::diagnosticKinds()
{
    using odbc::kindForDiagnostic;
    QCOMPARE(kindForDiagnostic(u"28000", 18456), ErrorKind::Authentication);
    QCOMPARE(kindForDiagnostic(u"28P01", 0), ErrorKind::Authentication);
    QCOMPARE(kindForDiagnostic(u"08001", 0), ErrorKind::Connection);
    QCOMPARE(kindForDiagnostic(u"08S01", 0), ErrorKind::Connection);
    for (const QString &state :
         { QStringLiteral("IM002"), QStringLiteral("IM003"), QStringLiteral("IM004"),
           QStringLiteral("IM010"), QStringLiteral("IM014") })
        QCOMPARE(kindForDiagnostic(state, 0), ErrorKind::Connection);
    QCOMPARE(kindForDiagnostic(u"IM001", 0), ErrorKind::Driver);
    QCOMPARE(kindForDiagnostic(u"HYT00", 0), ErrorKind::Timeout);
    QCOMPARE(kindForDiagnostic(u"HYT01", 0), ErrorKind::Timeout);
    QCOMPARE(kindForDiagnostic(u"HY008", 0), ErrorKind::Cancelled);
    QCOMPARE(kindForDiagnostic(u"HY000", 3617), ErrorKind::Cancelled);
    QCOMPARE(kindForDiagnostic(u"57014", 0), ErrorKind::Cancelled);
    QCOMPARE(kindForDiagnostic(u"23000", 2627), ErrorKind::Constraint);
    QCOMPARE(kindForDiagnostic(u"23505", 0), ErrorKind::Constraint);
    QCOMPARE(kindForDiagnostic(u"42S02", 208), ErrorKind::NotFound);
    QCOMPARE(kindForDiagnostic(u"42S12", 0), ErrorKind::NotFound);
    QCOMPARE(kindForDiagnostic(u"42S22", 207), ErrorKind::NotFound);
    QCOMPARE(kindForDiagnostic(u"42P01", 0), ErrorKind::NotFound);
    QCOMPARE(kindForDiagnostic(u"42703", 0), ErrorKind::NotFound);
    QCOMPARE(kindForDiagnostic(u"42501", 0), ErrorKind::Permission);
    QCOMPARE(kindForDiagnostic(u"42601", 0), ErrorKind::Syntax);
    QCOMPARE(kindForDiagnostic(u"42000", 102), ErrorKind::Syntax);
    // 42000 is split by the native code of SQL Server.
    for (const long code : { 229L, 230L, 297L, 262L, 300L })
        QCOMPARE(kindForDiagnostic(u"42000", code), ErrorKind::Permission);
    for (const long code : { 207L, 208L })
        QCOMPARE(kindForDiagnostic(u"42000", code), ErrorKind::NotFound);
    for (const long code : { 2627L, 2601L, 515L, 547L })
        QCOMPARE(kindForDiagnostic(u"42000", code), ErrorKind::Constraint);
    QCOMPARE(kindForDiagnostic(u"HYC00", 0), ErrorKind::Unsupported);
    QCOMPARE(kindForDiagnostic(u"HY000", 0), ErrorKind::Driver);
    QCOMPARE(kindForDiagnostic(u"22018", 245), ErrorKind::Driver);
    QCOMPARE(kindForDiagnostic(QStringView(), 0), ErrorKind::Driver);
    // Case does not matter.
    QCOMPARE(kindForDiagnostic(u"hy008", 0), ErrorKind::Cancelled);
    for (const auto kind :
         { ErrorKind::Authentication, ErrorKind::Connection, ErrorKind::Timeout,
           ErrorKind::Cancelled, ErrorKind::Syntax, ErrorKind::Permission, ErrorKind::Constraint,
           ErrorKind::NotFound, ErrorKind::Unsupported, ErrorKind::Driver })
        QVERIFY(!odbc::messageForKind(kind).isEmpty());
}

void TstOdbcProvider::sqlTypeMapping()
{
    using odbc::valueTypeForSqlType;
    for (const int type : { SQL_TINYINT, SQL_SMALLINT, SQL_INTEGER, SQL_BIGINT })
        QCOMPARE(valueTypeForSqlType(type), ValueType::Int64);
    for (const int type : { SQL_REAL, SQL_FLOAT, SQL_DOUBLE })
        QCOMPARE(valueTypeForSqlType(type), ValueType::Double);
    QCOMPARE(valueTypeForSqlType(SQL_DECIMAL), ValueType::Decimal);
    QCOMPARE(valueTypeForSqlType(SQL_NUMERIC), ValueType::Decimal);
    QCOMPARE(valueTypeForSqlType(SQL_BIT), ValueType::Bool);
    for (const int type : { SQL_CHAR, SQL_VARCHAR, SQL_LONGVARCHAR, SQL_WCHAR, SQL_WVARCHAR,
                            SQL_WLONGVARCHAR, odbc::kSqlSsXml })
        QCOMPARE(valueTypeForSqlType(type), ValueType::Text);
    for (const int type : { SQL_BINARY, SQL_VARBINARY, SQL_LONGVARBINARY })
        QCOMPARE(valueTypeForSqlType(type), ValueType::Bytes);
    QCOMPARE(valueTypeForSqlType(SQL_TYPE_DATE), ValueType::Date);
    QCOMPARE(valueTypeForSqlType(SQL_TYPE_TIME), ValueType::Time);
    QCOMPARE(valueTypeForSqlType(odbc::kSqlSsTime2), ValueType::Time);
    QCOMPARE(valueTypeForSqlType(SQL_TYPE_TIMESTAMP), ValueType::DateTime);
    QCOMPARE(valueTypeForSqlType(odbc::kSqlSsTimestampOffset), ValueType::DateTimeTz);
    QCOMPARE(valueTypeForSqlType(SQL_GUID), ValueType::Uuid);
    QCOMPARE(valueTypeForSqlType(odbc::kSqlSsUdt), ValueType::Other);
    QCOMPARE(valueTypeForSqlType(odbc::kSqlSsVariant), ValueType::Other);
    QCOMPARE(valueTypeForSqlType(SQL_INTERVAL_DAY), ValueType::Other);
    QCOMPARE(valueTypeForSqlType(0), ValueType::Other);
    for (const int type : { SQL_INTEGER, SQL_VARCHAR, SQL_TYPE_TIMESTAMP, odbc::kSqlSsXml, 9999 })
        QVERIFY(!odbc::genericTypeName(type).isEmpty());
    QCOMPARE(odbc::decoratedTypeName(QStringLiteral("nvarchar"), ValueType::Text, 255, 0),
             QStringLiteral("nvarchar(255)"));
    QCOMPARE(odbc::decoratedTypeName(QStringLiteral("numeric"), ValueType::Decimal, 10, 2),
             QStringLiteral("numeric(10,2)"));
    QCOMPARE(odbc::decoratedTypeName(QStringLiteral("text"), ValueType::Text, 2147483647, 0),
             QStringLiteral("text"));
    QCOMPARE(odbc::decoratedTypeName(QStringLiteral("int"), ValueType::Int64, 10, 0),
             QStringLiteral("int"));
}

void TstOdbcProvider::columnPlans()
{
    using odbc::planColumn;
    // Fixed sizes are always bound.
    auto integer = planColumn(SQL_BIGINT, 19, false);
    QCOMPARE(integer.type, ValueType::Int64);
    QCOMPARE(integer.cType, SQL_C_SBIGINT);
    QCOMPARE(integer.capacity, SQLLEN(8));
    auto real = planColumn(SQL_FLOAT, 53, false);
    QCOMPARE(real.cType, SQL_C_DOUBLE);
    QCOMPARE(real.capacity, SQLLEN(8));
    auto bit = planColumn(SQL_BIT, 1, false);
    QCOMPARE(bit.cType, SQL_C_BIT);
    QCOMPARE(bit.capacity, SQLLEN(1));
    for (const int type : { SQL_TYPE_DATE, SQL_TYPE_TIME, SQL_TYPE_TIMESTAMP, SQL_GUID,
                            odbc::kSqlSsTimestampOffset }) {
        const auto temporal = planColumn(type, 0, false);
        QCOMPARE(temporal.cType, SQL_C_WCHAR);
        QVERIFY(temporal.capacity >= SQLLEN(72)); // 35 characters and the NUL, two bytes each
    }
    // Text, binary and decimal columns only when the driver's size can be trusted and is small.
    QCOMPARE(planColumn(SQL_VARCHAR, 255, false).capacity, SQLLEN(0));
    const auto text = planColumn(SQL_WVARCHAR, 255, true);
    QCOMPARE(text.cType, SQL_C_WCHAR);
    QCOMPARE(text.capacity, SQLLEN((255 + 1) * 2)); // characters and the NUL, in bytes
    QCOMPARE(planColumn(SQL_WVARCHAR, 4000, true).capacity, SQLLEN(4001 * 2));
    QCOMPARE(planColumn(SQL_WVARCHAR, 4001, true).capacity, SQLLEN(0));
    QCOMPARE(planColumn(SQL_WVARCHAR, 0, true).capacity, SQLLEN(0)); // NVARCHAR(MAX)
    QCOMPARE(planColumn(SQL_LONGVARCHAR, 2147483647, true).capacity, SQLLEN(0));
    const auto bytes = planColumn(SQL_VARBINARY, 16, true);
    QCOMPARE(bytes.cType, SQL_C_BINARY);
    QCOMPARE(bytes.capacity, SQLLEN(16));
    QCOMPARE(planColumn(SQL_VARBINARY, 0, true).capacity, SQLLEN(0)); // VARBINARY(MAX)
    QCOMPARE(planColumn(SQL_VARBINARY, 0, true).cType, SQL_C_BINARY);
    QCOMPARE(planColumn(SQL_VARBINARY, 8001, true).capacity, SQLLEN(0));
    QCOMPARE(planColumn(SQL_DECIMAL, 10, false).capacity, SQLLEN(0));
    QVERIFY(planColumn(SQL_DECIMAL, 10, true).capacity >= SQLLEN(26)); // 13 characters
    QCOMPARE(planColumn(SQL_NUMERIC, 1000, true).capacity, SQLLEN(0));
    // Unknown types are text of unknown size.
    const auto other = planColumn(odbc::kSqlSsUdt, 0, true);
    QCOMPARE(other.type, ValueType::Other);
    QCOMPARE(other.cType, SQL_C_WCHAR);
    QCOMPARE(other.capacity, SQLLEN(0));
}

void TstOdbcProvider::driverMessagesAndPatterns()
{
    QCOMPARE(odbc::stripDriverPrefix(
                 QStringLiteral("[Microsoft][ODBC Driver 18 for SQL Server][SQL Server]Hello")),
             QStringLiteral("Hello"));
    QCOMPARE(odbc::stripDriverPrefix(QStringLiteral("[Microsoft][ODBC SQL Server Driver]Hi")),
             QStringLiteral("Hi"));
    // Only the driver's own prefix goes: a message that starts with a bracket stays.
    QCOMPARE(odbc::stripDriverPrefix(QStringLiteral("[dbo].[t] is gone")),
             QStringLiteral("[dbo].[t] is gone"));
    QCOMPARE(
        odbc::stripDriverPrefix(QStringLiteral("[Microsoft][ODBC Driver 18][SQL Server][x] y")),
        QStringLiteral("[x] y"));
    QCOMPARE(odbc::stripDriverPrefix(QString()), QString());

    QCOMPARE(odbc::escapePattern(QStringLiteral("my_table%"), QStringLiteral("\\")),
             QStringLiteral("my\\_table\\%"));
    QCOMPARE(odbc::escapePattern(QStringLiteral("a\\b"), QStringLiteral("\\")),
             QStringLiteral("a\\\\b"));
    QCOMPARE(odbc::escapePattern(QStringLiteral("my_table"), QString()),
             QStringLiteral("my_table"));
}

void TstOdbcProvider::inlineNullParametersSkipsQuotedText()
{
    // NULL markers become the literal; the other values keep their order.
    QString sql = QStringLiteral("INSERT INTO t VALUES (?, ?, ?, ?)");
    QVariantList values { 1, QVariant(), QStringLiteral("x"), QVariant() };
    odbc::inlineNullParameters(sql, values);
    QCOMPARE(sql, QStringLiteral("INSERT INTO t VALUES (?, NULL, ?, NULL)"));
    QCOMPARE(values, (QVariantList { 1, QStringLiteral("x") }));

    // A question mark inside text, identifiers, brackets or comments is not a marker.
    sql = QStringLiteral("SELECT '?', \"a?\", [b?], `c?` -- ?\n, /* ? */ ?");
    values = { QVariant() };
    odbc::inlineNullParameters(sql, values);
    QCOMPARE(sql, QStringLiteral("SELECT '?', \"a?\", [b?], `c?` -- ?\n, /* ? */ NULL"));
    QVERIFY(values.isEmpty());

    // Doubled quotes stay inside the text.
    sql = QStringLiteral("SELECT 'it''s ?', ?");
    values = { QVariant() };
    odbc::inlineNullParameters(sql, values);
    QCOMPARE(sql, QStringLiteral("SELECT 'it''s ?', NULL"));

    // Without NULLs, or when the counts differ, nothing changes.
    sql = QStringLiteral("SELECT ?");
    values = { 1 };
    odbc::inlineNullParameters(sql, values);
    QCOMPARE(sql, QStringLiteral("SELECT ?"));
    QCOMPARE(values.size(), 1);
    sql = QStringLiteral("SELECT ?, ?");
    values = { QVariant() };
    odbc::inlineNullParameters(sql, values);
    QCOMPARE(sql, QStringLiteral("SELECT ?, ?"));
    QCOMPARE(values.size(), 1);
}

void TstOdbcProvider::wideStringConversion()
{
    static_assert(sizeof(SQLWCHAR) == 2);
    const QString text = QStringLiteral("a\u00fc\u65e5\U0001F600");
    const std::vector<SQLWCHAR> wide = odbc::toWide(text);
    QCOMPARE(wide.size(), size_t(text.size() + 1)); // UTF-16 code units and the NUL
    QCOMPARE(wide.back(), SQLWCHAR(0));
    QCOMPARE(odbc::fromWide(wide.data(), text.size()), text);
    QCOMPARE(QString::fromUtf8(odbc::wideToUtf8(wide.data(), text.size())), text);
    QCOMPARE(odbc::toWide(QString()).size(), size_t(1));
    QCOMPARE(odbc::fromWide(wide.data(), 0), QString());
}

void TstOdbcProvider::dialectSpecifics()
{
    const ISqlDialect &d = m_provider->dialect();
    QCOMPARE(d.quoteIdentifier(u"a\"b"), QStringLiteral("\"a\"\"b\""));
    QCOMPARE(d.quoteIdentifier(QStringView()), QStringLiteral("\"\""));
    QCOMPARE(d.quoteLiteral(u"it's"), QStringLiteral("'it''s'"));
    QCOMPARE(d.quoteLiteral(u"a\\b"), QStringLiteral("'a\\b'")); // no backslash escapes
    QCOMPARE(d.placeholder(1), QStringLiteral("?"));
    QCOMPARE(d.placeholder(7), QStringLiteral("?"));
    QVERIFY(!d.rowIdentifier().has_value());
    QCOMPARE(d.highlightingDefinition(), QStringLiteral("SQL"));
    QCOMPARE(d.explainStatement(QStringLiteral("SELECT 1"), ExplainMode::Plan, {}), QString());
    QVERIFY(d.keywords().contains(QStringLiteral("SELECT")));
    QVERIFY(d.builtinFunctions().contains(QStringLiteral("coalesce")));

    QCOMPARE(d.limitClause(-1, 0), QString());
    QCOMPARE(d.limitClause(10, 0), QStringLiteral("FETCH FIRST 10 ROWS ONLY"));
    QCOMPARE(d.limitClause(10, 20), QStringLiteral("OFFSET 20 ROWS FETCH NEXT 10 ROWS ONLY"));
    QCOMPARE(d.limitClause(-1, 5), QStringLiteral("OFFSET 5 ROWS"));

    // Catalog, schema and table; a part the driver does not have is left out.
    QCOMPARE(d.qualifiedName({ { QStringLiteral("c"), QStringLiteral("s"), QStringLiteral("t") },
                               ObjectKind::Table }),
             QStringLiteral("\"c\".\"s\".\"t\""));
    QCOMPARE(d.qualifiedName(
                 { { QString(), QStringLiteral("s"), QStringLiteral("t\"x") }, ObjectKind::Table }),
             QStringLiteral("\"s\".\"t\"\"x\""));
    QCOMPARE(d.qualifiedName({ { QString(), QString(), QStringLiteral("t") }, ObjectKind::View }),
             QStringLiteral("\"t\""));
}

void TstOdbcProvider::splitStatementsAnsi()
{
    const ISqlDialect &d = m_provider->dialect();
    auto split = [&d](const QString &script) {
        QStringList out;
        for (const StatementRange &r : d.splitStatements(script))
            out << script.mid(r.begin, r.end - r.begin);
        return out;
    };
    // Nested block comments, doubled quotes in both kinds of quotes.
    QCOMPARE(split(QStringLiteral("/* a /* ; */ still comment ; */ SELECT 1; SELECT 2")),
             (QStringList { QStringLiteral("/* a /* ; */ still comment ; */ SELECT 1"),
                            QStringLiteral("SELECT 2") }));
    QCOMPARE(split(QStringLiteral("SELECT \"a\"\";b\", 'x'';y'; SELECT 2")),
             (QStringList { QStringLiteral("SELECT \"a\"\";b\", 'x'';y'"),
                            QStringLiteral("SELECT 2") }));
    // No backslash escapes in standard SQL: the quote after the backslash closes the string.
    QCOMPARE(split(QStringLiteral("SELECT 'a\\'; SELECT 2")),
             (QStringList { QStringLiteral("SELECT 'a\\'"), QStringLiteral("SELECT 2") }));
    QCOMPARE(split(QStringLiteral("SELECT 'unterminated; SELECT 2")),
             (QStringList { QStringLiteral("SELECT 'unterminated; SELECT 2") }));
    QCOMPARE(split(QStringLiteral("-- c1\nSELECT 1 -- c2\n;\n/* c3 */")),
             (QStringList { QStringLiteral("-- c1\nSELECT 1 -- c2") }));
    QCOMPARE(split(QStringLiteral("SELECT 1;\r\nSELECT 2;\r\n")),
             (QStringList { QStringLiteral("SELECT 1"), QStringLiteral("SELECT 2") }));
    // GO is not a command here.
    QCOMPARE(split(QStringLiteral("SELECT 1\nGO\nSELECT 2")),
             (QStringList { QStringLiteral("SELECT 1\nGO\nSELECT 2") }));
}

void TstOdbcProvider::unknownDriverIsNamedInTheError()
{
    ConnectionConfig config;
    config.values.insert(QStringLiteral("driver"), QStringLiteral("Quarry No Such Driver"));
    auto connection = m_provider->createConnection(config, {});
    auto opened = connection->open({});
    QVERIFY(!opened);
    const Error error = std::move(opened).error();
    QCOMPARE(error.kind, ErrorKind::Connection);
    QVERIFY2(error.message.contains(QLatin1String("Quarry No Such Driver")),
             qPrintable(error.message + QLatin1Char('|') + error.detail));
    QVERIFY(!error.detail.isEmpty());
    QVERIFY(!connection->isOpen());
}

void TstOdbcProvider::unknownDataSourceIsNamedInTheError()
{
    ConnectionConfig config;
    config.values.insert(QStringLiteral("dsn"), QStringLiteral("quarry_no_such_dsn"));
    config.values.insert(QStringLiteral("user"), QStringLiteral("nobody"));
    auto connection = m_provider->createConnection(config, Secrets { QStringLiteral("x") });
    auto opened = connection->open({});
    QVERIFY(!opened);
    const Error error = std::move(opened).error();
    QCOMPARE(error.kind, ErrorKind::Connection);
    QVERIFY2(error.message.contains(QLatin1String("quarry_no_such_dsn")),
             qPrintable(error.message));
    // The password is not in what the error reports.
    QVERIFY(!error.detail.contains(QLatin1String("PWD")));
}

void TstOdbcProvider::executeBeforeOpenFails()
{
    ConnectionConfig config;
    config.values.insert(QStringLiteral("dsn"), QStringLiteral("quarry_no_such_dsn"));
    auto connection = m_provider->createConnection(config, {});
    QVERIFY(!connection->isOpen());
    auto stream = connection->execute(Statement { QStringLiteral("SELECT 1"), {}, 500 }, {});
    QVERIFY(!stream);
    QCOMPARE(std::move(stream).error().kind, ErrorKind::Connection);
    auto children = connection->children(ObjectPath { {}, ObjectKind::Database }, {});
    QVERIFY(!children);
    QVERIFY(!connection->begin());
    connection->requestCancel(); // nothing to cancel, must not crash
    connection->close();
}

// ---- Microsoft Access Text Driver ----------------------------------------------------------

bool TstOdbcProvider::prepareTextFiles()
{
    if (!m_folder.isValid())
        return false;
    const QString dir = m_folder.path();
    QFile people(dir + QStringLiteral("/people.csv"));
    if (!people.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    people.write("id,name,price,created\r\n"
                 "1,Ann,12.5,2026-01-02 03:04:05\r\n"
                 "2,Bob,7,2026-02-03 04:05:06\r\n"
                 "3,Cy,0.25,2026-03-04 05:06:07\r\n");
    people.close();
    QFile many(dir + QStringLiteral("/many.csv"));
    if (!many.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    many.write("n,label\r\n");
    for (int i = 1; i <= 1200; ++i)
        many.write(QByteArray::number(i) + ",row" + QByteArray::number(i) + "\r\n");
    many.close();
    // Types are declared, not guessed from the first rows.
    QFile schema(dir + QStringLiteral("/schema.ini"));
    if (!schema.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    schema.write("[people.csv]\r\n"
                 "Format=CSVDelimited\r\n"
                 "ColNameHeader=True\r\n"
                 "CharacterSet=ANSI\r\n"
                 "DecimalSymbol=.\r\n"
                 "DateTimeFormat=yyyy-mm-dd hh:nn:ss\r\n"
                 "Col1=id Long\r\n"
                 "Col2=name Text Width 50\r\n"
                 "Col3=price Double\r\n"
                 "Col4=created DateTime\r\n"
                 "[many.csv]\r\n"
                 "Format=CSVDelimited\r\n"
                 "ColNameHeader=True\r\n"
                 "CharacterSet=ANSI\r\n"
                 "DecimalSymbol=.\r\n"
                 "Col1=n Long\r\n"
                 "Col2=label Text Width 20\r\n");
    schema.close();
    return true;
}

std::unique_ptr<IConnection> TstOdbcProvider::openTextDriver()
{
    if (!containsDriver(kTextDriver)) {
        QTest::qSkip("the Microsoft Access Text Driver is not installed", __FILE__, __LINE__);
        return nullptr;
    }
    if (!prepareTextFiles()) {
        QTest::qFail("cannot write the CSV files", __FILE__, __LINE__);
        return nullptr;
    }
    ConnectionConfig config;
    config.values.insert(QStringLiteral("driver"), kTextDriver);
    // The trailing backslash matters: without it the Text driver lists the parent folder.
    config.values.insert(QStringLiteral("connectionString"),
                         QStringLiteral("Dbq=%1;Extensions=asc,csv,tab,txt;")
                             .arg(QDir::toNativeSeparators(m_folder.path()) + QLatin1Char('\\')));
    auto connection = m_provider->createConnection(config, {});
    auto info = connection->open({});
    if (!info) {
        QTest::qFail(qPrintable(info.error().message + QStringLiteral(" | ") + info.error().detail),
                     __FILE__, __LINE__);
        return nullptr;
    }
    if (info.value().product.isEmpty()) {
        QTest::qFail("no product name", __FILE__, __LINE__);
        return nullptr;
    }
    return connection;
}

void TstOdbcProvider::textDriverConnectsAndSelects()
{
    auto connection = openTextDriver();
    if (!connection)
        return;
    QVERIFY(connection->isOpen());
    const auto d = drain(*connection,
                         QStringLiteral("SELECT id, name, price FROM people.csv "
                                        "ORDER BY id"));
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    QCOMPARE(d.rows, 3);
    QCOMPARE(d.columns.size(), size_t(3));
    QCOMPARE(d.columns[0].name, QStringLiteral("id"));
    const ResultBatch &b = d.batches.front();
    QCOMPARE(b.columns[0].int64At(0), 1);
    QCOMPARE(textAt(b, 1, 0), QStringLiteral("Ann"));
    QCOMPARE(textAt(b, 1, 2), QStringLiteral("Cy"));
    QCOMPARE(b.columns[2].doubleAt(1), 7.0);

    // The connection is reusable, and an empty result keeps its columns.
    const auto none
        = drain(*connection, QStringLiteral("SELECT id, name FROM people.csv WHERE id < 0"));
    QVERIFY2(none.ok, qPrintable(none.errorText()));
    QCOMPARE(none.rows, 0);
    QCOMPARE(none.columns.size(), size_t(2));
    connection->close();
    QVERIFY(!connection->isOpen());
}

void TstOdbcProvider::textDriverTree()
{
    auto connection = openTextDriver();
    if (!connection)
        return;
    std::vector<SchemaNode> nodes;
    collectNodes(*connection, ObjectPath { {}, ObjectKind::Database }, 0, nodes);
    QVERIFY(!nodes.empty());
    // The file shows up as a table, whatever the driver calls the catalog and the schema. (The Text
    // driver takes a Dbq without a trailing backslash as a file prefix: see openTextDriver().)
    const auto people = std::find_if(nodes.begin(), nodes.end(), [](const SchemaNode &n) {
        return n.path.kind == ObjectKind::Table
            && n.name.startsWith(QLatin1String("people"), Qt::CaseInsensitive);
    });
    QVERIFY2(people != nodes.end(), "people.csv is not in the tree");
    QCOMPARE(people->path.parts.size(), 3);
    QCOMPARE(people->path.parts.last(), people->name);
    QVERIFY(people->hasChildren);

    // Folders of a table, then its columns.
    auto folders = connection->children(people->path, {});
    QVERIFY2(folders, qPrintable(folders ? QString() : folders.error().detail));
    QCOMPARE(folders.value().size(), size_t(3));
    QCOMPARE(folders.value()[0].path.kind, ObjectKind::Folder);
    auto columns = connection->children(folders.value()[0].path, {});
    QVERIFY2(columns, qPrintable(columns ? QString() : columns.error().detail));
    QStringList names;
    for (const SchemaNode &column : columns.value()) {
        QCOMPARE(column.path.kind, ObjectKind::Column);
        QVERIFY(!column.hasChildren);
        QVERIFY(!column.detail.isEmpty());
        names << column.name;
    }
    QCOMPARE(names,
             (QStringList { QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("price"),
                            QStringLiteral("created") }));

    // Nothing but the nodes of the tree answers: an unknown folder or a column has no children.
    auto none = connection->children(columns.value().front().path, {});
    QVERIFY(none);
    QVERIFY(none.value().empty());
}

void TstOdbcProvider::textDriverDescribe()
{
    auto connection = openTextDriver();
    if (!connection)
        return;
    std::vector<SchemaNode> nodes;
    collectNodes(*connection, ObjectPath { {}, ObjectKind::Database }, 0, nodes);
    const auto people = std::find_if(nodes.begin(), nodes.end(), [](const SchemaNode &n) {
        return n.path.kind == ObjectKind::Table
            && n.name.startsWith(QLatin1String("people"), Qt::CaseInsensitive);
    });
    QVERIFY(people != nodes.end());
    auto details = connection->describe(people->path, {});
    QVERIFY2(details, qPrintable(details ? QString() : details.error().detail));
    const TableDetails &t = details.value();
    QCOMPARE(t.columns.size(), size_t(4));
    QCOMPARE(t.columns[0].name, QStringLiteral("id"));
    QCOMPARE(t.columns[0].type, ValueType::Int64);
    QCOMPARE(t.columns[1].type, ValueType::Text);
    QCOMPARE(t.columns[2].type, ValueType::Double);
    QCOMPARE(t.columns[3].type, ValueType::DateTime);
    for (const ColumnDef &column : t.columns)
        QVERIFY(!column.nativeType.isEmpty());
    // A flat-file driver has no keys: the optional catalog functions just give nothing.
    QVERIFY(t.foreignKeys.empty());

    ObjectPath missing = people->path;
    missing.parts.last() = QStringLiteral("no_such_table");
    auto absent = connection->describe(missing, {});
    QVERIFY(!absent);
    QCOMPARE(std::move(absent).error().kind, ErrorKind::NotFound);

    auto notATable
        = connection->describe(ObjectPath { { QStringLiteral("x") }, ObjectKind::Table }, {});
    QVERIFY(!notATable);
    QCOMPARE(std::move(notATable).error().kind, ErrorKind::NotFound);
}

void TstOdbcProvider::textDriverTypes()
{
    auto connection = openTextDriver();
    if (!connection)
        return;
    const auto d = drain(*connection,
                         QStringLiteral("SELECT id, name, price, created FROM people.csv "
                                        "WHERE id = 1"));
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    QCOMPARE(d.rows, 1);
    QCOMPARE(d.columns[0].type, ValueType::Int64);
    QCOMPARE(d.columns[1].type, ValueType::Text);
    QCOMPARE(d.columns[2].type, ValueType::Double);
    QCOMPARE(d.columns[3].type, ValueType::DateTime);
    const ResultBatch &b = d.batches.front();
    QCOMPARE(b.columns[0].typeAt(0), ValueType::Int64);
    QCOMPARE(b.columns[2].doubleAt(0), 12.5);
    // Dates arrive as the ISO text of the driver (the driver gives yyyy-mm-dd hh:mm:ss).
    QVERIFY2(textAt(b, 3).startsWith(QLatin1String("2026-01-02")), qPrintable(textAt(b, 3)));
    QVERIFY(textAt(b, 3).contains(QLatin1String("03:04:05")));
    QVERIFY(!d.columns[0].nativeType.isEmpty());
    QVERIFY(!d.columns[3].nativeType.isEmpty());

    // A literal and an expression have types too.
    const auto lit = drain(*connection,
                           QStringLiteral("SELECT 1 + 1 AS two FROM people.csv "
                                          "WHERE id = 1"));
    QVERIFY2(lit.ok, qPrintable(lit.errorText()));
    QCOMPARE(lit.rows, 1);
    QCOMPARE(lit.columns[0].name, QStringLiteral("two"));
}

void TstOdbcProvider::textDriverParameters()
{
    auto connection = openTextDriver();
    if (!connection)
        return;
    const auto d
        = drain(*connection, QStringLiteral("SELECT name FROM people.csv WHERE id = ?"), { 2 });
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    QCOMPARE(d.rows, 1);
    QCOMPARE(textAt(d.batches.front(), 0), QStringLiteral("Bob"));

    const auto text = drain(*connection, QStringLiteral("SELECT id FROM people.csv WHERE name = ?"),
                            { QStringLiteral("Cy") });
    QVERIFY2(text.ok, qPrintable(text.errorText()));
    QCOMPARE(text.rows, 1);
    QCOMPARE(text.batches.front().columns[0].int64At(0), 3);

    // The wrong number of parameters is an error, not a silent NULL.
    const auto wrong
        = drain(*connection, QStringLiteral("SELECT name FROM people.csv WHERE id = ?"), { 1, 2 });
    QVERIFY(!wrong.ok);
    QCOMPARE(wrong.error.kind, ErrorKind::Syntax);
}

void TstOdbcProvider::textDriverMissingTable()
{
    auto connection = openTextDriver();
    if (!connection)
        return;
    const auto d = drain(*connection, QStringLiteral("SELECT * FROM no_such_table.csv"));
    QVERIFY(!d.ok);
    QVERIFY(!d.error.message.isEmpty());
    QVERIFY(!d.error.detail.isEmpty());
    QVERIFY2(d.error.detail.contains(QLatin1String("SQLSTATE")), qPrintable(d.error.detail));
    QVERIFY2(d.error.kind == ErrorKind::NotFound || d.error.kind == ErrorKind::Driver,
             qPrintable(d.errorText()));
    QVERIFY(!d.error.sqlState.isEmpty());
    // The failed statement does not leave the connection busy.
    const auto next = drain(*connection, QStringLiteral("SELECT id FROM people.csv"));
    QVERIFY2(next.ok, qPrintable(next.errorText()));
    QCOMPARE(next.rows, 3);

    const auto syntax = drain(*connection, QStringLiteral("SELEC id FROM people.csv"));
    QVERIFY(!syntax.ok);
    QVERIFY(syntax.error.kind == ErrorKind::Syntax || syntax.error.kind == ErrorKind::Driver);
}

void TstOdbcProvider::textDriverEmptyResult()
{
    auto connection = openTextDriver();
    if (!connection)
        return;
    auto stream = connection->execute(Statement { QStringLiteral("-- nothing\n"), {}, 500 }, {});
    QVERIFY(stream);
    QVERIFY(stream.value()->atEnd());
    QVERIFY(stream.value()->columns().empty());
    auto batch = stream.value()->fetch(500, {});
    QVERIFY(batch);
    QCOMPARE(batch.value().rowCount, 0);
    QVERIFY(!stream.value()->affectedRows().has_value());
    auto more = stream.value()->nextResult({});
    QVERIFY(more);
    QVERIFY(!more.value());
}

void TstOdbcProvider::textDriverStreamsInBlocks()
{
    auto connection = openTextDriver();
    if (!connection)
        return;
    // 1200 rows with fetch sizes that do not divide it: every batch has the requested size
    // whatever the block the driver reads.
    auto stream = connection->execute(
        Statement { QStringLiteral("SELECT n, label FROM many.csv ORDER BY n"), {}, 100 }, {});
    QVERIFY2(stream, qPrintable(stream ? QString() : stream.error().detail));
    IResultStream &s = *stream.value();
    QCOMPARE(s.columns().size(), size_t(2));
    qint64 total = 0;
    std::vector<qsizetype> sizes;
    for (int guard = 0; !s.atEnd() && guard < 100; ++guard) {
        auto batch = s.fetch(70, {});
        QVERIFY2(batch, qPrintable(batch ? QString() : batch.error().detail));
        const ResultBatch &b = batch.value();
        for (qsizetype i = 0; i < b.rowCount; ++i) {
            QCOMPARE(b.columns[0].int64At(i), total + i + 1);
            QCOMPARE(textAt(b, 1, i), QStringLiteral("row%1").arg(total + i + 1));
        }
        total += b.rowCount;
        sizes.push_back(b.rowCount);
    }
    QVERIFY(s.atEnd());
    QCOMPARE(total, 1200);
    for (size_t i = 0; i + 1 < sizes.size(); ++i)
        QVERIFY2(sizes[i] == 70, "fetch size not honored");

    // A stream dropped half way does not keep the connection busy.
    {
        auto partial = connection->execute(
            Statement { QStringLiteral("SELECT n FROM many.csv ORDER BY n"), {}, 10 }, {});
        QVERIFY(partial);
        auto first = partial.value()->fetch(10, {});
        QVERIFY(first);
        QCOMPARE(first.value().rowCount, 10);
        QVERIFY(!partial.value()->atEnd());
        // A second statement while this one is open is refused, not mixed up with it.
        auto busy = connection->execute(Statement { QStringLiteral("SELECT 1"), {}, 10 }, {});
        QVERIFY(!busy);
        QCOMPARE(std::move(busy).error().kind, ErrorKind::Internal);
    }
    const auto after = drain(*connection, QStringLiteral("SELECT n FROM many.csv WHERE n = 5"));
    QVERIFY2(after.ok, qPrintable(after.errorText()));
    QCOMPARE(after.rows, 1);
}

void TstOdbcProvider::textDriverFixedColumnsAreReadInBlocks()
{
    auto connection = openTextDriver();
    if (!connection)
        return;
    // Every column has a fixed size, so the whole row is bound and read in blocks of the fetch
    // size (when the driver lets the block size change; if not it falls back to single rows).
    auto stream = connection->execute(
        Statement { QStringLiteral("SELECT n, n * 2 AS twice FROM many.csv ORDER BY n"), {}, 64 },
        {});
    QVERIFY2(stream, qPrintable(stream ? QString() : stream.error().detail));
    IResultStream &s = *stream.value();
    QCOMPARE(s.columns().size(), size_t(2));
    QCOMPARE(s.columns()[0].type, ValueType::Int64);
    qint64 total = 0;
    std::vector<qsizetype> sizes;
    for (int guard = 0; !s.atEnd() && guard < 100; ++guard) {
        auto batch = s.fetch(100, {}); // not a multiple of the block
        QVERIFY2(batch, qPrintable(batch ? QString() : batch.error().detail));
        const ResultBatch &b = batch.value();
        for (qsizetype i = 0; i < b.rowCount; ++i) {
            QCOMPARE(b.columns[0].int64At(i), total + i + 1);
            QVERIFY(!b.columns[1].isNull(i));
        }
        total += b.rowCount;
        sizes.push_back(b.rowCount);
    }
    QCOMPARE(total, 1200);
    for (size_t i = 0; i + 1 < sizes.size(); ++i)
        QCOMPARE(sizes[i], qsizetype(100));

    // Numbers, a double and a timestamp as text, with NULLs from an outer expression.
    const auto d = drain(*connection,
                         QStringLiteral("SELECT id, price, created FROM people.csv ORDER BY id"));
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    QCOMPARE(d.rows, 3);
    const ResultBatch &b = d.batches.front();
    QCOMPARE(b.columns[0].int64At(2), 3);
    QCOMPARE(b.columns[1].doubleAt(2), 0.25);
    QVERIFY2(textAt(b, 2, 1).startsWith(QLatin1String("2026-02-03")), qPrintable(textAt(b, 2, 1)));
}

QTEST_GUILESS_MAIN(TstOdbcProvider)
#include "tst_odbc_provider.moc"
