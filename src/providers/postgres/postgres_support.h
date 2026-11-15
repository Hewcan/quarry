#pragma once

// Pure helpers of the PostgreSQL provider: no libpq, so unit tests can use them without a server.

#include <quarry/result.h>
#include <quarry/value_type.h>

#include <QByteArray>
#include <QString>
#include <QVariantMap>

#include <optional>
#include <utility>
#include <vector>

namespace quarry::postgres {

// Keyword/value pairs for PQconnectdbParams. Never a conninfo string: values with spaces or
// quotes need no escaping this way.
using ConnParams = std::vector<std::pair<QByteArray, QByteArray>>;

// `values` are the form fields (host, port, database, user, sslmode, sslrootcert); the password
// comes apart because it is never persisted. Sets connect_timeout, application_name, UTF-8 and
// the session defaults the provider relies on (DateStyle=ISO, bytea_output=hex).
Result<ConnParams> buildConnParams(const QVariantMap &values, const QString &password);

// SQLSTATE (5 chars, may be empty) -> kind and plain-language message (ARCHITECTURE.md 6.3).
ErrorKind kindForSqlState(QByteArrayView sqlState);
QString messageForSqlState(QByteArrayView sqlState);

// PG_DIAG_STATEMENT_POSITION counts characters (1-based). Returns the 0-based UTF-16 offset
// into `sql`, or -1 if the position is out of range.
int utf16OffsetFromCharPosition(QStringView sql, int position);

// bytea in hex output format ("\x00ff") -> bytes; nullopt if it is not hex format.
std::optional<QByteArray> decodeByteaHex(QByteArrayView text);

ValueType valueTypeForOid(unsigned oid);
// Name of a well known type OID, or "oid:<n>".
QString nativeTypeName(unsigned oid);

// float4/float8/numeric text including NaN and +-Infinity.
std::optional<double> parseDouble(QByteArrayView text);

} // namespace quarry::postgres
