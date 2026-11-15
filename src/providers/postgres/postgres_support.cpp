#include "postgres_support.h"

#include "postgres_text.h"

#include <QStringList>

#include <cmath>
#include <limits>

namespace quarry::postgres {

namespace {

Error connectionError(const QString &message)
{
    return Error { ErrorKind::Connection, message, {}, {}, {} };
}

bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

struct OidInfo
{
    unsigned oid;
    ValueType type;
    const char *name;
};

constexpr OidInfo kOids[] = {
    { 16, ValueType::Bool, "bool" },
    { 17, ValueType::Bytes, "bytea" },
    { 19, ValueType::Text, "name" },
    { 20, ValueType::Int64, "int8" },
    { 21, ValueType::Int64, "int2" },
    { 23, ValueType::Int64, "int4" },
    { 25, ValueType::Text, "text" },
    { 114, ValueType::Json, "json" },
    { 700, ValueType::Double, "float4" },
    { 701, ValueType::Double, "float8" },
    { 705, ValueType::Text, "unknown" },
    { 1042, ValueType::Text, "bpchar" },
    { 1043, ValueType::Text, "varchar" },
    { 1082, ValueType::Date, "date" },
    { 1083, ValueType::Time, "time" },
    { 1114, ValueType::DateTime, "timestamp" },
    { 1184, ValueType::DateTimeTz, "timestamptz" },
    { 1700, ValueType::Decimal, "numeric" },
    { 2950, ValueType::Uuid, "uuid" },
    { 3802, ValueType::Json, "jsonb" },
};

const OidInfo *findOid(unsigned oid)
{
    for (const OidInfo &info : kOids) {
        if (info.oid == oid)
            return &info;
    }
    return nullptr;
}

} // namespace

Result<ConnParams> buildConnParams(const QVariantMap &values, const QString &password)
{
    ConnParams out;
    auto add = [&out](const char *key, const QString &value) {
        out.emplace_back(QByteArray(key), value.toUtf8());
    };
    auto text = [&values](const char *key) {
        return values.value(QString::fromLatin1(key)).toString().trimmed();
    };

    const QString host = text("host");
    add("host", host.isEmpty() ? QStringLiteral("localhost") : host);

    int port = 5432;
    if (const QVariant raw = values.value(QStringLiteral("port"));
        raw.isValid() && !raw.toString().trimmed().isEmpty()) {
        bool ok = false;
        port = raw.toString().trimmed().toInt(&ok);
        if (!ok || port < 1 || port > 65535)
            return fail(
                connectionError(Text::tr("The port must be a number between 1 and 65535.")));
    }
    add("port", QString::number(port));

    if (const QString database = text("database"); !database.isEmpty())
        add("dbname", database);
    if (const QString user = text("user"); !user.isEmpty())
        add("user", user);
    if (!password.isEmpty())
        add("password", password);

    QString sslmode = text("sslmode");
    if (sslmode.isEmpty())
        sslmode = QStringLiteral("prefer");
    static const QStringList kSslModes {
        QStringLiteral("disable"), QStringLiteral("allow"),     QStringLiteral("prefer"),
        QStringLiteral("require"), QStringLiteral("verify-ca"), QStringLiteral("verify-full")
    };
    if (!kSslModes.contains(sslmode))
        return fail(connectionError(Text::tr("Unknown SSL mode \"%1\".").arg(sslmode)));
    add("sslmode", sslmode);
    if (const QString root = text("sslrootcert"); !root.isEmpty())
        add("sslrootcert", root);

    add("connect_timeout", QStringLiteral("15"));
    add("application_name", QStringLiteral("Quarry"));
    add("client_encoding", QStringLiteral("UTF8"));
    add("options", QStringLiteral("-c DateStyle=ISO -c bytea_output=hex"));
    return out;
}

ErrorKind kindForSqlState(QByteArrayView sqlState)
{
    if (sqlState.size() != 5)
        return ErrorKind::Driver;
    const QByteArrayView cls = sqlState.first(2);
    if (cls == "28")
        return ErrorKind::Authentication;
    if (cls == "23")
        return ErrorKind::Constraint;
    if (cls == "08")
        return ErrorKind::Connection;
    if (sqlState == "42601")
        return ErrorKind::Syntax;
    if (sqlState == "42501")
        return ErrorKind::Permission;
    if (sqlState == "42P01" || sqlState == "42703" || sqlState == "3D000" || sqlState == "3F000")
        return ErrorKind::NotFound;
    if (sqlState == "57014")
        return ErrorKind::Cancelled;
    return ErrorKind::Driver;
}

QString messageForSqlState(QByteArrayView sqlState)
{
    switch (kindForSqlState(sqlState)) {
    case ErrorKind::Authentication:
        return Text::tr("Wrong user name or password.");
    case ErrorKind::Connection:
        return Text::tr("The connection to the server was lost or could not be established.");
    case ErrorKind::Syntax:
        return Text::tr("There is a syntax error in the statement.");
    case ErrorKind::Permission:
        return Text::tr("You do not have permission to do that.");
    case ErrorKind::Cancelled:
        return Text::tr("The operation was cancelled.");
    case ErrorKind::NotFound:
        if (sqlState == "42703")
            return Text::tr("The column does not exist.");
        if (sqlState == "3D000")
            return Text::tr("The database does not exist.");
        if (sqlState == "3F000")
            return Text::tr("The schema does not exist.");
        return Text::tr("The table or view does not exist.");
    case ErrorKind::Constraint:
        if (sqlState == "23505")
            return Text::tr("A unique or primary key constraint was violated.");
        if (sqlState == "23502")
            return Text::tr("A required (NOT NULL) value is missing.");
        if (sqlState == "23503")
            return Text::tr("A foreign key constraint was violated.");
        if (sqlState == "23514")
            return Text::tr("A CHECK constraint was violated.");
        return Text::tr("A constraint was violated.");
    default:
        return Text::tr("The database rejected the statement.");
    }
}

int utf16OffsetFromCharPosition(QStringView sql, int position)
{
    if (position < 1)
        return -1;
    qsizetype index = 0;
    for (int chars = 1; chars < position; ++chars) {
        if (index >= sql.size())
            return -1;
        index += sql[index].isHighSurrogate() && index + 1 < sql.size()
                && sql[index + 1].isLowSurrogate()
            ? 2
            : 1;
    }
    return static_cast<int>(index);
}

std::optional<QByteArray> decodeByteaHex(QByteArrayView text)
{
    if (text.size() < 2 || text[0] != '\\' || text[1] != 'x')
        return std::nullopt;
    const QByteArrayView digits = text.sliced(2);
    if (digits.size() % 2 != 0)
        return std::nullopt;
    for (const char c : digits) {
        if (!isHexDigit(c))
            return std::nullopt;
    }
    return QByteArray::fromHex(digits.toByteArray());
}

ValueType valueTypeForOid(unsigned oid)
{
    const OidInfo *info = findOid(oid);
    return info ? info->type : ValueType::Other;
}

QString nativeTypeName(unsigned oid)
{
    const OidInfo *info = findOid(oid);
    return info ? QString::fromLatin1(info->name) : QStringLiteral("oid:%1").arg(oid);
}

std::optional<double> parseDouble(QByteArrayView text)
{
    if (text == "NaN")
        return std::numeric_limits<double>::quiet_NaN();
    if (text == "Infinity")
        return std::numeric_limits<double>::infinity();
    if (text == "-Infinity")
        return -std::numeric_limits<double>::infinity();
    bool ok = false;
    const double value = text.toDouble(&ok);
    if (!ok)
        return std::nullopt;
    return value;
}

} // namespace quarry::postgres
