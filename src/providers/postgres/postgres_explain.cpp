#include "postgres_explain.h"

#include "postgres_text.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <deque>

namespace quarry::postgres {

namespace {

// The tree is also destroyed recursively: a hostile output must not be able to exhaust the stack.
constexpr int kMaxDepth = 200;

// Scalar properties worth showing, in the order the detail panel lists them.
constexpr const char *kDetailKeys[] = {
    "Join Type",
    "Strategy",
    "Scan Direction",
    "Index Cond",
    "Recheck Cond",
    "Hash Cond",
    "Merge Cond",
    "Join Filter",
    "Filter",
    "Sort Key",
    "Sort Method",
    "Sort Space Used",
    "Sort Space Type",
    "Group Key",
    "Rows Removed by Filter",
    "Rows Removed by Index Recheck",
    "Rows Removed by Join Filter",
    "Hash Batches",
    "Peak Memory Usage",
    "Heap Fetches",
    "Shared Hit Blocks",
    "Shared Read Blocks",
    "Shared Dirtied Blocks",
    "Shared Written Blocks",
    "Workers Planned",
    "Workers Launched",
    "Subplan Name",
    "CTE Name",
    "Plan Width",
};

Unexpected syntaxError(QString message, QString detail = {})
{
    return fail(Error { ErrorKind::Syntax, std::move(message), std::move(detail), {}, {} });
}

QString scalarText(const QJsonValue &value)
{
    switch (value.type()) {
    case QJsonValue::String:
        return value.toString();
    case QJsonValue::Double:
        return QString::number(value.toDouble(), 'g', 15);
    case QJsonValue::Bool:
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QJsonValue::Array: {
        QStringList parts;
        const QJsonArray array = value.toArray();
        for (const QJsonValue item : array)
            parts.append(scalarText(item));
        return parts.join(QStringLiteral(", "));
    }
    default:
        return {};
    }
}

std::optional<double> numberAt(const QJsonObject &object, const char *key)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isDouble())
        return value.toDouble();
    return std::nullopt;
}

QString labelOf(const QJsonObject &object)
{
    QString label = object.value(QLatin1String("Node Type")).toString();
    if (label.isEmpty())
        label = Text::tr("Unknown node");
    QString target = object.value(QLatin1String("Relation Name")).toString();
    if (target.isEmpty())
        target = object.value(QLatin1String("CTE Name")).toString();
    if (!target.isEmpty()) {
        label += QStringLiteral(" on ") + target;
        const QString alias = object.value(QLatin1String("Alias")).toString();
        if (!alias.isEmpty() && alias != target)
            label += QLatin1Char(' ') + alias;
    }
    const QString index = object.value(QLatin1String("Index Name")).toString();
    if (!index.isEmpty())
        label += QStringLiteral(" using ") + index;
    return label;
}

void addWarnings(ExplainNode &node, const QJsonObject &object)
{
    const auto estimated = node.estimatedRows;
    const auto actual = node.actualRows;
    const auto loops = node.loops;
    const bool executed = !loops || *loops > 0;
    if (estimated && actual && executed) {
        const double est = std::max(*estimated, 1.0);
        const double act = std::max(*actual, 1.0);
        const double ratio = std::max(est / act, act / est);
        if (ratio >= 10.0)
            node.warnings.append(Text::tr("Row estimate off by %1x").arg(qRound64(ratio)));
    }

    const auto removed = numberAt(object, "Rows Removed by Filter");
    if (removed && actual && *removed >= 1000.0 && *removed >= 10.0 * std::max(*actual, 1.0))
        node.warnings.append(Text::tr("Filter removed %1 rows after reading them to return %2")
                                 .arg(qRound64(*removed))
                                 .arg(qRound64(*actual)));

    const QString sortMethod = object.value(QLatin1String("Sort Method")).toString();
    const auto batches = numberAt(object, "Hash Batches");
    const bool spilled = sortMethod.contains(QLatin1String("external"), Qt::CaseInsensitive)
        || object.value(QLatin1String("Sort Space Type")).toString() == QLatin1String("Disk")
        || (batches && *batches > 1.0);
    if (spilled)
        node.warnings.append(Text::tr("Spilled to disk"));
}

