#include "generic_settings.h"

#include "odbc_support.h"
#include "odbc_text.h"

namespace quarry::odbc {

Result<GenericSettings> buildGenericSettings(const QVariantMap &values, const QString &password)
{
    GenericSettings out;
    auto text = [&values](const char *key) {
        return values.value(QString::fromLatin1(key)).toString().trimmed();
    };
    out.dsn = text("dsn");
    out.driver = text("driver");
    out.extra = text("connectionString");
    out.user = text("user");
    out.password = password;
    if (out.dsn.isEmpty() && out.driver.isEmpty() && out.extra.isEmpty()) {
        return fail(Error { ErrorKind::Connection,
                            Text::tr("Enter a data source name, a driver or a connection string."),
                            {},
                            {},
                            {} });
    }
    return out;
}

QString buildGenericConnectionString(const GenericSettings &settings)
{
    QString out;
    if (!settings.dsn.isEmpty())
        out += connectionStringPair(QStringLiteral("DSN"), settings.dsn);
    if (!settings.driver.isEmpty())
        out += connectionStringBracedPair(QStringLiteral("Driver"), settings.driver);
    if (!settings.extra.isEmpty()) {
        out += settings.extra;
        if (!settings.extra.endsWith(QLatin1Char(';')))
            out += QLatin1Char(';');
    }
    if (!settings.user.isEmpty())
        out += connectionStringPair(QStringLiteral("UID"), settings.user);
    if (!settings.password.isEmpty())
        out += connectionStringBracedPair(QStringLiteral("PWD"), settings.password);
    return out;
}

QString genericTarget(const GenericSettings &settings)
{
    if (!settings.dsn.isEmpty())
        return settings.dsn;
    if (!settings.driver.isEmpty())
        return settings.driver;
    return QStringLiteral("ODBC");
}

} // namespace quarry::odbc
