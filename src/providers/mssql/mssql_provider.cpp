#include "mssql_provider.h"

#include "mssql_connection.h"
#include "mssql_text.h"

namespace quarry::mssql {

ProviderInfo MssqlProvider::info() const
{
    return { QStringLiteral("mssql"), QStringLiteral("SQL Server"), QVersionNumber(0, 1, 0),
             Capability::Schemas | Capability::MultipleDatabases | Capability::Transactions
                 | Capability::Cancel | Capability::MultipleResultSets | Capability::Ssh };
}

std::vector<ConnectionField> MssqlProvider::connectionFields() const
{
    ConnectionField server;
    server.key = QStringLiteral("server");
    server.label = Text::tr("Server");
    server.kind = ConnectionField::Kind::Text;
    server.defaultValue = QStringLiteral("localhost");

    ConnectionField port;
    port.key = QStringLiteral("port");
    port.label = Text::tr("Port");
    port.kind = ConnectionField::Kind::Number;
    port.defaultValue = 1433;

    ConnectionField database;
    database.key = QStringLiteral("database");
    database.label = Text::tr("Database");
    database.kind = ConnectionField::Kind::Text;

    // Not required by the form: Windows authentication has no user. Checked when connecting.
    ConnectionField user;
    user.key = QStringLiteral("user");
    user.label = Text::tr("User");
    user.kind = ConnectionField::Kind::Text;

    ConnectionField authentication;
    authentication.key = QStringLiteral("authentication");
    authentication.label = Text::tr("Authentication");
    authentication.kind = ConnectionField::Kind::Enum;
    authentication.enumValues = { QStringLiteral("sql"), QStringLiteral("windows") };
    authentication.defaultValue = QStringLiteral("sql");

    ConnectionField encrypt;
    encrypt.key = QStringLiteral("encrypt");
    encrypt.label = Text::tr("Encryption");
    encrypt.kind = ConnectionField::Kind::Enum;
    encrypt.enumValues
        = { QStringLiteral("mandatory"), QStringLiteral("optional"), QStringLiteral("strict") };
    encrypt.defaultValue = QStringLiteral("mandatory");

    ConnectionField trust;
    trust.key = QStringLiteral("trustservercertificate");
    trust.label = Text::tr("Trust server certificate");
    trust.kind = ConnectionField::Kind::Bool;
    trust.defaultValue = false;
    trust.advanced = true;

    ConnectionField driver;
    driver.key = QStringLiteral("driver");
    driver.label = Text::tr("ODBC driver");
    driver.kind = ConnectionField::Kind::Text;
    driver.advanced = true;

    return { server, port, database, user, authentication, encrypt, trust, driver };
}

std::unique_ptr<IConnection> MssqlProvider::createConnection(const ConnectionConfig &config,
                                                             const Secrets &secrets) const
{
    return std::make_unique<MssqlConnection>(config, secrets.password);
}

} // namespace quarry::mssql
