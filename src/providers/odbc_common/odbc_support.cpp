#include "odbc_support.h"

#include "odbc_text.h"

#include <QRegularExpression>

#include <algorithm>
#include <initializer_list>

namespace quarry::odbc {

namespace {

// Room for the longest text of a date, time, timestamp with offset or GUID, without the NUL.
constexpr qsizetype kTemporalChars = 64;
constexpr unsigned long long kMaxBoundTextChars = 4000;
constexpr unsigned long long kMaxBoundBytes = 8000;
constexpr unsigned long long kMaxBoundDecimalDigits = 128;
constexpr SQLLEN kCharBytes = static_cast<SQLLEN>(sizeof(SQLWCHAR));

bool isOneOf(long value, std::initializer_list<long> values)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool needsBraces(const QString &value)
{
    static const QRegularExpression kSpecial(QStringLiteral("[;={}]"));
    return value.isEmpty() || value.trimmed() != value || value.contains(kSpecial);
}

} // namespace

std::vector<SQLWCHAR> toWide(const QString &text)
{
    std::vector<SQLWCHAR> out;
    out.reserve(static_cast<size_t>(text.size()) + 1);
    for (const QChar c : text)
        out.push_back(static_cast<SQLWCHAR>(c.unicode()));
    out.push_back(0);
    return out;
}

QString fromWide(const SQLWCHAR *text, qsizetype chars)
{
    QString out;
    out.reserve(chars);
    for (qsizetype i = 0; i < chars; ++i)
        out.push_back(QChar(static_cast<char16_t>(text[i])));
    return out;
}

QByteArray wideToUtf8(const SQLWCHAR *text, qsizetype chars)
{
    // SQLWCHAR is a 16-bit unsigned unit, the same representation as char16_t; this is the one
    // place where the buffers of the driver are read in place (callers keep them 2-aligned).
    return QStringView(reinterpret_cast<const char16_t *>(text), chars).toUtf8();
}

QString bracedValue(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('}'), QLatin1String("}}"));
    return QLatin1Char('{') + escaped + QLatin1Char('}');
}

QString connectionStringValue(const QString &value)
{
    return needsBraces(value) ? bracedValue(value) : value;
}

QString connectionStringPair(const QString &key, const QString &value)
{
    return key + QLatin1Char('=') + connectionStringValue(value) + QLatin1Char(';');
}

QString connectionStringBracedPair(const QString &key, const QString &value)
{
    return key + QLatin1Char('=') + bracedValue(value) + QLatin1Char(';');
}

ErrorKind kindForDiagnostic(QStringView sqlState, long nativeError)
{
    const QString state = sqlState.toString().toUpper();
    if (nativeError == 3617 || state == QLatin1String("HY008") || state == QLatin1String("57014"))
        return ErrorKind::Cancelled;
    if (state.startsWith(QLatin1String("28")))
        return ErrorKind::Authentication;
    if (state.startsWith(QLatin1String("08")) || state == QLatin1String("IM002")
        || state == QLatin1String("IM003") || state == QLatin1String("IM004")
        || state == QLatin1String("IM010") || state == QLatin1String("IM014"))
        return ErrorKind::Connection;
    if (state == QLatin1String("HYT00") || state == QLatin1String("HYT01"))
        return ErrorKind::Timeout;
    if (state.startsWith(QLatin1String("23")))
        return ErrorKind::Constraint;
    // 42P01, 42703, 42501 and 42601 are what PostgreSQL's driver reports instead of 42S02...
    if (state == QLatin1String("42S02") || state == QLatin1String("42S12")
        || state == QLatin1String("42S22") || state == QLatin1String("42P01")
        || state == QLatin1String("42703"))
        return ErrorKind::NotFound;
    if (state == QLatin1String("42501"))
        return ErrorKind::Permission;
    if (state == QLatin1String("42601"))
        return ErrorKind::Syntax;
    if (state == QLatin1String("42000")) {
        if (isOneOf(nativeError, { 229, 230, 297, 262, 300 }))
            return ErrorKind::Permission;
        // 4060: SQL Server cannot open the database of the login.
        if (isOneOf(nativeError, { 207, 208, 4060 }))
            return ErrorKind::NotFound;
        if (isOneOf(nativeError, { 2627, 2601, 515, 547 }))
            return ErrorKind::Constraint;
        return ErrorKind::Syntax;
    }
    if (state == QLatin1String("HYC00"))
        return ErrorKind::Unsupported;
    return ErrorKind::Driver;
}

