#pragma once

#include "export_sink.h"

#include <quarry/export_options.h>
#include <quarry/result_batch.h>

#include <memory>
#include <vector>

namespace quarry {

// Turns batches into the bytes of one export format. Runs on the exporter's worker thread only;
// the sink reports disk errors, so writers never fail by themselves.
class ExportWriter
{
public:
    virtual ~ExportWriter() = default;
    // Once, before the first batch: file start, header.
    virtual void begin(const std::vector<ColumnInfo> &columns, ByteSink &out) = 0;
    // `batch.columns` holds at least as many buffers as `begin` got columns.
    virtual void writeBatch(const ResultBatch &batch, ByteSink &out) = 0;
    // Once, after the last batch: closing brackets, the last INSERT.
    virtual void end(ByteSink &out) = 0;
};

std::unique_ptr<ExportWriter> makeExportWriter(const ExportOptions &options);

} // namespace quarry
