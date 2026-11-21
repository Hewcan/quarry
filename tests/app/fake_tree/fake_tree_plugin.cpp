#include <quarry/provider_api.h>

#include <QObject>

// A test-only engine ("FakeTree") that lists what SQLite cannot: several databases, a database
// with a schema, relations that arrive mixed (tables, views, a materialized view, a sequence),
// the indexes of a whole database and the server's users. Which tree it answers:
//   (root)             alpha, beta, gamma           (databases)
//   {alpha}            customers, orders (tables), recent_orders (view), totals (materialized
//                      view), seq_orders (sequence)
//   {beta}             nothing
//   {gamma}            the schema "public"
//   {gamma, public}    accounts (table), account_view (view)
//   {<db>[, schema], "Indexes"}   the indexes of alpha and of gamma.public
//   {"Users"}          admin, reader; an Error when the connection's "users" value is "fail"
namespace {

using namespace quarry;

class FakeDialect : public ISqlDialect
{
public:
    QString quoteIdentifier(QStringView name) const override { return name.toString(); }
    QString quoteLiteral(QStringView text) const override { return text.toString(); }
    QString highlightingDefinition() const override { return QStringLiteral("SQL"); }
    QStringList keywords() const override { return {}; }
    QStringList builtinFunctions() const override { return {}; }
    std::vector<StatementRange> splitStatements(QStringView) const override { return {}; }
    QString limitClause(qint64, qint64) const override { return {}; }
    QString placeholder(int) const override { return QStringLiteral("?"); }
    QString qualifiedName(const ObjectPath &) const override { return {}; }
    std::optional<QString> rowIdentifier() const override { return std::nullopt; }
};

SchemaNode node(QStringList parts, ObjectKind kind, const QString &name, bool hasChildren,
                const QString &detail = {})
{
    SchemaNode out;
    out.path = ObjectPath { std::move(parts), kind };
    out.name = name;
    out.hasChildren = hasChildren;
    out.detail = detail;
    return out;
}

class FakeConnection : public IConnection
{
public:
    explicit FakeConnection(bool failUsers)
        : m_failUsers(failUsers)
    { }

    Result<ServerInfo> open(const CancellationToken &) override
    {
        m_open = true;
        ServerInfo info;
        info.product = QStringLiteral("FakeTree");
        info.version = QStringLiteral("1.0");
        return info;
    }
    void close() override { m_open = false; }
    bool isOpen() const override { return m_open; }

