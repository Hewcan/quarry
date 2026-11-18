#pragma once

// Helpers shared by the MySQL and the MariaDB plan parsers (mysql_explain.cpp,
// mariadb_explain.cpp). Header only: the two parsers stay separate files on purpose.

#include "mysql_text.h"

#include <quarry/explain_plan.h>
#include <quarry/result.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <deque>

namespace quarry::mysql::explain_detail {

// The tree is also destroyed recursively: a hostile output must not be able to exhaust the stack.
inline constexpr int kMaxDepth = 200;

inline Unexpected syntaxError(QString message, QString detail = {})
{
    return fail(Error { ErrorKind::Syntax, std::move(message), std::move(detail), {}, {} });
}

inline QString scalarText(const QJsonValue &value)
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

// MySQL writes most numbers as strings ("1.05"), MariaDB as numbers.
inline std::optional<double> numberOf(const QJsonValue &value)
{
    if (value.isDouble())
        return value.toDouble();
    if (value.isString()) {
        bool ok = false;
        const double number = value.toString().toDouble(&ok);
        if (ok)
            return number;
    }
    return std::nullopt;
}

inline std::optional<double> numberAt(const QJsonObject &object, const char *key)
{
    return numberOf(object.value(QLatin1String(key)));
}

// "used_key_parts" -> "Used key parts"
inline QString humanize(const char *key)
{
    QString text = QString::fromLatin1(key);
    text.replace(QLatin1Char('_'), QLatin1Char(' '));
    text.replace(QLatin1Char('-'), QLatin1Char(' '));
    if (!text.isEmpty())
        text[0] = text.at(0).toUpper();
    return text;
}

// Adds the "Row estimate off" warning when the actual rows differ 10x or more from the estimate.
inline void addEstimateWarning(ExplainNode &node)
{
    const auto estimated = node.estimatedRows;
    const auto actual = node.actualRows;
    const auto loops = node.loops;
    if (!estimated || !actual || (loops && *loops <= 0.0))
        return;
    const double est = std::max(*estimated, 1.0);
    const double act = std::max(*actual, 1.0);
    const double ratio = std::max(est / act, act / est);
    if (ratio >= 10.0)
        node.warnings.append(Text::tr("Row estimate off by %1x").arg(qRound64(ratio)));
}

inline qsizetype countNodes(const ExplainNode &node)
{
    qsizetype total = 1;
    for (const auto &child : node.children)
        total += countNodes(child);
    return total;
}

// Keeps the first `limit` nodes in breadth-first order: the top of the plan survives.
inline void trimBreadthFirst(ExplainNode &root, qsizetype limit)
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

// Applies kMaxExplainNodes and fills nodeCount. `hiddenRoot` is 1 when the root is the unlabeled
// wrapper the UI hides: it does not count.
inline void finishPlan(ExplainPlan &plan, qsizetype hiddenRoot = 0)
{
    if (countNodes(plan.root) - hiddenRoot > kMaxExplainNodes) {
        trimBreadthFirst(plan.root, kMaxExplainNodes + hiddenRoot);
        plan.truncated = true;
    }
    plan.nodeCount = countNodes(plan.root) - hiddenRoot;
}

} // namespace quarry::mysql::explain_detail
