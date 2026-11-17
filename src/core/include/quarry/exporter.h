#pragma once

#include <quarry/export_options.h>
#include <quarry/result.h>
#include <quarry/result_batch.h>
#include <quarry/session.h>

#include <QElapsedTimer>
#include <QMetaType>
#include <QObject>

#include <memory>
#include <vector>

class QThread;

namespace quarry {

class ExporterWorker;

struct ExportStats
{
    qint64 rows = 0;
    qint64 bytes = 0;
    qint64 elapsedMs = 0;
    QString filePath;
};

// Writes a result to a file without the UI thread touching the disk. One Exporter = one export;
// create another for the next one. Lives on the UI thread; formatting and file I/O run on its
// own worker thread. Memory stays constant: one batch is in flight at a time and the next
// fetch is requested only after the previous batch is written (backpressure).
// The file is written to a temporary sibling and renamed over filePath on success; on failure
// or cancel the temporary file is deleted and an existing filePath is left as it was.
class Exporter : public QObject
{
    Q_OBJECT

public:
    explicit Exporter(ExportOptions options, QObject *parent = nullptr);
    // Cancels a running export, waits up to 5 s for the thread and removes the temporary file.
    ~Exporter() override;

    // Streams `statement` through `session`, which must already be open and idle. The Exporter
    // takes ownership and closes it when it ends. It calls execute() with a large fetchSize
    // (5000) and then fetchMore() batch by batch. Session is a UI-thread object: the Exporter
    // drives it from the UI thread and hands only the batches to the worker thread.
    // Not a slot: moc cannot pass a move-only std::unique_ptr.
    void start(std::unique_ptr<quarry::Session> session, quarry::Statement statement);

public slots:
    // Writes rows already in memory (the grid's current result). Same signals.
    void startBatches(std::vector<quarry::ColumnInfo> columns,
                      std::vector<std::shared_ptr<const quarry::ResultBatch>> batches);
    // Ends the run with cancelled(). Does nothing when idle or already finished.
    void cancel();

signals:
    void progress(qint64 rows); // at most every ~100 ms and at the end of each batch
    void finished(quarry::ExportStats stats);
    void failed(quarry::Error error); // disk errors (Permission when access is denied, else
                                      // Internal) and the session's own
    void cancelled();

private:
    enum class End { None, Finished, Failed, Cancelled };

    void beginRun();
    void pump();
    void writeBatch(std::shared_ptr<const ResultBatch> batch);
    void requestFinish();
    void conclude(End how, Error error = {});
    void closeSession();
    void reportProgress(qint64 rows, bool force);

    QThread *m_thread = nullptr;
    ExporterWorker *m_worker = nullptr; // lives on m_thread, deleted there
    std::unique_ptr<Session> m_session;
    ExportOptions m_options;
    Statement m_statement;
    std::vector<std::shared_ptr<const ResultBatch>> m_batches; // startBatches() source
    std::vector<ColumnInfo> m_columns;
    size_t m_nextBatch = 0;
    ExportStats m_stats;
    Error m_error;
    End m_end = End::None;
    qint64 m_rows = 0;
    qint64 m_lastProgressRows = -1;
    QElapsedTimer m_timer;
    QElapsedTimer m_progressTimer;
    bool m_started = false;
    bool m_fromSession = false;
    bool m_sourceIdle = false; // the session answered the last execute/fetchMore
    bool m_atEnd = false;
    bool m_workerBusy = false; // a batch is in flight on the worker
    bool m_finishing = false; // finish() was sent; too late to cancel
    bool m_concluding = false; // a terminal signal is pending or was sent
};

} // namespace quarry

Q_DECLARE_METATYPE(quarry::ExportStats)