QString messageForKind(ErrorKind kind)
{
    switch (kind) {
    case ErrorKind::Authentication:
        return Text::tr("The server rejected the user name or password.");
    case ErrorKind::Connection:
        return Text::tr("The connection to the server failed.");
    case ErrorKind::Timeout:
        return Text::tr("The server did not answer in time.");
    case ErrorKind::Cancelled:
        return Text::tr("The statement was cancelled.");
    case ErrorKind::Syntax:
        return Text::tr("The SQL statement has a syntax error.");
    case ErrorKind::Permission:
        return Text::tr("You do not have permission to do that.");
    case ErrorKind::Constraint:
        return Text::tr("The statement violates a constraint.");
    case ErrorKind::NotFound:
        return Text::tr("The database, table or column does not exist.");
    case ErrorKind::Unsupported:
        return Text::tr("The driver does not support this operation.");
    case ErrorKind::Driver:
    case ErrorKind::Internal:
        break;
    }
    return Text::tr("The database driver reported an error.");
}

ValueType valueTypeForSqlType(int sqlType)
{
    switch (sqlType) {
    case SQL_TINYINT:
    case SQL_SMALLINT:
    case SQL_INTEGER:
    case SQL_BIGINT:
        return ValueType::Int64;
    case SQL_REAL:
    case SQL_FLOAT:
    case SQL_DOUBLE:
        return ValueType::Double;
    case SQL_DECIMAL:
    case SQL_NUMERIC:
        return ValueType::Decimal;
    case SQL_BIT:
        return ValueType::Bool;
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
    case kSqlSsXml:
        return ValueType::Text;
    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
        return ValueType::Bytes;
    case SQL_TYPE_DATE:
        return ValueType::Date;
    case SQL_TYPE_TIME:
    case kSqlSsTime2:
        return ValueType::Time;
    case SQL_TYPE_TIMESTAMP:
        return ValueType::DateTime;
    case kSqlSsTimestampOffset:
        return ValueType::DateTimeTz;
    case SQL_GUID:
        return ValueType::Uuid;
    default:
        return ValueType::Other;
    }
}

QString genericTypeName(int sqlType)
{
    switch (sqlType) {
    case SQL_TINYINT:
        return QStringLiteral("tinyint");
    case SQL_SMALLINT:
        return QStringLiteral("smallint");
    case SQL_INTEGER:
        return QStringLiteral("integer");
    case SQL_BIGINT:
        return QStringLiteral("bigint");
    case SQL_REAL:
        return QStringLiteral("real");
    case SQL_FLOAT:
        return QStringLiteral("float");
    case SQL_DOUBLE:
        return QStringLiteral("double");
    case SQL_DECIMAL:
        return QStringLiteral("decimal");
    case SQL_NUMERIC:
        return QStringLiteral("numeric");
    case SQL_BIT:
        return QStringLiteral("bit");
    case SQL_CHAR:
        return QStringLiteral("char");
    case SQL_VARCHAR:
        return QStringLiteral("varchar");
    case SQL_LONGVARCHAR:
        return QStringLiteral("text");
    case SQL_WCHAR:
        return QStringLiteral("nchar");
    case SQL_WVARCHAR:
        return QStringLiteral("nvarchar");
    case SQL_WLONGVARCHAR:
        return QStringLiteral("ntext");
    case SQL_BINARY:
        return QStringLiteral("binary");
    case SQL_VARBINARY:
        return QStringLiteral("varbinary");
    case SQL_LONGVARBINARY:
        return QStringLiteral("blob");
    case SQL_TYPE_DATE:
        return QStringLiteral("date");
    case SQL_TYPE_TIME:
    case kSqlSsTime2:
        return QStringLiteral("time");
    case SQL_TYPE_TIMESTAMP:
        return QStringLiteral("timestamp");
    case kSqlSsTimestampOffset:
        return QStringLiteral("datetimeoffset");
    case SQL_GUID:
        return QStringLiteral("guid");
    case kSqlSsXml:
        return QStringLiteral("xml");
    default:
        return QStringLiteral("other");
    }
}

