#include "odbc_provider.h"

#include "generic_connection.h"
#include "odbc_text.h"

namespace quarry::odbc {

ProviderInfo OdbcProvider::info() const
{
    // No Schemas or MultipleDatabases: the tree adapts to what the driver reports, and the
    // capabilities are about the engine, which is unknown here.
    return { QStringLiteral("odbc"), QStringLiteral("ODBC (generic)"), QVersionNumber(0, 1, 0),
             Capability::Transactions | Capability::Cancel | Capability::MultipleResultSets };
}

std::vector<ConnectionField> OdbcProvider::connectionFields() const
{
    ConnectionField dsn;
    dsn.key = QStringLiteral("dsn");
    dsn.label = Text::tr("Data source name (DSN)");
    dsn.kind = ConnectionField::Kind::Text;

    ConnectionField driver;
    driver.key = QStringLiteral("driver");
    driver.label = Text::tr("Driver");
    driver.kind = ConnectionField::Kind::Text;

    ConnectionField extra;
    extra.key = QStringLiteral("connectionString");
    extra.label = Text::tr("Connection string");
    extra.kind = ConnectionField::Kind::Text;
    extra.advanced = true;

    ConnectionField user;
    user.key = QStringLiteral("user");
    user.label = Text::tr("User");
    user.kind = ConnectionField::Kind::Text;

    return { dsn, driver, extra, user };
}

std::unique_ptr<IConnection> OdbcProvider::createConnection(const ConnectionConfig &config,
                                                            const Secrets &secrets) const
{
    return std::make_unique<GenericConnection>(config, secrets.password);
}

} // namespace quarry::odbc
