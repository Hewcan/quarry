#include "session_worker.h"

namespace quarry {

namespace {

constexpr int kDefaultFetchSize = 500;

} // namespace

SessionWorker::SessionWorker(std::unique_ptr<IConnection> connection,
                             std::shared_ptr<std::atomic<int>> inflight,
                             std::shared_ptr<std::atomic<bool>> execRunning)
    : m_connection(std::move(connection))
    , m_inflight(std::move(inflight))
    , m_execRunning(std::move(execRunning))
{ }

SessionWorker::~SessionWorker()
{
    m_stream.reset();
    if (m_connection && m_connection->isOpen())
        m_connection->close();
}

// Every operation ends with exactly one terminal signal, and the in-flight count drops
// just before it: a handler reacting to that signal (e.g. fetchMore) must see an idle session.
void SessionWorker::endOperation()
{
    m_execRunning->store(false);
    m_inflight->fetch_sub(1);
}

void SessionWorker::reportError(Error error)
{
    m_stream.reset();
    endOperation();
    if (error.kind == ErrorKind::Cancelled)
        emit cancelled();
    else
        emit failed(std::move(error));
}

void SessionWorker::open(const CancellationToken &token)
{
    if (token.isCancelled()) {
        m_openFailed = true;
        endOperation();
        emit cancelled();
        return;
    }
    auto result = m_connection->open(token);
    m_openFailed = !result;
    if (!result)
        reportError(std::move(result).error());
    else {
        endOperation();
        emit opened(std::move(result).value());
    }
}

void SessionWorker::execute(const Statement &statement, const CancellationToken &token)
{
    m_stream.reset();
    m_rows = 0;
    m_elapsedMs = 0;
    if (token.isCancelled()) {
        endOperation();
        emit cancelled();
        return;
    }
    if (m_openFailed) { // the open() queued before this statement did not succeed
        endOperation();
        emit failed(Error { ErrorKind::Connection,
                            tr("The connection is not open."),
                            tr("The preceding open request failed or was cancelled."),
                            {},
                            {} });
        return;
    }
    m_execRunning->store(true);
    emit started();
    QElapsedTimer timer;
    timer.start();
    auto result = m_connection->execute(statement, token);
    if (!result) {
        reportError(std::move(result).error());
        return;
    }
    m_stream = std::move(result).value();
    emit columnsReady(m_stream->columns());
    fetchBatch(statement.fetchSize > 0 ? statement.fetchSize : kDefaultFetchSize, token, timer);
}

void SessionWorker::fetchMore(qsizetype maxRows, const CancellationToken &token)
{
    if (token.isCancelled()) {
        endOperation();
        emit cancelled();
        return;
    }
    m_execRunning->store(true);
    QElapsedTimer timer;
    timer.start();
    if (!m_stream) { // already at the end (or nothing executed): nothing more to read
        endOperation();
        emit finished(ExecutionStats { m_elapsedMs, m_rows, std::nullopt, {}, true });
        return;
    }
    fetchBatch(maxRows > 0 ? maxRows : kDefaultFetchSize, token, timer);
}

void SessionWorker::fetchBatch(qsizetype maxRows, const CancellationToken &token,
                               QElapsedTimer &timer)
{
    auto result = m_stream->fetch(maxRows, token);
    if (!result) {
        reportError(std::move(result).error());
        return;
    }
    ResultBatch filled = std::move(result).value();
    for (auto &column : filled.columns)
        column.shrinkToFit(); // the batch is immutable from here on; drop growth slack
    auto batch = std::make_shared<const ResultBatch>(std::move(filled));
    m_rows += batch->rowCount;
    if (batch->rowCount > 0)
        emit batchReady(batch);
    m_elapsedMs += timer.elapsed();
    ExecutionStats stats { m_elapsedMs, m_rows, m_stream->affectedRows(), m_stream->notices(),
                           m_stream->atEnd() };
    if (stats.atEnd)
        m_stream.reset(); // releases the connection as soon as the result is exhausted
    endOperation();
    emit finished(std::move(stats));
}

void SessionWorker::children(quint64 requestId, const ObjectPath &parent,
                             const CancellationToken &token)
{
    // Not an execute: m_execRunning stays false so a new execute() never interrupts this call.
    auto failWith = [&](Error error) {
        endOperation();
        emit childrenFailed(requestId, parent, std::move(error));
    };
    const Error cancelledError {
        ErrorKind::Cancelled, tr("The operation was cancelled."), {}, {}, {}
    };
    if (token.isCancelled()) {
        failWith(cancelledError);
        return;
    }
    if (!m_connection->isOpen()) {
        failWith(Error { ErrorKind::Connection, tr("The connection is not open."), {}, {}, {} });
        return;
    }
    auto result = m_connection->children(parent, token);
    if (!result) {
        failWith(std::move(result).error());
        return;
    }
    if (token.isCancelled()) { // cancelled while the provider was finishing: honour the contract
        failWith(cancelledError);
        return;
    }
    endOperation();
    emit childrenReady(requestId, parent, std::move(result).value());
}

void SessionWorker::describe(quint64 requestId, const ObjectPath &table,
                             const CancellationToken &token)
{
    // Same shape as children(): not an execute, so m_execRunning stays false.
    auto failWith = [&](Error error) {
        endOperation();
        emit describeFailed(requestId, table, std::move(error));
    };
    const Error cancelledError {
        ErrorKind::Cancelled, tr("The operation was cancelled."), {}, {}, {}
    };
    if (token.isCancelled()) {
        failWith(cancelledError);
        return;
    }
    if (!m_connection->isOpen()) {
        failWith(Error { ErrorKind::Connection, tr("The connection is not open."), {}, {}, {} });
        return;
    }
    auto result = m_connection->describe(table, token);
    if (!result) {
        failWith(std::move(result).error());
        return;
    }
    if (token.isCancelled()) { // cancelled while the provider was finishing: honour the contract
        failWith(cancelledError);
        return;
    }
    endOperation();
    emit described(requestId, table, std::move(result).value());
}

void SessionWorker::failChanges(int index, Error error)
{
    // The rollback error is dropped on purpose: the original failure is what the user must see.
    // Cancel state is cleared by the provider at the start of each call, so this call is safe.
    if (m_connection->isOpen())
        (void)m_connection->rollback();
    endOperation();
    emit changesFailed(index, std::move(error));
}

void SessionWorker::applyChanges(const std::vector<PlannedStatement> &statements,
                                 const CancellationToken &token)
{
    const Error cancelledError {
        ErrorKind::Cancelled, tr("The operation was cancelled."), {}, {}, {}
    };
    m_stream.reset(); // an open result stream would keep a server connection busy
    if (token.isCancelled()) {
        endOperation();
        emit changesFailed(-1, cancelledError);
        return;
    }
    if (m_openFailed || !m_connection->isOpen()) {
        endOperation();
        emit changesFailed(
            -1, Error { ErrorKind::Connection, tr("The connection is not open."), {}, {}, {} });
        return;
    }
    m_execRunning->store(true); // a newer execute() cancels the apply
    if (auto begun = m_connection->begin(); !begun) {
        failChanges(-1, std::move(begun).error());
        return;
    }
    for (size_t i = 0; i < statements.size(); ++i) {
        const int index = static_cast<int>(i);
        const PlannedStatement &planned = statements[i];
        if (token.isCancelled()) {
            failChanges(index, cancelledError);
            return;
        }
        auto result = m_connection->execute(planned.statement, token);
        if (!result) {
            failChanges(index, std::move(result).error());
            return;
        }
        std::unique_ptr<IResultStream> stream = std::move(result).value();
        for (int guard = 0; !stream->atEnd() && guard < 1'000'000; ++guard) {
            if (auto batch = stream->fetch(kDefaultFetchSize, token); !batch) {
                failChanges(index, std::move(batch).error());
                return;
            }
        }
        const std::optional<qint64> affected = stream->affectedRows();
        stream.reset();
        if (planned.expectOneRow && affected != 1) {
            failChanges(index,
                        Error { ErrorKind::Constraint,
                                affected.value_or(0) == 0
                                    ? tr("The row was not found or was changed by someone else; "
                                         "nothing was saved.")
                                    : tr("The change would affect more than one row; nothing was "
                                         "saved."),
                                tr("Statement %1 affected %2 row(s), expected 1.")
                                    .arg(index + 1)
                                    .arg(affected ? QString::number(*affected)
                                                  : tr("an unknown number of")),
                                {},
                                {} });
            return;
        }
    }
    if (token.isCancelled()) {
        failChanges(static_cast<int>(statements.size()) - 1, cancelledError);
        return;
    }
    if (auto done = m_connection->commit(); !done) {
        failChanges(-1, std::move(done).error());
        return;
    }
    endOperation();
    emit changesApplied(static_cast<int>(statements.size()));
}

void SessionWorker::close()
{
    m_stream.reset();
    m_connection->close();
    endOperation();
    emit closed();
}

} // namespace quarry
