#pragma once

#include <quarry/explain_plan.h>
#include <quarry/provider_api.h>

namespace quarry::sqlite {

// `output` is the result of EXPLAIN QUERY PLAN: one row per line, cells joined by a tab
// (id, parent, notused, detail). Never throws; a malformed output is a Syntax error.
Result<ExplainPlan> parseExplainPlan(const QString &output, ExplainMode mode);

} // namespace quarry::sqlite
