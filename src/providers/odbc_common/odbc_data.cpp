#include "odbc_data.h"

#include "odbc_diag.h"
#include "odbc_support.h"
#include "odbc_text.h"

#include <algorithm>
#include <limits>

namespace quarry::odbc {

namespace {

constexpr SQLLEN kChunkChars = 4096;
constexpr SQLLEN kChunkBytes = 16384;
constexpr SQLLEN kCharBytes = static_cast<SQLLEN>(sizeof(SQLWCHAR));
constexpr SQLULEN kMaxVarChars = 4000;
constexpr SQLULEN kMaxVarBytes = 8000;
constexpr SQLULEN kDoubleDigits = 15;
constexpr SQLULEN kBigintDigits = 19;
constexpr SQLULEN kIntegerDigits = 10;

// What a driver reports for a value that is not there: nothing, or "unknown size".
SQLLEN knownLength(SQLLEN indicator)
{
    return indicator < 0 ? 0 : indicator;
}

} // namespace

SQLRETURN getWideData(SQLHSTMT statement, SQLUSMALLINT column, std::vector<SQLWCHAR> &out,
                      bool &isNull)
{
    out.clear();
    isNull = false;
    std::vector<SQLWCHAR> chunk(static_cast<size_t>(kChunkChars));
    // The driver ends every piece with a NUL it counts in the buffer but not in the length.
    constexpr SQLLEN kPayload = kChunkChars * kCharBytes - kCharBytes;
    for (bool first = true;; first = false) {
        SQLLEN indicator = 0;
        const SQLRETURN rc = SQLGetData(statement, column, SQL_C_WCHAR, chunk.data(),
                                        kChunkChars * kCharBytes, &indicator);
        if (rc == SQL_NO_DATA)
            return SQL_SUCCESS; // the previous piece was the last one
        if (!succeeded(rc))
            return rc;
        if (first && indicator == SQL_NULL_DATA) {
            isNull = true;
            return SQL_SUCCESS;
        }
        const bool more = indicator == SQL_NO_TOTAL || indicator > kPayload;
        const SQLLEN bytes = more ? kPayload : knownLength(indicator);
        out.insert(out.end(), chunk.begin(), chunk.begin() + bytes / kCharBytes);
        if (!more)
            return SQL_SUCCESS;
    }
}

SQLRETURN getBinaryData(SQLHSTMT statement, SQLUSMALLINT column, QByteArray &out, bool &isNull)
{
    out.clear();
    isNull = false;
    QByteArray chunk(static_cast<qsizetype>(kChunkBytes), '\0');
    for (bool first = true;; first = false) {
        SQLLEN indicator = 0;
        const SQLRETURN rc
            = SQLGetData(statement, column, SQL_C_BINARY, chunk.data(), kChunkBytes, &indicator);
        if (rc == SQL_NO_DATA)
            return SQL_SUCCESS;
        if (!succeeded(rc))
            return rc;
        if (first && indicator == SQL_NULL_DATA) {
            isNull = true;
            return SQL_SUCCESS;
        }
        const bool more = indicator == SQL_NO_TOTAL || indicator > kChunkBytes;
        const SQLLEN bytes = more ? kChunkBytes : knownLength(indicator);
        out.append(chunk.constData(), static_cast<qsizetype>(bytes));
        if (!more)
            return SQL_SUCCESS;
    }
}

Outcome bindParameters(SQLHSTMT statement, const QVariantList &values,
                       std::vector<ParamSlot> &storage)
{
    storage.clear();
    storage.reserve(static_cast<size_t>(values.size()));
    for (qsizetype i = 0; i < values.size(); ++i) {
        const QVariant &value = values.at(i);
        const SQLUSMALLINT number = static_cast<SQLUSMALLINT>(i + 1);
        ParamSlot &slot = storage.emplace_back();

        SQLSMALLINT cType = SQL_C_CHAR;
        SQLSMALLINT sqlType = SQL_VARCHAR;
        SQLULEN size = 1;
        SQLSMALLINT digits = 0;
        SQLPOINTER data = &slot.bit;
        SQLLEN length = 0;

        if (!value.isValid()) {
            SQLSMALLINT nullable = 0;
            if (!succeeded(
                    SQLDescribeParam(statement, number, &sqlType, &size, &digits, &nullable))) {
                sqlType = SQL_VARCHAR;
                size = 1;
                digits = 0;
            }
            slot.indicator = SQL_NULL_DATA;
        } else {
            switch (value.typeId()) {
            case QMetaType::Bool:
                slot.bit = value.toBool() ? 1 : 0;
                cType = SQL_C_BIT;
                sqlType = SQL_BIT;
                data = &slot.bit;
                length = static_cast<SQLLEN>(sizeof(slot.bit));
                break;
            case QMetaType::Int:
            case QMetaType::UInt:
            case QMetaType::Long:
            case QMetaType::LongLong:
            case QMetaType::Short:
            case QMetaType::UShort:
            case QMetaType::Char:
            case QMetaType::SChar:
            case QMetaType::UChar:
                slot.integer = value.toLongLong();
                if (slot.integer >= std::numeric_limits<std::int32_t>::min()
                    && slot.integer <= std::numeric_limits<std::int32_t>::max()) {
                    // The 32-bit forms are the ones every driver supports; SQL_C_SBIGINT is an
                    // ODBC 3.0 addition that older drivers (flat-file ones) refuse.
                    slot.narrow = static_cast<std::int32_t>(slot.integer);
                    cType = SQL_C_SLONG;
                    sqlType = SQL_INTEGER;
                    size = kIntegerDigits;
                    data = &slot.narrow;
                    length = static_cast<SQLLEN>(sizeof(slot.narrow));
                    break;
                }
                cType = SQL_C_SBIGINT;
                sqlType = SQL_BIGINT;
                size = kBigintDigits;
                data = &slot.integer;
                length = static_cast<SQLLEN>(sizeof(slot.integer));
                break;
            case QMetaType::Double:
            case QMetaType::Float:
                slot.real = value.toDouble();
                cType = SQL_C_DOUBLE;
                sqlType = SQL_DOUBLE;
                size = kDoubleDigits;
                data = &slot.real;
                length = static_cast<SQLLEN>(sizeof(slot.real));
                break;
            case QMetaType::QByteArray: {
                slot.bytes = value.toByteArray();
                const SQLULEN count = static_cast<SQLULEN>(slot.bytes.size());
                cType = SQL_C_BINARY;
                // shortcut: above 8000 bytes the parameter is SQL_LONGVARBINARY (image on SQL
                // Server); use SQLPutData with a max-sized type if comparing them ever matters.
                sqlType = count <= kMaxVarBytes ? SQL_VARBINARY : SQL_LONGVARBINARY;
                size = std::max<SQLULEN>(count, 1);
                data = slot.bytes.data();
                length = static_cast<SQLLEN>(count);
                break;
            }
            default: {
                // Text, and everything without a better ODBC type (dates, UUIDs...) as text:
                // the server converts it to the column type.
                slot.wide = toWide(value.toString());
                const SQLULEN chars = slot.wide.size() - 1;
                cType = SQL_C_WCHAR;
                sqlType = chars <= kMaxVarChars ? SQL_WVARCHAR : SQL_WLONGVARCHAR;
                size = std::max<SQLULEN>(chars, 1);
                data = slot.wide.data();
                length = static_cast<SQLLEN>(chars) * kCharBytes;
                break;
            }
            }
            slot.indicator = length;
        }
        const SQLLEN bufferLength = value.isValid() ? length : 0;
        const SQLRETURN rc = SQLBindParameter(statement, number, SQL_PARAM_INPUT, cType, sqlType,
                                              size, digits, data, bufferLength, &slot.indicator);
        if (!succeeded(rc))
            return fail(errorFromHandle(SQL_HANDLE_STMT, statement));
    }
    return Done {};
}

Outcome runStatement(SQLHSTMT statement, const QString &sql, const QVariantList &parameters,
                     std::vector<ParamSlot> &storage, QStringList &notices)
{
    std::vector<SQLWCHAR> text = toWide(sql);
    SQLRETURN rc = SQL_SUCCESS;
    if (parameters.isEmpty()) {
        rc = SQLExecDirectW(statement, text.data(), SQL_NTS);
    } else {
        rc = SQLPrepareW(statement, text.data(), SQL_NTS);
        if (!succeeded(rc))
            return fail(errorFromHandle(SQL_HANDLE_STMT, statement));
        SQLSMALLINT expected = 0;
        if (succeeded(SQLNumParams(statement, &expected)) && expected != parameters.size()) {
            Error error;
            error.kind = ErrorKind::Syntax;
            error.message = Text::tr("The statement has %1 parameters but %2 values were given.")
                                .arg(expected)
                                .arg(parameters.size());
            error.detail = error.message;
            return fail(std::move(error));
        }
        if (auto bound = bindParameters(statement, parameters, storage); !bound)
            return fail(bound.error());
        rc = SQLExecute(statement);
    }
    if (rc == SQL_NO_DATA)
        return Done {};
    if (rc == SQL_SUCCESS_WITH_INFO)
        collectNotices(SQL_HANDLE_STMT, statement, notices);
    if (!succeeded(rc))
        return fail(errorFromHandle(SQL_HANDLE_STMT, statement));
    return Done {};
}

} // namespace quarry::odbc
