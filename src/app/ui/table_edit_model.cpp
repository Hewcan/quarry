#include "table_edit_model.h"

#include <quarry/cell_formatter.h>

#include <QClipboard>
#include <QGuiApplication>

#include <algorithm>

namespace quarry::ui {

namespace {

// A non-null empty string: QVariant(QString()) would be NULL for the providers.
QString nonNull(const QString &text)
{
    return text.isNull() ? QStringLiteral("") : text;
}

QString oneLine(QString text)
{
    for (QChar &ch : text)
        if (ch == QLatin1Char('\t') || ch == QLatin1Char('\n') || ch == QLatin1Char('\r'))
            ch = QLatin1Char(' ');
    return text;
}

} // namespace

TableEditModel::TableEditModel(ResultTableModel *base, QObject *parent)
    : QAbstractTableModel(parent)
    , m_base(base)
{
    connect(m_base, &QAbstractItemModel::rowsAboutToBeInserted, this,
            [this](const QModelIndex &, int first, int last) { beginInsertRows({}, first, last); });
    connect(m_base, &QAbstractItemModel::rowsInserted, this, [this] {
        endInsertRows();
        emit rowsChanged();
    });
    // A new result drops every pending change: the controller asks before it restarts the select.
    connect(m_base, &QAbstractItemModel::modelAboutToBeReset, this, [this] {
        beginResetModel();
        m_rows.clear();
        m_new.clear();
        m_history.clear();
        m_head = 0;
        m_lastRow = -1;
        m_lastError.clear();
    });
    connect(m_base, &QAbstractItemModel::modelReset, this, [this] {
        endResetModel();
        emit rowsChanged();
        emit pendingChanged();
        emit lastErrorChanged();
    });
    connect(m_base, &ResultTableModel::columnsChanged, this, &TableEditModel::resolve);
}

// ---- layout ------------------------------------------------------------------------------------

int TableEditModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : baseRows() + static_cast<int>(m_new.size());
}

int TableEditModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_base->columnCount();
}

QHash<int, QByteArray> TableEditModel::roleNames() const
{
    return m_base->roleNames();
}

QVariant TableEditModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    return m_base->headerData(section, orientation, role);
}

const TableEditModel::RowDelta *TableEditModel::deltaOf(int row) const
{
    if (row < baseRows()) {
        const auto it = m_rows.find(row);
        return it == m_rows.end() ? nullptr : &it->second;
    }
    const auto i = static_cast<std::size_t>(row - baseRows());
    return i < m_new.size() ? &m_new[i] : nullptr;
}

TableEditModel::Value TableEditModel::originalOf(int row, int column) const
{
    if (m_base->sourceIsNull(row, column + m_hidden))
        return Value { true, {} };
    return Value { false, nonNull(m_base->sourceText(row, column + m_hidden)) };
}

// What the cell holds now: the pending value, else (a base row) the original.
TableEditModel::Value TableEditModel::valueOf(int row, int column) const
{
    if (const RowDelta *d = deltaOf(row)) {
        const auto it = d->cells.find(column);
        if (it != d->cells.end())
            return it->second;
    }
    return row < baseRows() ? originalOf(row, column) : Value { true, {} };
}

TableEditModel::RowDelta TableEditModel::stateOf(int row) const
{
    const RowDelta *d = deltaOf(row);
    return d != nullptr ? *d : RowDelta {};
}

QString TableEditModel::shown(const Value &v)
{
    if (v.null)
        return QStringLiteral("NULL");
    const qsizetype lineEnd = v.text.indexOf(QLatin1Char('\n'));
    const qsizetype cut = lineEnd >= 0 ? lineEnd : v.text.size();
    if (cut > CellFormatter::kDisplayChars || lineEnd >= 0)
        return v.text.left(std::min<qsizetype>(cut, CellFormatter::kDisplayChars))
            + QStringLiteral("…");
    return v.text;
}

