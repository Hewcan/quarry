#pragma once

#include <quarry/result.h>

#include <QString>

#include <mysql.h>

namespace quarry::mysql {

// Error from the last failed call on `conn` (mysql_errno, mysql_error, mysql_sqlstate).
Error errorFromHandle(MYSQL *conn);

// Same, from the pieces: what connect() reports when there is no usable handle.
Error errorFromCode(unsigned code, const char *text, const char *sqlState);

// Error of a failed connection attempt: anything the code table does not know is a connection
// problem there, not a statement the server rejected.
Error connectError(unsigned code, const char *text, const char *sqlState);

Error cancelledError();
Error notOpenError();
Error busyError();

} // namespace quarry::mysql
