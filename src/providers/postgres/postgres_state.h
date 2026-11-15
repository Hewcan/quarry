#pragma once

#include <QString>
#include <QStringList>

#include <atomic>
#include <condition_variable>
#include <libpq-fe.h>
#include <mutex>
#include <thread>

namespace quarry::postgres {

// Handle shared by a connection and its streams. Everything is single-threaded (the session
// thread) except requestCancel(), which may run anywhere.
struct PgState
{
    PGconn *conn = nullptr; // written under `mutex` (open/close)
    std::mutex mutex; // guards conn lifetime against requestCancel(), inCall and cancelInFlight
    std::condition_variable cancelDone;
    bool inCall = false; // a provider call is in flight
    bool cancelInFlight = false; // a cancel connection is talking to the server
    // A cancel that landed while the call was running; lets the call re-send it if the cancel
    // arrived before the statement reached the server.
    std::atomic<bool> cancel { false };
    QStringList notices; // filled by the notice receiver on the session thread

    PgState() = default;
    PgState(const PgState &) = delete;
    PgState &operator=(const PgState &) = delete;
    ~PgState()
    {
        closeConnection();
        if (canceller.joinable())
            canceller.join();
    }

    // Session thread only, blocking: asks the server to cancel what the connection is running
    // (re-send path and discarding an unfinished stream). The mutex is not held during the
    // network round trip, so requestCancel() never waits for it.
    void sendCancel()
    {
        PGcancelConn *cancelConn = nullptr;
        {
            std::unique_lock lock(mutex);
            cancelDone.wait(lock, [this] { return !cancelInFlight; });
            if (!conn)
                return;
            cancelConn = PQcancelCreate(conn);
            if (!cancelConn)
                return;
            cancelInFlight = true;
        }
        runCancel(cancelConn);
    }

    // Thread-safe and non-blocking (microseconds): the round trip runs on a worker thread. Only
    // acts while a call is in flight: an idle connection has nothing to cancel, and a stream
    // between fetches must not lose its query.
    void requestCancel()
    {
        std::lock_guard lock(mutex);
        if (!inCall || !conn)
            return;
        cancel.store(true, std::memory_order_release);
        if (cancelInFlight)
            return;
        PGcancelConn *cancelConn = PQcancelCreate(conn); // independent of conn from here on
        if (!cancelConn)
            return;
        if (canceller.joinable())
            canceller.join(); // the previous one is done: cancelInFlight was false
        cancelInFlight = true;
        canceller = std::thread([this, cancelConn] { runCancel(cancelConn); });
    }

    // Waits for an in-flight cancel, so it cannot reach a later statement, then closes.
    void closeConnection()
    {
        std::unique_lock lock(mutex);
        cancelDone.wait(lock, [this] { return !cancelInFlight; });
        if (conn) {
            PQfinish(conn);
            conn = nullptr;
        }
    }

private:
    // Bounded by the connect_timeout that the cancel connection inherits from conn.
    void runCancel(PGcancelConn *cancelConn)
    {
        PQcancelBlocking(cancelConn);
        PQcancelFinish(cancelConn);
        {
            std::lock_guard lock(mutex);
            cancelInFlight = false;
        }
        cancelDone.notify_all();
    }

    std::thread canceller;
};

// Marks one provider call (open/children/describe/execute/fetch) and discards any cancel
// request that arrived before it started (cancellation contract).
class CallScope
{
public:
    explicit CallScope(PgState &state)
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
    PgState &m_state;
};

// Reads and drops every pending result so the connection is ready for the next command.
inline void discardResults(PGconn *conn)
{
    while (PGresult *result = PQgetResult(conn)) {
        const ExecStatusType status = PQresultStatus(result);
        PQclear(result);
        if (status == PGRES_COPY_IN) {
            PQputCopyEnd(conn, "COPY is not supported");
        } else if (status == PGRES_COPY_OUT) {
            char *buffer = nullptr;
            while (PQgetCopyData(conn, &buffer, 0) > 0)
                PQfreemem(buffer);
        } else if (status == PGRES_COPY_BOTH) {
            break;
        }
    }
}

} // namespace quarry::postgres
