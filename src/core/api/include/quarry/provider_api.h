#pragma once

#include <quarry/cancellation.h>
#include <quarry/explain_plan.h>
#include <quarry/object_path.h>
#include <quarry/result.h>
#include <quarry/result_batch.h>
#include <quarry/value_type.h>

#include <QFlags>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QVersionNumber>
#include <QtPlugin>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace quarry {

inline constexpr int kProviderApiVersion = 1;

enum class Capability : uint32_t {
    Schemas = 1U << 0,
    MultipleDatabases = 1U << 1,
    Transactions = 1U << 2,
    Cancel = 1U << 3,
    Explain = 1U << 4,
    ServerSideCursor = 1U << 5,
    MultipleResultSets = 1U << 6,
    Ssh = 1U << 7,
    Users = 1U << 8 // children() of the Users folder lists the server's logins or roles
};
Q_DECLARE_FLAGS(Capabilities, Capability)
Q_DECLARE_OPERATORS_FOR_FLAGS(Capabilities)

struct ProviderInfo
{
    QString id; // "postgres"
    QString displayName; // "PostgreSQL"
    QVersionNumber version;
    Capabilities caps;
};

// Describes the connection form; the app builds it without knowing the engine.
struct ConnectionField
{
    enum class Kind { Text, Number, Path, Enum, Bool, Secret };

    QString key; // "host", "port", "database", "file", "sslmode"...
    QString label; // translatable
    Kind kind = Kind::Text;
    QVariant defaultValue;
    QStringList enumValues;
    bool required = false;
    bool advanced = false; // hidden under "Advanced options"
};

struct ConnectionConfig
{
    QUuid id;
    QVariantMap values; // no secrets
};

struct Secrets // comes from CredentialStore, never persisted
{
    // Not an aggregate: `Secrets { password }` stays valid without -Wmissing-field-initializers.
    Secrets() = default;
    Secrets(QString password, QString sshPassword = {}, QString sshPassphrase = {})
        : password(std::move(password))
        , sshPassword(std::move(sshPassword))
        , sshPassphrase(std::move(sshPassphrase))
    { }

    QString password;
    // SSH tunnel (H8): only the one that matches SshTunnelConfig::auth is filled in.
    QString sshPassword;
    QString sshPassphrase;
};

struct SchemaNode
{
    ObjectPath path;
    QString name;
    bool hasChildren = false; // draws the expander without loading children
    QString detail; // "int4 NOT NULL", "~12 345 rows"...
};

struct ColumnDef
{
    QString name;
    ValueType type = ValueType::Other;
    QString nativeType;
    bool nullable = true;
    QString defaultExpression;
    QString comment; // "" = none (or the engine has no comments)
    // nullopt: the provider cannot tell; the UI infers it from the type and the default.
    std::optional<bool> autoIncrement;
};

struct IndexDef
{
    QString name;
    QStringList columns;
    bool unique = false;
};

struct ForeignKeyDef
{
    QString name;
    QStringList columns;
    ObjectPath referencedTable;
    QStringList referencedColumns;
};

struct TriggerDef
{
    QString name;
    QString timing; // "BEFORE", "AFTER", "INSTEAD OF"
    QString event; // "INSERT", "UPDATE OR DELETE"...
    QString definition; // the engine's own SQL, "" when it does not give it
};

struct TableDetails
{
    std::vector<ColumnDef> columns;
    std::vector<IndexDef> indexes;
    std::vector<ForeignKeyDef> foreignKeys;
    std::optional<QStringList> primaryKey;
    // Table properties (QUA-23). nullopt = the provider does not read it for its engine.
    QString comment;
    std::optional<bool> strict; // SQLite STRICT tables; nullopt elsewhere
    std::optional<std::vector<TriggerDef>> triggers;
    // Incoming foreign keys: `referencedTable` is the table that points here, `columns` are its
    // columns and `referencedColumns` the columns of this table.
    std::optional<std::vector<ForeignKeyDef>> references;
};

struct ServerInfo
{
    QString product; // "PostgreSQL"
    QString version; // "18.1"
};

struct Statement
{
    QString sql;
    QVariantList parameters; // positional; each dialect translates placeholders
    int fetchSize = 500;
};

// Half-open range [begin, end) into the script text.
struct StatementRange
{
    qsizetype begin = 0;
    qsizetype end = 0;
};

enum class ExplainMode : uint8_t { Plan, Analyze };

class IResultStream
{
public:
    virtual ~IResultStream() = default;
    virtual const std::vector<ColumnInfo> &columns() const = 0; // empty if no rows are returned
    // Blocks until maxRows rows are available or the end is reached. An empty batch with
    // atEnd() == true marks the end. Must discard any pending cancel request on entry (see
    // IConnection::requestCancel()).
    virtual Result<ResultBatch> fetch(qsizetype maxRows, const CancellationToken &) = 0;
    virtual bool atEnd() const = 0;
    virtual std::optional<qint64> affectedRows() const = 0; // UPDATE/DELETE/INSERT
    virtual QStringList notices() const = 0; // RAISE NOTICE, warnings
    // Several result sets (SQL Server, MySQL CALL...). Returns false if there are no more.
    virtual Result<bool> nextResult(const CancellationToken &) = 0;
};

