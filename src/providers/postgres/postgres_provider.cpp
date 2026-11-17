#include "postgres_provider.h"

#include "postgres_connection.h"
#include "postgres_text.h"

namespace quarry::postgres {

ProviderInfo PostgresProvider::info() const
{
    // MultipleDatabases is left out on purpose: one profile shows one database (ARCHITECTURE 6.3).
    return { QStringLiteral("postgres"), QStringLiteral("PostgreSQL"), QVersionNumber(0, 1, 0),
             Capability::Schemas | Capability::Transactions | Capability::Cancel
                 | Capability::Explain | Capability::Ssh };
}

std::vector<ConnectionField> PostgresProvider::connectionFields() const
{
    ConnectionField host;
    host.key = QStringLiteral("host");
    host.label = Text::tr("Host");
    host.kind = ConnectionField::Kind::Text;
    host.defaultValue = QStringLiteral("localhost");

    ConnectionField port;
    port.key = QStringLiteral("port");
    port.label = Text::tr("Port");
    port.kind = ConnectionField::Kind::Number;
    port.defaultValue = 5432;

    ConnectionField database;
    database.key = QStringLiteral("database");
    database.label = Text::tr("Database");
    database.kind = ConnectionField::Kind::Text;
    database.required = true;

    ConnectionField user;
    user.key = QStringLiteral("user");
    user.label = Text::tr("User");
    user.kind = ConnectionField::Kind::Text;
    user.required = true;

    ConnectionField sslmode;
    sslmode.key = QStringLiteral("sslmode");
    sslmode.label = Text::tr("SSL mode");
    sslmode.kind = ConnectionField::Kind::Enum;
    sslmode.enumValues
        = { QStringLiteral("disable"), QStringLiteral("allow"),     QStringLiteral("prefer"),
            QStringLiteral("require"), QStringLiteral("verify-ca"), QStringLiteral("verify-full") };
    sslmode.defaultValue = QStringLiteral("prefer");

    ConnectionField rootCert;
    rootCert.key = QStringLiteral("sslrootcert");
    rootCert.label = Text::tr("SSL root certificate");
    rootCert.kind = ConnectionField::Kind::Path;
    rootCert.advanced = true;

    return { host, port, database, user, sslmode, rootCert };
}

std::unique_ptr<IConnection> PostgresProvider::createConnection(const ConnectionConfig &config,
                                                                const Secrets &secrets) const
{
    return std::make_unique<PostgresConnection>(config, secrets.password);
}

} // namespace quarry::postgres
