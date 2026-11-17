#include "exporter_worker.h"

#include <quarry/exporter.h>

#include <QLoggingCategory>
#include <QMetaObject>
#include <QThread>

Q_LOGGING_CATEGORY(lcExporter, "quarry.exporter")

namespace quarry {

namespace {

constexpr int kShutdownTimeoutMs = 5000;
constexpr int kProgressIntervalMs = 100;
constexpr int kFetchSize = 5000;

void registerMetaTypes()
{
    static const bool done = [] {
        qRegisterMetaType<quarry::Error>();
        qRegisterMetaType<quarry::ExportStats>();
        qRegisterMetaType<std::vector<quarry::ColumnInfo>>();
        qRegisterMetaType<std::shared_ptr<const quarry::ResultBatch>>();
        return true;
    }();
    Q_UNUSED(done)
}

} // namespace

Exporter::Exporter(ExportOptions options, QObject *parent)
    : QObject(parent)
    , m_thread(new QThread) // no parent: must outlive this object if the join times out
    , m_options(options)
{
    registerMetaTypes();
    m_worker = new ExporterWorker(std::move(options));
    m_worker->moveToThread(m_thread);
    // The worker (and the temporary file with it) dies on its own thread.
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    const auto q = Qt::QueuedConnection;
    connect(
        m_worker, &ExporterWorker::opened, this,
        [this] {
            if (m_concluding)
                return;
            if (m_fromSession) {
                m_session->execute(m_statement);
                return;
            }
            QMetaObject::invokeMethod(
                m_worker, [w = m_worker, columns = m_columns] { w->begin(columns); },
                Qt::QueuedConnection);
            pump();
        },
        q);
    connect(
        m_worker, &ExporterWorker::batchWritten, this,
        [this](qint64 rows) {
            if (m_concluding)
                return;
            m_workerBusy = false;
            m_rows = rows;
            reportProgress(rows, false);
            pump();
        },
        q);
    connect(
        m_worker, &ExporterWorker::done, this,
        [this](ExportStats stats) {
            if (m_concluding)
                return;
            m_stats = std::move(stats);
            conclude(End::Finished);
        },
        q);
    connect(
        m_worker, &ExporterWorker::failed, this,
        [this](Error error) { conclude(End::Failed, std::move(error)); }, q);
    connect(
        m_worker, &ExporterWorker::aborted, this,
        [this] {
            if (m_end == End::Failed)
                emit failed(m_error);
            else if (m_end == End::Cancelled)
                emit cancelled();
        },
        q);
    m_thread->start();
}

Exporter::~Exporter()
{
    m_concluding = true; // no signal may come out of a dying object
    m_worker->disconnect(this);
    if (m_session)
        m_session->cancel();
    QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->abort(); }, Qt::QueuedConnection);
    m_thread->quit();
    if (m_thread->wait(kShutdownTimeoutMs)) {
        delete m_thread;
    } else {
        qCWarning(lcExporter) << "Export thread did not stop within" << kShutdownTimeoutMs
                              << "ms; leaking it until the disk write returns";
        connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    }
}

void Exporter::start(std::unique_ptr<Session> session, Statement statement)
{
    if (m_started)
        return;
    m_started = true;
    m_fromSession = true;
    m_session = std::move(session);
    if (!m_session) {
        conclude(End::Failed,
                 Error { ErrorKind::Internal, tr("The export has no connection."), {}, {}, {} });
        return;
    }
    statement.fetchSize = kFetchSize;
    m_statement = std::move(statement);

    connect(m_session.get(), &Session::columnsReady, this, [this](std::vector<ColumnInfo> columns) {
        if (m_concluding)
            return;
        QMetaObject::invokeMethod(
            m_worker, [w = m_worker, columns = std::move(columns)] { w->begin(columns); },
            Qt::QueuedConnection);
    });
    connect(m_session.get(), &Session::batchReady, this,
            [this](std::shared_ptr<const ResultBatch> batch) {
                if (!m_concluding)
                    writeBatch(std::move(batch));
            });
    connect(m_session.get(), &Session::finished, this, [this](const ExecutionStats &stats) {
        if (m_concluding)
            return;
        m_sourceIdle = true;
        m_atEnd = stats.atEnd;
        pump();
    });
    connect(m_session.get(), &Session::failed, this,
            [this](Error error) { conclude(End::Failed, std::move(error)); });
    connect(m_session.get(), &Session::cancelled, this, [this] { conclude(End::Cancelled); });
    beginRun();
}

void Exporter::startBatches(std::vector<ColumnInfo> columns,
                            std::vector<std::shared_ptr<const ResultBatch>> batches)
{
    if (m_started)
        return;
    m_started = true;
    m_columns = std::move(columns);
    m_batches = std::move(batches);
    beginRun();
}

void Exporter::cancel()
{
    if (!m_started || m_concluding || m_finishing)
        return;
    conclude(End::Cancelled);
}

void Exporter::beginRun()
{
    m_timer.start();
    QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->open(); }, Qt::QueuedConnection);
}

// One batch in flight, one fetch at a time: the next fetchMore is asked for only when the
// worker has written the previous batch and the session is idle again.
void Exporter::pump()
{
    if (m_concluding || m_workerBusy || m_finishing)
        return;
    if (m_fromSession) {
        if (!m_sourceIdle)
            return;
        if (m_atEnd) {
            requestFinish();
            return;
        }
        m_sourceIdle = false;
        m_session->fetchMore(kFetchSize);
        return;
    }
    while (m_nextBatch < m_batches.size()) {
        std::shared_ptr<const ResultBatch> batch = std::move(m_batches[m_nextBatch++]);
        if (batch && batch->rowCount > 0) {
            writeBatch(std::move(batch));
            return;
        }
    }
    requestFinish();
}

void Exporter::writeBatch(std::shared_ptr<const ResultBatch> batch)
{
    m_workerBusy = true;
    QMetaObject::invokeMethod(
        m_worker, [w = m_worker, batch = std::move(batch)] { w->write(batch); },
        Qt::QueuedConnection);
}

void Exporter::requestFinish()
{
    m_finishing = true;
    QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->finish(); }, Qt::QueuedConnection);
}

void Exporter::reportProgress(qint64 rows, bool force)
{
    if (!force && m_progressTimer.isValid() && m_progressTimer.elapsed() < kProgressIntervalMs)
        return;
    m_progressTimer.start();
    m_lastProgressRows = rows;
    emit progress(rows);
}

void Exporter::closeSession()
{
    if (!m_session)
        return;
    Session *session = m_session.get();
    session->disconnect(this);
    connect(session, &Session::closed, this, [this] {
        if (m_session)
            m_session.release()->deleteLater();
    });
    session->close();
}

void Exporter::conclude(End how, Error error)
{
    if (m_concluding)
        return;
    m_concluding = true;
    m_end = how;
    closeSession();
    if (how == End::Finished) {
        m_stats.elapsedMs = m_timer.elapsed();
        if (m_lastProgressRows != m_stats.rows)
            reportProgress(m_stats.rows, true);
        emit finished(m_stats);
        return;
    }
    // The temporary file must be gone before the signal: wait for the worker to confirm.
    m_error = std::move(error);
    QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->abort(); }, Qt::QueuedConnection);
}

} // namespace quarry