class IConnection
{
public:
    virtual ~IConnection() = default;
    virtual Result<ServerInfo> open(const CancellationToken &) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // On-demand introspection: only the direct children of `parent`. Two folders ask for a whole
    // list at once (H12 phase A, ARCHITECTURE 6.12):
    //  - {Folder, ["Users"]}: the logins or roles of the server, as ObjectKind::User nodes
    //    {"Users", name} with `detail` (e.g. "can log in", "superuser"). Only asked when the
    //    provider has Capability::Users. A failure (usually permissions) is an Error and the
    //    app shows it in that folder.
    //  - {Folder, <the parent of the relations> + ["Indexes"]}: every index of the tables under
    //    that Database (no schemas) or Schema node, as ObjectKind::Index leaves with the same path
    //    shape the per-table "Indexes" folder already uses, `name` = the index name and `detail`
    //    = "table (col1, col2)" plus "unique" when it is. Providers that cannot list them return
    //    an empty vector.
    virtual Result<std::vector<SchemaNode>> children(const ObjectPath &parent,
                                                     const CancellationToken &)
        = 0;
    virtual Result<TableDetails> describe(const ObjectPath &table, const CancellationToken &) = 0;

    // Streaming execution. The stream keeps the connection busy until it is destroyed.
    virtual Result<std::unique_ptr<IResultStream>> execute(const Statement &,
                                                           const CancellationToken &)
        = 0;

    // Thread-safe: called from the UI thread while another thread is in execute()/fetch().
    // Must make the blocked call return soon with ErrorKind::Cancelled. It affects only the
    // call in flight: implementations must discard any pending cancel request at the start of
    // each open/children/describe/execute/fetch call, so a late cancel never aborts the next
    // statement.
    virtual void requestCancel() = 0;

    // Explicit transactions (milestone 2, data editing)
    virtual Result<void> begin() = 0;
    virtual Result<void> commit() = 0;
    virtual Result<void> rollback() = 0;
};

class ISqlDialect
{
public:
    virtual ~ISqlDialect() = default;
    virtual QString quoteIdentifier(QStringView) const = 0; // "a""b" | `a``b` | [a]]b]
    virtual QString quoteLiteral(QStringView) const = 0;
    virtual QString highlightingDefinition() const = 0; // KSyntaxHighlighting definition name
    virtual QStringList keywords() const = 0; // for autocompletion
    // Built-in functions of the engine for autocompletion: lowercase, without parentheses
    // ("count", "coalesce"). Static list, no database query.
    virtual QStringList builtinFunctions() const = 0;
    // Splits a script into statements: handles $$...$$, DELIMITER, comments and strings.
    virtual std::vector<StatementRange> splitStatements(QStringView script) const = 0;
    virtual QString limitClause(qint64 limit, qint64 offset) const = 0;
    // Statement that returns the plan of `sql` (one SELECT, no trailing `;`), or empty when the
    // engine has no EXPLAIN. `product` is ServerInfo::product, for engines whose syntax depends on
    // the server (MySQL vs MariaDB). Plan only plans; Analyze executes `sql`.
    virtual QString explainStatement(const QString &sql, ExplainMode mode,
                                     const QString &product) const
    {
        Q_UNUSED(sql)
        Q_UNUSED(mode)
        Q_UNUSED(product)
        return {};
    }
    // Turns the rows that explainStatement() returned into a plan. `output` has the cells of a row
    // joined by a tab and the rows joined by a newline (JSON engines return one cell). Never
    // throws; a malformed output is an Error (the UI then shows the text).
    virtual Result<ExplainPlan> parseExplain(const QString &output, ExplainMode mode,
                                             const QString &product) const
    {
        Q_UNUSED(output)
        Q_UNUSED(mode)
        Q_UNUSED(product)
        return fail(Error {
            ErrorKind::Unsupported, QStringLiteral("This engine has no query plan."), {}, {}, {} });
    }
    // Positional parameter marker; index is 1-based: "?" | "$1".
    virtual QString placeholder(int index) const = 0;
    // Quoted name of a table as the engine expects it in FROM/INSERT: "t" | "\"public\".\"t\"".
    virtual QString qualifiedName(const ObjectPath &table) const = 0;
    // Hidden column that identifies a row of a table without primary key ("rowid" | "ctid").
    virtual std::optional<QString> rowIdentifier() const = 0;
};

class IDatabaseProvider
{
public:
    virtual ~IDatabaseProvider() = default;
    virtual ProviderInfo info() const = 0;
    virtual std::vector<ConnectionField> connectionFields() const = 0;
    virtual const ISqlDialect &dialect() const = 0;
    // Creates a connection without opening it. Does not touch the network.
    virtual std::unique_ptr<IConnection> createConnection(const ConnectionConfig &,
                                                          const Secrets &) const
        = 0;
};

class IProviderPlugin
{
public:
    virtual ~IProviderPlugin() = default;
    virtual std::unique_ptr<IDatabaseProvider> createProvider() = 0;
};

} // namespace quarry

#define QUARRY_PROVIDER_IID "dev.quarry.IProviderPlugin/1"
Q_DECLARE_INTERFACE(quarry::IProviderPlugin, QUARRY_PROVIDER_IID)
