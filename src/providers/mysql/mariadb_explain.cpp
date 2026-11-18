#include "mysql_explain.h"
#include "mysql_explain_util.h"

#include <QJsonDocument>

namespace quarry::mysql {

namespace {

using namespace explain_detail;

struct Walk
{
    bool tooDeep = false;
};

// Scalar properties shown in the detail panel, in this order. ANALYZE adds the r_* ones.
constexpr const char *kDetailKeys[] = {
    "access_type",
    "possible_keys",
    "key",
    "key_length",
    "used_key_parts",
    "ref",
    "rows",
    "filtered",
    "r_loops",
    "r_table_rows",
    "r_index_rows",
    "r_rows",
    "r_filtered",
    "attached_condition",
    "index_condition",
    "using_index",
    "sort_key",
    "r_used_priority_queue",
    "r_buffer_size",
    "buffer_type",
    "buffer_size",
    "join_type",
    "message",
};

enum class Flag : uint8_t { None, Filesort, TemporaryTable, JoinBuffer };

struct Container
{
    const char *key;
    const char *label;
    Flag flag;
};

constexpr Container kContainers[] = {
    { "filesort", "Filesort", Flag::Filesort },
    { "temporary_table", "Temporary table", Flag::TemporaryTable },
    { "duplicates_removal", "Duplicates removal", Flag::None },
    { "window_functions_computation", "Window functions", Flag::None },
    { "block-nl-join", "Block nested loop join", Flag::JoinBuffer },
    { "materialized", "Materialized subquery", Flag::None },
    { "expression_cache", "Expression cache", Flag::None },
    { "union_result", "Union result", Flag::None },
};

struct ListKey
{
    const char *key;
    const char *blockName;
};

constexpr ListKey kLists[] = {
    { "query_specifications", "Union branch" },
    { "subqueries", "Subquery" },
};

// Fills what every kind of step shares. MariaDB reports r_rows per loop and r_total_time_ms for
// all the loops of the step, so no multiplication is needed here.
ExplainNode makeNode(const QString &label, const QJsonObject &object)
{
    ExplainNode node;
    node.label = label;
    node.totalCost = numberAt(object, "cost"); // absent in most versions
    node.estimatedRows = numberAt(object, "rows");
    node.actualRows = numberAt(object, "r_rows");
    if (!node.actualRows)
        node.actualRows = numberAt(object, "r_output_rows");
    node.loops = numberAt(object, "r_loops");
    node.actualTimeMs = numberAt(object, "r_total_time_ms");
    for (const char *name : kDetailKeys) {
        const QJsonValue value = object.value(QLatin1String(name));
        if (value.isUndefined() || value.isNull() || value.isObject())
            continue;
        node.details.push_back(ExplainProperty { humanize(name), scalarText(value) });
    }
    addEstimateWarning(node);
    return node;
}

void appendChildren(ExplainNode &parent, const QJsonObject &object, int depth, Walk &walk,
                    const QString &blockName);

ExplainNode mapTable(const QJsonObject &table, int depth, Walk &walk)
{
    if (depth > kMaxDepth) {
        walk.tooDeep = true;
        return {};
    }
    const QString access = table.value(QLatin1String("access_type")).toString();
    const QString key = table.value(QLatin1String("key")).toString();
    QString label = QStringLiteral("Table ") + table.value(QLatin1String("table_name")).toString();
    if (!access.isEmpty()) {
        label += QStringLiteral(" (") + access;
        if (!key.isEmpty())
            label += QStringLiteral(" via ") + key;
        label += QLatin1Char(')');
    }
    ExplainNode node = makeNode(label, table);
    if (access == QLatin1String("ALL"))
        node.warnings.append(Text::tr("Full table scan"));
    appendChildren(node, table, depth + 1, walk, QStringLiteral("Subquery"));
    return node;
}

ExplainNode mapContainer(const Container &container, const QJsonObject &object, int depth,
                         Walk &walk)
{
    if (depth > kMaxDepth) {
        walk.tooDeep = true;
        return {};
    }
    ExplainNode node = makeNode(QString::fromLatin1(container.label), object);
    switch (container.flag) {
    case Flag::Filesort:
        node.warnings.append(Text::tr("Uses filesort"));
        break;
    case Flag::TemporaryTable:
        node.warnings.append(Text::tr("Uses a temporary table"));
        break;
    case Flag::JoinBuffer:
        node.warnings.append(Text::tr("Uses a join buffer"));
        break;
    case Flag::None:
        break;
    }
    appendChildren(node, object, depth + 1, walk, QStringLiteral("Subquery"));
    return node;
}

ExplainNode mapBlock(const QJsonObject &block, const QString &name, int depth, Walk &walk)
{
    if (depth > kMaxDepth) {
        walk.tooDeep = true;
        return {};
    }
    QString label = name;
    const QJsonValue id = block.value(QLatin1String("select_id"));
    if (id.isDouble())
        label += QStringLiteral(" #") + scalarText(id);
    ExplainNode node = makeNode(label, block);
    appendChildren(node, block, depth + 1, walk, QStringLiteral("Subquery"));
    return node;
}

// Children of `object` in a fixed order (QJsonObject sorts its keys anyway).
void appendChildren(ExplainNode &parent, const QJsonObject &object, int depth, Walk &walk,
                    const QString &blockName)
{
    const QJsonValue table = object.value(QLatin1String("table"));
    if (table.isObject()) {
        parent.children.push_back(mapTable(table.toObject(), depth, walk));
        if (walk.tooDeep)
            return;
    }

    const QJsonValue loop = object.value(QLatin1String("nested_loop"));
    if (loop.isArray()) {
        if (depth > kMaxDepth) {
            walk.tooDeep = true;
            return;
        }
        ExplainNode nested;
        nested.label = QStringLiteral("Nested loop");
        const QJsonArray items = loop.toArray();
        nested.children.reserve(static_cast<size_t>(items.size()));
        for (const QJsonValue item : items) {
            if (!item.isObject())
                continue;
            appendChildren(nested, item.toObject(), depth + 1, walk, blockName);
            if (walk.tooDeep)
                return;
        }
        parent.children.push_back(std::move(nested));
    }

    for (const Container &container : kContainers) {
        const QJsonValue value = object.value(QLatin1String(container.key));
        if (!value.isObject())
            continue;
        parent.children.push_back(mapContainer(container, value.toObject(), depth, walk));
        if (walk.tooDeep)
            return;
    }

    const QJsonValue block = object.value(QLatin1String("query_block"));
    if (block.isObject()) {
        parent.children.push_back(mapBlock(block.toObject(), blockName, depth, walk));
        if (walk.tooDeep)
            return;
    }

    for (const ListKey &list : kLists) {
        const QJsonValue value = object.value(QLatin1String(list.key));
        if (!value.isArray())
            continue;
        const QJsonArray items = value.toArray();
        for (const QJsonValue item : items) {
            if (!item.isObject())
                continue;
            appendChildren(parent, item.toObject(), depth, walk,
                           QString::fromLatin1(list.blockName));
            if (walk.tooDeep)
                return;
        }
    }
}

} // namespace

Result<ExplainPlan> parseMariadbExplain(const QString &output, ExplainMode mode)
{
    if (output.trimmed().isEmpty())
        return syntaxError(Text::tr("The query plan is empty."));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(output.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return syntaxError(
            Text::tr("The query plan is not valid JSON."),
            QStringLiteral("%1 (offset %2)").arg(parseError.errorString()).arg(parseError.offset));
    const QJsonValue block = document.object().value(QLatin1String("query_block"));
    if (!block.isObject())
        return syntaxError(Text::tr("The query plan has an unexpected format."),
                           QStringLiteral("No \"query_block\" object found"));

    Walk walk;
    ExplainPlan plan;
    plan.root = mapBlock(block.toObject(), QStringLiteral("Query block"), 1, walk);
    if (walk.tooDeep)
        return syntaxError(Text::tr("The query plan is nested too deeply to be shown."),
                           QStringLiteral("More than %1 levels").arg(kMaxDepth));
    plan.rawText = output;
    plan.engine = QStringLiteral("MariaDB");
    plan.analyzed = mode == ExplainMode::Analyze;
    finishPlan(plan);
    return plan;
}

} // namespace quarry::mysql
