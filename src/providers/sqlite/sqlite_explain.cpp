#include "sqlite_explain.h"

#include "sqlite_text.h"

#include <QHash>

#include <deque>
#include <vector>

namespace quarry::sqlite {

namespace {

// The tree is also destroyed recursively: a hostile output must not be able to exhaust the stack.
constexpr int kMaxDepth = 200;

struct Step
{
    QString detail;
    std::vector<int> kids;
};

Unexpected syntaxError(QString message, QString detail = {})
{
    return fail(Error { ErrorKind::Syntax, std::move(message), std::move(detail), {}, {} });
}

void annotate(ExplainNode &node)
{
    const QString &d = node.label;
    const bool scan = d.startsWith(QLatin1String("SCAN ")) && !d.contains(QLatin1String(" USING "))
        && !d.startsWith(QLatin1String("SCAN CONSTANT ROW"))
        && !d.startsWith(QLatin1String("SCAN SUBQUERY"));
    if (scan)
        node.warnings.append(Text::tr("Full table scan"));
    if (d.contains(QLatin1String("USE TEMP B-TREE")))
        node.warnings.append(Text::tr("Uses a temporary b-tree"));
}

ExplainNode build(const std::vector<Step> &steps, int index)
{
    ExplainNode node;
    node.label = steps[static_cast<size_t>(index)].detail;
    annotate(node);
    const auto &kids = steps[static_cast<size_t>(index)].kids;
    node.children.reserve(kids.size());
    for (const int kid : kids)
        node.children.push_back(build(steps, kid));
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

Result<ExplainPlan> parseExplainPlan(const QString &output, ExplainMode mode)
{
    std::vector<Step> steps;
    std::vector<int> depth;
    std::vector<int> tops;
    QHash<int, int> indexById;

    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.endsWith(QLatin1Char('\r')) ? raw.chopped(1) : raw;
        if (line.trimmed().isEmpty())
            continue;
        const qsizetype t1 = line.indexOf(QLatin1Char('\t'));
        const qsizetype t2 = t1 < 0 ? -1 : line.indexOf(QLatin1Char('\t'), t1 + 1);
        const qsizetype t3 = t2 < 0 ? -1 : line.indexOf(QLatin1Char('\t'), t2 + 1);
        if (t3 < 0)
            return syntaxError(
                Text::tr("The query plan has an unexpected format."),
                QStringLiteral("Expected id, parent, notused and detail: %1").arg(line.left(200)));
        bool okId = false;
        bool okParent = false;
        const int id = QStringView(line).left(t1).toInt(&okId);
        const int parent = QStringView(line).mid(t1 + 1, t2 - t1 - 1).toInt(&okParent);
        if (!okId || !okParent)
            return syntaxError(
                Text::tr("The query plan has an unexpected format."),
                QStringLiteral("Id or parent is not a number: %1").arg(line.left(200)));

        const int index = static_cast<int>(steps.size());
        int level = 1;
        if (parent == 0) {
            tops.push_back(index);
        } else {
            const auto it = indexById.constFind(parent);
            if (it == indexById.constEnd())
                return syntaxError(
                    Text::tr("The query plan has an unexpected format."),
                    QStringLiteral("Unknown parent %1 for step %2").arg(parent).arg(id));
            steps[static_cast<size_t>(it.value())].kids.push_back(index);
            level = depth[static_cast<size_t>(it.value())] + 1;
        }
        if (level > kMaxDepth)
            return syntaxError(Text::tr("The query plan is nested too deeply to be shown."),
                               QStringLiteral("More than %1 levels").arg(kMaxDepth));
        depth.push_back(level);
        indexById.insert(id, index);
        steps.push_back(Step { line.mid(t3 + 1), {} });
    }
    if (tops.empty())
        return syntaxError(Text::tr("The query plan is empty."));

    ExplainPlan plan;
    plan.rawText = output;
    plan.engine = QStringLiteral("SQLite");
    plan.analyzed = mode == ExplainMode::Analyze;

    // Several top-level steps hang from a root without label; it is not counted as a node.
    qsizetype hidden = 0;
    if (tops.size() == 1) {
        plan.root = build(steps, tops.front());
    } else {
        hidden = 1;
        plan.root.children.reserve(tops.size());
        for (const int top : tops)
            plan.root.children.push_back(build(steps, top));
    }

    const qsizetype total = countNodes(plan.root);
    if (total - hidden > kMaxExplainNodes) {
        trimBreadthFirst(plan.root, kMaxExplainNodes + hidden);
        plan.truncated = true;
    }
    plan.nodeCount = countNodes(plan.root) - hidden;
    return plan;
}

} // namespace quarry::sqlite
