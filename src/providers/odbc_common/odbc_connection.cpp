#include "odbc_connection.h"

#include "odbc_data.h"
#include "odbc_diag.h"
#include "odbc_stream.h"
#include "odbc_support.h"
#include "odbc_text.h"

#include <algorithm>

namespace quarry::odbc {

namespace {

constexpr SQLSMALLINT kInfoChars = 256;
constexpr SQLSMALLINT kOutputChars = 1024;

// SQLGetInfoW for a string value. The buffer length and the result are in bytes.
QString infoString(SQLHDBC connection, SQLUSMALLINT info)
{
    std::vector<SQLWCHAR> buffer(static_cast<size_t>(kInfoChars));
    SQLSMALLINT bytes = 0;
    if (!succeeded(SQLGetInfoW(connection, info, buffer.data(),
                               static_cast<SQLSMALLINT>(buffer.size() * sizeof(SQLWCHAR)), &bytes)))
        return {};
    const qsizetype chars
        = std::min<qsizetype>(bytes / static_cast<qsizetype>(sizeof(SQLWCHAR)), kInfoChars - 1);
    return fromWide(buffer.data(), chars).trimmed();
}

} // namespace

OdbcConnection::OdbcConnection()
    : m_state(std::make_shared<OdbcState>())
{ }

OdbcConnection::~OdbcConnection()
{
    m_state->closeConnection();
}

Result<ServerInfo> OdbcConnection::open(const CancellationToken &token)
{
    CallScope scope(*m_state);
    if (m_state->dbc)
        return fail(internalError(Text::tr("The connection is already open.")));
    if (token.isCancelled())
        return fail(cancelledError());

    auto request = connectRequest();
    if (!request)
        return fail(std::move(request).error());
    const SQLHENV env = environment();
    if (!env) {
        Error error;
        error.kind = ErrorKind::Driver;
        error.message = Text::tr("The ODBC driver manager could not be started.");
        error.detail = error.message;
        return fail(std::move(error));
    }
    SQLHDBC connection = SQL_NULL_HDBC;
    if (!succeeded(SQLAllocHandle(SQL_HANDLE_DBC, env, &connection)))
        return fail(errorFromHandle(SQL_HANDLE_ENV, env));
    // The connection string may say it too; this covers drivers with another keyword.
    setConnectAttr(connection, SQL_ATTR_LOGIN_TIMEOUT, kLoginTimeoutSeconds);

    std::vector<SQLWCHAR> text = toWide(request.value().connectionString);
    std::vector<SQLWCHAR> completed(static_cast<size_t>(kOutputChars));
    SQLSMALLINT completedLength = 0;
    const SQLRETURN rc
        = SQLDriverConnectW(connection, nullptr, text.data(), SQL_NTS, completed.data(),
                            kOutputChars, &completedLength, SQL_DRIVER_NOPROMPT);
    if (!succeeded(rc)) {
        Error error
            = connectError(readDiagnostics(SQL_HANDLE_DBC, connection), request.value().target);
        SQLFreeHandle(SQL_HANDLE_DBC, connection);
        return fail(std::move(error));
    }

    m_state->dbmsName = infoString(connection, SQL_DBMS_NAME);
    m_state->dbmsVersion = infoString(connection, SQL_DBMS_VER);
    m_state->searchEscape = infoString(connection, SQL_SEARCH_PATTERN_ESCAPE);
    {
        std::lock_guard lock(m_state->mutex);
        m_state->dbc = connection;
    }
    return ServerInfo { productName(m_state->dbmsName), m_state->dbmsVersion };
}

void OdbcConnection::close()
{
    m_state->closeConnection();
}

Result<std::unique_ptr<IResultStream>> OdbcConnection::execute(const Statement &statement,
                                                               const CancellationToken &token)
{
    CallScope scope(*m_state);
    if (!m_state->dbc)
        return fail(notOpenError());
    if (token.isCancelled())
        return fail(cancelledError());
    if (m_state->streamOpen)
        return fail(busyError());

    // Some servers answer "empty query" to nothing but comments; the rest of the suite takes it
    // as an empty result.
    if (sqlDialect().splitStatements(statement.sql).empty())
        return std::unique_ptr<IResultStream>(OdbcStream::createEmpty(m_state));

    auto stream = OdbcStream::run(m_state, statement, bindsVariableLength());
    if (!stream)
        return fail(std::move(stream).error());
    return std::unique_ptr<IResultStream>(std::move(stream).value());
}

Result<void> OdbcConnection::begin()
{
    CallScope scope(*m_state);
    if (!m_state->dbc)
        return fail(notOpenError());
    // Autocommit off is how ODBC starts a transaction: the driver opens one with the next
    // statement and SQLEndTran closes it.
    if (!succeeded(setConnectAttr(m_state->dbc, SQL_ATTR_AUTOCOMMIT, SQL_AUTOCOMMIT_OFF)))
        return fail(errorFromHandle(SQL_HANDLE_DBC, m_state->dbc));
    m_state->inTransaction = true;
    return {};
}

Result<void> OdbcConnection::endTransaction(SQLSMALLINT completion)
{
    CallScope scope(*m_state);
    if (!m_state->dbc)
        return fail(notOpenError());
    const bool ended = succeeded(SQLEndTran(SQL_HANDLE_DBC, m_state->dbc, completion));
    // The error has to be read before anything else is called on the connection.
    Error error;
    if (!ended)
        error = errorFromHandle(SQL_HANDLE_DBC, m_state->dbc);
    // Back to autocommit whatever happened: the transaction is over, or it is lost.
    setConnectAttr(m_state->dbc, SQL_ATTR_AUTOCOMMIT, SQL_AUTOCOMMIT_ON);
    m_state->inTransaction = false;
    if (!ended)
        return fail(std::move(error));
    return {};
}

Result<OdbcConnection::Rows> OdbcConnection::readRows(SQLHSTMT statement)
{
    Rows rows;
    SQLSMALLINT columns = 0;
    if (!succeeded(SQLNumResultCols(statement, &columns)))
        return fail(errorFromHandle(SQL_HANDLE_STMT, statement));
    if (columns == 0)
        return rows;
    std::vector<SQLWCHAR> scratch;
    for (;;) {
        const SQLRETURN rc = SQLFetch(statement);
        if (rc == SQL_NO_DATA)
            break;
        if (!succeeded(rc))
            return fail(errorFromHandle(SQL_HANDLE_STMT, statement));
        QVariantList row;
        row.reserve(columns);
        for (SQLSMALLINT i = 1; i <= columns; ++i) {
            bool isNull = false;
            if (!succeeded(getWideData(statement, static_cast<SQLUSMALLINT>(i), scratch, isNull)))
                return fail(errorFromHandle(SQL_HANDLE_STMT, statement));
            row.push_back(isNull ? QVariant()
                                 : QVariant(fromWide(scratch.data(),
                                                     static_cast<qsizetype>(scratch.size()))));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

Result<OdbcConnection::Rows> OdbcConnection::queryRows(const QString &sql,
                                                       const QVariantList &arguments)
{
    if (m_state->streamOpen)
        return fail(busyError());
    auto handle = allocateStatement(m_state);
    if (!handle)
        return fail(std::move(handle).error());
    ActiveStatement active(*m_state, handle.value().get());
    if (!active)
        return fail(cancelledError());
    std::vector<ParamSlot> storage;
    QStringList notices;
    if (auto ran = runStatement(handle.value().get(), sql, arguments, storage, notices); !ran)
        return fail(ran.error());
    return readRows(handle.value().get());
}

Result<OdbcConnection::Rows> OdbcConnection::catalogRows(const CatalogCall &call)
{
    if (m_state->streamOpen)
        return fail(busyError());
    auto handle = allocateStatement(m_state);
    if (!handle)
        return fail(std::move(handle).error());
    ActiveStatement active(*m_state, handle.value().get());
    if (!active)
        return fail(cancelledError());
    const SQLRETURN rc = call(handle.value().get());
    if (rc == SQL_NO_DATA)
        return Rows {};
    if (!succeeded(rc))
        return fail(errorFromHandle(SQL_HANDLE_STMT, handle.value().get()));
    return readRows(handle.value().get());
}

} // namespace quarry::odbc
