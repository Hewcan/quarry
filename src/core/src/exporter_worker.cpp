#include "exporter_worker.h"

#include <QDir>
#include <QFileInfo>

#include <exception>

namespace quarry {

namespace {

template<typename F> std::optional<QString> guarded(F &&body)
{
    try {
        body();
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    } catch (...) {
        return QStringLiteral("unknown exception");
    }
    return std::nullopt;
}

} // namespace

ExporterWorker::ExporterWorker(ExportOptions options)
    : m_options(std::move(options))
{ }

ExporterWorker::~ExporterWorker()
{
    discard();
}

// cancelWriting() only marks the file; the temporary sibling is deleted when QSaveFile dies.
void ExporterWorker::discard()
{
    m_writer.reset();
    m_sink.reset();
    if (m_file) {
        if (m_file->isOpen())
            m_file->cancelWriting();
        m_file.reset();
    }
}

void ExporterWorker::fail(Error error)
{
    if (m_failed)
        return;
    m_failed = true;
    discard();
    emit failed(std::move(error));
}

void ExporterWorker::failFromFile(const QString &message)
{
    const QFileInfo target(m_options.filePath);
    const bool denied = m_file->error() == QFileDevice::PermissionsError
        || (target.exists() && !target.isWritable());
    fail(Error { denied ? ErrorKind::Permission : ErrorKind::Internal,
                 denied ? tr("Access to the file was denied. Check the permissions of the folder.")
                        : message,
                 m_file->errorString() + QStringLiteral(" (") + m_options.filePath
                     + QLatin1Char(')'),
                 {},
                 {} });
}

void ExporterWorker::open()
{
    if (m_options.filePath.isEmpty()) {
        fail(Error { ErrorKind::Internal, tr("No file was chosen for the export."), {}, {}, {} });
        return;
    }
    const QFileInfo info(m_options.filePath);
    if (!info.absoluteDir().exists()) {
        fail(Error { ErrorKind::Internal,
                     tr("The destination folder does not exist."),
                     m_options.filePath,
                     {},
                     {} });
        return;
    }
    if (info.isDir()) {
        fail(Error { ErrorKind::Internal,
                     tr("The destination is a folder, not a file."),
                     m_options.filePath,
                     {},
                     {} });
        return;
    }
    m_file = std::make_unique<QSaveFile>(m_options.filePath);
    m_file->setDirectWriteFallback(false); // never write into the target itself
    if (!m_file->open(QIODevice::WriteOnly)) {
        failFromFile(tr("The file could not be created."));
        return;
    }
    m_sink = std::make_unique<ByteSink>(*m_file);
    m_writer = makeExportWriter(m_options);
    emit opened();
}

void ExporterWorker::begin(const std::vector<ColumnInfo> &columns)
{
    if (!usable())
        return;
    m_columnCount = columns.size();
    if (const auto problem = guarded([&] { m_writer->begin(columns, *m_sink); })) {
        fail(
            Error { ErrorKind::Internal, tr("The export failed unexpectedly."), *problem, {}, {} });
        return;
    }
    if (m_sink->failed())
        failFromFile(tr("Writing the file failed. Check that the disk has free space."));
}

void ExporterWorker::write(const std::shared_ptr<const ResultBatch> &batch)
{
    if (!usable())
        return;
    if (!batch || batch->columns.size() < m_columnCount) {
        fail(Error { ErrorKind::Internal,
                     tr("The export failed unexpectedly."),
                     QStringLiteral("batch has fewer columns than the result"),
                     {},
                     {} });
        return;
    }
    if (const auto problem = guarded([&] { m_writer->writeBatch(*batch, *m_sink); })) {
        fail(
            Error { ErrorKind::Internal, tr("The export failed unexpectedly."), *problem, {}, {} });
        return;
    }
    m_rows += batch->rowCount;
    if (m_sink->failed()) {
        failFromFile(tr("Writing the file failed. Check that the disk has free space."));
        return;
    }
    emit batchWritten(m_rows);
}

void ExporterWorker::finish()
{
    if (!usable())
        return;
    if (const auto problem = guarded([&] { m_writer->end(*m_sink); })) {
        fail(
            Error { ErrorKind::Internal, tr("The export failed unexpectedly."), *problem, {}, {} });
        return;
    }
    if (!m_sink->flush()) {
        failFromFile(tr("Writing the file failed. Check that the disk has free space."));
        return;
    }
    const qint64 bytes = m_sink->bytes();
    if (!m_file->commit()) {
        failFromFile(tr("The file could not be saved to its destination."));
        return;
    }
    m_failed = true; // finished: nothing else may touch the file
    emit done(ExportStats { m_rows, bytes, 0, m_options.filePath });
}

void ExporterWorker::abort()
{
    m_failed = true;
    discard();
    emit aborted();
}

} // namespace quarry
