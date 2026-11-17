#include "odbc_stream.h"

#include "odbc_data.h"
#include "odbc_diag.h"
#include "odbc_support.h"
#include "odbc_text.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace quarry::odbc {

namespace {

constexpr qsizetype kDefaultFetch = 500;
constexpr qsizetype kMaxReserve = 8192;
constexpr qsizetype kCancelCheckMask = 1023; // look at the token every 1024 rows
constexpr size_t kMaxBlockBytes = size_t { 8 } * 1024 * 1024;
constexpr size_t kIndicatorBytes = 8; // room of the indicator in a row (>= sizeof(SQLLEN))
constexpr size_t kUnit = sizeof(std::uint64_t);
constexpr SQLSMALLINT kNameChars = 256;
constexpr SQLSMALLINT kTypeNameChars = 128;

static_assert(sizeof(SQLLEN) <= kIndicatorBytes);

size_t alignUp(size_t value)
{
    return (value + kUnit - 1) / kUnit * kUnit;
}

} // namespace

OdbcStream::OdbcStream(std::shared_ptr<OdbcState> state, StatementHandle statement,
                       qsizetype fetchSize, bool bindVariable)
    : m_state(std::move(state))
    , m_statement(std::move(statement))
    , m_fetchSize(fetchSize > 0 ? fetchSize : kDefaultFetch)
    , m_bindVariable(bindVariable)
{ }

Result<std::unique_ptr<OdbcStream>> OdbcStream::run(const std::shared_ptr<OdbcState> &state,
                                                    const Statement &statement, bool bindVariable)
{
    auto handle = allocateStatement(state);
    if (!handle)
        return fail(std::move(handle).error());
    std::unique_ptr<OdbcStream> stream(new OdbcStream(state, std::move(handle).value(),
                                                      static_cast<qsizetype>(statement.fetchSize),
                                                      bindVariable));
    state->streamOpen = true;
    stream->m_owner = true;

    // From here until the result is positioned a cancel reaches the driver through SQLCancel.
    ActiveStatement active(*state, stream->m_statement.get());
    if (!active)
        return fail(cancelledError());
    std::vector<ParamSlot> storage;
    auto executed = runStatement(stream->m_statement.get(), statement.sql, statement.parameters,
                                 storage, stream->m_notices);
    if (!executed)
        return fail(executed.error());
    if (auto loaded = stream->loadResult(); !loaded)
        return fail(loaded.error());
    // A cancel that came in as the statement finished: report it, as the repeated SQLCancel may
    // already have discarded the result.
    if (state->cancel.load(std::memory_order_acquire))
        return fail(cancelledError());
    // The parameter buffers go away with `storage`: the driver must not keep pointing at them.
    if (stream->m_statement)
        SQLFreeStmt(stream->m_statement.get(), SQL_RESET_PARAMS);
    return stream;
}

std::unique_ptr<OdbcStream> OdbcStream::createEmpty(std::shared_ptr<OdbcState> state)
{
    return std::unique_ptr<OdbcStream>(
        new OdbcStream(std::move(state), StatementHandle(), 0, false));
}

OdbcStream::~OdbcStream()
{
    // Dropped before the end: the server may still be producing rows. Cancelling first means
    // closing the cursor does not read them all (the statement handle does it in reset()).
    if (m_statement && (!m_rowsetDone || m_hasMore))
        m_statement.closeCursor(true);
    if (m_owner)
        m_state->streamOpen = false;
}

void OdbcStream::finish()
{
    m_rowsetDone = true;
    m_hasMore = false;
    m_blockRows = m_blockPos = 0;
    if (m_statement)
        m_statement.closeCursor(false);
    if (m_owner) {
        m_state->streamOpen = false;
        m_owner = false;
    }
}

void OdbcStream::abort()
{
    m_rowsetDone = true;
    m_hasMore = false;
    m_blockRows = m_blockPos = 0;
    if (m_statement)
        m_statement.closeCursor(true);
    if (m_owner) {
        m_state->streamOpen = false;
        m_owner = false;
    }
}

