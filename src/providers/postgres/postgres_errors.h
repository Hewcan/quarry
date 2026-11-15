#pragma once

#include <quarry/result.h>

#include <QString>

#include <libpq-fe.h>

namespace quarry::postgres {

// Error from a failed command result. `sql` is the statement text, used to turn the
// 1-based character position of the server into a 0-based UTF-16 offset.
Error errorFromResult(const PGresult *result, PGconn *conn, QStringView sql);

// Error from a failed connection attempt (PQerrorMessage text; there is no SQLSTATE yet).
Error connectError(const QString &libpqMessage);

// Error for a call that failed before any result arrived (PQsend* returned 0).
Error sendError(PGconn *conn);

Error cancelledError();
Error notOpenError();

} // namespace quarry::postgres