QVariant TableEditModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.column() >= columnCount() || index.row() >= rowCount())
        return {};
    const int row = index.row();
    const int column = index.column();
    const RowDelta *d = deltaOf(row);
    const bool isNewRow = row >= baseRows();
    const auto cell = d != nullptr ? d->cells.find(column) : std::map<int, Value>::const_iterator();
    const bool has = d != nullptr && cell != d->cells.end();
    switch (role) {
    case Qt::DisplayRole:
        if (has)
            return shown(cell->second);
        if (isNewRow)
            return tr("DEFAULT");
        return m_base->data(m_base->index(row, column), Qt::DisplayRole);
    case IsNullRole:
        if (has)
            return cell->second.null;
        if (isNewRow)
            return true; // DEFAULT is shown muted and in italics
        return m_base->data(m_base->index(row, column), ResultTableModel::IsNullRole);
    case CellStateRole:
        return static_cast<int>(has ? CellChanged : (isNewRow ? CellDefault : CellPlain));
    case RowStateRole:
        if (isNewRow)
            return static_cast<int>(RowNew);
        return static_cast<int>(d == nullptr ? RowPlain : (d->deleted ? RowDeleted : RowModified));
    default:
        return {};
    }
}

// ---- target ------------------------------------------------------------------------------------

void TableEditModel::configure(const Config &config)
{
    m_config = config;
    m_active = true;
    m_hidden = config.keyKind == RowKey::Kind::RowIdentifier ? 1 : 0;
    m_base->setHiddenColumns(m_hidden);
    resolve();
}

void TableEditModel::unconfigure()
{
    m_config = {};
    m_active = false;
    m_hidden = 0;
    m_base->setHiddenColumns(0);
    resolve();
}

void TableEditModel::resolve()
{
    m_keySource.clear();
    m_editable = false;
    m_reason.clear();
    if (m_active) {
        if (!m_config.readOnlyReason.isEmpty()) {
            m_reason = m_config.readOnlyReason;
        } else if (m_base->columns() > 0) {
            if (m_config.keyKind == RowKey::Kind::RowIdentifier) {
                m_keySource.push_back(0); // the hidden first column
            } else {
                for (const QString &name : m_config.keyColumns) {
                    int found = -1;
                    for (int c = 0; c < m_base->columns() && found < 0; ++c)
                        if (m_base->columnName(c) == name)
                            found = c;
                    if (found < 0) {
                        m_keySource.clear();
                        m_reason = tr("The primary key column “%1” is not in the result, so rows "
                                      "cannot be identified.")
                                       .arg(name);
                        break;
                    }
                    m_keySource.push_back(found + m_hidden);
                }
            }
            m_editable = m_reason.isEmpty() && !m_keySource.empty();
        }
    }
    emit targetChanged();
}

EditTarget TableEditModel::target() const
{
    EditTarget t;
    t.table = m_config.table;
    for (int c = 0; c < m_base->columns(); ++c)
        t.columns.append(m_base->columnName(c));
    t.key.kind = m_config.keyKind;
    t.key.columns = m_config.keyColumns;
    return t;
}

QVariant TableEditModel::keyValue(int row, int sourceColumn) const
{
    if (m_base->sourceIsNull(row, sourceColumn))
        return {};
    return QVariant(nonNull(m_base->sourceText(row, sourceColumn)));
}

std::vector<RowEdit> TableEditModel::buildEdits(std::vector<int> *rowsOut) const
{
    std::vector<RowEdit> out;
    const auto toVariant = [](const Value &v) { return v.null ? QVariant() : QVariant(v.text); };
    const auto cellsOf = [&](const RowDelta &d) {
        std::vector<CellChange> cells;
        cells.reserve(d.cells.size());
        for (const auto &[column, value] : d.cells)
            cells.push_back({ column, toVariant(value) });
        return cells;
    };
    for (const auto &[row, d] : m_rows) {
        RowEdit edit;
        edit.kind = d.deleted ? RowEdit::Kind::Delete : RowEdit::Kind::Update;
        for (int source : m_keySource)
            edit.key.append(keyValue(row, source));
        if (!d.deleted)
            edit.cells = cellsOf(d);
        out.push_back(std::move(edit));
        if (rowsOut != nullptr)
            rowsOut->push_back(row);
    }
    for (std::size_t i = 0; i < m_new.size(); ++i) {
        RowEdit edit;
        edit.kind = RowEdit::Kind::Insert;
        edit.cells = cellsOf(m_new[i]);
        out.push_back(std::move(edit));
        if (rowsOut != nullptr)
            rowsOut->push_back(baseRows() + static_cast<int>(i));
    }
    return out;
}

