#pragma once

#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace quarry {

struct ExplainProperty
{
    QString name; // "Index Cond", "Sort Key"...
    QString value;
};

// One step of a query plan. Costs are in the engine's own units: not comparable between engines
// (nor between a PostgreSQL cost and a MySQL one). Absent numbers are nullopt, never 0.
struct ExplainNode
{
    QString label; // "Seq Scan on orders", "SEARCH t USING INDEX i (a=?)"
    std::vector<ExplainProperty> details; // everything else worth showing, in engine order
    std::optional<double> startupCost;
    std::optional<double> totalCost; // of this node including its children
    std::optional<double> estimatedRows;
    std::optional<double> actualRows; // only after ANALYZE; per loop average as the engine reports
    std::optional<double> actualTimeMs; // only after ANALYZE; total for the node (all loops)
    std::optional<double> loops;
    QStringList warnings; // "Rows estimate off by 1000x", "Using temporary", "Full table scan"
    std::vector<ExplainNode> children;
};

// More nodes than this are dropped by the parser (breadth first) and `truncated` is set.
inline constexpr qsizetype kMaxExplainNodes = 5000;

struct ExplainPlan
{
    // When the engine reports several top-level steps (SQLite), the parser wraps them in a root
    // whose label is empty: the UI hides that root and shows its children as the top level.
    ExplainNode root;
    QString rawText; // the engine's output as received, for the text view
    QString engine; // "PostgreSQL", "MySQL", "MariaDB", "SQLite"
    bool analyzed = false; // the statement was executed (ExplainMode::Analyze)
    bool truncated = false; // kMaxExplainNodes reached
    qsizetype nodeCount = 0;
    std::optional<double> planningTimeMs;
    std::optional<double> executionTimeMs;
};

} // namespace quarry
