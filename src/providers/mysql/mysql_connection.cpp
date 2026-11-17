#include "mysql_connection.h"

#include "mysql_dialect.h"
#include "mysql_errors.h"
#include "mysql_stream.h"
#include "mysql_support.h"
#include "mysql_text.h"

#include <QCoreApplication>

namespace quarry::mysql {

namespace {

const MysqlDialect kDialect;

QVariant cellText(const MYSQL_ROW &row, const unsigned long *lengths, unsigned index)
{
    return row[index]
        ? QVariant(QString::fromUtf8(row[index], static_cast<qsizetype>(lengths[index])))
        : QVariant();
}

} // namespace

MysqlConnection::MysqlConnection(ConnectionConfig config, QString password)
    : m_config(std::move(config))
    , m_password(std::move(password))
    , m_state(std::make_shared<MyState>())
{ }

MysqlConnection::~MysqlConnection()
{
    m_state->closeConnection();
}

bool MysqlConnection::isOpen() const
{
    return m_state->conn != nullptr;
}

Result<ServerInfo> MysqlConnection::open(const CancellationToken &token)
{
    CallScope scope(*m_state);
    if (m_state->conn)
        return fail(
            Error { ErrorKind::Internal, Text::tr("The connection is already open."), {}, {}, {} });
    if (token.isCancelled())
        return fail(cancelledError());

    auto settings = buildConnSettings(m_config.values, m_password);
    if (!settings)
        return fail(std::move(settings).error());

    ensureLibrary();
    // The C API asks every thread that uses a handle to call this; the session thread does it
    // here, the cancel thread in MyState::requestCancel(). Cheap if libmariadb has nothing to set
    // up.
    mysql_thread_init();

    ConnectOptions options;
    options.settings = std::move(settings).value();
    options.pluginDir = pluginDirectory(QCoreApplication::applicationDirPath()).toUtf8();
    const SslMode mode = options.settings.sslMode;

    // "prefer" does not exist in libmariadb: try with TLS and, if the server does not offer it
    // or the handshake fails, once more without. require and verify-* never fall back.
    ConnectResult connection = connect(options, mode != SslMode::Disable, true);
    if (!connection.handle && mode == SslMode::Prefer && shouldRetryWithoutTls(connection.code))
        connection = connect(options, false, true);
    if (!connection.handle)
        return fail(std::move(connection.error));

    MYSQL *conn = connection.handle;
    if (mysql_set_character_set(conn, "utf8mb4") != 0) {
        Error error = errorFromHandle(conn);
        mysql_close(conn);
        return fail(std::move(error));
    }
    options.tls = mysql_get_ssl_cipher(conn) != nullptr;
    const QByteArray serverInfo(mysql_get_server_info(conn));
    {
        std::lock_guard lock(m_state->mutex);
        m_state->options = std::move(options);
        m_state->threadId = mysql_thread_id(conn);
        m_state->conn = conn;
    }
    return ServerInfo { isMariaDb(serverInfo) ? QStringLiteral("MariaDB") : QStringLiteral("MySQL"),
                        plainVersion(serverInfo) };
}

void MysqlConnection::close()
{
    m_state->closeConnection();
}

Result<void> MysqlConnection::exec(const char *sql)
{
    CallScope scope(*m_state);
    MYSQL *conn = m_state->conn;
    if (!conn)
        return fail(notOpenError());
    if (mysql_real_query(conn, sql, static_cast<unsigned long>(qstrlen(sql))) != 0)
        return fail(errorFromHandle(conn));
    if (MYSQL_RES *result = mysql_store_result(conn))
        mysql_free_result(result);
    m_state->drainMoreResults();
    return {};
}

Result<QByteArray> MysqlConnection::bind(const QString &sql, const QVariantList &args)
{
    MYSQL *conn = m_state->conn;
    return bindParameters(sql, args, [conn](const QByteArray &in) {
        QByteArray out(in.size() * 2 + 1, '\0');
        const unsigned long length = mysql_real_escape_string(
            conn, out.data(), in.constData(), static_cast<unsigned long>(in.size()));
        out.truncate(static_cast<qsizetype>(length));
        return out;
    });
}

Result<MysqlConnection::Rows> MysqlConnection::runRows(const QByteArray &sql)
{
    MYSQL *conn = m_state->conn;
    if (mysql_real_query(conn, sql.constData(), static_cast<unsigned long>(sql.size())) != 0)
        return fail(errorFromHandle(conn));
    MYSQL_RES *result = mysql_store_result(conn);
    if (!result) {
        if (mysql_field_count(conn) != 0)
            return fail(errorFromHandle(conn));
        return Rows {};
    }
    Rows rows;
    rows.reserve(static_cast<size_t>(mysql_num_rows(result)));
    const unsigned columns = mysql_num_fields(result);
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        const unsigned long *lengths = mysql_fetch_lengths(result);
        QVariantList values;
        values.reserve(columns);
        for (unsigned i = 0; i < columns; ++i)
            values.push_back(cellText(row, lengths, i));
        rows.push_back(std::move(values));
    }
    mysql_free_result(result);
    m_state->drainMoreResults();
    return rows;
}

Result<MysqlConnection::Rows> MysqlConnection::queryRows(const QString &sql,
                                                         const QVariantList &args)
{
    auto bound = bind(sql, args);
    if (!bound)
        return fail(std::move(bound).error());
    return runRows(bound.value());
}

Result<std::unique_ptr<IResultStream>> MysqlConnection::execute(const Statement &statement,
                                                                const CancellationToken &token)
{
    CallScope scope(*m_state);
    MYSQL *conn = m_state->conn;
    if (!conn)
        return fail(notOpenError());
    if (token.isCancelled())
        return fail(cancelledError());
    if (m_state->result)
        return fail(busyError());

    // The server answers "Query was empty" to nothing but comments; every other engine of the
    // suite takes it as an empty result.
    if (kDialect.splitStatements(statement.sql).empty())
        return std::unique_ptr<IResultStream>(MysqlStream::createEmpty(m_state));

    QByteArray sql;
    if (statement.parameters.isEmpty()) {
        sql = statement.sql.toUtf8();
    } else {
        auto bound = bind(statement.sql, statement.parameters);
        if (!bound)
            return fail(std::move(bound).error());
        sql = std::move(bound).value();
    }

    // A cancel that came in while the text was prepared found nothing to kill.
    if (m_state->cancel.load(std::memory_order_acquire))
        return fail(cancelledError());
    if (mysql_real_query(conn, sql.constData(), static_cast<unsigned long>(sql.size())) != 0) {
        Error error = errorFromHandle(conn);
        // The connection takes one statement per call; the server just calls the second one a
        // syntax error.
        if (error.kind == ErrorKind::Syntax && kDialect.splitStatements(statement.sql).size() > 1) {
            error.kind = ErrorKind::Unsupported;
            error.message = Text::tr("Run one statement at a time.");
        }
        return fail(std::move(error));
    }

    auto stream = MysqlStream::create(m_state);
    if (!stream)
        return fail(std::move(stream).error());
    return std::unique_ptr<IResultStream>(std::move(stream).value());
}

} // namespace quarry::mysql