ExplainNode parseNode(const QJsonObject &object, int depth, bool &tooDeep)
{
    ExplainNode node;
    if (depth > kMaxDepth) {
        tooDeep = true;
        return node;
    }
    node.label = labelOf(object);
    node.startupCost = numberAt(object, "Startup Cost");
    node.totalCost = numberAt(object, "Total Cost");
    node.estimatedRows = numberAt(object, "Plan Rows");
    // Actual Rows and Actual Total Time are per loop in PostgreSQL's output.
    node.actualRows = numberAt(object, "Actual Rows");
    node.loops = numberAt(object, "Actual Loops");
    const auto perLoopMs = numberAt(object, "Actual Total Time");
    if (perLoopMs)
        node.actualTimeMs = *perLoopMs * node.loops.value_or(1.0);

    for (const char *key : kDetailKeys) {
        const QJsonValue value = object.value(QLatin1String(key));
        if (value.isUndefined() || value.isNull() || value.isObject())
            continue;
        node.details.push_back(ExplainProperty { QString::fromLatin1(key), scalarText(value) });
    }
    addWarnings(node, object);

    const QJsonValue plans = object.value(QLatin1String("Plans"));
    if (plans.isArray()) {
        const QJsonArray children = plans.toArray();
        node.children.reserve(static_cast<size_t>(children.size()));
        for (const QJsonValue child : children) {
            if (!child.isObject())
                continue;
            node.children.push_back(parseNode(child.toObject(), depth + 1, tooDeep));
            if (tooDeep)
                return node;
        }
    }
    return node;
}

qsizetype countNodes(const ExplainNode &node)
{
    qsizetype total = 1;
    for (const auto &child : node.children)
        total += countNodes(child);
    return total;
}

// Keeps the first `limit` nodes in breadth-first order: the top of the plan survives.
void trimBreadthFirst(ExplainNode &root, qsizetype limit)
{
    std::deque<ExplainNode *> queue { &root };
    qsizetype kept = 1;
    while (!queue.empty()) {
        ExplainNode *node = queue.front();
        queue.pop_front();
        const qsizetype room = limit - kept;
        if (static_cast<qsizetype>(node->children.size()) > room)
            node->children.erase(node->children.begin() + room, node->children.end());
        kept += static_cast<qsizetype>(node->children.size());
        for (auto &child : node->children)
            queue.push_back(&child);
    }
}

} // namespace

Result<ExplainPlan> parseExplainJson(const QString &output, ExplainMode mode)
{
    if (output.trimmed().isEmpty())
        return syntaxError(Text::tr("The query plan is empty."));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(output.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return syntaxError(
            Text::tr("The query plan is not valid JSON."),
            QStringLiteral("%1 (offset %2)").arg(parseError.errorString()).arg(parseError.offset));

    QJsonObject top;
    if (document.isArray() && !document.array().isEmpty() && document.array().first().isObject())
        top = document.array().first().toObject();
    else if (document.isObject())
        top = document.object();
    const QJsonValue planValue = top.value(QLatin1String("Plan"));
    if (!planValue.isObject())
        return syntaxError(Text::tr("The query plan has an unexpected format."),
                           QStringLiteral("No \"Plan\" object found"));

    bool tooDeep = false;
    ExplainPlan plan;
    plan.root = parseNode(planValue.toObject(), 1, tooDeep);
    if (tooDeep)
        return syntaxError(Text::tr("The query plan is nested too deeply to be shown."),
                           QStringLiteral("More than %1 levels").arg(kMaxDepth));

    plan.rawText = output;
    plan.engine = QStringLiteral("PostgreSQL");
    plan.analyzed = mode == ExplainMode::Analyze;
    plan.planningTimeMs = numberAt(top, "Planning Time");
    plan.executionTimeMs = numberAt(top, "Execution Time");
    if (countNodes(plan.root) > kMaxExplainNodes) {
        trimBreadthFirst(plan.root, kMaxExplainNodes);
        plan.truncated = true;
    }
    plan.nodeCount = countNodes(plan.root);
    return plan;
}

} // namespace quarry::postgres