// ---- state -------------------------------------------------------------------------------------

int TableEditModel::pendingCount() const
{
    return static_cast<int>(m_rows.size() + m_new.size());
}

QString TableEditModel::pendingText() const
{
    const int n = pendingCount();
    if (n == 0)
        return tr("No pending changes");
    return n == 1 ? tr("1 pending change") : tr("%1 pending changes").arg(n);
}

void TableEditModel::setCellEditing(bool editing)
{
    if (editing == m_cellEditing)
        return;
    m_cellEditing = editing;
    emit cellEditingChanged();
}

void TableEditModel::setSaving(bool saving)
{
    if (saving == m_saving)
        return;
    m_saving = saving;
    emit savingChanged();
}

void TableEditModel::setLastError(const QString &error)
{
    if (error == m_lastError)
        return;
    m_lastError = error;
    emit lastErrorChanged();
}

QString TableEditModel::rowStateText(int row) const
{
    if (row < 0 || row >= rowCount())
        return {};
    switch (data(index(row, 0), RowStateRole).toInt()) {
    case RowModified:
        return tr("Modified");
    case RowNew:
        return tr("New");
    case RowDeleted:
        return tr("Deleted");
    default:
        return {};
    }
}

QString TableEditModel::cellStateText(int row, int column) const
{
    if (row < 0 || row >= rowCount() || column < 0 || column >= columnCount())
        return {};
    const QModelIndex i = index(row, column);
    if (data(i, RowStateRole).toInt() == RowDeleted)
        return tr("deleted");
    if (data(i, RowStateRole).toInt() == RowNew)
        return tr("new row");
    return data(i, CellStateRole).toInt() == CellChanged ? tr("modified") : QString();
}

QString TableEditModel::cellText(int row, int column) const
{
    return data(index(row, column), Qt::DisplayRole).toString();
}

QString TableEditModel::rowLabel(int row) const
{
    return row >= baseRows() ? tr("New row %1").arg(row - baseRows() + 1)
                             : tr("Row %1").arg(row + 1);
}

QString TableEditModel::blockReason(int row, int column) const
{
    if (m_saving)
        return tr("Saving changes…");
    if (!m_active)
        return tr("Rows can only be edited in a table opened from the tree.");
    if (!m_editable)
        return m_reason.isEmpty() ? tr("This table is not editable.") : m_reason;
    if (row < 0 || row >= rowCount() || column < 0 || column >= columnCount())
        return tr("There is no such cell.");
    if (const RowDelta *d = deltaOf(row); d != nullptr && d->deleted)
        return tr("This row is marked as deleted. Undo to restore it.");
    if (row < baseRows() && m_base->isBinary(row, column))
        return tr("Binary values cannot be edited.");
    return {};
}

QString TableEditModel::editText(int row, int column) const
{
    const Value v = valueOf(row, column);
    return v.null ? QString() : v.text;
}

// ---- editing -----------------------------------------------------------------------------------

bool TableEditModel::setValue(int row, int column, const Value &value)
{
    if (!blockReason(row, column).isEmpty())
        return false;
    const RowDelta before = stateOf(row);
    RowDelta after = before;
    // Back to the original value: the cell is no longer changed.
    if (row < baseRows() && value == originalOf(row, column))
        after.cells.erase(column);
    else
        after.cells[column] = value;
    if (after == before)
        return true;
    Change change;
    change.isNew = row >= baseRows();
    change.index = change.isNew ? row - baseRows() : row;
    change.before = before;
    change.after = after;
    push({ tr("edit of %1, %2").arg(m_base->columnName(column), rowLabel(row)),
           { std::move(change) } });
    return true;
}

bool TableEditModel::setCell(int row, int column, const QString &text)
{
    return setValue(row, column, Value { false, nonNull(text) });
}

