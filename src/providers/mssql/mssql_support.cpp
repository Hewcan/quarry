#include "mssql_support.h"

#include "mssql_text.h"
#include "odbc_support.h"

#include <QRegularExpression>

namespace quarry::mssql {

namespace {

constexpr int kDefaultPort = 1433;

Error invalid(const QString &message)
{
    return Error { ErrorKind::Connection, message, {}, {}, {} };
}

QString yesNo(bool value)
{
    return value ? QStringLiteral("yes") : QStringLiteral("no");
}

} // namespace

Result<ConnSettings> buildConnSettings(const QVariantMap &values, const QString &password)
{
    ConnSettings out;
    auto text = [&values](const char *key) {
        return values.value(QString::fromLatin1(key)).toString().trimmed();
    };

    QString server = text("server");
    if (server.isEmpty())
        server = QStringLiteral("localhost");
    if (const qsizetype slash = server.indexOf(QLatin1Char('\\')); slash >= 0) {
        out.host = server.left(slash).trimmed();
        out.instance = server.mid(slash + 1).trimmed();
        if (out.host.isEmpty() || out.instance.isEmpty())
            return fail(invalid(Text::tr("The server must be written host or host\\instance.")));
    } else {
        out.host = server;
    }

    if (const QString raw = text("port"); !raw.isEmpty()) {
        bool ok = false;
        const int port = raw.toInt(&ok);
        if (!ok || port < 1 || port > 65535)
            return fail(invalid(Text::tr("The port must be a number between 1 and 65535.")));
        out.port = port;
    } else {
        out.port = kDefaultPort;
    }

    out.database = text("database");
    out.user = text("user");
    out.password = password;
    out.driver = text("driver");
    out.trustServerCertificate = values.value(QStringLiteral("trustservercertificate")).toBool();

    const QString authentication = text("authentication").toLower();
    if (authentication.isEmpty() || authentication == QLatin1String("sql")) {
        out.windowsAuthentication = false;
    } else if (authentication == QLatin1String("windows")) {
        out.windowsAuthentication = true;
    } else {
        return fail(invalid(Text::tr("The authentication must be \"sql\" or \"windows\".")));
    }
    if (!out.windowsAuthentication && out.user.isEmpty())
        return fail(invalid(Text::tr("Enter a user name for SQL Server authentication.")));

    const QString encrypt = text("encrypt").toLower();
    if (encrypt.isEmpty() || encrypt == QLatin1String("mandatory"))
        out.encrypt = EncryptMode::Mandatory;
    else if (encrypt == QLatin1String("optional"))
        out.encrypt = EncryptMode::Optional;
    else if (encrypt == QLatin1String("strict"))
        out.encrypt = EncryptMode::Strict;
    else
        return fail(invalid(Text::tr("The encryption must be mandatory, optional or strict.")));
    return out;
}

std::optional<QString> pickDriver(const QString &requested, const QStringList &installed)
{
    QString asked = requested.trimmed();
    if (!asked.isEmpty())
        return asked;
    for (const QString &wanted : { QStringLiteral("ODBC Driver 18 for SQL Server"),
                                   QStringLiteral("ODBC Driver 17 for SQL Server") }) {
        for (const QString &name : installed) {
            if (name.compare(wanted, Qt::CaseInsensitive) == 0)
                return name;
        }
    }
    return std::nullopt;
}

bool supportsStrictEncryption(const QString &driver)
{
    static const QRegularExpression kVersion(QStringLiteral("(\\d+)"));
    const QRegularExpressionMatch match = kVersion.match(driver);
    return match.hasMatch() && match.captured(1).toInt() >= 18;
}

QString serverSpec(const ConnSettings &settings)
{
    if (!settings.instance.isEmpty())
        return settings.host + QLatin1Char('\\') + settings.instance;
    return QStringLiteral("%1,%2").arg(settings.host).arg(settings.port);
}

Result<QString> buildConnectionString(const ConnSettings &settings, const QString &driver)
{
    if (settings.encrypt == EncryptMode::Strict && !supportsStrictEncryption(driver)) {
        return fail(Error { ErrorKind::Unsupported,
                            Text::tr("Strict encryption needs the Microsoft ODBC Driver 18 for "
                                     "SQL Server."),
                            {},
                            {},
                            {} });
    }
    QString out = odbc::connectionStringBracedPair(QStringLiteral("Driver"), driver);
    out += odbc::connectionStringPair(QStringLiteral("Server"), serverSpec(settings));
    if (!settings.database.isEmpty())
        out += odbc::connectionStringPair(QStringLiteral("Database"), settings.database);
    if (settings.windowsAuthentication) {
        out += QStringLiteral("Trusted_Connection=yes;");
    } else {
        out += odbc::connectionStringPair(QStringLiteral("UID"), settings.user);
        out += odbc::connectionStringBracedPair(QStringLiteral("PWD"), settings.password);
    }
    switch (settings.encrypt) {
    case EncryptMode::Mandatory:
        out += QStringLiteral("Encrypt=yes;");
        break;
    case EncryptMode::Optional:
        out += QStringLiteral("Encrypt=no;");
        break;
    case EncryptMode::Strict:
        out += QStringLiteral("Encrypt=strict;");
        break;
    }
    out += QStringLiteral("TrustServerCertificate=") + yesNo(settings.trustServerCertificate)
        + QLatin1Char(';');
    out += QStringLiteral("Application Name=Quarry;");
    out += QStringLiteral("LoginTimeout=%1;").arg(odbc::kLoginTimeoutSeconds);
    return out;
}

ValueType valueTypeForTypeName(QStringView typeName)
{
    const QString name = typeName.toString().toLower();
    static const QStringList kInteger { QStringLiteral("tinyint"), QStringLiteral("smallint"),
                                        QStringLiteral("int"), QStringLiteral("bigint") };
    static const QStringList kDecimal { QStringLiteral("decimal"), QStringLiteral("numeric"),
                                        QStringLiteral("money"), QStringLiteral("smallmoney") };
    static const QStringList kText { QStringLiteral("char"),     QStringLiteral("varchar"),
                                     QStringLiteral("text"),     QStringLiteral("nchar"),
                                     QStringLiteral("nvarchar"), QStringLiteral("ntext"),
                                     QStringLiteral("sysname"),  QStringLiteral("xml") };
    static const QStringList kBytes { QStringLiteral("binary"), QStringLiteral("varbinary"),
                                      QStringLiteral("image"), QStringLiteral("timestamp"),
                                      QStringLiteral("rowversion") };
    static const QStringList kDateTime { QStringLiteral("datetime"), QStringLiteral("datetime2"),
                                         QStringLiteral("smalldatetime") };
    if (kInteger.contains(name))
        return ValueType::Int64;
    if (name == QLatin1String("real") || name == QLatin1String("float"))
        return ValueType::Double;
    if (kDecimal.contains(name))
        return ValueType::Decimal;
    if (name == QLatin1String("bit"))
        return ValueType::Bool;
    if (kText.contains(name))
        return ValueType::Text;
    if (kBytes.contains(name))
        return ValueType::Bytes;
    if (name == QLatin1String("date"))
        return ValueType::Date;
    if (name == QLatin1String("time"))
        return ValueType::Time;
    if (kDateTime.contains(name))
        return ValueType::DateTime;
    if (name == QLatin1String("datetimeoffset"))
        return ValueType::DateTimeTz;
    if (name == QLatin1String("uniqueidentifier"))
        return ValueType::Uuid;
    if (name == QLatin1String("json"))
        return ValueType::Json;
    return ValueType::Other; // sql_variant, geography, geometry, hierarchyid, CLR types
}

QString nativeTypeName(const QString &typeName, int maxLength, int precision, int scale)
{
    const QString name = typeName.toLower();
    auto length = [maxLength](int divisor) {
        return maxLength < 0 ? QStringLiteral("max") : QString::number(maxLength / divisor);
    };
    if (name == QLatin1String("nchar") || name == QLatin1String("nvarchar"))
        return QStringLiteral("%1(%2)").arg(typeName, length(2));
    if (name == QLatin1String("char") || name == QLatin1String("varchar")
        || name == QLatin1String("binary") || name == QLatin1String("varbinary"))
        return QStringLiteral("%1(%2)").arg(typeName, length(1));
    if (name == QLatin1String("decimal") || name == QLatin1String("numeric"))
        return QStringLiteral("%1(%2,%3)").arg(typeName).arg(precision).arg(scale);
    if (name == QLatin1String("datetime2") || name == QLatin1String("time")
        || name == QLatin1String("datetimeoffset"))
        return QStringLiteral("%1(%2)").arg(typeName).arg(scale);
    return typeName;
}

} // namespace quarry::mssql
