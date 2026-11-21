#include "mysql_provider.h"

#include "mysql_connection.h"
#include "mysql_text.h"

namespace quarry::mysql {

ProviderInfo MysqlProvider::info() const
{
    // No Schemas: MySQL has no schema level below the database (ARCHITECTURE 6.6).
    return { QStringLiteral("mysql"), QStringLiteral("MySQL / MariaDB"), QVersionNumber(0, 1, 0),
             Capability::MultipleDatabases | Capability::Transactions | Capability::Cancel
                 | Capability::Explain | Capability::Ssh | Capability::Users };
}

std::vector<ConnectionField> MysqlProvider::connectionFields() const
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
    port.defaultValue = 3306;

    ConnectionField database;
    database.key = QStringLiteral("database");
    database.label = Text::tr("Database");
    database.kind = ConnectionField::Kind::Text;

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
        = { QStringLiteral("disable"), QStringLiteral("prefer"), QStringLiteral("require"),
            QStringLiteral("verify-ca"), QStringLiteral("verify-full") };
    sslmode.defaultValue = QStringLiteral("prefer");

    ConnectionField rootCert;
    rootCert.key = QStringLiteral("sslrootcert");
    rootCert.label = Text::tr("SSL root certificate");
    rootCert.kind = ConnectionField::Kind::Path;
    rootCert.advanced = true;

    return { host, port, database, user, sslmode, rootCert };
}

std::unique_ptr<IConnection> MysqlProvider::createConnection(const ConnectionConfig &config,
                                                             const Secrets &secrets) const
{
    return std::make_unique<MysqlConnection>(config, secrets.password);
}

} // namespace quarry::mysql
