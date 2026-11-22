#include <quarry/ai/script_diff.h>

#include <QRegularExpression>

#include <vector>

namespace quarry::ai {

using namespace Qt::StringLiterals;

namespace {

// shortcut: a quadratic LCS; above this many cells the change becomes one hunk. Good for scripts
// of a few thousand lines; use Myers' O(ND) if users edit much larger files with the AI.
constexpr qsizetype kMaxCells = 4'000'000;

QStringList splitLines(QStringView text, bool *trailingNewline)
{
    QStringList lines = text.toString().split(u'\n');
    const bool trailing = text.endsWith(u'\n');
    if (trailing)
        lines.removeLast();
    if (trailingNewline)
        *trailingNewline = trailing;
    if (text.isEmpty())
        lines.clear();
    return lines;
}

} // namespace

bool ScriptDiff::modifiesData(const QStringList &lines)
{
    // Conservative on purpose: a false alarm costs a glance, a miss could cost data.
    static const QRegularExpression kWrite(
        uR"(\b(insert|update|delete|merge|upsert|replace|truncate|create|alter|drop|rename|grant|revoke|call|exec|execute|copy|load)\b)"_s,
        QRegularExpression::CaseInsensitiveOption);
    for (const QString &line : lines) {
        const qsizetype comment = line.indexOf(u"--");
        if (kWrite.match(comment < 0 ? line : line.left(comment)).hasMatch())
            return true;
    }
    return false;
}

ScriptDiff ScriptDiff::compute(QStringView original, QStringView proposed)
{
    ScriptDiff diff;
    diff.m_original = original.toString();
    diff.m_originalLines = splitLines(original, &diff.m_trailingNewline);
    const QStringList a = diff.m_originalLines;
    const QStringList b = splitLines(proposed, nullptr);
    const qsizetype n = a.size();
    const qsizetype m = b.size();

    auto addHunk = [&](qsizetype aStart, qsizetype aEnd, qsizetype bStart, qsizetype bEnd) {
        if (aStart == aEnd && bStart == bEnd)
            return;
        DiffHunk hunk;
        hunk.originalLine = aStart;
        hunk.removed = a.mid(aStart, aEnd - aStart);
        hunk.added = b.mid(bStart, bEnd - bStart);
        hunk.modifiesData = modifiesData(hunk.added);
        diff.m_hunks.push_back(std::move(hunk));
    };

    if ((n + 1) * (m + 1) > kMaxCells) {
        qsizetype prefix = 0;
        while (prefix < n && prefix < m && a[prefix] == b[prefix])
            ++prefix;
        addHunk(prefix, n, prefix, m);
        return diff;
    }

    // lcs[i][j]: length of the LCS of a[i..] and b[j..].
    const qsizetype width = m + 1;
    std::vector<int> lcs(static_cast<size_t>((n + 1) * width), 0);
    for (qsizetype i = n - 1; i >= 0; --i)
        for (qsizetype j = m - 1; j >= 0; --j)
            lcs[i * width + j] = a[i] == b[j]
                ? lcs[(i + 1) * width + j + 1] + 1
                : std::max(lcs[(i + 1) * width + j], lcs[i * width + j + 1]);

    qsizetype i = 0;
    qsizetype j = 0;
    qsizetype hunkA = 0;
    qsizetype hunkB = 0;
    while (i < n || j < m) {
        if (i < n && j < m && a[i] == b[j]) {
            addHunk(hunkA, i, hunkB, j);
            ++i;
            ++j;
            hunkA = i;
            hunkB = j;
        } else if (j < m && (i == n || lcs[i * width + j + 1] >= lcs[(i + 1) * width + j])) {
            ++j;
        } else {
            ++i;
        }
    }
    addHunk(hunkA, n, hunkB, m);
    return diff;
}

bool ScriptDiff::hasPending() const
{
    for (const DiffHunk &h : m_hunks)
        if (h.decision == DiffHunk::Decision::Pending)
            return true;
    return false;
}

void ScriptDiff::decide(qsizetype hunk, DiffHunk::Decision decision, bool record)
{
    if (hunk < 0 || hunk >= static_cast<qsizetype>(m_hunks.size()))
        return;
    if (record) {
        std::vector<DiffHunk::Decision> snapshot;
        snapshot.reserve(m_hunks.size());
        for (const DiffHunk &h : m_hunks)
            snapshot.push_back(h.decision);
        m_undo.push_back(std::move(snapshot));
    }
    m_hunks[static_cast<size_t>(hunk)].decision = decision;
}

void ScriptDiff::accept(qsizetype hunk)
{
    decide(hunk, DiffHunk::Decision::Accepted, true);
}

void ScriptDiff::reject(qsizetype hunk)
{
    decide(hunk, DiffHunk::Decision::Rejected, true);
}

void ScriptDiff::acceptAll()
{
    for (qsizetype i = 0; i < static_cast<qsizetype>(m_hunks.size()); ++i)
        decide(i, DiffHunk::Decision::Accepted, i == 0);
}

void ScriptDiff::rejectAll()
{
    for (qsizetype i = 0; i < static_cast<qsizetype>(m_hunks.size()); ++i)
        decide(i, DiffHunk::Decision::Rejected, i == 0);
}

bool ScriptDiff::undo()
{
    if (m_undo.empty())
        return false;
    const auto snapshot = std::move(m_undo.back());
    m_undo.pop_back();
    for (size_t i = 0; i < m_hunks.size(); ++i)
        m_hunks[i].decision = snapshot[i];
    return true;
}

QString ScriptDiff::result() const
{
    QStringList lines;
    qsizetype next = 0;
    for (const DiffHunk &h : m_hunks) {
        lines += m_originalLines.mid(next, h.originalLine - next);
        lines += h.decision == DiffHunk::Decision::Accepted ? h.added : h.removed;
        next = h.originalLine + h.removed.size();
    }
    lines += m_originalLines.mid(next);
    QString out = lines.join(u'\n');
    if (m_trailingNewline && !lines.isEmpty())
        out += u'\n';
    return out;
}

} // namespace quarry::ai
