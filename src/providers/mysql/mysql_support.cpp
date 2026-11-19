#include "mysql_support.h"

#include "mysql_text.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QStringList>
#include <QTime>
#include <QUuid>

#include <cmath>
#include <mysql.h>

namespace quarry::mysql {

namespace {

constexpr unsigned kBinaryCharset = 63;

Error connectionError(const QString &message)
{
    return Error { ErrorKind::Connection, message, {}, {}, {} };
}

bool isIntegerType(unsigned type)
{
    switch (type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_YEAR:
        return true;
    default:
        return false;
    }
}

// Literal of one value; nullopt when it has none (NaN and infinities).
std::optional<QByteArray> literalFor(const QVariant &value, const EscapeFn &escape)
{
    if (value.isNull())
        return QByteArray("NULL");
    switch (value.metaType().id()) {
    case QMetaType::Bool:
        return QByteArray(value.toBool() ? "1" : "0");
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::Long:
    case QMetaType::LongLong:
    case QMetaType::Short:
    case QMetaType::UShort:
    case QMetaType::Char:
    case QMetaType::SChar:
    case QMetaType::UChar:
        return QByteArray::number(value.toLongLong());
    case QMetaType::ULong:
    case QMetaType::ULongLong:
        return QByteArray::number(value.toULongLong());
    case QMetaType::Double:
    case QMetaType::Float: {
        const double number = value.toDouble();
        if (!std::isfinite(number))
            return std::nullopt;
        return QByteArray::number(number, 'g', 17);
    }
    case QMetaType::QByteArray:
        return "_binary X'" + value.toByteArray().toHex() + '\'';
    case QMetaType::QDateTime:
        return '\''
            + value.toDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")).toUtf8()
            + '\'';
    case QMetaType::QDate:
        return '\'' + value.toDate().toString(Qt::ISODate).toUtf8() + '\'';
    case QMetaType::QTime:
        return '\'' + value.toTime().toString(QStringLiteral("HH:mm:ss.zzz")).toUtf8() + '\'';
    case QMetaType::QUuid:
        return '\'' + value.toUuid().toString(QUuid::WithoutBraces).toUtf8() + '\'';
    default:
        return '\'' + escape(value.toString().toUtf8()) + '\'';
    }
}

// Index just past the quoted run that opens at `i` (quote is ', " or `); n if unterminated.
qsizetype skipQuotedRun(QStringView s, qsizetype i, QChar quote)
{
    const qsizetype n = s.size();
    const bool backslash = quote != QLatin1Char('`');
    ++i;
    while (i < n) {
        if (backslash && s[i] == QLatin1Char('\\')) {
            i += 2;
        } else if (s[i] == quote) {
            if (i + 1 < n && s[i + 1] == quote)
                i += 2;
            else
                return i + 1;
        } else {
            ++i;
        }
    }
    return n;
}

} // namespace

Result<ConnSettings> buildConnSettings(const QVariantMap &values, const QString &password)
{
    ConnSettings out;
    auto text = [&values](const char *key) {
        return values.value(QString::fromLatin1(key)).toString().trimmed();
    };

    const QString host = text("host");
    out.host = (host.isEmpty() ? QStringLiteral("localhost") : host).toUtf8();

    if (const QVariant raw = values.value(QStringLiteral("port"));
        raw.isValid() && !raw.toString().trimmed().isEmpty()) {
        bool ok = false;
        const int port = raw.toString().trimmed().toInt(&ok);
        if (!ok || port < 1 || port > 65535)
            return fail(
                connectionError(Text::tr("The port must be a number between 1 and 65535.")));
        out.port = static_cast<unsigned>(port);
    }

    out.database = text("database").toUtf8();
    out.user = text("user").toUtf8();
    out.password = password.toUtf8();

    QString sslmode = text("sslmode");
    if (sslmode.isEmpty())
        sslmode = QStringLiteral("prefer");
    if (sslmode == QLatin1String("disable"))
        out.sslMode = SslMode::Disable;
    else if (sslmode == QLatin1String("prefer"))
        out.sslMode = SslMode::Prefer;
    else if (sslmode == QLatin1String("require"))
        out.sslMode = SslMode::Require;
    else if (sslmode == QLatin1String("verify-ca"))
        out.sslMode = SslMode::VerifyCa;
    else if (sslmode == QLatin1String("verify-full"))
        out.sslMode = SslMode::VerifyFull;
    else
        return fail(connectionError(Text::tr("Unknown SSL mode \"%1\".").arg(sslmode)));
    out.rootCert = text("sslrootcert").toUtf8();
    return out;
}

bool shouldRetryWithoutTls(unsigned code)
{
    return code == 2026 || code == 2013; // CR_SSL_CONNECTION_ERROR, CR_SERVER_LOST
}

ErrorKind kindForErrno(unsigned code)
{
    switch (code) {
    case 1045: // ER_ACCESS_DENIED_ERROR
    case 1698: // ER_ACCESS_DENIED_NO_PASSWORD_ERROR
        return ErrorKind::Authentication;
    case 1044: // ER_DBACCESS_DENIED_ERROR
    case 1142: // ER_TABLEACCESS_DENIED_ERROR
    case 1143: // ER_COLUMNACCESS_DENIED_ERROR
    case 1227: // ER_SPECIFIC_ACCESS_DENIED_ERROR
        return ErrorKind::Permission;
    case 1062: // ER_DUP_ENTRY
    case 1048: // ER_BAD_NULL_ERROR
    case 1216: // ER_NO_REFERENCED_ROW
    case 1217: // ER_ROW_IS_REFERENCED
    case 1451: // ER_ROW_IS_REFERENCED_2
    case 1452: // ER_NO_REFERENCED_ROW_2
    case 3819: // ER_CHECK_CONSTRAINT_VIOLATED (MySQL)
    case 4025: // ER_CONSTRAINT_FAILED (MariaDB's CHECK violation)
        return ErrorKind::Constraint;
    case 1064: // ER_PARSE_ERROR
    case 1149: // ER_SYNTAX_ERROR
        return ErrorKind::Syntax;
    case 1049: // ER_BAD_DB_ERROR
    case 1054: // ER_BAD_FIELD_ERROR
    case 1146: // ER_NO_SUCH_TABLE
    case 1305: // ER_SP_DOES_NOT_EXIST
        return ErrorKind::NotFound;
    case 1317: // ER_QUERY_INTERRUPTED
        return ErrorKind::Cancelled;
    case 1205: // ER_LOCK_WAIT_TIMEOUT
    case 1969: // ER_STATEMENT_TIMEOUT
        return ErrorKind::Timeout;
    case 2002: // CR_CONNECTION_ERROR
    case 2003: // CR_CONN_HOST_ERROR
    case 2005: // CR_UNKNOWN_HOST
    case 2006: // CR_SERVER_GONE_ERROR
    case 2013: // CR_SERVER_LOST
    case 2026: // CR_SSL_CONNECTION_ERROR: not in the contract, it is what a refused TLS gives
        return ErrorKind::Connection;
    default:
        return ErrorKind::Driver;
    }
}

QString messageForErrno(unsigned code)
{
    switch (kindForErrno(code)) {
    case ErrorKind::Authentication:
        return Text::tr("Wrong user name or password.");
    case ErrorKind::Connection:
        if (code == 2026)
            return Text::tr("Could not establish a secure (TLS) connection.");
        return Text::tr("The connection to the server was lost or could not be established.");
    case ErrorKind::Timeout:
        return Text::tr("The server did not answer in time.");
    case ErrorKind::Syntax:
        return Text::tr("There is a syntax error in the statement.");
    case ErrorKind::Permission:
        return Text::tr("You do not have permission to do that.");
    case ErrorKind::Cancelled:
        return Text::tr("The operation was cancelled.");
    case ErrorKind::NotFound:
        if (code == 1049)
            return Text::tr("The database does not exist.");
        if (code == 1054)
            return Text::tr("The column does not exist.");
        if (code == 1305)
            return Text::tr("The routine does not exist.");
        return Text::tr("The table or view does not exist.");
    case ErrorKind::Constraint:
        if (code == 1062)
            return Text::tr("A unique or primary key constraint was violated.");
        if (code == 1048)
            return Text::tr("A required (NOT NULL) value is missing.");
        if (code == 3819 || code == 4025)
            return Text::tr("A CHECK constraint was violated.");
        if (code == 1216 || code == 1452)
            return Text::tr("A foreign key constraint was violated.");
        return Text::tr("A row is still referenced by a foreign key.");
    default:
        return Text::tr("The database rejected the statement.");
    }
}

ValueType valueTypeForColumn(unsigned fieldType, unsigned charset)
{
    if (isIntegerType(fieldType))
        return ValueType::Int64;
    switch (fieldType) {
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
        return ValueType::Double;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
        return ValueType::Decimal;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
        return ValueType::Date;
    case MYSQL_TYPE_TIME:
        return ValueType::Time;
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
        return ValueType::DateTime;
    case MYSQL_TYPE_JSON:
        return ValueType::Json;
    case MYSQL_TYPE_BIT:
        return ValueType::Int64;
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
        // TEXT is a BLOB with a text charset; BINARY/VARBINARY/BLOB have the binary one (63).
        return charset == kBinaryCharset ? ValueType::Bytes : ValueType::Text;
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
        return ValueType::Text;
    default: // NULL, GEOMETRY, VECTOR...
        return ValueType::Other;
    }
}

QString nativeTypeName(unsigned fieldType, unsigned charset, unsigned flags)
{
    const bool binary = charset == kBinaryCharset;
    const QString unsignedSuffix
        = (flags & UNSIGNED_FLAG) != 0 ? QStringLiteral(" unsigned") : QString();
    switch (fieldType) {
    case MYSQL_TYPE_TINY:
        return QStringLiteral("tinyint") + unsignedSuffix;
    case MYSQL_TYPE_SHORT:
        return QStringLiteral("smallint") + unsignedSuffix;
    case MYSQL_TYPE_INT24:
        return QStringLiteral("mediumint") + unsignedSuffix;
    case MYSQL_TYPE_LONG:
        return QStringLiteral("int") + unsignedSuffix;
    case MYSQL_TYPE_LONGLONG:
        return QStringLiteral("bigint") + unsignedSuffix;
    case MYSQL_TYPE_YEAR:
        return QStringLiteral("year");
    case MYSQL_TYPE_FLOAT:
        return QStringLiteral("float");
    case MYSQL_TYPE_DOUBLE:
        return QStringLiteral("double");
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
        return QStringLiteral("decimal");
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
        return QStringLiteral("date");
    case MYSQL_TYPE_TIME:
        return QStringLiteral("time");
    case MYSQL_TYPE_DATETIME:
        return QStringLiteral("datetime");
    case MYSQL_TYPE_TIMESTAMP:
        return QStringLiteral("timestamp");
    case MYSQL_TYPE_JSON:
        return QStringLiteral("json");
    case MYSQL_TYPE_BIT:
        return QStringLiteral("bit");
    case MYSQL_TYPE_ENUM:
        return QStringLiteral("enum");
    case MYSQL_TYPE_SET:
        return QStringLiteral("set");
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
        return binary ? QStringLiteral("blob") : QStringLiteral("text");
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
        return binary ? QStringLiteral("varbinary") : QStringLiteral("varchar");
    case MYSQL_TYPE_STRING:
        if ((flags & ENUM_FLAG) != 0)
            return QStringLiteral("enum");
        if ((flags & SET_FLAG) != 0)
            return QStringLiteral("set");
        return binary ? QStringLiteral("binary") : QStringLiteral("char");
    case MYSQL_TYPE_GEOMETRY:
        return QStringLiteral("geometry");
    case MYSQL_TYPE_NULL:
        return QStringLiteral("null");
    default:
        return QStringLiteral("type:%1").arg(fieldType);
    }
}

ValueType valueTypeForDataType(QStringView dataType)
{
    struct Entry
    {
        const char *name;
        ValueType type;
    };
    static constexpr Entry kTypes[] = {
        { "tinyint", ValueType::Int64 },      { "smallint", ValueType::Int64 },
        { "mediumint", ValueType::Int64 },    { "int", ValueType::Int64 },
        { "integer", ValueType::Int64 },      { "bigint", ValueType::Int64 },
        { "year", ValueType::Int64 },         { "bit", ValueType::Int64 },
        { "float", ValueType::Double },       { "double", ValueType::Double },
        { "real", ValueType::Double },        { "decimal", ValueType::Decimal },
        { "numeric", ValueType::Decimal },    { "date", ValueType::Date },
        { "time", ValueType::Time },          { "datetime", ValueType::DateTime },
        { "timestamp", ValueType::DateTime }, { "json", ValueType::Json },
        { "binary", ValueType::Bytes },       { "varbinary", ValueType::Bytes },
        { "tinyblob", ValueType::Bytes },     { "blob", ValueType::Bytes },
        { "mediumblob", ValueType::Bytes },   { "longblob", ValueType::Bytes },
        { "char", ValueType::Text },          { "varchar", ValueType::Text },
        { "tinytext", ValueType::Text },      { "text", ValueType::Text },
        { "mediumtext", ValueType::Text },    { "longtext", ValueType::Text },
        { "enum", ValueType::Text },          { "set", ValueType::Text },
    };
    for (const Entry &entry : kTypes) {
        if (dataType.compare(QLatin1String(entry.name), Qt::CaseInsensitive) == 0)
            return entry.type;
    }
    return ValueType::Other;
}

QString pluginDirectory(const QString &executableDir)
{
    // Inside an .app the plugins live in Contents/PlugIns, not in Contents/MacOS: code in MacOS
    // next to the executable is a signing problem, and PlugIns is where the providers are.
    const QString dir = QDir::cleanPath(executableDir);
    if (dir.endsWith(QLatin1String("/Contents/MacOS")))
        return QDir::cleanPath(dir + QStringLiteral("/../PlugIns/mariadb"));
    return QDir::cleanPath(executableDir + QStringLiteral("/mariadb-plugins"));
}

bool isMariaDb(QByteArrayView serverInfo)
{
    return serverInfo.indexOf("MariaDB") >= 0;
}

QString plainVersion(QByteArrayView serverInfo)
{
    QByteArrayView text = serverInfo;
    if (text.startsWith("5.5.5-"))
        text = text.sliced(6);
    qsizetype end = 0;
    while (end < text.size() && ((text[end] >= '0' && text[end] <= '9') || text[end] == '.'))
        ++end;
    return QString::fromLatin1(text.first(end));
}

std::optional<qint64> bitValue(QByteArrayView bytes)
{
    if (bytes.size() > 8)
        return std::nullopt;
    quint64 value = 0;
    for (const char byte : bytes)
        value = (value << 8) | static_cast<quint8>(byte);
    return static_cast<qint64>(value);
}

Result<QByteArray> bindParameters(const QString &sql, const QVariantList &parameters,
                                  const EscapeFn &escape)
{
    QByteArray out;
    out.reserve(sql.size() + 16);
    const qsizetype n = sql.size();
    qsizetype used = 0;
    qsizetype copied = 0; // sql[0, copied) is already in `out`
    auto copyTo
        = [&](qsizetype end) { out += QStringView(sql).mid(copied, end - copied).toUtf8(); };
    auto mismatch = [] {
        return fail(Error { ErrorKind::Syntax,
                            Text::tr("The number of values does not match the placeholders."),
                            {},
                            {},
                            {} });
    };

    qsizetype i = 0;
    while (i < n) {
        const QChar c = sql[i];
        if (c == QLatin1Char('\'') || c == QLatin1Char('"') || c == QLatin1Char('`')) {
            i = skipQuotedRun(sql, i, c);
        } else if (c == QLatin1Char('#')
                   || (c == QLatin1Char('-') && i + 1 < n && sql[i + 1] == QLatin1Char('-')
                       && (i + 2 >= n || sql[i + 2].isSpace() || sql[i + 2].unicode() < 32))) {
            while (i < n && sql[i] != QLatin1Char('\n'))
                ++i;
        } else if (c == QLatin1Char('/') && i + 1 < n && sql[i + 1] == QLatin1Char('*')) {
            const qsizetype close = sql.indexOf(QLatin1String("*/"), i + 2);
            i = close < 0 ? n : close + 2;
        } else if (c == QLatin1Char('?')) {
            if (used >= parameters.size())
                return mismatch();
            const auto literal = literalFor(parameters.at(used), escape);
            if (!literal)
                return fail(Error { ErrorKind::Unsupported,
                                    Text::tr("NaN and infinite numbers cannot be sent."),
                                    {},
                                    {},
                                    {} });
            copyTo(i);
            out += literal.value_or(QByteArray());
            copied = i + 1;
            ++used;
            ++i;
        } else {
            ++i;
        }
    }
    if (used != parameters.size())
        return mismatch();
    copyTo(n);
    return out;
}

} // namespace quarry::mysql