Result<bool> OdbcStream::moreResults()
{
    const SQLRETURN rc = SQLMoreResults(m_statement.get());
    if (rc == SQL_NO_DATA)
        return false;
    if (rc == SQL_SUCCESS_WITH_INFO)
        collectNotices(SQL_HANDLE_STMT, m_statement.get(), m_notices);
    if (succeeded(rc))
        return true;
    Error error = errorFromHandle(SQL_HANDLE_STMT, m_statement.get());
    // Drivers without batches (flat-file drivers) do not implement it: one result, then the end.
    if (error.sqlState == QLatin1String("IM001") || error.sqlState == QLatin1String("HYC00")
        || error.sqlState == QLatin1String("HY010"))
        return false;
    return fail(std::move(error));
}

Outcome OdbcStream::loadResult()
{
    m_columns.clear();
    m_plans.clear();
    m_boundCount = 0;
    m_blockRows = m_blockPos = 0;
    m_hasMore = false;
    m_rowsetDone = false;
    m_affected.reset();
    SQLFreeStmt(m_statement.get(), SQL_UNBIND);

    qint64 total = -1;
    for (;;) {
        SQLSMALLINT count = 0;
        if (!succeeded(SQLNumResultCols(m_statement.get(), &count)))
            return fail(errorFromHandle(SQL_HANDLE_STMT, m_statement.get()));
        if (count > 0) {
            m_affected.reset();
            if (auto described = describeColumns(count); !described)
                return described;
            bindColumns();
            return Done {};
        }
        // A result without columns only has a row count. A procedure that reports one per
        // statement would show nothing but counts: they are summed and the first rowset wins.
        SQLLEN rows = -1;
        if (succeeded(SQLRowCount(m_statement.get(), &rows)) && rows >= 0)
            total = (total < 0 ? 0 : total) + static_cast<qint64>(rows);
        if (total >= 0)
            m_affected = total;
        auto more = moreResults();
        if (!more)
            return fail(std::move(more).error());
        if (!more.value()) {
            finish();
            return Done {};
        }
    }
}

Outcome OdbcStream::describeColumns(SQLSMALLINT count)
{
    m_columns.reserve(static_cast<size_t>(count));
    m_plans.reserve(static_cast<size_t>(count));
    std::vector<SQLWCHAR> name(static_cast<size_t>(kNameChars));
    std::vector<SQLWCHAR> typeName(static_cast<size_t>(kTypeNameChars));
    for (SQLSMALLINT i = 1; i <= count; ++i) {
        SQLSMALLINT nameLength = 0;
        SQLSMALLINT sqlType = 0;
        SQLSMALLINT digits = 0;
        SQLSMALLINT nullable = SQL_NULLABLE_UNKNOWN;
        SQLULEN size = 0;
        SQLRETURN rc = SQLDescribeColW(m_statement.get(), static_cast<SQLUSMALLINT>(i), name.data(),
                                       static_cast<SQLSMALLINT>(name.size()), &nameLength, &sqlType,
                                       &size, &digits, &nullable);
        if (succeeded(rc) && static_cast<size_t>(nameLength) >= name.size()) {
            name.resize(static_cast<size_t>(nameLength) + 1);
            rc = SQLDescribeColW(m_statement.get(), static_cast<SQLUSMALLINT>(i), name.data(),
                                 static_cast<SQLSMALLINT>(name.size()), &nameLength, &sqlType,
                                 &size, &digits, &nullable);
        }
        if (!succeeded(rc))
            return fail(errorFromHandle(SQL_HANDLE_STMT, m_statement.get()));

        ColumnInfo info;
        info.name = fromWide(name.data(), nameLength);
        if (info.name.isEmpty())
            info.name = Text::tr("(no name)");
        info.nullable = nullable != SQL_NO_NULLS;

        // The driver's own name for the type ("nvarchar", "datetime2"); the SQL code if it gives
        // none. The length is in bytes for this call.
        SQLSMALLINT typeBytes = 0;
        QString typeText;
        if (succeeded(SQLColAttributeW(m_statement.get(), static_cast<SQLUSMALLINT>(i),
                                       SQL_DESC_TYPE_NAME, typeName.data(),
                                       static_cast<SQLSMALLINT>(typeName.size() * sizeof(SQLWCHAR)),
                                       &typeBytes, nullptr))) {
            const qsizetype chars = std::min<qsizetype>(
                typeBytes / static_cast<qsizetype>(sizeof(SQLWCHAR)), kTypeNameChars - 1);
            typeText = fromWide(typeName.data(), chars);
        }
        if (typeText.isEmpty())
            typeText = genericTypeName(sqlType);

        const ColumnPlanSpec spec = planColumn(sqlType, size, m_bindVariable);
        info.type = spec.type;
        info.nativeType = decoratedTypeName(typeText, spec.type, size, digits);

        ColumnPlan plan;
        plan.type = spec.type;
        plan.cType = spec.cType;
        plan.capacity = spec.capacity;
        m_columns.push_back(std::move(info));
        m_plans.push_back(plan);
    }
    return Done {};
}

