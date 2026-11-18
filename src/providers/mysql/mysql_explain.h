#pragma once

#include <quarry/explain_plan.h>
#include <quarry/provider_api.h>

namespace quarry::mysql {

// MySQL 8: EXPLAIN FORMAT=JSON ({"query_block": ...}) or the indented text that EXPLAIN ANALYZE
// returns (lines starting with "->"). Never throws; a malformed output is a Syntax error.
Result<ExplainPlan> parseMysqlExplain(const QString &output, ExplainMode mode);

// MariaDB: EXPLAIN FORMAT=JSON / ANALYZE FORMAT=JSON. Same rules.
Result<ExplainPlan> parseMariadbExplain(const QString &output, ExplainMode mode);

} // namespace quarry::mysql
