#pragma once

#include "export_sink.h"

#include <quarry/column_buffer.h>

#include <QByteArray>
#include <QByteArrayView>

#include <cstdint>

namespace quarry::exporttext {

constexpr size_t kNumberBufferSize = 40; // longest shortest-round-trip double is ~24 chars

// Shortest text that reads back to the same value. `value` must be finite.
QByteArrayView formatInt64(char (&buffer)[kNumberBufferSize], int64_t value);
QByteArrayView formatDouble(char (&buffer)[kNumberBufferSize], double value);

// RFC 8259 number grammar, whole input.
bool isJsonNumber(QByteArrayView text);
// Validates one JSON value (any type) and writes it to `out` without insignificant whitespace.
// Returns false (and leaves `out` unspecified) when `text` is not valid JSON. Depth is capped.
bool compactJson(QByteArrayView text, QByteArray &out);

// `"..."` with the JSON escapes. Bytes >= 0x80 pass through (UTF-8 in, UTF-8 out).
void appendJsonString(ByteSink &out, QByteArrayView text);
void appendHex(ByteSink &out, QByteArrayView bytes);

// Text of one cell for the plain-text formats (CSV, TSV, Markdown): NULL is empty, Bool is
// true/false, numbers are the shortest round trip (NaN, Infinity, -Infinity), Bytes are base64
// and everything else is the provider's own text. The view is valid until the next call.
class CellText
{
public:
    QByteArrayView text(const ColumnBuffer &column, qsizetype row);

private:
    char m_number[kNumberBufferSize] = {};
    QByteArray m_scratch;
};

} // namespace quarry::exporttext
