#include "mysql_errors.h"

#include "mysql_support.h"
#include "mysql_text.h"

namespace quarry::mysql {

Error errorFromCode(unsigned code, const char *text, const char *sqlState)
{
    Error error;
    error.kind = kindForErrno(code);
    error.message = messageForErrno(code);
    error.sqlState = QString::fromLatin1(sqlState ? sqlState : "");
    error.detail = QStringLiteral("%1 (error %2, SQLSTATE %3)")
                       .arg(QString::fromUtf8(text ? text : ""))
                       .arg(code)
                       .arg(error.sqlState.isEmpty() ? QStringLiteral("-") : error.sqlState);
    return error;
}

Error errorFromHandle(MYSQL *conn)
{
    return errorFromCode(mysql_errno(conn), mysql_error(conn), mysql_sqlstate(conn));
}

Error connectError(unsigned code, const char *text, const char *sqlState)
{
    Error error = errorFromCode(code, text, sqlState);
    if (error.kind == ErrorKind::Driver) {
        error.kind = ErrorKind::Connection;
        error.message = Text::tr("Could not connect to the server.");
    }
    return error;
}

Error cancelledError()
{
    return Error { ErrorKind::Cancelled,
                   messageForErrno(1317),
                   QStringLiteral("1317 query interrupted"),
                   QStringLiteral("70100"),
                   {} };
}

Error notOpenError()
{
    return Error { ErrorKind::Connection, Text::tr("The connection is not open."), {}, {}, {} };
}

Error busyError()
{
    return Error {
        ErrorKind::Internal, Text::tr("The connection is busy with another result."), {}, {}, {}
    };
}

} // namespace quarry::mysql
