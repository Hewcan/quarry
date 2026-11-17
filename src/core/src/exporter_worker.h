#pragma once

#include "export_sink.h"
#include "export_writer.h"

#include <quarry/exporter.h>

#include <QObject>
#include <QSaveFile>

#include <memory>
#include <vector>

namespace quarry {

// Private to Exporter. Lives on the export thread: formats batches and owns the file. The
// output goes to a temporary sibling that only replaces the target in finish(). The first
// error ends the run: the temporary file is dropped, failed() is emitted once and later calls
// do nothing.
class ExporterWorker : public QObject
{
    Q_OBJECT

public:
    explicit ExporterWorker(ExportOptions options);
    ~ExporterWorker() override;

    void open();
    void begin(const std::vector<ColumnInfo> &columns);
    void write(const std::shared_ptr<const ResultBatch> &batch);
    void finish();
    // Drops the temporary file; always answers with aborted(), even when nothing is open.
    void abort();

signals:
    void opened();
    void batchWritten(qint64 totalRows);
    void done(quarry::ExportStats stats);
    void failed(quarry::Error error);
    void aborted();

private:
    void fail(Error error);
    void failFromFile(const QString &message);
    void discard();
    bool usable() const { return m_file && !m_failed; }

    ExportOptions m_options;
    std::unique_ptr<QSaveFile> m_file;
    std::unique_ptr<ByteSink> m_sink;
    std::unique_ptr<ExportWriter> m_writer;
    size_t m_columnCount = 0;
    qint64 m_rows = 0;
    bool m_failed = false;
};

} // namespace quarry
