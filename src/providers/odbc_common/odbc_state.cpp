#include "odbc_state.h"

#include "odbc_support.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace quarry::odbc {

namespace {

constexpr int kMaxCancelRepeats = 100; // 5 s
constexpr std::chrono::milliseconds kCancelRepeatInterval { 50 };

// The attributes of the API carry integers in a pointer argument.
SQLPOINTER integerAsPointer(std::uintptr_t value)
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr): that is how ODBC takes an integer attribute
    return reinterpret_cast<SQLPOINTER>(value);
}

SQLHENV allocateEnvironment()
{
    SQLHENV handle = SQL_NULL_HENV;
    if (!succeeded(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &handle)))
        return SQL_NULL_HENV;
        // ODBC 3.8 where the driver manager knows it, 3.0 otherwise.
#ifdef SQL_OV_ODBC3_80
    if (succeeded(
            SQLSetEnvAttr(handle, SQL_ATTR_ODBC_VERSION, integerAsPointer(SQL_OV_ODBC3_80), 0)))
        return handle;
#endif
    if (succeeded(SQLSetEnvAttr(handle, SQL_ATTR_ODBC_VERSION, integerAsPointer(SQL_OV_ODBC3), 0)))
        return handle;
    SQLFreeHandle(SQL_HANDLE_ENV, handle);
    return SQL_NULL_HENV;
}

} // namespace

SQLHENV environment()
{
    static const SQLHENV kHandle = allocateEnvironment();
    return kHandle;
}

QStringList installedDrivers()
{
    QStringList out;
    const SQLHENV env = environment();
    if (!env)
        return out;
    constexpr SQLSMALLINT kChars = 512;
    std::vector<SQLWCHAR> description(kChars);
    std::vector<SQLWCHAR> attributes(kChars);
    SQLUSMALLINT direction = SQL_FETCH_FIRST;
    for (int guard = 0; guard < 1000; ++guard) {
        SQLSMALLINT descriptionLength = 0;
        SQLSMALLINT attributesLength = 0;
        const SQLRETURN rc
            = SQLDriversW(env, direction, description.data(), kChars, &descriptionLength,
                          attributes.data(), kChars, &attributesLength);
        if (!succeeded(rc))
            break;
        out << fromWide(description.data(), std::min<qsizetype>(descriptionLength, kChars - 1));
        direction = SQL_FETCH_NEXT;
    }
    return out;
}

OdbcState::~OdbcState()
{
    closeConnection();
    {
        std::lock_guard lock(mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    if (m_repeater.joinable())
        m_repeater.join();
}

void OdbcState::requestCancel()
{
    std::thread finished;
    {
        std::lock_guard lock(mutex);
        if (!inCall)
            return;
        cancel.store(true, std::memory_order_release);
        if (!active)
            return; // the call checks the flag before it enters the driver
        SQLCancel(active);
        m_target = callId;
        if (!m_repeaterRunning) {
            finished = std::move(m_repeater); // already past its last locked step
            m_repeaterRunning = true;
            m_repeater = std::thread([this] { repeatCancel(); });
        }
    }
    if (finished.joinable())
        finished.join();
}

void OdbcState::repeatCancel()
{
    std::unique_lock lock(mutex);
    for (int round = 0; round < kMaxCancelRepeats && !m_stopping; ++round) {
        m_wake.wait_for(lock, kCancelRepeatInterval);
        if (m_stopping || !inCall || !active || callId != m_target)
            break;
        SQLCancel(active);
    }
    m_repeaterRunning = false;
}

void OdbcState::closeConnection()
{
    SQLHDBC handle = SQL_NULL_HDBC;
    {
        std::lock_guard lock(mutex);
        handle = dbc;
        dbc = SQL_NULL_HDBC;
        epoch.fetch_add(1);
    }
    streamOpen = false;
    if (!handle)
        return;
    // Disconnecting with a transaction pending is an error (25000).
    if (inTransaction) {
        SQLEndTran(SQL_HANDLE_DBC, handle, SQL_ROLLBACK);
        inTransaction = false;
    }
    SQLDisconnect(handle);
    SQLFreeHandle(SQL_HANDLE_DBC, handle);
}

ActiveStatement::ActiveStatement(OdbcState &state, SQLHSTMT statement)
    : m_state(state)
{
    std::lock_guard lock(m_state.mutex);
    if (m_state.cancel.load(std::memory_order_acquire))
        return;
    m_state.active = statement;
    m_attached = true;
}

ActiveStatement::~ActiveStatement()
{
    if (!m_attached)
        return;
    std::lock_guard lock(m_state.mutex);
    m_state.active = SQL_NULL_HSTMT;
}

StatementHandle::StatementHandle(std::shared_ptr<OdbcState> state, SQLHSTMT handle)
    : m_state(std::move(state))
    , m_handle(handle)
    , m_epoch(m_state->epoch.load())
{ }

StatementHandle::StatementHandle(StatementHandle &&other) noexcept
    : m_state(std::move(other.m_state))
    , m_handle(std::exchange(other.m_handle, nullptr))
    , m_epoch(other.m_epoch)
{ }

StatementHandle &StatementHandle::operator=(StatementHandle &&other) noexcept
{
    if (this != &other) {
        reset();
        m_state = std::move(other.m_state);
        m_handle = std::exchange(other.m_handle, nullptr);
        m_epoch = other.m_epoch;
    }
    return *this;
}

void StatementHandle::closeCursor(bool discard)
{
    if (!m_handle || m_state->epoch.load() != m_epoch)
        return;
    // SQLCancel with no call running ends the pending result without reading it (SQL Server
    // sends an attention); SQLFreeStmt(SQL_CLOSE) then leaves the statement reusable.
    if (discard)
        SQLCancel(m_handle);
    SQLFreeStmt(m_handle, SQL_CLOSE);
}

void StatementHandle::reset()
{
    if (!m_handle)
        return;
    if (m_state->epoch.load() == m_epoch) {
        SQLCancel(m_handle);
        SQLFreeHandle(SQL_HANDLE_STMT, m_handle);
    }
    m_handle = SQL_NULL_HSTMT;
    m_state.reset();
}

Result<StatementHandle> allocateStatement(const std::shared_ptr<OdbcState> &state)
{
    SQLHSTMT handle = SQL_NULL_HSTMT;
    if (!state->dbc)
        return fail(notOpenError());
    const SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_STMT, state->dbc, &handle);
    if (!succeeded(rc) || !handle)
        return fail(errorFromHandle(SQL_HANDLE_DBC, state->dbc));
    return StatementHandle(state, handle);
}

SQLRETURN setStmtAttr(SQLHSTMT statement, SQLINTEGER attribute, std::uintptr_t value)
{
    return SQLSetStmtAttr(statement, attribute, integerAsPointer(value), 0);
}

SQLRETURN setConnectAttr(SQLHDBC connection, SQLINTEGER attribute, std::uintptr_t value)
{
    return SQLSetConnectAttr(connection, attribute, integerAsPointer(value), 0);
}

} // namespace quarry::odbc