bool TableEditModel::setNull(int row, int column)
{
    return setValue(row, column, Value { true, {} });
}

int TableEditModel::insertRow()
{
    if (!m_editable || m_saving)
        return -1;
    Change change;
    change.isNew = true;
    change.index = static_cast<int>(m_new.size());
    change.before.present = false;
    push({ tr("insert of a new row"), { std::move(change) } });
    return baseRows() + static_cast<int>(m_new.size()) - 1;
}

// shortcut: only the primary key columns are left out (they are the ones that would collide); a
// server-generated value in another column (a timestamp, a UUID default) is copied. Improve it
// when the providers report which columns are generated.
int TableEditModel::duplicateRow(int row)
{
    if (!m_editable || m_saving || row < 0 || row >= rowCount())
        return -1;
    const RowDelta *source = deltaOf(row);
    if (source != nullptr && source->deleted)
        return -1;
    const bool isNewRow = row >= baseRows();
    RowDelta copy;
    for (int c = 0; c < columnCount(); ++c) {
        if (m_config.keyKind == RowKey::Kind::PrimaryKey
            && m_config.keyColumns.contains(m_base->columnName(c)))
            continue;
        if (isNewRow) { // the cells never touched stay DEFAULT
            const auto it = source != nullptr ? source->cells.find(c)
                                              : std::map<int, Value>::const_iterator();
            if (source != nullptr && it != source->cells.end())
                copy.cells[c] = it->second;
        } else if (!m_base->isBinary(row, c)) { // binary values cannot be edited, nor copied
            copy.cells[c] = valueOf(row, c);
        }
    }
    Change change;
    change.isNew = true;
    change.index = static_cast<int>(m_new.size());
    change.before.present = false;
    change.after = std::move(copy);
    push({ tr("duplicate of %1").arg(rowLabel(row)), { std::move(change) } });
    return baseRows() + static_cast<int>(m_new.size()) - 1;
}

int TableEditModel::deleteRows(int top, int bottom)
{
    if (!m_editable || m_saving)
        return -1;
    top = std::max(top, 0);
    bottom = std::min(bottom, rowCount() - 1);
    if (top > bottom)
        return 0;
    if (bottom - top + 1 > kMaxDeleteRows)
        return -1;
    Command command;
    const int base = baseRows();
    for (int r = top; r <= std::min(bottom, base - 1); ++r) {
        Change change;
        change.index = r;
        change.before = stateOf(r);
        if (change.before.deleted)
            continue;
        change.after.deleted = true; // the edits of the row go with it
        command.changes.push_back(std::move(change));
    }
    // New rows are removed from the last to the first, so the positions stay valid.
    for (int r = bottom; r >= std::max(top, base); --r) {
        Change change;
        change.isNew = true;
        change.index = r - base;
        change.before = stateOf(r);
        change.after.present = false;
        command.changes.push_back(std::move(change));
    }
    const auto count = static_cast<int>(command.changes.size());
    if (count == 0)
        return 0;
    command.label = count == 1 ? tr("deletion of %1").arg(rowLabel(top))
                               : tr("deletion of %1 rows").arg(count);
    push(std::move(command));
    return count;
}

void TableEditModel::push(Command command)
{
    m_history.resize(m_head); // a new step drops what could be redone
    run(command, true);
    m_history.push_back(std::move(command));
    m_head = m_history.size();
    noteChange();
}

QString TableEditModel::undo()
{
    if (m_saving || m_head == 0)
        return {};
    const Command &c = m_history[--m_head];
    run(c, false);
    noteChange();
    return tr("Undone: %1").arg(c.label);
}

QString TableEditModel::redo()
{
    if (m_saving || m_head >= m_history.size())
        return {};
    const Command &c = m_history[m_head++];
    run(c, true);
    noteChange();
    return tr("Redone: %1").arg(c.label);
}

