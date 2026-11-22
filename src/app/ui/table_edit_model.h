#pragma once

#include "result_table_model.h"

#include <quarry/change_set.h>

#include <QAbstractTableModel>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <map>
#include <vector>

namespace quarry::ui {

// The pending changes of a data tab (ARCHITECTURE section 6.4), as a layer over the rows of a
// ResultTableModel. Only the deltas are stored: the edited cells of the rows that changed, the
// rows marked as deleted and the new rows. The base rows (up to millions) are never copied; a
// cell without a delta is read from the base in data().
//
// Rows: the base rows come first, the new rows follow. Base rows only ever grow at the end, so a
// delta keyed by base row stays valid while "Fetch all" loads more rows (new rows then move down).
//
// Undo/redo works per user action (one cell edit, one inserted row, one delete of a selection):
// each step keeps the before/after state of the rows it touched. Steps are applied and undone in
// strict LIFO order, which is what makes the row numbers of a new row in a step still valid.
class TableEditModel : public QAbstractTableModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by QueryState")

    // The tab was opened from the tree (it may still be read-only, see editable).
    Q_PROPERTY(bool active READ active NOTIFY targetChanged FINAL)
    Q_PROPERTY(bool editable READ editable NOTIFY targetChanged FINAL)
    Q_PROPERTY(QString readOnlyReason READ readOnlyReason NOTIFY targetChanged FINAL)
    // Shown beside the grid when rows are identified by ctid/rowid.
    Q_PROPERTY(QString keyWarning READ keyWarning NOTIFY targetChanged FINAL)
    Q_PROPERTY(QString tableName READ tableName NOTIFY targetChanged FINAL)
    // Rows of the model: the base rows plus the new ones.
    Q_PROPERTY(int rows READ rows NOTIFY rowsChanged FINAL)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY pendingChanged FINAL)
    // "No pending changes", "1 pending change", "3 pending changes".
    Q_PROPERTY(QString pendingText READ pendingText NOTIFY pendingChanged FINAL)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY pendingChanged FINAL)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY pendingChanged FINAL)
    Q_PROPERTY(int lastRow READ lastRow NOTIFY pendingChanged FINAL)
    Q_PROPERTY(int maxDeleteRows READ maxDeleteRows CONSTANT FINAL)
    // A save is in flight: every mutator is refused until the controller says it ended.
    Q_PROPERTY(bool saving READ saving NOTIFY savingChanged FINAL)
    // The grid has a cell editor open: text shortcuts (undo, delete word) belong to it.
    Q_PROPERTY(
        bool cellEditing READ cellEditing WRITE setCellEditing NOTIFY cellEditingChanged FINAL)
    // The last failure of Apply, in words; empty when there is none.
    Q_PROPERTY(QString lastError READ lastError WRITE setLastError NOTIFY lastErrorChanged FINAL)

