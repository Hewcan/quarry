#include "postgres_errors.h"

#include "postgres_support.h"
#include "postgres_text.h"

namespace quarry::postgres {

namespace {

QString fieldText(const PGresult *result, int field)
{
    const char *value = result ? PQresultErrorField(result, field) : nullptr;
    return value ? QString::fromUtf8(value) : QString();
}

} // namespace

Error cancelledError()
{
    return Error { ErrorKind::Cancelled,
                   messageForSqlState("57014"),
                   QStringLiteral("57014 query_canceled"),
                   QStringLiteral("57014"),
                   {} };
}

Error notOpenError()
{
    return Error { ErrorKind::Connection, Text::tr("The connection is not open."), {}, {}, {} };
}

Error errorFromResult(const PGresult *result, PGconn *conn, QStringView sql)
{
    const QString sqlState = fieldText(result, PG_DIAG_SQLSTATE);
    const QByteArray state = sqlState.toLatin1();
    Error error;
    error.sqlState = sqlState;
    error.detail = QString::fromUtf8(result ? PQresultErrorMessage(result) : "").trimmed();
    if (error.detail.isEmpty() && conn)
        error.detail = QString::fromUtf8(PQerrorMessage(conn)).trimmed();

    if (sqlState.isEmpty()) {
        // No SQLSTATE: the server never answered (lost connection) or libpq failed locally.
        if (conn && PQstatus(conn) == CONNECTION_BAD) {
            error.kind = ErrorKind::Connection;
            error.message = messageForSqlState("08006");
        } else {
            error.kind = ErrorKind::Driver;
            error.message = messageForSqlState({});
        }
        return error;
    }

    error.kind = kindForSqlState(state);
    error.message = messageForSqlState(state);
    // The extended protocol takes one statement per call.
    if (error.kind == ErrorKind::Syntax
        && fieldText(result, PG_DIAG_MESSAGE_PRIMARY)
               .startsWith(QLatin1String("cannot insert multiple commands"))) {
        error.kind = ErrorKind::Unsupported;
        error.message = Text::tr("Run one statement at a time.");
        return error;
    }
    if (const int position = fieldText(result, PG_DIAG_STATEMENT_POSITION).toInt(); position > 0) {
        if (const int offset = utf16OffsetFromCharPosition(sql, position); offset >= 0)
            error.position = offset;
    }
    return error;
}

Error connectError(const QString &libpqMessage)
{
    Error error;
    error.detail = libpqMessage.trimmed();
    const QString lower = error.detail.toLower();
    if (lower.contains(QLatin1String("authentication failed"))
        || lower.contains(QLatin1String("no password supplied"))
        || lower.contains(QLatin1String("pg_hba.conf"))
        || lower.contains(QLatin1String("password is required"))) {
        error.kind = ErrorKind::Authentication;
        error.message = messageForSqlState("28000");
    } else if (lower.contains(QLatin1String("timeout expired"))) {
        error.kind = ErrorKind::Timeout;
        error.message = Text::tr("The server did not answer in time.");
    } else if (lower.contains(QLatin1String("database"))
               && lower.contains(QLatin1String("does not exist"))) {
        error.kind = ErrorKind::NotFound;
        error.message = messageForSqlState("3D000");
    } else {
        error.kind = ErrorKind::Connection;
        error.message = Text::tr("Could not connect to the server.");
    }
    return error;
}

Error sendError(PGconn *conn)
{
    Error error;
    error.kind = ErrorKind::Connection;
    error.message = messageForSqlState("08006");
    error.detail = QString::fromUtf8(PQerrorMessage(conn)).trimmed();
    return error;
}

} // namespace quarry::postgres
