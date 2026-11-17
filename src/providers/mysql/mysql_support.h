#pragma once

// Pure helpers of the MySQL / MariaDB provider: they never call libmariadb, so unit tests can use
// them without a server (the headers are only used for the MYSQL_TYPE_* and *_FLAG constants).

#include <quarry/result.h>
#include <quarry/value_type.h>

#include <QByteArray>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <optional>

namespace quarry::mysql {

constexpr unsigned kConnectTimeoutSeconds = 15; // ARCHITECTURE.md 5
constexpr unsigned kCancelTimeoutSeconds = 5; // auxiliary KILL QUERY connection

// Emulated like libpq's sslmode (libmariadb has no "prefer"): see MysqlConnection::open().
enum class SslMode { Disable, Prefer, Require, VerifyCa, VerifyFull };

struct ConnSettings
{
    QByteArray host;
    unsigned port = 3306;
    QByteArray user;
    QByteArray password; // empty: libmariadb falls back to MYSQL_PWD
    QByteArray database; // empty: no default database
    QByteArray rootCert; // empty: none
    SslMode sslMode = SslMode::Prefer;
};

// `values` are the form fields (host, port, database, user, sslmode, sslrootcert); the password
// comes apart because it is never persisted.
Result<ConnSettings> buildConnSettings(const QVariantMap &values, const QString &password);

// Whether a failed TLS attempt of sslmode=prefer is worth a second try without TLS: the server
// does not offer TLS, or dropped the connection during the handshake.
bool shouldRetryWithoutTls(unsigned code);

// mysql_errno() -> kind and plain-language message (ARCHITECTURE.md 6.6).
ErrorKind kindForErrno(unsigned code);
QString messageForErrno(unsigned code);

// MYSQL_FIELD::type, ::charsetnr and ::flags -> normalized type (ARCHITECTURE.md 6.6).
ValueType valueTypeForColumn(unsigned fieldType, unsigned charset);
// "bigint unsigned", "varchar", "blob"...
QString nativeTypeName(unsigned fieldType, unsigned charset, unsigned flags);
// information_schema.COLUMNS.DATA_TYPE ("int", "varchar"...) -> normalized type.
ValueType valueTypeForDataType(QStringView dataType);

// Where libmariadb finds its authentication plugins: <executable dir>/mariadb-plugins. They can
// not live in plugins/quarry, which rejects every library that is not a Quarry plugin.
QString pluginDirectory(const QString &executableDir);

bool isMariaDb(QByteArrayView serverInfo);
// "11.8.10-MariaDB" -> "11.8.10"; drops the "5.5.5-" prefix MariaDB sends for old clients.
QString plainVersion(QByteArrayView serverInfo);

// BIT values arrive as big-endian bytes; nullopt if they do not fit 64 bits.
std::optional<qint64> bitValue(QByteArrayView bytes);

// Body of a string literal (without the quotes) as mysql_real_escape_string makes it: it knows
// the charset of the connection and NO_BACKSLASH_ESCAPES.
using EscapeFn = std::function<QByteArray(const QByteArray &)>;
// Replaces every '?' outside strings, identifiers and comments with the literal of the matching
// parameter, as UTF-8. Fails if the counts differ or a value has no literal form.
// shortcut: client-side binding on the text protocol (mysql_real_query + mysql_use_result do not
// mix with prepared statements); switch to mysql_stmt_* if the binary protocol is ever needed.
Result<QByteArray> bindParameters(const QString &sql, const QVariantList &parameters,
                                  const EscapeFn &escape);

} // namespace quarry::mysql
