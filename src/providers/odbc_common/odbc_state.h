#pragma once

#include "odbc_diag.h"
#include "odbc_headers.h"

#include <quarry/result.h>

#include <QString>
#include <QStringList>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace quarry::odbc {

// The environment handle of the process (ODBC 3.8, one per plugin copy). Null if the driver
// manager cannot be initialized. Never freed: the plugin may be unloaded while the process goes
// on, and the handle is a few bytes.
SQLHENV environment();

// Names of the drivers registered in the driver manager (SQLDrivers), in its order.
QStringList installedDrivers();

// Handle shared by a connection and its streams. Everything is single-threaded (the session
// thread) except requestCancel(), which may run anywhere.
struct OdbcState
{
    SQLHDBC dbc = SQL_NULL_HDBC; // written under `mutex` (open/close)
    // Bumped whenever `dbc` is released: a statement that outlives it must not be freed (the
    // driver manager freed it with the connection).
    std::atomic<std::uint64_t> epoch { 1 };
    QString dbmsName; // SQLGetInfo(SQL_DBMS_NAME)
    QString dbmsVersion; // SQLGetInfo(SQL_DBMS_VER)
    QString searchEscape; // SQLGetInfo(SQL_SEARCH_PATTERN_ESCAPE)
    bool streamOpen = false; // session thread only: one active cursor per connection
    bool inTransaction = false; // session thread only

    std::mutex mutex; // guards dbc lifetime, inCall, active and callId against requestCancel()
    bool inCall = false; // a provider call is in flight
    SQLHSTMT active = SQL_NULL_HSTMT; // statement the call is blocked in, if any
    std::uint64_t callId = 0;
    // A cancel that landed while the call was running. Checked before the call enters the driver
    // (a cancel that came earlier found nothing to cancel).
    std::atomic<bool> cancel { false };

    OdbcState() = default;
    OdbcState(const OdbcState &) = delete;
    OdbcState &operator=(const OdbcState &) = delete;
    ~OdbcState();

    // Thread-safe and fast: SQLCancel on the statement of the call in flight. Does nothing for an
    // idle connection (it would discard the cursor of a stream between fetches). A cancel can
    // reach the driver in the instant between "statement marked active" and "driver entered the
    // call" and be lost, so a helper thread repeats it every 50 ms while the same call runs.
    void requestCancel();

    // Session thread only: rolls back an open transaction, disconnects and frees the connection.
    void closeConnection();

private:
    void repeatCancel();

    std::condition_variable m_wake;
    std::thread m_repeater;
    bool m_repeaterRunning = false; // under `mutex`
    bool m_stopping = false; // under `mutex`
    std::uint64_t m_target = 0; // call the repeater is serving, under `mutex`
};

// Marks one provider call (open/children/describe/execute/fetch) and discards any cancel
// request that arrived before it started (cancellation contract).
class CallScope
{
public:
    explicit CallScope(OdbcState &state)
        : m_state(state)
    {
        std::lock_guard lock(m_state.mutex);
        m_state.cancel.store(false, std::memory_order_release);
        m_state.inCall = true;
        ++m_state.callId;
    }
    CallScope(const CallScope &) = delete;
    CallScope &operator=(const CallScope &) = delete;
    ~CallScope()
    {
        std::lock_guard lock(m_state.mutex);
        m_state.inCall = false;
    }

private:
    OdbcState &m_state;
};

// Makes `statement` the target of requestCancel() while a blocking driver call runs. Refuses
// (explicit operator bool is false) when a cancel is already pending: the caller reports
// Cancelled without entering the driver.
class ActiveStatement
{
public:
    ActiveStatement(OdbcState &state, SQLHSTMT statement);
    ActiveStatement(const ActiveStatement &) = delete;
    ActiveStatement &operator=(const ActiveStatement &) = delete;
    ~ActiveStatement();
    explicit operator bool() const noexcept { return m_attached; }

private:
    OdbcState &m_state;
    bool m_attached = false;
};

// Owner of an SQLHSTMT. Frees it unless the connection was released in the meantime.
class StatementHandle
{
public:
    StatementHandle() = default;
    StatementHandle(std::shared_ptr<OdbcState> state, SQLHSTMT handle);
    StatementHandle(StatementHandle &&other) noexcept;
    StatementHandle &operator=(StatementHandle &&other) noexcept;
    StatementHandle(const StatementHandle &) = delete;
    StatementHandle &operator=(const StatementHandle &) = delete;
    ~StatementHandle() { reset(); }

    SQLHSTMT get() const noexcept { return m_handle; }
    explicit operator bool() const noexcept { return m_handle != SQL_NULL_HSTMT; }
    // Closes the cursor so the connection can run the next statement. `discard`: cancel first,
    // so the server does not send the rest of an unfinished result.
    void closeCursor(bool discard);
    void reset();

private:
    std::shared_ptr<OdbcState> m_state;
    SQLHSTMT m_handle = SQL_NULL_HSTMT;
    std::uint64_t m_epoch = 0;
};

// SQLAllocHandle(SQL_HANDLE_STMT) on the connection of `state`.
Result<StatementHandle> allocateStatement(const std::shared_ptr<OdbcState> &state);

// SQLSetStmtAttr / SQLSetConnectAttr with an integer value (the API passes it as a pointer).
SQLRETURN setStmtAttr(SQLHSTMT statement, SQLINTEGER attribute, std::uintptr_t value);
SQLRETURN setConnectAttr(SQLHDBC connection, SQLINTEGER attribute, std::uintptr_t value);

} // namespace quarry::odbc