void OdbcStream::bindColumns()
{
    // The bound columns are a prefix: SQLGetData serves what comes after them.
    size_t rowSize = 0;
    size_t bound = 0;
    for (ColumnPlan &plan : m_plans) {
        if (plan.capacity <= 0)
            break;
        plan.bound = true;
        plan.offset = rowSize;
        rowSize += kIndicatorBytes + alignUp(static_cast<size_t>(plan.capacity));
        ++bound;
    }
    const auto unbind = [this] {
        for (ColumnPlan &plan : m_plans)
            plan.bound = false;
        m_boundCount = 0;
        m_arraySize = 1;
        SQLFreeStmt(m_statement.get(), SQL_UNBIND);
    };
    if (bound == 0) {
        m_arraySize = 1;
        m_status.assign(1, 0);
        setStmtAttr(m_statement.get(), SQL_ATTR_ROW_ARRAY_SIZE, 1);
        SQLSetStmtAttr(m_statement.get(), SQL_ATTR_ROWS_FETCHED_PTR, &m_rowsFetched, 0);
        return;
    }

    // Blocks of rows only when every column is bound (SQLGetData cannot address one row of a
    // block).
    size_t rows = 1;
    if (bound == m_plans.size()) {
        rows = static_cast<size_t>(std::max<qsizetype>(m_fetchSize, 1));
        rows = std::min(rows, std::max<size_t>(kMaxBlockBytes / std::max<size_t>(rowSize, 1), 1));
    }
    m_rowSize = rowSize;
    m_boundCount = bound;

    if (!succeeded(setStmtAttr(m_statement.get(), SQL_ATTR_ROW_BIND_TYPE,
                               static_cast<std::uintptr_t>(rowSize)))) {
        unbind(); // no row-wise binding: everything through SQLGetData
        return;
    }
    setStmtAttr(m_statement.get(), SQL_ATTR_ROW_ARRAY_SIZE, static_cast<std::uintptr_t>(rows));
    // The driver may have kept another size (a flat-file driver answers 1 with a warning).
    SQLULEN effective = 1;
    if (!succeeded(
            SQLGetStmtAttr(m_statement.get(), SQL_ATTR_ROW_ARRAY_SIZE, &effective, 0, nullptr))
        || effective == 0)
        effective = 1;
    m_arraySize = std::min<SQLULEN>(effective, rows);
    m_status.assign(static_cast<size_t>(m_arraySize), 0);
    SQLSetStmtAttr(m_statement.get(), SQL_ATTR_ROWS_FETCHED_PTR, &m_rowsFetched, 0);
    SQLSetStmtAttr(m_statement.get(), SQL_ATTR_ROW_STATUS_PTR, m_status.data(), 0);

    m_block.assign((m_rowSize * static_cast<size_t>(m_arraySize) + kUnit - 1) / kUnit, 0);
    // The block is raw storage the driver writes into; the cast gives it the byte view that
    // SQLBindCol wants and that the reads in appendBound() use.
    unsigned char *base = reinterpret_cast<unsigned char *>(m_block.data());
    for (size_t i = 0; i < m_boundCount; ++i) {
        const ColumnPlan &plan = m_plans[i];
        // The addresses of the first row; the driver adds the row size for the others. The
        // indicator slot is 8-aligned, so the pointer to it is a valid SQLLEN *.
        SQLLEN *indicator = reinterpret_cast<SQLLEN *>(base + plan.offset);
        const SQLRETURN rc
            = SQLBindCol(m_statement.get(), static_cast<SQLUSMALLINT>(i + 1), plan.cType,
                         base + plan.offset + kIndicatorBytes, plan.capacity, indicator);
        if (!succeeded(rc)) {
            unbind();
            return;
        }
    }
}

