#include "plan_model.h"

#include <QLocale>

#include <algorithm>
#include <cmath>

namespace quarry::ui {

namespace {

// Rounded first, then written as short as it goes: "12.5", "0.42", "1,234".
QString shortNumber(double value, int decimals)
{
    const double scale = std::pow(10.0, decimals);
    return QLocale().toString(std::round(value * scale) / scale, 'f',
                              QLocale::FloatingPointShortest);
}

QString countText(double value)
{
    return QLocale().toString(static_cast<qlonglong>(std::llround(value)));
}

QString costText(double cost)
{
    return PlanModel::tr("cost %1").arg(shortNumber(cost, cost >= 1000 ? 0 : cost >= 10 ? 1 : 2));
}

QString milliseconds(double ms)
{
    if (ms >= 1000)
        return PlanModel::tr("%1 s").arg(shortNumber(ms / 1000, 2));
    return PlanModel::tr("%1 ms").arg(shortNumber(ms, ms >= 100 ? 0 : ms >= 10 ? 1 : 2));
}

QString rowsText(const quarry::ExplainNode &n)
{
    const auto rows = [](double v) {
        return std::llround(v) == 1 ? PlanModel::tr("1 row")
                                    : PlanModel::tr("%1 rows").arg(countText(v));
    };
    QString text;
    if (n.actualRows)
        text = rows(*n.actualRows);
    else if (n.estimatedRows)
        text = PlanModel::tr("est. %1").arg(rows(*n.estimatedRows));
    if (!text.isEmpty() && n.loops && *n.loops > 1)
        text += PlanModel::tr(" × %1 loops").arg(countText(*n.loops));
    return text;
}

QString percentText(double fraction)
{
    const double percent = fraction * 100;
    if (percent > 0 && percent < 1)
        return PlanModel::tr("<1%");
    return QLocale().toString(static_cast<qlonglong>(std::llround(percent))) + QLocale().percent();
}

// What the node costs by itself: its total minus the totals of its children, never below 0.
template<class Get> double ownOf(const quarry::ExplainNode &n, Get get)
{
    double children = 0;
    for (const quarry::ExplainNode &c : n.children)
        children += get(c).value_or(0.0);
    return std::max(0.0, get(n).value_or(0.0) - children);
}

} // namespace

PlanModel::PlanModel(QObject *parent)
    : QAbstractItemModel(parent)
{ }

QString PlanModel::state() const
{
    switch (m_state) {
    case State::Idle:
        return QStringLiteral("idle");
    case State::Running:
        return QStringLiteral("running");
    case State::Ready:
        return QStringLiteral("ready");
    case State::Failed:
        return QStringLiteral("failed");
    case State::Cancelled:
        return QStringLiteral("cancelled");
    }
    return {};
}

QString PlanModel::planningTimeText() const
{
    return m_planningMs >= 0 ? milliseconds(m_planningMs) : QString();
}

QString PlanModel::executionTimeText() const
{
    return m_executionMs >= 0 ? milliseconds(m_executionMs) : QString();
}

QString PlanModel::barBasis() const
{
    switch (m_basis) {
    case Basis::None:
        return QStringLiteral("none");
    case Basis::Cost:
        return QStringLiteral("cost");
    case Basis::Time:
        return QStringLiteral("time");
    }
    return {};
}

void PlanModel::reset(State state)
{
    beginResetModel();
    m_nodes.clear();
    m_topCount = 0;
    m_basis = Basis::None;
    m_state = state;
    m_errorMessage.clear();
    m_errorDetail.clear();
    m_rawText.clear();
    m_engine.clear();
    m_truncated = false;
    m_planningMs = -1;
    m_executionMs = -1;
    endResetModel();
}

void PlanModel::begin(bool analyze)
{
    reset(State::Running);
    m_requested = true;
    m_analyzed = analyze;
    m_notice.clear();
    emit changed();
}

void PlanModel::setCancelled()
{
    if (m_state != State::Running)
        return;
    m_state = State::Cancelled;
    emit changed();
}

void PlanModel::setNotice(const QString &notice)
{
    m_requested = true;
    m_notice = notice;
    emit changed();
}

void PlanModel::clear()
{
    if (!m_requested && m_state == State::Idle && m_notice.isEmpty())
        return;
    reset(State::Idle);
    m_requested = false;
    m_analyzed = false;
    m_notice.clear();
    emit changed();
}

void PlanModel::setFailure(const QString &message, const QString &detail, const QString &rawText)
{
    const bool analyzed = m_analyzed;
    reset(State::Failed);
    m_requested = true;
    m_analyzed = analyzed;
    m_errorMessage = message;
    m_errorDetail = detail;
    m_rawText = rawText;
    emit changed();
}

void PlanModel::setPlan(const quarry::ExplainPlan &plan)
{
    beginResetModel();
    m_nodes.clear();
    m_state = State::Ready;
    m_requested = true;
    m_errorMessage.clear();
    m_errorDetail.clear();
    m_rawText = plan.rawText;
    m_engine = plan.engine;
    m_analyzed = plan.analyzed;
    m_truncated = plan.truncated;
    m_planningMs = plan.planningTimeMs.value_or(-1.0);
    m_executionMs = plan.executionTimeMs.value_or(-1.0);

    // Breadth first: `source` is parallel to m_nodes.
    std::vector<const quarry::ExplainNode *> source;
    if (plan.root.label.isEmpty()) {
        for (const quarry::ExplainNode &c : plan.root.children)
            source.push_back(&c);
    } else {
        source.push_back(&plan.root);
    }
    m_topCount = static_cast<qsizetype>(source.size());
    m_nodes.resize(source.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
        // The vector grows below: take what is needed from the source first.
        const quarry::ExplainNode &n = *source[i];
        const qsizetype first = static_cast<qsizetype>(source.size());
        m_nodes[i].firstChild = first;
        m_nodes[i].childCount = static_cast<int>(n.children.size());
        const std::size_t before = source.size();
        for (const quarry::ExplainNode &c : n.children)
            source.push_back(&c);
        m_nodes.resize(source.size());
        for (std::size_t k = before; k < source.size(); ++k) {
            m_nodes[k].parent = static_cast<qsizetype>(i);
            m_nodes[k].level = m_nodes[i].level + 1;
            m_nodes[k].position = static_cast<int>(k - before) + 1;
            m_nodes[k].setSize = static_cast<int>(n.children.size());
        }
    }
    for (qsizetype i = 0; i < m_topCount; ++i) {
        m_nodes[static_cast<std::size_t>(i)].position = static_cast<int>(i) + 1;
        m_nodes[static_cast<std::size_t>(i)].setSize = static_cast<int>(m_topCount);
    }

    // The total of the plan: its root's, or the sum of the top level when the root is hidden.
    const auto total = [&](auto get) {
        if (!plan.root.label.isEmpty())
            return get(plan.root).value_or(0.0);
        double sum = 0;
        for (const quarry::ExplainNode &c : plan.root.children)
            sum += get(c).value_or(0.0);
        return get(plan.root).value_or(sum);
    };
    const auto cost = [](const quarry::ExplainNode &n) { return n.totalCost; };
    const auto time = [](const quarry::ExplainNode &n) { return n.actualTimeMs; };
    const double totalTime = plan.analyzed ? total(time) : 0.0;
    const double totalCost = total(cost);
    m_basis = totalTime > 0 ? Basis::Time : totalCost > 0 ? Basis::Cost : Basis::None;
    const double denominator = m_basis == Basis::Time ? totalTime : totalCost;
    const QString shareFormat = m_basis == Basis::Time ? tr("%1 of the total execution time")
                                                       : tr("%1 of the total estimated cost");

    for (std::size_t i = 0; i < source.size(); ++i) {
        const quarry::ExplainNode &n = *source[i];
        Node &node = m_nodes[i];
        node.label = n.label;
        node.details = n.details;
        node.warnings = n.warnings;
        if (n.totalCost)
            node.costText = costText(*n.totalCost);
        node.rowsText = rowsText(n);
        if (n.actualTimeMs)
            node.timeText = milliseconds(*n.actualTimeMs);
        if (m_basis != Basis::None) {
            const double own = m_basis == Basis::Time ? ownOf(n, time) : ownOf(n, cost);
            node.fraction = std::clamp(own / denominator, 0.0, 1.0);
            node.percentText = percentText(node.fraction);
            node.shareText = shareFormat.arg(node.percentText);
        }
    }
    endResetModel();
    emit changed();
}

qsizetype PlanModel::nodeOf(const QModelIndex &index) const
{
    return index.isValid() ? static_cast<qsizetype>(index.internalId()) : -1;
}

qsizetype PlanModel::childAt(qsizetype parent, int row) const
{
    return parent < 0 ? row : m_nodes[static_cast<std::size_t>(parent)].firstChild + row;
}

QModelIndex PlanModel::index(int row, int column, const QModelIndex &parent) const
{
    if (column != 0 || row < 0 || !hasIndex(row, column, parent))
        return {};
    return createIndex(row, column, static_cast<quintptr>(childAt(nodeOf(parent), row)));
}

QModelIndex PlanModel::parent(const QModelIndex &child) const
{
    const qsizetype node = nodeOf(child);
    if (node < 0)
        return {};
    const qsizetype parentNode = m_nodes[static_cast<std::size_t>(node)].parent;
    if (parentNode < 0)
        return {};
    return createIndex(m_nodes[static_cast<std::size_t>(parentNode)].position - 1, 0,
                       static_cast<quintptr>(parentNode));
}

int PlanModel::rowCount(const QModelIndex &parent) const
{
    if (parent.column() > 0)
        return 0;
    const qsizetype node = nodeOf(parent);
    return node < 0 ? static_cast<int>(m_topCount)
                    : m_nodes[static_cast<std::size_t>(node)].childCount;
}

int PlanModel::columnCount(const QModelIndex &) const
{
    return 1;
}

QString PlanModel::describe(const Node &node) const
{
    QStringList parts;
    if (!node.shareText.isEmpty())
        parts.append(node.shareText);
    for (const QString &p : { node.costText, node.rowsText, node.timeText })
        if (!p.isEmpty())
            parts.append(p);
    for (const QString &w : node.warnings)
        parts.append(tr("warning: %1").arg(w));
    parts.append(tr("level %1").arg(node.level + 1));
    parts.append(tr("%1 of %2").arg(node.position).arg(node.setSize));
    return parts.join(QStringLiteral(", "));
}

QVariant PlanModel::data(const QModelIndex &index, int role) const
{
    const qsizetype i = nodeOf(index);
    if (i < 0 || i >= static_cast<qsizetype>(m_nodes.size()) || index.column() != 0)
        return {};
    const Node &n = m_nodes[static_cast<std::size_t>(i)];
    switch (role) {
    case Qt::DisplayRole:
    case LabelRole:
        return n.label;
    case DetailsRole: {
        QVariantList list;
        list.reserve(static_cast<qsizetype>(n.details.size()));
        for (const quarry::ExplainProperty &p : n.details)
            list.append(QVariantMap { { QStringLiteral("name"), p.name },
                                      { QStringLiteral("value"), p.value } });
        return list;
    }
    case CostTextRole:
        return n.costText;
    case RowsTextRole:
        return n.rowsText;
    case TimeTextRole:
        return n.timeText;
    case CostFractionRole:
        return n.fraction;
    case CostPercentTextRole:
        return n.percentText;
    case WarningsRole:
        return n.warnings;
    case LevelRole:
        return n.level;
    case AccessibleDescriptionRole:
        return describe(n);
    case HasBarRole:
        return m_basis != Basis::None;
    case PositionRole:
        return n.position;
    case SetSizeRole:
        return n.setSize;
    case ShareTextRole:
        return n.shareText;
    default:
        return {};
    }
}

QHash<int, QByteArray> PlanModel::roleNames() const
{
    return { { LabelRole, "label" },
             { DetailsRole, "details" },
             { CostTextRole, "costText" },
             { RowsTextRole, "rowsText" },
             { TimeTextRole, "timeText" },
             { CostFractionRole, "costFraction" },
             { CostPercentTextRole, "costPercentText" },
             { WarningsRole, "warnings" },
             { LevelRole, "level" },
             { AccessibleDescriptionRole, "accessibleDescription" },
             { HasBarRole, "hasBar" },
             { PositionRole, "position" },
             { SetSizeRole, "setSize" },
             { ShareTextRole, "shareText" } };
}

} // namespace quarry::ui
