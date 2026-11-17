#pragma once

// Pure helpers shared by the ODBC providers: they never call the driver manager, so unit tests
// can use them without a server (the ODBC headers only supply the SQL_* constants).

#include "odbc_headers.h"

#include <quarry/result.h>
#include <quarry/value_type.h>

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <vector>

namespace quarry::odbc {

constexpr int kLoginTimeoutSeconds = 15; // ARCHITECTURE.md 5
constexpr qsizetype kMaxNotices = 100;

// SQL Server types that sqlext.h does not define (they live in the driver's own header).
constexpr int kSqlSsVariant = -150;
constexpr int kSqlSsUdt = -151;
constexpr int kSqlSsXml = -152;
constexpr int kSqlSsTime2 = -154;
constexpr int kSqlSsTimestampOffset = -155;

// QString <-> the UTF-16 code units of the W functions. toWide() appends a NUL as its last
// element, so data() works with SQL_NTS.
std::vector<SQLWCHAR> toWide(const QString &text);
QString fromWide(const SQLWCHAR *text, qsizetype chars);
QByteArray wideToUtf8(const SQLWCHAR *text, qsizetype chars);

// "{...}" with every } doubled: the only way ODBC connection strings carry ; = { } and spaces.
QString bracedValue(const QString &value);
// bracedValue() when the value needs it, the plain text otherwise.
QString connectionStringValue(const QString &value);
// "Key=value;" with the value as connectionStringValue() writes it.
QString connectionStringPair(const QString &key, const QString &value);
// Same, but the value always goes in braces (PWD, Driver).
QString connectionStringBracedPair(const QString &key, const QString &value);

// SQLSTATE and native error code -> kind (ARCHITECTURE.md 6.7).
ErrorKind kindForDiagnostic(QStringView sqlState, long nativeError);
// Plain-language, translatable message of a kind.
QString messageForKind(ErrorKind kind);

// SQL_* data type of SQLDescribeCol (or the extended code of the driver) -> normalized type.
ValueType valueTypeForSqlType(int sqlType);
// "integer", "varchar"... for drivers that give no type name.
QString genericTypeName(int sqlType);
// "nvarchar(255)", "decimal(10,2)": the type name with its size where one makes sense.
QString decoratedTypeName(const QString &name, ValueType type, unsigned long long size, int digits);

// How a result column is read: the C type for SQLBindCol/SQLGetData and, when the driver
// reports a size that can be trusted, the bytes of the buffer of a bound column. capacity 0 means
// the column is read with SQLGetData (long or unbounded data).
struct ColumnPlanSpec
{
    ValueType type = ValueType::Other;
    SQLSMALLINT cType = SQL_C_WCHAR;
    SQLLEN capacity = 0;
};
// bindVariable: whether text, binary and decimal columns may be bound at the size the driver
// reports (SQL Server does not lie about it; generic drivers sometimes do).
ColumnPlanSpec planColumn(int sqlType, unsigned long long columnSize, bool bindVariable);

// "[Microsoft][ODBC Driver 18 for SQL Server][SQL Server]Hello" -> "Hello".
QString stripDriverPrefix(const QString &message);

// Escapes % and _ (and the escape character) so a name can be used as a search pattern.
QString escapePattern(const QString &name, const QString &escape);

// Replaces the `?` markers whose value is NULL (an invalid QVariant) with the literal NULL and
// drops those values: a NULL bound with a guessed SQL type is rejected by SQL Server when the
// column is another type ("Implicit conversion from data type varchar to varbinary(max) is not
// allowed"), while the literal converts to any column. Markers inside strings, quoted
// identifiers, [brackets] and comments are not markers. If the number of markers differs from
// the number of values, nothing changes and the driver reports the mismatch.
void inlineNullParameters(QString &sql, QVariantList &values);

} // namespace quarry::odbc