QString decoratedTypeName(const QString &name, ValueType type, unsigned long long size, int digits)
{
    if (size == 0 || size > kMaxBoundBytes)
        return name;
    if (type == ValueType::Text || type == ValueType::Bytes)
        return QStringLiteral("%1(%2)").arg(name).arg(size);
    if (type == ValueType::Decimal)
        return QStringLiteral("%1(%2,%3)").arg(name).arg(size).arg(digits);
    return name;
}

ColumnPlanSpec planColumn(int sqlType, unsigned long long columnSize, bool bindVariable)
{
    ColumnPlanSpec spec;
    spec.type = valueTypeForSqlType(sqlType);
    switch (spec.type) {
    case ValueType::Int64:
        spec.cType = SQL_C_SBIGINT;
        spec.capacity = static_cast<SQLLEN>(sizeof(SQLBIGINT));
        break;
    case ValueType::Double:
        spec.cType = SQL_C_DOUBLE;
        spec.capacity = static_cast<SQLLEN>(sizeof(SQLDOUBLE));
        break;
    case ValueType::Bool:
        spec.cType = SQL_C_BIT;
        spec.capacity = static_cast<SQLLEN>(sizeof(SQLCHAR));
        break;
    case ValueType::Date:
    case ValueType::Time:
    case ValueType::DateTime:
    case ValueType::DateTimeTz:
    case ValueType::Uuid:
        spec.capacity = (kTemporalChars + 1) * kCharBytes;
        break;
    case ValueType::Decimal:
        if (bindVariable && columnSize >= 1 && columnSize <= kMaxBoundDecimalDigits)
            spec.capacity = (static_cast<SQLLEN>(columnSize) + 8) * kCharBytes;
        break;
    case ValueType::Text:
        if (bindVariable && columnSize >= 1 && columnSize <= kMaxBoundTextChars)
            spec.capacity = (static_cast<SQLLEN>(columnSize) + 1) * kCharBytes;
        break;
    case ValueType::Bytes:
        spec.cType = SQL_C_BINARY;
        if (bindVariable && columnSize >= 1 && columnSize <= kMaxBoundBytes)
            spec.capacity = static_cast<SQLLEN>(columnSize);
        break;
    default:
        // Unknown types (SQL Server UDT, sql_variant, intervals): text of unknown size.
        break;
    }
    return spec;
}

QString stripDriverPrefix(const QString &message)
{
    if (!message.startsWith(QLatin1String("[Microsoft]")))
        return message;
    qsizetype pos = 0;
    for (int groups = 0; groups < 3 && pos < message.size() && message.at(pos) == QLatin1Char('[');
         ++groups) {
        const qsizetype close = message.indexOf(QLatin1Char(']'), pos);
        if (close < 0)
            break;
        pos = close + 1;
    }
    return message.mid(pos).trimmed();
}

QString escapePattern(const QString &name, const QString &escape)
{
    if (escape.isEmpty())
        return name;
    QString out;
    out.reserve(name.size() + 4);
    for (const QChar c : name) {
        if (c == QLatin1Char('%') || c == QLatin1Char('_') || c == escape.front())
            out += escape;
        out += c;
    }
    return out;
}

} // namespace quarry::odbc
