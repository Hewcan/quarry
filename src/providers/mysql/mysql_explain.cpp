#include "mysql_explain.h"

#include "mysql_explain_util.h"

#include <QJsonDocument>
#include <QRegularExpression>

#include <vector>

namespace quarry::mysql {

namespace {

using namespace explain_detail;

struct Walk
{
    bool tooDeep = false;
};

constexpr const char *kTableDetailKeys[] = {
    "access_type",
    "possible_keys",
    "key",
    "used_key_parts",
    "key_length",
    "ref",
    "filtered",
    "rows_examined_per_scan",
    "rows_produced_per_join",
    "index_condition",
    "attached_condition",
    "used_columns",
    "using_index",
    "using_join_buffer",
    "partitions",
};

constexpr const char *kCostKeys[]
    = { "read_cost", "eval_cost", "prefix_cost", "data_read_per_join" };

struct Operation
{
    const char *key;
    const char *label;
};

constexpr Operation kOperations[] = {
    { "ordering_operation", "Ordering" },           { "grouping_operation", "Grouping" },
    { "duplicates_removal", "Duplicates removal" }, { "windowing", "Windowing" },
    { "buffer_result", "Buffer result" },
};

void applyFlags(const QJsonObject &object, ExplainNode &node)
{
    if (object.value(QLatin1String("using_filesort")).toBool())
        node.warnings.append(Text::tr("Uses filesort"));
    if (object.value(QLatin1String("using_temporary_table")).toBool())
        node.warnings.append(Text::tr("Uses a temporary table"));
    if (object.contains(QLatin1String("using_join_buffer")))
        node.warnings.append(Text::tr("Uses a join buffer"));
}

QString operationLabel(const char *base, const QJsonObject &object)
{
    QStringList parts;
    if (object.value(QLatin1String("using_temporary_table")).toBool())
        parts.append(QStringLiteral("temporary table"));
    if (object.value(QLatin1String("using_filesort")).toBool())
        parts.append(QStringLiteral("filesort"));
    const QString label = QString::fromLatin1(base);
    return parts.isEmpty()
        ? label
        : label + QStringLiteral(" (") + parts.join(QStringLiteral(", ")) + QLatin1Char(')');
}

void appendChildren(ExplainNode &parent, const QJsonObject &object, int depth, Walk &walk,
                    const QString &blockName);

ExplainNode mapTable(const QJsonObject &table, int depth, Walk &walk)
{
    ExplainNode node;
    if (depth > kMaxDepth) {
        walk.tooDeep = true;
        return node;
    }
    const QString access = table.value(QLatin1String("access_type")).toString();
    const QString key = table.value(QLatin1String("key")).toString();
    node.label = QStringLiteral("Table ") + table.value(QLatin1String("table_name")).toString();
    if (!access.isEmpty()) {
        node.label += QStringLiteral(" (") + access;
        if (!key.isEmpty())
            node.label += QStringLiteral(" via ") + key;
        node.label += QLatin1Char(')');
    }

    const QJsonObject cost = table.value(QLatin1String("cost_info")).toObject();
    node.totalCost = numberAt(cost, "prefix_cost");
    node.estimatedRows = numberAt(table, "rows_produced_per_join");
    if (!node.estimatedRows)
        node.estimatedRows = numberAt(table, "rows_examined_per_scan");

    for (const char *name : kTableDetailKeys) {
        const QJsonValue value = table.value(QLatin1String(name));
        if (value.isUndefined() || value.isNull() || value.isObject())
            continue;
        node.details.push_back(ExplainProperty { humanize(name), scalarText(value) });
    }
    for (const char *name : kCostKeys) {
        const QJsonValue value = cost.value(QLatin1String(name));
        if (!value.isUndefined() && !value.isNull())
            node.details.push_back(ExplainProperty { humanize(name), scalarText(value) });
    }

    if (access == QLatin1String("ALL"))
        node.warnings.append(Text::tr("Full table scan"));
    applyFlags(table, node);

    appendChildren(node, table, depth + 1, walk, QStringLiteral("Query block"));
    return node;
}

ExplainNode mapOperation(const QString &label, const QJsonObject &object, int depth, Walk &walk)
{
    ExplainNode node;
    if (depth > kMaxDepth) {
        walk.tooDeep = true;
        return node;
    }
    node.label = label;
    node.totalCost = numberAt(object.value(QLatin1String("cost_info")).toObject(), "query_cost");
    applyFlags(object, node);
    const QJsonValue table = object.value(QLatin1String("table_name"));
    if (table.isString())
        node.details.push_back(ExplainProperty { QStringLiteral("Table name"), table.toString() });
    appendChildren(node, object, depth + 1, walk, QStringLiteral("Query block"));
    return node;
}

ExplainNode mapBlock(const QJsonObject &block, const QString &name, int depth, Walk &walk)
{
    ExplainNode node;
    if (depth > kMaxDepth) {
        walk.tooDeep = true;
        return node;
    }
    node.label = name;
    const QJsonValue id = block.value(QLatin1String("select_id"));
    if (id.isDouble())
        node.label += QStringLiteral(" #") + scalarText(id);
    node.totalCost = numberAt(block.value(QLatin1String("cost_info")).toObject(), "query_cost");
    const QJsonValue message = block.value(QLatin1String("message"));
    if (message.isString())
        node.details.push_back(ExplainProperty { QStringLiteral("Message"), message.toString() });
    applyFlags(block, node);
    appendChildren(node, block, depth + 1, walk, QStringLiteral("Query block"));
    return node;
}

// Children of `object` in a fixed order (QJsonObject sorts its keys anyway). Each kind of key
// that can hold more plan steps is handled here, so a table, a block and an operation share it.
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
        // The cost of a join is the accumulated cost of its last table.
        for (auto it = nested.children.rbegin(); it != nested.children.rend(); ++it) {
            if (it->totalCost) {
                nested.totalCost = it->totalCost;
                break;
            }
        }
        parent.children.push_back(std::move(nested));
    }

