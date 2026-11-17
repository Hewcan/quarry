#pragma once

#include "mysql_support.h"

#include <quarry/result.h>

#include <QByteArray>
#include <QStringList>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <mysql.h>
#include <thread>

namespace quarry::mysql {

// Initializes libmariadb once per process (mysql_library_init is not thread-safe on its own).
// There is no matching mysql_library_end: the plugin may be unloaded while the process goes on,
// and ending the library twice is worse than keeping its few global buffers.
void ensureLibrary();

// How to reach the server again: the main connection and the KILL QUERY one use the same.
struct ConnectOptions
{
    ConnSettings settings;
    bool tls = false; // what the main connection ended up using
    QByteArray pluginDir; // MYSQL_PLUGIN_DIR: authentication plugins (caching_sha2_password...)
    bool boundedIo = false; // read/write timeouts of kCancelTimeoutSeconds (auxiliary connection)
};

// Result of connect(): the handle, or the libmariadb error that stopped it.
struct ConnectResult
{
    MYSQL *handle = nullptr;
    Error error;
    unsigned code = 0; // mysql_errno()
};

// mysql_init + options + mysql_real_connect. `withDatabase` false skips the default schema.
// The caller owns the handle (mysql_close).
ConnectResult connect(const ConnectOptions &options, bool tls, bool withDatabase);

// Opens a short-lived connection and runs KILL QUERY <threadId> on it. Any thread. Failures are
// ignored: if the server is gone the query is gone too.
void killQuery(const ConnectOptions &options, unsigned long threadId);

// Handle shared by a connection and its streams. Everything is single-threaded (the session
// thread) except requestCancel(), which may run anywhere.
struct MyState
{
    MYSQL *conn = nullptr; // written under `mutex` (open/close)
    MYSQL_RES *result = nullptr; // use_result of the stream that owns the connection, if any
    unsigned long threadId = 0; // mysql_thread_id(conn): the target of KILL QUERY
    ConnectOptions options; // set before `conn`, read by the cancel path
    std::mutex mutex; // guards conn lifetime against requestCancel(), inCall and cancelInFlight
    std::condition_variable cancelDone;
    bool inCall = false; // a provider call is in flight
    bool cancelInFlight = false; // an auxiliary connection is talking to the server
    // A cancel that landed while the call was running. MySQL has no way to re-send it, so the
    // call checks it before it sends the statement, and after: SLEEP() returns normally when it
    // is killed, and that has to be reported as a cancellation.
    std::atomic<bool> cancel { false };

    MyState() = default;
    MyState(const MyState &) = delete;
    MyState &operator=(const MyState &) = delete;
    ~MyState();

    // Session thread only, blocking: KILL QUERY for what the connection runs (discarding an
    // unfinished stream). The mutex is not held during the network round trip, so requestCancel()
    // never waits for it.
    void sendKill();

    // Thread-safe and non-blocking: the round trip runs on a worker thread. Only acts while a
    // call is in flight: an idle connection has nothing to cancel, and a stream between fetches
    // must not lose its query.
    void requestCancel();

    // Reads the rest of the open result (after KILL QUERY, so it is short), waits for an
    // in-flight cancel so it cannot reach a later statement, then closes.
    void closeConnection();

    // Frees `result` after asking the server to stop sending rows. Session thread only.
    void discardResult();
    // Reads and drops every further result of a multi-result statement (CALL).
    void drainMoreResults();

private:
    std::thread m_canceller;
};

// Marks one provider call (open/children/describe/execute/fetch) and discards any cancel
// request that arrived before it started (cancellation contract).
class CallScope
{
public:
    explicit CallScope(MyState &state)
        : m_state(state)
    {
        std::unique_lock lock(m_state.mutex);
        // A cancel still travelling belongs to the previous call: let it land first.
        m_state.cancelDone.wait(lock, [this] { return !m_state.cancelInFlight; });
        m_state.cancel.store(false, std::memory_order_release);
        m_state.inCall = true;
    }
    CallScope(const CallScope &) = delete;
    CallScope &operator=(const CallScope &) = delete;
    ~CallScope()
    {
        std::lock_guard lock(m_state.mutex);
        m_state.inCall = false;
    }

private:
    MyState &m_state;
};

} // namespace quarry::mysql