void TableEditModel::run(const Command &command, bool forward)
{
    int top = -1;
    int bottom = -1;
    const auto n = command.changes.size();
    for (std::size_t k = 0; k < n; ++k)
        apply(command.changes[forward ? k : n - 1 - k], forward, &top, &bottom);
    if (top >= 0 && columnCount() > 0)
        emit dataChanged(index(top, 0), index(bottom, columnCount() - 1));
    if (n > 0) {
        const Change &first = command.changes.front();
        m_lastRow = first.isNew ? baseRows() + first.index : first.index;
        m_lastRow = std::min(m_lastRow, std::max(rowCount() - 1, 0));
    }
}

void TableEditModel::apply(const Change &change, bool forward, int *top, int *bottom)
{
    const RowDelta &from = forward ? change.before : change.after;
    const RowDelta &to = forward ? change.after : change.before;
    if (!change.isNew) {
        if (to.plain())
            m_rows.erase(change.index);
        else
            m_rows[change.index] = to;
        *top = *top < 0 ? change.index : std::min(*top, change.index);
        *bottom = std::max(*bottom, change.index);
        return;
    }
    const int row = baseRows() + change.index;
    const auto at = m_new.begin() + change.index;
    if (from.present && !to.present) {
        beginRemoveRows({}, row, row);
        m_new.erase(at);
        endRemoveRows();
        emit rowsChanged();
    } else if (!from.present && to.present) {
        beginInsertRows({}, row, row);
        m_new.insert(at, to);
        endInsertRows();
        emit rowsChanged();
    } else {
        m_new[static_cast<std::size_t>(change.index)] = to;
        *top = *top < 0 ? row : std::min(*top, row);
        *bottom = std::max(*bottom, row);
    }
}

void TableEditModel::noteChange()
{
    emit pendingChanged();
}

void TableEditModel::discardAll()
{
    if (m_saving || (m_rows.empty() && m_new.empty() && m_history.empty()))
        return;
    if (!m_new.empty()) {
        const int first = baseRows();
        beginRemoveRows({}, first, first + static_cast<int>(m_new.size()) - 1);
        m_new.clear();
        endRemoveRows();
        emit rowsChanged();
    }
    if (!m_rows.empty()) {
        const int top = m_rows.begin()->first;
        const int bottom = m_rows.rbegin()->first;
        m_rows.clear();
        if (columnCount() > 0)
            emit dataChanged(index(top, 0), index(bottom, columnCount() - 1));
    }
    m_history.clear();
    m_head = 0;
    m_lastRow = -1;
    emit pendingChanged();
}

// ---- copy --------------------------------------------------------------------------------------

QString TableEditModel::copyText(int top, int left, int bottom, int right, bool withHeader) const
{
    if (m_rows.empty() && m_new.empty())
        return m_base->copyText(top, left, bottom, right, withHeader);
    top = std::max(top, 0);
    left = std::max(left, 0);
    bottom = std::min(bottom, rowCount() - 1);
    right = std::min(right, columnCount() - 1);
    if (static_cast<qint64>(bottom - top + 1) * (right - left + 1)
        > ResultTableModel::kMaxCopyCells)
        return {};
    QString out;
    if (withHeader && left <= right) {
        for (int c = left; c <= right; ++c) {
            if (c > left)
                out += QLatin1Char('\t');
            out += oneLine(m_base->columnName(c));
        }
        if (top <= bottom)
            out += QLatin1Char('\n');
    }
    for (int r = top; r <= bottom; ++r) {
        for (int c = left; c <= right; ++c) {
            if (c > left)
                out += QLatin1Char('\t');
            const Value v = valueOf(r, c);
            out += v.null ? QString() : oneLine(v.text);
        }
        if (r < bottom)
            out += QLatin1Char('\n');
    }
    return out;
}

bool TableEditModel::copyToClipboard(int top, int left, int bottom, int right,
                                     bool withHeader) const
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    const qint64 height = std::max(0, std::min(bottom, rowCount() - 1) - std::max(top, 0) + 1);
    const qint64 width = std::max(0, std::min(right, columnCount() - 1) - std::max(left, 0) + 1);
    if (clipboard == nullptr || height * width > ResultTableModel::kMaxCopyCells)
        return false;
    clipboard->setText(copyText(top, left, bottom, right, withHeader));
    return true;
}

} // namespace quarry::ui