Result<bool> OdbcStream::fetchBlock()
{
    m_rowsFetched = 0;
    std::fill(m_status.begin(), m_status.end(), SQL_ROW_NOROW);
    const SQLRETURN rc = SQLFetch(m_statement.get());
    if (rc == SQL_NO_DATA) {
        if (auto ended = endRowset(); !ended)
            return fail(ended.error());
        return false;
    }
    if (rc == SQL_SUCCESS_WITH_INFO)
        collectNotices(SQL_HANDLE_STMT, m_statement.get(), m_notices);
    else if (rc != SQL_SUCCESS)
        return fail(errorFromHandle(SQL_HANDLE_STMT, m_statement.get()));

    m_blockRows = std::min<size_t>(static_cast<size_t>(m_rowsFetched), m_status.size());
    m_blockPos = 0;
    if (m_boundCount > 0) {
        for (size_t i = 0; i < m_blockRows; ++i) {
            if (m_status[i] == SQL_ROW_ERROR)
                return fail(errorFromHandle(SQL_HANDLE_STMT, m_statement.get()));
        }
    } else {
        m_blockRows = 1; // nothing bound: the driver reports no row count worth trusting
    }
    if (m_blockRows == 0)
        return fetchBlock();
    return true;
}

Outcome OdbcStream::endRowset()
{
    m_rowsetDone = true;
    m_blockRows = m_blockPos = 0;
    auto more = moreResults();
    if (!more)
        return fail(std::move(more).error());
    if (more.value())
        m_hasMore = true; // the connection stays busy until nextResult() or the stream goes away
    else
        finish();
    return Done {};
}

Result<ResultBatch> OdbcStream::fetch(qsizetype maxRows, const CancellationToken &token)
{
    CallScope scope(*m_state);
    ResultBatch batch;
    batch.columns.resize(m_columns.size());
    if (m_rowsetDone)
        return batch;
    if (maxRows <= 0)
        maxRows = kDefaultFetch;
    if (token.isCancelled()) {
        abort();
        return fail(cancelledError());
    }
    // Everything below may block in the driver: a cancel from another thread goes to SQLCancel.
    ActiveStatement active(*m_state, m_statement.get());
    if (!active) {
        abort();
        return fail(cancelledError());
    }
    for (auto &column : batch.columns)
        column.reserve(std::min(maxRows, kMaxReserve));

    qsizetype rows = 0;
    while (rows < maxRows) {
        if (rows > 0 && (rows & kCancelCheckMask) == 0 && token.isCancelled()) {
            abort();
            return fail(cancelledError());
        }
        if (m_blockPos >= m_blockRows) {
            auto more = fetchBlock();
            if (!more) {
                abort();
                return fail(std::move(more).error());
            }
            if (!more.value())
                break;
        }
        if (auto appended = appendRow(batch, m_blockPos++); !appended) {
            abort();
            return fail(appended.error());
        }
        ++rows;
    }

    if (m_state->cancel.load(std::memory_order_acquire)) {
        abort(); // the cursor may be gone already: SQLCancel is repeated while a call runs
        return fail(cancelledError());
    }
    batch.rowCount = rows;
    for (size_t i = 0; i < m_columns.size(); ++i) {
        if (batch.columns[i].isMixed())
            m_columns[i].type = ValueType::Other;
    }
    if (m_truncated && !m_notices.contains(Text::tr("Some values were cut short."))) {
        m_notices << Text::tr("Some values were cut short.");
        m_truncated = false;
    }
    return batch;
}

Result<bool> OdbcStream::nextResult(const CancellationToken &token)
{
    CallScope scope(*m_state);
    if (!m_hasMore)
        return false;
    if (token.isCancelled()) {
        abort();
        return fail(cancelledError());
    }
    ActiveStatement active(*m_state, m_statement.get());
    if (!active) {
        abort();
        return fail(cancelledError());
    }
    if (auto loaded = loadResult(); !loaded) {
        abort();
        return fail(loaded.error());
    }
    return true;
}

Outcome OdbcStream::appendRow(ResultBatch &batch, size_t row)
{
    for (size_t i = 0; i < m_plans.size(); ++i) {
        const ColumnPlan &plan = m_plans[i];
        if (plan.bound) {
            appendBound(batch.columns[i], plan, row);
        } else if (auto fetched = appendFetched(batch.columns[i], plan, i); !fetched) {
            return fetched;
        }
    }
    return Done {};
}

