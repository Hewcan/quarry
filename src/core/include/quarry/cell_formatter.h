#pragma once

#include <quarry/column_buffer.h>

#include <QString>

namespace quarry {

// Generic cell-to-text conversion for the results grid and the clipboard (ARCHITECTURE 6.2).
class CellFormatter
{
public:
    static constexpr qsizetype kDisplayChars = 500;

    // Text to paint. NULL -> "NULL"; text types are cut at the first line break and at
    // kDisplayChars UTF-16 units, adding U+2026 when cut.
    static QString display(const ColumnBuffer &column, qsizetype row);
    // Full text for the clipboard. NULL -> ""; Bytes -> "0x" + lowercase hex; never cut.
    static QString copy(const ColumnBuffer &column, qsizetype row);
    static bool isNull(const ColumnBuffer &column, qsizetype row) { return column.isNull(row); }
};

} // namespace quarry