public:
    enum Role {
        IsNullRole = ResultTableModel::IsNullRole,
        CellStateRole = ResultTableModel::CellStateRole, // CellState
        RowStateRole = ResultTableModel::RowStateRole, // RowState
    };
    enum CellState { CellPlain = 0, CellChanged = 1, CellDefault = 2 };
    enum RowState { RowPlain = 0, RowModified = 1, RowNew = 2, RowDeleted = 3 };
    Q_ENUM(CellState)
    Q_ENUM(RowState)

    // Deleting more rows than this in one step is refused (the step would copy that many deltas).
    static constexpr int kMaxDeleteRows = 100000;
    // Record panel: characters of a value the list shows, and read to tell what kind of value it
    // is.
    static constexpr qsizetype kRecordChars = 2000;
    static constexpr qsizetype kDetectChars = 65536;

    // What the controller learned about the table before the select.
    struct Config
    {
        ObjectPath table;
        RowKey::Kind keyKind = RowKey::Kind::PrimaryKey;
        // The primary key columns, or {the row identifier column actually used}. With a row
        // identifier the select starts with that hidden column.
        QStringList keyColumns;
        QString readOnlyReason; // set for views: the tab is read-only and says why
        QString warning;
    };

    explicit TableEditModel(ResultTableModel *base, QObject *parent = nullptr);

    bool active() const { return m_active; }
    bool editable() const { return m_editable; }
    QString readOnlyReason() const { return m_reason; }
    QString keyWarning() const { return m_config.warning; }
    QString tableName() const
    {
        return m_config.table.parts.value(m_config.table.parts.size() - 1);
    }
    int rows() const { return rowCount(); }
    int pendingCount() const;
    QString pendingText() const;
    bool canUndo() const { return m_head > 0; }
    bool canRedo() const { return m_head < m_history.size(); }
    int lastRow() const { return m_lastRow; }
    static int maxDeleteRows() { return kMaxDeleteRows; }
    bool saving() const { return m_saving; }
    void setSaving(bool saving);
    bool cellEditing() const { return m_cellEditing; }
    void setCellEditing(bool editing);
    QString lastError() const { return m_lastError; }
    void setLastError(const QString &error);

    // Controller side. configure() also hides the key column of the base model (call it before
    // the base's begin()); unconfigure() returns to a plain read-only layer.
    void configure(const Config &config);
    void unconfigure();
    // The target of ChangeSqlBuilder for the current result columns.
    EditTarget target() const;
    // One RowEdit per changed row (updates and deletes by ascending base row, then the new rows in
    // order). `rowsOut` gets, for each edit, the row of this model it came from.
    std::vector<RowEdit> buildEdits(std::vector<int> *rowsOut) const;

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Why a cell cannot be edited, in words; empty when it can.
    Q_INVOKABLE QString blockReason(int row, int column) const;
    // The cell holds NULL now (pending value included; a new row's unset cell is NULL).
    Q_INVOKABLE bool isNullAt(int row, int column) const;
    // What the cell editor starts with (the whole value; NULL is empty).
    Q_INVOKABLE QString editText(int row, int column) const;
    Q_INVOKABLE bool setCell(int row, int column, const QString &text);
    Q_INVOKABLE bool setNull(int row, int column);
    // The row of the new row, or -1 when the table is read-only.
    Q_INVOKABLE int insertRow();
    // A new row with the values of `row` (pending ones included), except the primary key columns.
    // Undoable like insertRow. The row of the copy, or -1 when read-only, saving or no such row.
    Q_INVOKABLE int duplicateRow(int row);
    // Marks rows [top..bottom] as deleted (a new row is just removed). Returns how many rows
    // changed, or -1 when the range is too big (kMaxDeleteRows) or the table is read-only.
    Q_INVOKABLE int deleteRows(int top, int bottom);
    // Each returns what to announce ("Undone: edit of name, row 3"), empty when there is nothing.
    Q_INVOKABLE QString undo();
    Q_INVOKABLE QString redo();
    // Drops every pending change (not undoable).
    Q_INVOKABLE void discardAll();
    // "Modified", "New", "Deleted" or "": the row status shown in the first column.
    Q_INVOKABLE QString rowStateText(int row) const;
    // For the screen reader: "modified", "new row", "deleted" or "" for a cell.
    Q_INVOKABLE QString cellStateText(int row, int column) const;
    // The text the grid shows for a cell.
    Q_INVOKABLE QString cellText(int row, int column) const;
    // "Row 12" or "New row 2".
    Q_INVOKABLE QString rowLabel(int row) const;
    // ---- record panel (H11 phase 3) ----
    // The fields of one row, with the pending values: [{column, name, type (the engine's own
    // name), text (cut at kRecordChars), cut, isNull, state ("modified", "new row", "deleted" or
    // ""), kind (the viewer the value gets: "json", "xml", "text", "blob" or ""), size (BLOB: its
    // bytes, else -1)}]. Never reads more than kDetectChars of a value.
    Q_INVOKABLE QVariantList recordFields(int row) const;
    // The whole value of a field made ready for its viewer (ValueViewer, capped): {ok, kind, name,
    // type, text, notice, size ("" or "N bytes (...)")}. A BLOB gives its hex dump.
    Q_INVOKABLE QVariantMap fieldView(int row, int column) const;
    // The whole value on the clipboard, like Copy in the grid (NULL is empty, a BLOB is 0x...).
    Q_INVOKABLE bool copyField(int row, int column) const;
    // Like ResultTableModel, but with the pending values.
    Q_INVOKABLE QString copyText(int top, int left, int bottom, int right, bool withHeader) const;
    Q_INVOKABLE bool copyToClipboard(int top, int left, int bottom, int right,
                                     bool withHeader) const;

signals:
    void targetChanged();
    void rowsChanged();
    void pendingChanged();
    void cellEditingChanged();
    void savingChanged();
    void lastErrorChanged();
    // The grid should select and focus this row (a failed Apply names it).
    void focusRowRequested(int row);

private:
    struct Value
    {
        bool null = false;
        QString text; // never a null QString when `null` is false
        bool operator==(const Value &) const = default;
    };
    // What one row differs in. For a new row `present` says whether it exists.
    struct RowDelta
    {
        bool present = true;
        bool deleted = false;
        std::map<int, Value> cells;
        bool plain() const { return !deleted && cells.empty(); }
        bool operator==(const RowDelta &) const = default;
    };
    struct Change
    {
        bool isNew = false;
        int index = 0; // base row, or position among the new rows
        RowDelta before;
        RowDelta after;
    };
    struct Command
    {
        QString label;
        std::vector<Change> changes;
    };

    int baseRows() const { return m_base->rowCount(); }
    const RowDelta *deltaOf(int row) const;
    RowDelta stateOf(int row) const;
    Value originalOf(int row, int column) const;
    Value valueOf(int row, int column) const;
    static QString shown(const Value &v);
    QVariant keyValue(int row, int sourceColumn) const;
    void resolve();
    bool setValue(int row, int column, const Value &value);
    void push(Command command);
    void run(const Command &command, bool forward);
    void apply(const Change &change, bool forward, int *top, int *bottom);
    void noteChange();

    ResultTableModel *m_base;
    Config m_config;
    bool m_active = false;
    bool m_editable = false;
    QString m_reason;
    int m_hidden = 0;
    std::vector<int> m_keySource; // source columns (hidden ones included) of the key values
    std::map<int, RowDelta> m_rows; // base rows with a delta; never holds a plain one
    std::vector<RowDelta> m_new;
    std::vector<Command> m_history; // shortcut: unbounded; deltas are small, cap it if needed
    std::size_t m_head = 0; // steps applied; the rest can be redone
    int m_lastRow = -1;
    bool m_cellEditing = false;
    bool m_saving = false;
    QString m_lastError;
};

} // namespace quarry::ui
