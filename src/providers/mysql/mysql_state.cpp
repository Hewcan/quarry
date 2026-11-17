#include "mysql_state.h"

#include "mysql_errors.h"

#include <mutex>

namespace quarry::mysql {

void ensureLibrary()
{
    static std::once_flag once;
    std::call_once(once, [] { mysql_library_init(0, nullptr, nullptr); });
}

ConnectResult connect(const ConnectOptions &options, bool tls, bool withDatabase)
{
    ensureLibrary();
    ConnectResult out;
    MYSQL *mysql = mysql_init(nullptr);
    if (!mysql) {
        out.error = Error { ErrorKind::Internal, QStringLiteral("mysql_init failed"), {}, {}, {} };
        return out;
    }
    const ConnSettings &settings = options.settings;

    // Plain TCP also for "localhost": the form only has host and port, and a socket or named
    // pipe default would not be found.
    const unsigned protocol = MYSQL_PROTOCOL_TCP;
    mysql_options(mysql, MYSQL_OPT_PROTOCOL, &protocol);
    const unsigned connectTimeout
        = options.boundedIo ? kCancelTimeoutSeconds : kConnectTimeoutSeconds;
    mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeout);
    if (options.boundedIo) {
        const unsigned ioTimeout = kCancelTimeoutSeconds;
        mysql_options(mysql, MYSQL_OPT_READ_TIMEOUT, &ioTimeout);
        mysql_options(mysql, MYSQL_OPT_WRITE_TIMEOUT, &ioTimeout);
    }
    const bool no = false;
    mysql_options(mysql, MYSQL_OPT_RECONNECT, &no);
    const unsigned noLocalInfile = 0; // a server must not be able to ask for client files
    mysql_options(mysql, MYSQL_OPT_LOCAL_INFILE, &noLocalInfile);
    mysql_options(mysql, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    mysql_optionsv(mysql, MYSQL_OPT_CONNECT_ATTR_ADD, "program_name", "Quarry");
    if (!options.pluginDir.isEmpty())
        mysql_options(mysql, MYSQL_PLUGIN_DIR, options.pluginDir.constData());

    const bool verify
        = tls && (settings.sslMode == SslMode::VerifyCa || settings.sslMode == SslMode::VerifyFull);
    mysql_options(mysql, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify);
    if (tls) {
        const bool yes = true;
        mysql_options(mysql, MYSQL_OPT_SSL_ENFORCE, &yes);
        if (!settings.rootCert.isEmpty())
            mysql_options(mysql, MYSQL_OPT_SSL_CA, settings.rootCert.constData());
    }

    // CLIENT_FOUND_ROWS: UPDATE reports the rows it matched, not only the ones it changed, as
    // the other engines do (the grid treats "0 rows" as a lost update).
    if (!mysql_real_connect(
            mysql, settings.host.constData(),
            settings.user.isEmpty() ? nullptr : settings.user.constData(),
            settings.password.isEmpty() ? nullptr : settings.password.constData(),
            !withDatabase || settings.database.isEmpty() ? nullptr : settings.database.constData(),
            settings.port, nullptr, CLIENT_FOUND_ROWS)) {
        out.code = mysql_errno(mysql);
        out.error = connectError(out.code, mysql_error(mysql), mysql_sqlstate(mysql));
        mysql_close(mysql);
        return out;
    }
    out.handle = mysql;
    return out;
}

void killQuery(const ConnectOptions &options, unsigned long threadId)
{
    ConnectOptions aux = options;
    aux.boundedIo = true;
    ConnectResult connection = connect(aux, aux.tls, false);
    if (!connection.handle)
        return;
    const QByteArray sql = "KILL QUERY " + QByteArray::number(static_cast<qulonglong>(threadId));
    mysql_real_query(connection.handle, sql.constData(), static_cast<unsigned long>(sql.size()));
    mysql_close(connection.handle);
}

MyState::~MyState()
{
    closeConnection();
    if (m_canceller.joinable())
        m_canceller.join();
}

void MyState::sendKill()
{
    ConnectOptions copy;
    unsigned long id = 0;
    {
        std::unique_lock lock(mutex);
        cancelDone.wait(lock, [this] { return !cancelInFlight; });
        if (!conn)
            return;
        copy = options;
        id = threadId;
        cancelInFlight = true;
    }
    killQuery(copy, id);
    {
        std::lock_guard lock(mutex);
        cancelInFlight = false;
    }
    cancelDone.notify_all();
}

void MyState::requestCancel()
{
    std::lock_guard lock(mutex);
    if (!inCall || !conn)
        return;
    cancel.store(true, std::memory_order_release);
    if (cancelInFlight)
        return;
    if (m_canceller.joinable())
        m_canceller.join(); // the previous one is done: cancelInFlight was false
    cancelInFlight = true;
    m_canceller = std::thread([this, copy = options, id = threadId] {
        mysql_thread_init();
        killQuery(copy, id);
        mysql_thread_end();
        {
            std::lock_guard guard(mutex);
            cancelInFlight = false;
        }
        cancelDone.notify_all();
    });
}

void MyState::drainMoreResults()
{
    while (conn && mysql_more_results(conn)) {
        if (mysql_next_result(conn) > 0)
            break;
        if (MYSQL_RES *extra = mysql_store_result(conn))
            mysql_free_result(extra);
    }
}

void MyState::discardResult()
{
    if (!result)
        return;
    if (conn)
        sendKill();
    mysql_free_result(result); // reads what the server still sends: the error of the kill
    result = nullptr;
    drainMoreResults();
}

void MyState::closeConnection()
{
    if (result && conn)
        discardResult();
    std::unique_lock lock(mutex);
    cancelDone.wait(lock, [this] { return !cancelInFlight; });
    if (result) {
        mysql_free_result(result);
        result = nullptr;
    }
    if (conn) {
        mysql_close(conn);
        conn = nullptr;
    }
}

} // namespace quarry::mysql
