#include "ai_script_diff.h"

#include <QCoreApplication>

#include <algorithm>

namespace quarry::ui {

namespace {

using Decision = quarry::ai::DiffHunk::Decision;

QString decisionName(Decision decision)
{
    switch (decision) {
    case Decision::Pending:
        return QStringLiteral("pending");
    case Decision::Accepted:
        return QStringLiteral("accepted");
    case Decision::Rejected:
        break;
    }
    return QStringLiteral("rejected");
}

QString lineCount(qsizetype n)
{
    return n == 1 ? AiScriptDiff::tr("1 line") : AiScriptDiff::tr("%1 lines").arg(n);
}

} // namespace

AiScriptDiff::AiScriptDiff(quarry::ai::ScriptDiff diff, QObject *parent)
    : QAbstractListModel(parent)
    , m_diff(std::move(diff))
{
    m_lines = m_diff.original().split(QLatin1Char('\n'));
    if (m_diff.original().endsWith(QLatin1Char('\n')) || m_diff.original().isEmpty())
        m_lines.removeLast();

    // The hunks come in order of the original: the stretch before each one is an unchanged row.
    const std::vector<quarry::ai::DiffHunk> &hunks = m_diff.hunks();
    m_rows.reserve(hunks.size() * 2 + 1);
    qsizetype next = 0;
    for (qsizetype i = 0; i < static_cast<qsizetype>(hunks.size()); ++i) {
        const quarry::ai::DiffHunk &h = hunks[static_cast<size_t>(i)];
        if (h.originalLine > next)
            m_rows.push_back({ -1, next, h.originalLine - next, 0 });
        m_rows.push_back({ i, 0, 0, ++m_changes });
        m_modifiesData = m_modifiesData || h.modifiesData;
        next = h.originalLine + h.removed.size();
    }
    if (next < m_lines.size())
        m_rows.push_back({ -1, next, m_lines.size() - next, 0 });
}

int AiScriptDiff::pendingCount() const
{
    return static_cast<int>(std::count_if(
        m_diff.hunks().begin(), m_diff.hunks().end(),
        [](const quarry::ai::DiffHunk &h) { return h.decision == Decision::Pending; }));
}

QString AiScriptDiff::kindOf(const Row &row) const
{
    if (row.hunk < 0)
        return QStringLiteral("unchanged");
    const quarry::ai::DiffHunk &h = m_diff.hunks()[static_cast<size_t>(row.hunk)];
    if (h.removed.isEmpty())
        return QStringLiteral("added");
    return h.added.isEmpty() ? QStringLiteral("removed") : QStringLiteral("changed");
}

// "Change 2 of 3, adds 4 lines, replaces 1 line, pending, changes data"
QString AiScriptDiff::descriptionOf(const Row &row) const
{
    if (row.hunk < 0)
        return tr("Unchanged, %1").arg(lineCount(row.lines));
    const quarry::ai::DiffHunk &h = m_diff.hunks()[static_cast<size_t>(row.hunk)];
    QStringList parts;
    parts << tr("Change %1 of %2").arg(row.change).arg(m_changes);
    if (h.removed.isEmpty())
        parts << tr("adds %1").arg(lineCount(h.added.size()));
    else if (h.added.isEmpty())
        parts << tr("removes %1").arg(lineCount(h.removed.size()));
    else
        parts << tr("replaces %1 with %2")
                     .arg(lineCount(h.removed.size()), lineCount(h.added.size()));
    switch (h.decision) {
    case Decision::Pending:
        parts << tr("pending");
        break;
    case Decision::Accepted:
        parts << tr("accepted");
        break;
    case Decision::Rejected:
        parts << tr("rejected");
        break;
    }
    if (h.modifiesData)
        parts << tr("changes data");
    return parts.join(QStringLiteral(", "));
}

int AiScriptDiff::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant AiScriptDiff::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= count())
        return {};
    const Row &row = m_rows[static_cast<size_t>(index.row())];
    const quarry::ai::DiffHunk *hunk
        = row.hunk >= 0 ? &m_diff.hunks()[static_cast<size_t>(row.hunk)] : nullptr;
    switch (role) {
    case KindRole:
        return kindOf(row);
    case OldLinesRole:
        return hunk ? hunk->removed : m_lines.mid(row.first, row.lines);
    case NewLinesRole:
        return hunk ? hunk->added : QStringList();
    case DecisionRole:
        return hunk ? decisionName(hunk->decision) : QString();
    case ModifiesDataRole:
        return hunk && hunk->modifiesData;
    case ChangeNumberRole:
        return row.change;
    case StartLineRole:
        return static_cast<int>(hunk ? hunk->originalLine + 1 : row.first + 1);
    case DescriptionRole:
        return descriptionOf(row);
    case AddedCountRole:
        return static_cast<int>(hunk ? hunk->added.size() : 0);
    case RemovedCountRole:
        return static_cast<int>(hunk ? hunk->removed.size() : 0);
    default:
        break;
    }
    return {};
}

QHash<int, QByteArray> AiScriptDiff::roleNames() const
{
    return { { KindRole, "kind" },
             { OldLinesRole, "oldLines" },
             { NewLinesRole, "newLines" },
             { DecisionRole, "decision" },
             { ModifiesDataRole, "modifiesData" },
             { ChangeNumberRole, "changeNumber" },
             { StartLineRole, "startLine" },
             { DescriptionRole, "description" },
             { AddedCountRole, "addedCount" },
             { RemovedCountRole, "removedCount" } };
}

void AiScriptDiff::touched()
{
    // A decision changes the decision and the text read aloud of the changes only.
    if (!m_rows.empty())
        emit dataChanged(index(0), index(count() - 1), { DecisionRole, DescriptionRole });
    emit decisionsChanged();
}

void AiScriptDiff::accept(int block)
{
    if (block < 0 || block >= count() || m_rows[static_cast<size_t>(block)].hunk < 0)
        return;
    m_diff.accept(m_rows[static_cast<size_t>(block)].hunk);
    touched();
}

void AiScriptDiff::reject(int block)
{
    if (block < 0 || block >= count() || m_rows[static_cast<size_t>(block)].hunk < 0)
        return;
    m_diff.reject(m_rows[static_cast<size_t>(block)].hunk);
    touched();
}

void AiScriptDiff::acceptAll()
{
    if (m_changes == 0)
        return;
    m_diff.acceptAll();
    touched();
}

void AiScriptDiff::rejectAll()
{
    if (m_changes == 0)
        return;
    m_diff.rejectAll();
    touched();
}

void AiScriptDiff::undo()
{
    if (m_diff.undo())
        touched();
}

QString AiScriptDiff::result() const
{
    return m_diff.result();
}

int AiScriptDiff::nextChange(int from, int step) const
{
    for (int i = from + (step < 0 ? -1 : 1); i >= 0 && i < count(); i += (step < 0 ? -1 : 1)) {
        if (m_rows[static_cast<size_t>(i)].hunk >= 0)
            return i;
    }
    return -1;
}

} // namespace quarry::ui
