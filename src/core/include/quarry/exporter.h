#pragma once

#include <quarry/export_options.h>
#include <quarry/result.h>
#include <quarry/result_batch.h>
#include <quarry/session.h>

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

public slots:
    // Streams `statement` through `session`, which must already be open and idle. The Exporter
    // takes ownership and closes it when it ends. It calls execute() with a large fetchSize
    // (5000) and then fetchMore() batch by batch. Session is a UI-thread object: the Exporter
    // drives it from the UI thread and hands only the batches to the worker thread.
    void start(std::unique_ptr<quarry::Session> session, quarry::Statement statement);
    // Writes rows already in memory (the grid's current result). Same signals.
    void startBatches(std::vector<quarry::ColumnInfo> columns,
                      std::vector<std::shared_ptr<const quarry::ResultBatch>> batches);
    // Ends the run with cancelled(). Does nothing when idle or already finished.
    void cancel();

signals:
    void progress(qint64 rows); // at most every ~100 ms and at the end of each batch
    void finished(quarry::ExportStats stats);
    void failed(quarry::Error error); // disk errors (Permission when access is denied, else Internal) and the session's own
    void cancelled();

private:
    QThread *m_thread = nullptr;
    ExporterWorker *m_worker = nullptr; // lives on m_thread, deleted there
    std::unique_ptr<Session> m_session;
    ExportOptions m_options;
};

} // namespace quarry

Q_DECLARE_METATYPE(quarry::ExportStats)