    Result<std::vector<SchemaNode>> children(const ObjectPath &parent,
                                             const CancellationToken &) override
    {
        const QStringList &p = parent.parts;
        std::vector<SchemaNode> out;
        if (p.isEmpty()) {
            for (const char *db : { "alpha", "beta", "gamma" }) {
                const QString name = QLatin1String(db);
                out.push_back(node({ name }, ObjectKind::Database, name, true));
            }
            return out;
        }
        if (parent.kind == ObjectKind::Folder && p == QStringList { QStringLiteral("Users") }) {
            if (m_failUsers)
                return fail(Error { ErrorKind::Permission,
                                    QStringLiteral("Not allowed to list the users."),
                                    QStringLiteral("permission denied for table users"),
                                    {},
                                    {} });
            out.push_back(node({ QStringLiteral("Users"), QStringLiteral("admin") },
                               ObjectKind::User, QStringLiteral("admin"), false,
                               QStringLiteral("superuser")));
            out.push_back(node({ QStringLiteral("Users"), QStringLiteral("reader") },
                               ObjectKind::User, QStringLiteral("reader"), false,
                               QStringLiteral("can log in")));
            return out;
        }
        if (parent.kind == ObjectKind::Folder && p.last() == QLatin1String("Indexes")
            && (p.size() == 2 || p.size() == 3)) {
            if (p.first() == QLatin1String("alpha") && p.size() == 2) {
                out.push_back(node({ QStringLiteral("alpha"), QStringLiteral("customers"),
                                     QStringLiteral("customers_name_idx") },
                                   ObjectKind::Index, QStringLiteral("customers_name_idx"), false,
                                   QStringLiteral("customers (name)")));
                out.push_back(node({ QStringLiteral("alpha"), QStringLiteral("orders"),
                                     QStringLiteral("orders_date_idx") },
                                   ObjectKind::Index, QStringLiteral("orders_date_idx"), false,
                                   QStringLiteral("orders (placed_on) unique")));
            } else if (p.first() == QLatin1String("gamma") && p.size() == 3) {
                out.push_back(node({ QStringLiteral("gamma"), QStringLiteral("public"),
                                     QStringLiteral("accounts"), QStringLiteral("accounts_pkey") },
                                   ObjectKind::Index, QStringLiteral("accounts_pkey"), false,
                                   QStringLiteral("accounts (id) unique")));
            }
            return out;
        }
        if (parent.kind == ObjectKind::Database && p.size() == 1) {
            const QString db = p.first();
            if (db == QLatin1String("alpha")) {
                out.push_back(node({ db, QStringLiteral("customers") }, ObjectKind::Table,
                                   QStringLiteral("customers"), true));
                out.push_back(node({ db, QStringLiteral("orders") }, ObjectKind::Table,
                                   QStringLiteral("orders"), true));
                out.push_back(node({ db, QStringLiteral("recent_orders") }, ObjectKind::View,
                                   QStringLiteral("recent_orders"), true));
                out.push_back(node({ db, QStringLiteral("totals") }, ObjectKind::MaterializedView,
                                   QStringLiteral("totals"), true));
                out.push_back(node({ db, QStringLiteral("seq_orders") }, ObjectKind::Sequence,
                                   QStringLiteral("seq_orders"), false));
            } else if (db == QLatin1String("gamma")) {
                out.push_back(node({ db, QStringLiteral("public") }, ObjectKind::Schema,
                                   QStringLiteral("public"), true));
            }
            return out;
        }
        if (parent.kind == ObjectKind::Schema && p.size() == 2) {
            out.push_back(node({ p.at(0), p.at(1), QStringLiteral("accounts") }, ObjectKind::Table,
                               QStringLiteral("accounts"), true));
            out.push_back(node({ p.at(0), p.at(1), QStringLiteral("account_view") },
                               ObjectKind::View, QStringLiteral("account_view"), true));
            return out;
        }
        return out;
    }
    Result<TableDetails> describe(const ObjectPath &, const CancellationToken &) override
    {
        return fail(Error { ErrorKind::Unsupported, QStringLiteral("Unsupported."), {}, {}, {} });
    }
    Result<std::unique_ptr<IResultStream>> execute(const Statement &,
                                                   const CancellationToken &) override
    {
        return fail(Error { ErrorKind::Unsupported, QStringLiteral("Unsupported."), {}, {}, {} });
    }
    void requestCancel() override { }
    Result<void> begin() override { return unsupported(); }
    Result<void> commit() override { return unsupported(); }
    Result<void> rollback() override { return unsupported(); }

private:
    static Unexpected unsupported()
    {
        return fail(Error { ErrorKind::Unsupported, QStringLiteral("Unsupported."), {}, {}, {} });
    }
    bool m_failUsers;
    bool m_open = false;
};

class FakeProvider : public IDatabaseProvider
{
public:
    ProviderInfo info() const override
    {
        return { QStringLiteral("faketree"), QStringLiteral("FakeTree"), QVersionNumber(0, 1, 0),
                 Capabilities(Capability::MultipleDatabases | Capability::Schemas
                              | Capability::Users) };
    }
    std::vector<ConnectionField> connectionFields() const override { return {}; }
    const ISqlDialect &dialect() const override { return m_dialect; }
    std::unique_ptr<IConnection> createConnection(const ConnectionConfig &config,
                                                  const Secrets &) const override
    {
        return std::make_unique<FakeConnection>(
            config.values.value(QStringLiteral("users")).toString() == QLatin1String("fail"));
    }

private:
    FakeDialect m_dialect;
};

} // namespace

class FakeTreePlugin : public QObject, public quarry::IProviderPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QUARRY_PROVIDER_IID FILE "fake_tree.json")
    Q_INTERFACES(quarry::IProviderPlugin)

public:
    std::unique_ptr<quarry::IDatabaseProvider> createProvider() override
    {
        return std::make_unique<FakeProvider>();
    }
};

#include "fake_tree_plugin.moc"
