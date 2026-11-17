#pragma once

#include <cstdint>

namespace quarry {

enum class ExportFormat : uint8_t { Csv, Tsv, Json, JsonLines, SqlInsert, Markdown };

} // namespace quarry
