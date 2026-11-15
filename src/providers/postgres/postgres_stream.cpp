#include "postgres_stream.h"

#include "postgres_errors.h"
#include "postgres_support.h"
#include "postgres_text.h"

#include <algorithm>
#include <utility>

namespace quarry::postgres {

namespace {

constexpr qsizetype kDefaultFetch = 500;
constexpr qsizetype kMaxReserve = 8192;

// COPY is out of scope for v1: leave copy mode so the connection stays usable.
void leaveCopyMode(PGconn *conn, ExecStatusType status)
{
    if (status == PGRES_COPY_IN) {
        PQputCopyEnd(conn, "COPY is not supported");
    } else if (status == PGRES_COPY_OUT) {
        char *buffer = nullptr;
        while (PQgetCopyData(conn, &buffer, 0) > 0)
            PQfreemem(buffer);
    }
}

} // namespace

Result<std::unique_ptr<PostgresStream>> PostgresStream::create(std::shared_ptr<PgState> state,
                                                               const QString &sql)
{
    PGconn *conn = state->conn;
    std::unique_ptr<PostgresStream> stream(new PostgresStream(std::move(state), sql));
    auto first = stream->accept(PQgetResult(conn));
    if (!first)
        return fail(std::move(first).error());
    return stream;
}

PostgresStream::PostgresStream(std::shared_ptr<PgState> state, QString sql)
    : m_state(std::move(state))
    , m_sql(std::move(sql))
{ }

PostgresStream::~PostgresStream()
{
    PQclear(m_current);
    discard(true);
}

void PostgresStream::noteAffected(const PGresult *result)
{
    // SELECT and DDL tags also carry counts or none: only data changes count as "affected".
    const QByteArrayView tag(PQcmdStatus(const_cast<PGresult *>(result)));
    if (tag.startsWith("INSERT") || tag.startsWith("UPDATE") || tag.startsWith("DELETE")
        || tag.startsWith("MERGE"))
        m_affected = QByteArrayView(PQcmdTuples(const_cast<PGresult *>(result))).toLongLong();
}

void PostgresStream::takeNotices()
{
    if (!m_state->notices.isEmpty())
        m_notices += std::exchange(m_state->notices, {});
}

void PostgresStream::discard(bool cancelServer)
{
    if (m_finished)
        return;
    m_finished = true;
    if (PGconn *conn = m_state->conn) {
        if (cancelServer)
            m_state->sendCancel();
        discardResults(conn);
    }
    takeNotices();
}

Result<ResultBatch> PostgresStream::abort(Error error, bool cancelServer)
{
    PQclear(m_current);
    m_current = nullptr;
    discard(cancelServer);
    return fail(std::move(error));
}

Result<void> PostgresStream::accept(PGresult *result)
{
    takeNotices();
    PGconn *conn = m_state->conn;
    if (!result) { // end of the command
        m_finished = true;
        return {};
    }
    const ExecStatusType status = PQresultStatus(result);
    switch (status) {
    case PGRES_TUPLES_CHUNK:
    case PGRES_TUPLES_OK: {
        if (m_columns.empty()) {
            const int count = PQnfields(result);
            m_columns.reserve(static_cast<size_t>(count));
            for (int i = 0; i < count; ++i) {
                const unsigned oid = PQftype(result, i);
                ColumnInfo info;
                info.name = QString::fromUtf8(PQfname(result, i));
                info.type = valueTypeForOid(oid);
                info.nativeType = nativeTypeName(oid);
                m_columns.push_back(std::move(info));
                m_types.push_back(m_columns.back().type);
            }
        }
        noteAffected(result); // in chunked mode the command tag rides on the last chunk
        if (status == PGRES_TUPLES_OK) { // last result of the command
            if (conn)
                discardResults(conn); // the trailing null
            m_finished = true;
            takeNotices();
        }
        if (PQntuples(result) > 0) {
            m_current = result;
            m_row = 0;
        } else {
            PQclear(result);
        }
        return {};
    }
    case PGRES_COMMAND_OK:
    case PGRES_EMPTY_QUERY:
        noteAffected(result);
        PQclear(result);
        if (conn)
            discardResults(conn);
        m_finished = true;
        takeNotices();
        return {};
    case PGRES_COPY_IN:
    case PGRES_COPY_OUT:
    case PGRES_COPY_BOTH:
        if (conn)
            leaveCopyMode(conn, status);
        PQclear(result);
        discard(false);
        return fail(
            Error { ErrorKind::Unsupported, Text::tr("COPY is not supported yet."), {}, {}, {} });
    default: {
        Error error = errorFromResult(result, conn, m_sql);
        PQclear(result);
        discard(false);
        return fail(std::move(error));
    }
    }
}

Result<ResultBatch> PostgresStream::fetch(qsizetype maxRows, const CancellationToken &token)
{
    CallScope scope(*m_state);
    ResultBatch batch;
    batch.columns.resize(m_columns.size());
    if (atEnd())
        return batch;
    PGconn *conn = m_state->conn;
    if (!conn)
        return abort(
            Error { ErrorKind::Connection, Text::tr("The connection was closed."), {}, {}, {} },
            false);
    if (maxRows <= 0)
        maxRows = kDefaultFetch;
    if (token.isCancelled())
        return abort(cancelledError(), true);
    for (auto &column : batch.columns)
        column.reserve(std::min(maxRows, kMaxReserve));

    qsizetype rows = 0;
    while (rows < maxRows && !atEnd()) {
        if (!m_current) {
            if (token.isCancelled())
                return abort(cancelledError(), true);
            if (auto next = accept(PQgetResult(conn)); !next)
                return fail(std::move(next).error());
            continue;
        }
        const qsizetype available = PQntuples(m_current) - m_row;
        const qsizetype take = std::min(available, maxRows - rows);
        for (qsizetype i = 0; i < take; ++i)
            appendRow(batch, m_row + static_cast<int>(i));
        m_row += static_cast<int>(take);
        rows += take;
        if (m_row >= PQntuples(m_current)) {
            PQclear(m_current);
            m_current = nullptr;
            m_row = 0;
        }
    }

    batch.rowCount = rows;
    for (size_t i = 0; i < m_columns.size(); ++i) {
        if (batch.columns[i].isMixed())
            m_columns[i].type = ValueType::Other;
    }
    return batch;
}

void PostgresStream::appendRow(ResultBatch &batch, int row) const
{
    const int count = static_cast<int>(m_columns.size());
    for (int i = 0; i < count; ++i) {
        ColumnBuffer &out = batch.columns[static_cast<size_t>(i)];
        if (PQgetisnull(m_current, row, i)) {
            out.appendNull();
            continue;
        }
        const QByteArrayView text(PQgetvalue(m_current, row, i), PQgetlength(m_current, row, i));
        const ValueType type = m_types[static_cast<size_t>(i)];
        bool ok = true;
        switch (type) {
        case ValueType::Int64: {
            const qint64 value = text.toLongLong(&ok);
            if (ok)
                out.append(static_cast<int64_t>(value));
            break;
        }
        case ValueType::Double:
            if (const auto value = parseDouble(text))
                out.append(*value);
            else
                ok = false;
            break;
        case ValueType::Bool:
            out.appendBool(!text.isEmpty() && text[0] == 't');
            break;
        case ValueType::Bytes:
            if (const auto bytes = decodeByteaHex(text))
                out.appendBytes(*bytes, ValueType::Bytes);
            else
                ok = false;
            break;
        default:
            out.appendBytes(text, type);
            break;
        }
        if (!ok) // never seen from a healthy server; keep the text rather than lose the cell
            out.appendBytes(text, ValueType::Other);
    }
}

} // namespace quarry::postgres
