#pragma once

#include "odbc_diag.h"
#include "odbc_headers.h"

#include <quarry/result.h>

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <cstdint>
#include <vector>

namespace quarry::odbc {

// Reads column `column` of the current row as text with SQLGetData, in chunks, so long values
// (NVARCHAR(MAX), text, SQL_NO_TOTAL) arrive whole. Returns SQL_SUCCESS or the failing return
// code (the caller reads the diagnostics). `isNull` tells a NULL apart from an empty string.
SQLRETURN getWideData(SQLHSTMT statement, SQLUSMALLINT column, std::vector<SQLWCHAR> &out,
                      bool &isNull);
// Same for binary data.
SQLRETURN getBinaryData(SQLHSTMT statement, SQLUSMALLINT column, QByteArray &out, bool &isNull);

// Storage of one bound parameter: the driver reads it when the statement executes, so it must
// stay alive and at the same address until then.
struct ParamSlot
{
    std::int64_t integer = 0;
    std::int32_t narrow = 0;
    double real = 0;
    unsigned char bit = 0;
    std::vector<SQLWCHAR> wide;
    QByteArray bytes;
    SQLLEN indicator = 0;
};

// SQLBindParameter for every value (`?` markers). NULL takes the type the server reports for the
// parameter (SQLDescribeParam), because a NULL typed as text cannot go into a binary column.
Outcome bindParameters(SQLHSTMT statement, const QVariantList &values,
                       std::vector<ParamSlot> &storage);

// SQLExecDirectW without parameters, SQLPrepareW + SQLBindParameter + SQLExecute with them.
// SQL_NO_DATA (a searched UPDATE or DELETE that touched no row) is success. Informational
// messages go to `notices`.
Outcome runStatement(SQLHSTMT statement, const QString &sql, const QVariantList &parameters,
                     std::vector<ParamSlot> &storage, QStringList &notices);

} // namespace quarry::odbc
