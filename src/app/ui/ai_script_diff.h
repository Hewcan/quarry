#pragma once

#include <quarry/ai/script_diff.h>

#include <QAbstractListModel>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <vector>

namespace quarry::ui {

// The review of the changes Claude proposed (H13 phase C, ARCHITECTURE section 6.13): a model of
// blocks over quarry::ai::ScriptDiff for the editor's review view. The rows are the whole script in
// order: the stretches that do not change ("unchanged") and the proposed changes ("added",
// "removed" or "changed"), each with a decision. Nothing here runs anything or touches the editor:
// result() is only text, and writing it into the editor (one undoable edit) is up to the view.
class AiScriptDiff : public QAbstractListModel
{
    Q_OBJECT
    QML_ANONYMOUS

    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
    // How many blocks are changes (not "unchanged"), and how many of them still wait for a
    // decision.
    Q_PROPERTY(int changeCount READ changeCount CONSTANT FINAL)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY decisionsChanged FINAL)
    Q_PROPERTY(bool hasPending READ hasPending NOTIFY decisionsChanged FINAL)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY decisionsChanged FINAL)
    // Some block (decided or not) adds DML or DDL: the view warns before the script is run.
    Q_PROPERTY(bool modifiesData READ modifiesData CONSTANT FINAL)
    // The script as it was when it was sent to Claude: the base of the diff.
    Q_PROPERTY(QString original READ original CONSTANT FINAL)

public:
    enum Role {
        KindRole = Qt::UserRole + 1, // "unchanged" | "added" | "removed" | "changed"
        OldLinesRole, // the original lines of the block (what a removal or a change takes away)
        NewLinesRole, // the lines Claude proposes (empty for "unchanged" and "removed")
        DecisionRole, // "pending" | "accepted" | "rejected" ("" for "unchanged")
        ModifiesDataRole,
        ChangeNumberRole, // 1-based among the changes, 0 for "unchanged"
        StartLineRole, // 1-based line of the original where the block starts
        DescriptionRole, // "Change 2 of 3, adds 4 lines, pending, changes data": for the screen
                         // reader
        AddedCountRole,
        RemovedCountRole,
    };
    Q_ENUM(Role)

    explicit AiScriptDiff(quarry::ai::ScriptDiff diff, QObject *parent = nullptr);

    int count() const { return static_cast<int>(m_rows.size()); }
    int changeCount() const { return m_changes; }
    int pendingCount() const;
    bool hasPending() const { return m_diff.hasPending(); }
    bool canUndo() const { return m_diff.canUndo(); }
    bool modifiesData() const { return m_modifiesData; }
    QString original() const { return m_diff.original(); }

    // `block` is a row of the model; an "unchanged" row or a row out of range does nothing.
    Q_INVOKABLE void accept(int block);
    Q_INVOKABLE void reject(int block);
    Q_INVOKABLE void acceptAll();
    Q_INVOKABLE void rejectAll();
    Q_INVOKABLE void undo();
    // The script with only the accepted blocks applied (the original where pending or rejected).
    Q_INVOKABLE QString result() const;
    // The next / previous row that is a change, from `from` (exclusive); -1 when there is none.
    Q_INVOKABLE int nextChange(int from, int step) const;
    Q_INVOKABLE int firstChange() const { return nextChange(-1, 1); }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();
    void decisionsChanged();

private:
    struct Row
    {
        qsizetype hunk = -1; // index in the ScriptDiff; -1 for an unchanged stretch
        qsizetype first = 0; // unchanged: first original line (0-based)
        qsizetype lines = 0; // unchanged: how many
        int change = 0; // 1-based among the changes
    };

    QString kindOf(const Row &row) const;
    QString descriptionOf(const Row &row) const;
    void touched();

    quarry::ai::ScriptDiff m_diff;
    QStringList m_lines; // the original, line by line (no empty last element)
    std::vector<Row> m_rows;
    int m_changes = 0;
    bool m_modifiesData = false;
};

} // namespace quarry::ui
