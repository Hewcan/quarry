#include "mysql_stream.h"

#include "mysql_errors.h"
#include "mysql_support.h"
#include "mysql_text.h"

#include <algorithm>
#include <utility>

namespace quarry::mysql {

namespace {

constexpr qsizetype kDefaultFetch = 500;
constexpr qsizetype kMaxReserve = 8192;
constexpr unsigned kMaxNotices = 100;
constexpr qsizetype kCancelCheckMask = 1023; // look at the token every 1024 rows

} // namespace

Result<std::unique_ptr<MysqlStream>> MysqlStream::create(std::shared_ptr<MyState> state)
{
    MYSQL *conn = state->conn;
    std::unique_ptr<MysqlStream> stream(new MysqlStream(std::move(state)));
    MYSQL_RES *result = mysql_use_result(conn);
    if (!result) {
        if (mysql_field_count(conn) != 0)
            return fail(errorFromHandle(conn)); // the rows were announced but never came
        const my_ulonglong affected = mysql_affected_rows(conn);
        if (affected != static_cast<my_ulonglong>(-1))
            stream->m_affected = static_cast<qint64>(affected);
        stream->finish();
    } else {
        stream->m_state->result = result;
        stream->setColumns(result);
    }
    // The statement ended on its own right after a KILL QUERY (SLEEP() returns 1): that is still
    // a cancellation, and the half-read result goes away without a second kill.
    if (stream->m_state->cancel.load(std::memory_order_acquire)) {
        if (stream->m_state->result) {
            mysql_free_result(stream->m_state->result);
            stream->m_state->result = nullptr;
            stream->m_state->drainMoreResults();
        }
        stream->m_finished = true;
        return fail(cancelledError());
    }
    return stream;
}

std::unique_ptr<MysqlStream> MysqlStream::createEmpty(std::shared_ptr<MyState> state)
{
    std::unique_ptr<MysqlStream> stream(new MysqlStream(std::move(state)));
    stream->m_finished = true;
    return stream;
}

MysqlStream::MysqlStream(std::shared_ptr<MyState> state)
    : m_state(std::move(state))
{ }

MysqlStream::~MysqlStream()
{
    // Dropped before the end: the server may still be producing millions of rows, and freeing the
    // result reads every one of them unless the statement is killed first.
    if (!m_finished)
        m_state->discardResult();
}

void MysqlStream::setColumns(MYSQL_RES *result)
{
    const unsigned count = mysql_num_fields(result);
    const MYSQL_FIELD *fields = mysql_fetch_fields(result);
    m_columns.reserve(count);
    m_types.reserve(count);
    m_isBit.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        const MYSQL_FIELD &field = fields[i];
        ColumnInfo info;
        info.name = QString::fromUtf8(field.name, static_cast<qsizetype>(field.name_length));
        info.type = valueTypeForColumn(field.type, field.charsetnr);
        info.nativeType = nativeTypeName(field.type, field.charsetnr, field.flags);
        info.nullable = (field.flags & NOT_NULL_FLAG) == 0;
        m_types.push_back(info.type);
        m_isBit.push_back(field.type == MYSQL_TYPE_BIT);
        m_columns.push_back(std::move(info));
    }
}

void MysqlStream::takeWarnings(unsigned count)
{
    if (count == 0 || !m_state->conn)
        return;
    MYSQL *conn = m_state->conn;
    constexpr char kSql[] = "SHOW WARNINGS";
    if (mysql_real_query(conn, kSql, sizeof(kSql) - 1) != 0)
        return;
    MYSQL_RES *warnings = mysql_store_result(conn);
    if (!warnings)
        return;
    unsigned taken = 0;
    while (MYSQL_ROW row = mysql_fetch_row(warnings)) {
        if (taken++ >= kMaxNotices)
            break;
        const unsigned long *lengths = mysql_fetch_lengths(warnings);
        auto cell = [&](unsigned index) {
            return row[index]
                ? QString::fromUtf8(row[index], static_cast<qsizetype>(lengths[index]))
                : QString();
        };
        // Level, Code, Message: "Warning (1292): Truncated incorrect DOUBLE value"
        m_notices.push_back(QStringLiteral("%1 (%2): %3").arg(cell(0), cell(1), cell(2)));
    }
    mysql_free_result(warnings);
}