    for (const Operation &operation : kOperations) {
        const QJsonValue value = object.value(QLatin1String(operation.key));
        if (!value.isObject())
            continue;
        const QJsonObject inner = value.toObject();
        parent.children.push_back(
            mapOperation(operationLabel(operation.label, inner), inner, depth, walk));
        if (walk.tooDeep)
            return;
    }

    const QJsonValue unionResult = object.value(QLatin1String("union_result"));
    if (unionResult.isObject()) {
        const QJsonObject inner = unionResult.toObject();
        parent.children.push_back(
            mapOperation(operationLabel("Union result", inner), inner, depth, walk));
        if (walk.tooDeep)
            return;
    }

    const QJsonValue materialized = object.value(QLatin1String("materialized_from_subquery"));
    if (materialized.isObject()) {
        appendChildren(parent, materialized.toObject(), depth, walk, QStringLiteral("Subquery"));
        if (walk.tooDeep)
            return;
    }

    const QJsonValue block = object.value(QLatin1String("query_block"));
    if (block.isObject()) {
        parent.children.push_back(mapBlock(block.toObject(), blockName, depth, walk));
        if (walk.tooDeep)
            return;
    }

    struct ListKey
    {
        const char *key;
        const char *blockName;
    };
    constexpr ListKey kLists[] = {
        { "query_specifications", "Union branch" },
        { "attached_subqueries", "Subquery" },
        { "optimized_away_subqueries", "Subquery (optimized away)" },
    };
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

Result<ExplainPlan> parseJson(const QString &output, ExplainMode mode)
{
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
    plan.engine = QStringLiteral("MySQL");
    plan.analyzed = mode == ExplainMode::Analyze;
    finishPlan(plan);
    return plan;
}

// EXPLAIN ANALYZE text: "-> Node  (cost=1.2 rows=3) (actual time=0.05..0.07 rows=3 loops=1)",
// children indented below their parent.
struct TreeLine
{
    ExplainNode node;
    std::vector<int> kids;
};

ExplainNode buildTree(std::vector<TreeLine> &lines, int index)
{
    ExplainNode node = std::move(lines[static_cast<size_t>(index)].node);
    const std::vector<int> kids = lines[static_cast<size_t>(index)].kids;
    node.children.reserve(kids.size());
    for (const int kid : kids)
        node.children.push_back(buildTree(lines, kid));
    return node;
}

void parseTreeText(const QString &text, ExplainNode &node)
{
    static const QString number = QStringLiteral("(\\d+(?:\\.\\d+)?(?:[eE][+\\-]?\\d+)?)");
    static const QRegularExpression costRe(QStringLiteral("\\(cost=%1\\s+rows=%1\\)").arg(number));
    static const QRegularExpression actualRe(
        QStringLiteral("\\(actual time=%1\\.\\.%1\\s+rows=%1\\s+loops=%1\\)").arg(number));
    static const QString never = QStringLiteral("(never executed)");

    qsizetype labelEnd = text.size();
    const auto cost = costRe.match(text);
    if (cost.hasMatch()) {
        node.totalCost = cost.captured(1).toDouble();
        node.estimatedRows = cost.captured(2).toDouble();
        labelEnd = std::min(labelEnd, cost.capturedStart(0));
    }
    const auto actual = actualRe.match(text);
    if (actual.hasMatch()) {
        const double loops = actual.captured(4).toDouble();
        node.actualRows = actual.captured(3).toDouble();
        node.loops = loops;
        node.actualTimeMs = actual.captured(2).toDouble() * loops;
        node.details.push_back(ExplainProperty {
            QStringLiteral("Actual time per loop"),
            QStringLiteral("%1..%2 ms").arg(actual.captured(1), actual.captured(2)) });
        labelEnd = std::min(labelEnd, actual.capturedStart(0));
    }
    const qsizetype neverAt = text.indexOf(never);
    if (neverAt >= 0) {
        node.loops = 0.0;
        labelEnd = std::min(labelEnd, neverAt);
    }
    node.label = text.left(labelEnd).trimmed();

    if (node.label.startsWith(QLatin1String("Table scan on")))
        node.warnings.append(Text::tr("Full table scan"));
    if (node.label.contains(QLatin1String("temporary table"), Qt::CaseInsensitive))
        node.warnings.append(Text::tr("Uses a temporary table"));
    addEstimateWarning(node);
}

Result<ExplainPlan> parseTree(const QString &output, ExplainMode mode)
{
    std::vector<TreeLine> lines;
    std::vector<int> depth;
    std::vector<int> tops;
    std::vector<std::pair<qsizetype, int>> stack; // indent, index

    const QStringList rows = output.split(QLatin1Char('\n'));
    for (const QString &raw : rows) {
        const QString row = raw.endsWith(QLatin1Char('\r')) ? raw.chopped(1) : raw;
        if (row.trimmed().isEmpty())
            continue;
        qsizetype indent = 0;
        while (indent < row.size() && row.at(indent).isSpace())
            ++indent;
        if (!row.mid(indent).startsWith(QLatin1String("->")))
            return syntaxError(
                Text::tr("The query plan has an unexpected format."),
                QStringLiteral("Line does not start with \"->\": %1").arg(row.left(200)));
        while (!stack.empty() && stack.back().first >= indent)
            stack.pop_back();

        const int index = static_cast<int>(lines.size());
        int level = 1;
        if (stack.empty()) {
            tops.push_back(index);
        } else {
            const int parent = stack.back().second;
            lines[static_cast<size_t>(parent)].kids.push_back(index);
            level = depth[static_cast<size_t>(parent)] + 1;
        }
        if (level > kMaxDepth)
            return syntaxError(Text::tr("The query plan is nested too deeply to be shown."),
                               QStringLiteral("More than %1 levels").arg(kMaxDepth));
        depth.push_back(level);
        stack.emplace_back(indent, index);
        TreeLine line;
        parseTreeText(row.mid(indent + 2).trimmed(), line.node);
        lines.push_back(std::move(line));
    }
    if (tops.empty())
        return syntaxError(Text::tr("The query plan is empty."));

    ExplainPlan plan;
    plan.rawText = output;
    plan.engine = QStringLiteral("MySQL");
    plan.analyzed = mode == ExplainMode::Analyze;
    qsizetype hidden = 0;
    if (tops.size() == 1) {
        plan.root = buildTree(lines, tops.front());
    } else {
        hidden = 1;
        plan.root.children.reserve(tops.size());
        for (const int top : tops)
            plan.root.children.push_back(buildTree(lines, top));
    }
    finishPlan(plan, hidden);
    return plan;
}

} // namespace

Result<ExplainPlan> parseMysqlExplain(const QString &output, ExplainMode mode)
{
    const QString trimmed = output.trimmed();
    if (trimmed.isEmpty())
        return syntaxError(Text::tr("The query plan is empty."));
    if (trimmed.startsWith(QLatin1String("->")))
        return parseTree(output, mode);
    return parseJson(output, mode);
}

} // namespace quarry::mysql
