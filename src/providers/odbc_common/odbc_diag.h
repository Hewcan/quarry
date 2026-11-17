#pragma once

#include "odbc_headers.h"

#include <quarry/result.h>

#include <QString>
#include <QStringList>

#include <vector>

namespace quarry::odbc {

// One record of SQLGetDiagRecW.
struct Diagnostic
{
    QString sqlState;
    long nativeError = 0;
    QString message;
};

// Outcome of a step that has no value. Result<void> keeps its error in a std::optional, which the
// static analyzer reports as an uninitialized copy when it is propagated; this one has a plain
// member.
struct Done
{
};
using Outcome = Result<Done>;

inline bool succeeded(SQLRETURN rc)
{
    return rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO;
}

// Every diagnostic record of a handle (SQL_HANDLE_ENV/DBC/STMT), in the order of the driver.
std::vector<Diagnostic> readDiagnostics(SQLSMALLINT handleType, SQLHANDLE handle);

// Error from records: the first one that is not a warning (01xxx) decides the kind. `message` is
// plain language; `detail` has SQLSTATE, native error and driver text of every record.
Error errorFromDiagnostics(const std::vector<Diagnostic> &records);
Error errorFromHandle(SQLSMALLINT handleType, SQLHANDLE handle);

// Same for a failed connection attempt. `target` is what the user asked for (DSN or driver);
// anything that is not about credentials, drivers or timeouts is a connection problem here, not
// a statement the server rejected.
Error connectError(const std::vector<Diagnostic> &records, const QString &target);

// Informational records of a call that returned SQL_SUCCESS_WITH_INFO (PRINT, warnings), as
// notices. Skips the "data truncated" and "option value changed" noise.
void collectNotices(SQLSMALLINT handleType, SQLHANDLE handle, QStringList &notices);

Error notOpenError();
Error busyError();
Error cancelledError();
Error internalError(const QString &text);

} // namespace quarry::odbc