void MysqlStream::finish()
{
    m_finished = true;
    MYSQL *conn = m_state->conn;
    if (!conn)
        return;
    if (m_state->result) {
        mysql_free_result(m_state->result);
        m_state->result = nullptr;
    }
    const unsigned warnings = mysql_warning_count(conn);
    m_state->drainMoreResults();
    takeWarnings(warnings);
}

Result<ResultBatch> MysqlStream::failWith(Error error)
{
    if (m_state->result) {
        mysql_free_result(m_state->result); // the error packet is the end of the stream
        m_state->result = nullptr;
    }
    m_state->drainMoreResults();
    m_finished = true;
    return fail(std::move(error));
}

Result<ResultBatch> MysqlStream::fetch(qsizetype maxRows, const CancellationToken &token)
{
    CallScope scope(*m_state);
    ResultBatch batch;
    batch.columns.resize(m_columns.size());
    if (atEnd())
        return batch;
    MYSQL *conn = m_state->conn;
    MYSQL_RES *result = m_state->result;
    if (!conn || !result) {
        m_finished = true;
        return fail(
            Error { ErrorKind::Connection, Text::tr("The connection was closed."), {}, {}, {} });
    }
    if (maxRows <= 0)
        maxRows = kDefaultFetch;
    if (token.isCancelled()) {
        m_state->discardResult();
        m_finished = true;
        return fail(cancelledError());
    }
    for (auto &column : batch.columns)
        column.reserve(std::min(maxRows, kMaxReserve));

    qsizetype rows = 0;
    while (rows < maxRows) {
        if (rows > 0 && (rows & kCancelCheckMask) == 0 && token.isCancelled()) {
            m_state->discardResult();
            m_finished = true;
            return fail(cancelledError());
        }
        MYSQL_ROW row = mysql_fetch_row(result);
        if (!row) {
            if (mysql_errno(conn) != 0)
                return failWith(errorFromHandle(conn));
            finish();
            break;
        }
        appendRow(batch, row, mysql_fetch_lengths(result));
        ++rows;
    }

    batch.rowCount = rows;
    for (size_t i = 0; i < m_columns.size(); ++i) {
        if (batch.columns[i].isMixed())
            m_columns[i].type = ValueType::Other;
    }
    return batch;
}

void MysqlStream::appendRow(ResultBatch &batch, MYSQL_ROW row, const unsigned long *lengths) const
{
    const size_t count = m_columns.size();
    for (size_t i = 0; i < count; ++i) {
        ColumnBuffer &out = batch.columns[i];
        if (!row[i]) {
            out.appendNull();
            continue;
        }
        const QByteArrayView text(row[i], static_cast<qsizetype>(lengths[i]));
        const ValueType type = m_types[i];
        bool ok = true;
        switch (type) {
        case ValueType::Int64:
            if (m_isBit[i]) {
                if (const auto bits = bitValue(text))
                    out.append(static_cast<int64_t>(*bits));
                else
                    ok = false;
            } else {
                const qint64 value = text.toLongLong(&ok);
                if (ok)
                    out.append(static_cast<int64_t>(value));
            }
            break;
        case ValueType::Double: {
            const double value = text.toDouble(&ok);
            if (ok)
                out.append(value);
            break;
        }
        default:
            out.appendBytes(text, type);
            break;
        }
        if (!ok) // BIGINT UNSIGNED above 2^63 and the like: keep the text rather than lose the cell
            out.appendBytes(text, ValueType::Other);
    }
}

} // namespace quarry::mysql