void OdbcStream::appendBound(ColumnBuffer &out, const ColumnPlan &plan, size_t row)
{
    const unsigned char *slot
        = reinterpret_cast<const unsigned char *>(m_block.data()) + row * m_rowSize + plan.offset;
    SQLLEN indicator = 0;
    std::memcpy(&indicator, slot, sizeof(indicator));
    if (indicator == SQL_NULL_DATA) {
        out.appendNull();
        return;
    }
    const unsigned char *data = slot + kIndicatorBytes;
    switch (plan.cType) {
    case SQL_C_SBIGINT: {
        SQLBIGINT value = 0;
        std::memcpy(&value, data, sizeof(value));
        out.append(static_cast<int64_t>(value));
        return;
    }
    case SQL_C_DOUBLE: {
        double value = 0;
        std::memcpy(&value, data, sizeof(value));
        out.append(value);
        return;
    }
    case SQL_C_BIT:
        out.appendBool(*data != 0);
        return;
    case SQL_C_BINARY: {
        // A driver that understated the size sends more than fits: keep what fits.
        const bool cut = indicator == SQL_NO_TOTAL || indicator > plan.capacity;
        m_truncated = m_truncated || cut;
        const SQLLEN bytes = cut ? plan.capacity : indicator;
        out.appendBytes(QByteArrayView(data, static_cast<qsizetype>(bytes)), ValueType::Bytes);
        return;
    }
    default: {
        // SQL_C_WCHAR: the length is in bytes and the buffer ends with a NUL.
        const SQLLEN room = plan.capacity - static_cast<SQLLEN>(sizeof(SQLWCHAR));
        const bool cut = indicator == SQL_NO_TOTAL || indicator > room;
        m_truncated = m_truncated || cut;
        const SQLLEN bytes = cut ? room : indicator;
        const qsizetype chars
            = static_cast<qsizetype>(bytes) / static_cast<qsizetype>(sizeof(SQLWCHAR));
        // The row storage are 8-aligned, so the text is a valid array of 16-bit units.
        const auto *text = reinterpret_cast<const SQLWCHAR *>(data);
        out.appendBytes(wideToUtf8(text, chars), plan.type);
        return;
    }
    }
}

Outcome OdbcStream::appendFetched(ColumnBuffer &out, const ColumnPlan &plan, size_t index)
{
    const SQLUSMALLINT column = static_cast<SQLUSMALLINT>(index + 1);
    SQLLEN indicator = 0;
    SQLRETURN rc = SQL_SUCCESS;
    switch (plan.cType) {
    case SQL_C_SBIGINT: {
        SQLBIGINT value = 0;
        rc = SQLGetData(m_statement.get(), column, SQL_C_SBIGINT, &value, sizeof(value),
                        &indicator);
        if (!succeeded(rc))
            break;
        if (indicator == SQL_NULL_DATA)
            out.appendNull();
        else
            out.append(static_cast<int64_t>(value));
        return Done {};
    }
    case SQL_C_DOUBLE: {
        double value = 0;
        rc = SQLGetData(m_statement.get(), column, SQL_C_DOUBLE, &value, sizeof(value), &indicator);
        if (!succeeded(rc))
            break;
        if (indicator == SQL_NULL_DATA)
            out.appendNull();
        else
            out.append(value);
        return Done {};
    }
    case SQL_C_BIT: {
        SQLCHAR value = 0;
        rc = SQLGetData(m_statement.get(), column, SQL_C_BIT, &value, sizeof(value), &indicator);
        if (!succeeded(rc))
            break;
        if (indicator == SQL_NULL_DATA)
            out.appendNull();
        else
            out.appendBool(value != 0);
        return Done {};
    }
    case SQL_C_BINARY: {
        bool isNull = false;
        rc = getBinaryData(m_statement.get(), column, m_byteScratch, isNull);
        if (!succeeded(rc))
            break;
        if (isNull)
            out.appendNull();
        else
            out.appendBytes(m_byteScratch, ValueType::Bytes);
        return Done {};
    }
    default: {
        bool isNull = false;
        rc = getWideData(m_statement.get(), column, m_wideScratch, isNull);
        if (!succeeded(rc))
            break;
        if (isNull)
            out.appendNull();
        else
            out.appendBytes(
                wideToUtf8(m_wideScratch.data(), static_cast<qsizetype>(m_wideScratch.size())),
                plan.type);
        return Done {};
    }
    }
    return fail(errorFromHandle(SQL_HANDLE_STMT, m_statement.get()));
}

} // namespace quarry::odbc
