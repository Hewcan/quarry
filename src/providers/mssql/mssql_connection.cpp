#include "mssql_connection.h"

#include "mssql_support.h"
#include "mssql_text.h"
#include "odbc_state.h"

namespace quarry::mssql {

Result<odbc::OdbcConnection::ConnectRequest> MssqlConnection::connectRequest()
{
    auto settings = buildConnSettings(m_config.values, m_password);
    if (!settings)
        return fail(std::move(settings).error());
    const QStringList installed = odbc::installedDrivers();
    const std::optional<QString> driver = pickDriver(settings.value().driver, installed);
    if (!driver) {
        Error error;
        error.kind = ErrorKind::Connection;
        error.message = Text::tr("Install the Microsoft ODBC Driver 18 for SQL Server.");
        error.detail = installed.isEmpty()
            ? Text::tr("No ODBC driver is registered.")
            : Text::tr("Registered ODBC drivers: %1").arg(installed.join(QLatin1String(", ")));
        return fail(std::move(error));
    }
    auto text = buildConnectionString(settings.value(), driver.value_or(QString()));
    if (!text)
        return fail(std::move(text).error());
    return ConnectRequest { std::move(text).value(), driver.value_or(QString()) };
}

QString MssqlConnection::productName(const QString &dbmsName) const
{
    // The driver says "Microsoft SQL Server".
    return dbmsName.contains(QLatin1String("SQL Server"), Qt::CaseInsensitive)
        ? QStringLiteral("SQL Server")
        : dbmsName;
}

MssqlConnection::MssqlConnection(ConnectionConfig config, QString password)
    : m_config(std::move(config))
    , m_password(std::move(password))
{ }

QString MssqlConnection::inDatabase(const QString &sql, const QString &database) const
{
    return QString(sql).replace(QLatin1String("%1"), m_dialect.quoteIdentifier(database));
}

} // namespace quarry::mssql
