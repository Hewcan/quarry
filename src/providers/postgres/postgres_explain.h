#pragma once

#include <quarry/explain_plan.h>
#include <quarry/provider_api.h>

namespace quarry::postgres {

// `output` is the JSON that EXPLAIN (FORMAT JSON) returns: [ { "Plan": {...}, ... } ]. Never
// throws; a malformed output is a Syntax error.
Result<ExplainPlan> parseExplainJson(const QString &output, ExplainMode mode);

} // namespace quarry::postgres
