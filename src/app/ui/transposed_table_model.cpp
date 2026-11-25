#include "transposed_table_model.h"

#include "result_table_model.h"

#include <QClipboard>
#include <QGuiApplication>

#include <algorithm>

namespace quarry::ui {

TransposedTableModel::TransposedTableModel(QObject *parent)
    : QAbstractTableModel(parent)
{ }

void TransposedTableModel::setSource(QAbstractItemModel *source)
{
    if (m_source == source)
        return;
    if (m_source)
        disconnect(m_source, nullptr, this, nullptr);
    beginResetModel();
    m_source = source;
    reconnectSource();
    endResetModel();
    emit sourceChanged();
    emit layoutUpdated();
}

void TransposedTableModel::setStartRow(int row)
{
    if (m_startRow == row)
        return;
    beginResetModel();
    m_startRow = row;
    endResetModel();
    emit rangeChanged();
    emit layoutUpdated();
}

void TransposedTableModel::setEndRow(int row)
{
    if (m_endRow == row)
        return;
    beginResetModel();
    m_endRow = row;
    endResetModel();
    emit rangeChanged();
    emit layoutUpdated();
}

void TransposedTableModel::setRange(int startRow, int endRow)
{
    if (m_startRow == startRow && m_endRow == endRow)
        return;
    beginResetModel();
    m_startRow = startRow;
    m_endRow = endRow;
    endResetModel();
    emit rangeChanged();
    emit layoutUpdated();
}

void TransposedTableModel::reconnectSource()
{
    if (!m_source)
        return;
    connect(m_source, &QAbstractItemModel::dataChanged, this,
            &TransposedTableModel::onSourceDataChanged);
    connect(m_source, &QAbstractItemModel::modelReset, this,
            &TransposedTableModel::resetFromSource);
    connect(m_source, &QAbstractItemModel::rowsInserted, this,
            &TransposedTableModel::resetFromSource);
    connect(m_source, &QAbstractItemModel::rowsRemoved, this,
            &TransposedTableModel::resetFromSource);
    connect(m_source, &QAbstractItemModel::columnsInserted, this,
            &TransposedTableModel::resetFromSource);
    connect(m_source, &QAbstractItemModel::columnsRemoved, this,
            &TransposedTableModel::resetFromSource);
}

void TransposedTableModel::resetFromSource()
{
    beginResetModel();
    endResetModel();
    emit layoutUpdated();
}

void TransposedTableModel::onSourceDataChanged(const QModelIndex &topLeft,
                                               const QModelIndex &bottomRight,
                                               const QList<int> &roles)
{
    const int cols = clampedCount();
    if (cols <= 0 || !topLeft.isValid() || !bottomRight.isValid())
        return;
    const int firstSrcRow = std::max(topLeft.row(), m_startRow);
    const int lastSrcRow = std::min(bottomRight.row(), m_startRow + cols - 1);
    if (firstSrcRow > lastSrcRow)
        return;
    const QModelIndex tl = index(topLeft.column(), firstSrcRow - m_startRow);
    const QModelIndex br = index(bottomRight.column(), lastSrcRow - m_startRow);
    emit dataChanged(tl, br, roles);
}

int TransposedTableModel::clampedCount() const
{
    if (!m_source)
        return 0;
    const int srcRows = m_source->rowCount();
    if (srcRows <= 0 || m_startRow < 0 || m_startRow >= srcRows || m_endRow < m_startRow)
        return 0;
    const int end = std::min(m_endRow, srcRows - 1);
    return end - m_startRow + 1;
}

int TransposedTableModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() || !m_source)
        return 0;
    return m_source->columnCount();
}

int TransposedTableModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return clampedCount();
}

QVariant TransposedTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || !m_source)
        return {};
    const int srcRow = m_startRow + index.column();
    const int srcCol = index.row();
    if (srcRow < 0 || srcRow >= m_source->rowCount() || srcCol < 0
        || srcCol >= m_source->columnCount())
        return {};
    return m_source->data(m_source->index(srcRow, srcCol), role);
}

QHash<int, QByteArray> TransposedTableModel::roleNames() const
{
    QHash<int, QByteArray> roles = QAbstractTableModel::roleNames();
    roles.insert(ResultTableModel::IsNullRole, "isNull");
    roles.insert(ResultTableModel::CellStateRole, "cellState");
    roles.insert(ResultTableModel::RowStateRole, "rowState");
    return roles;
}

int TransposedTableModel::sampleChars(int column) const
{
    if (!m_source || column < 0 || column >= columnCount())
        return 0;
    const int srcRow = m_startRow + column;
    const int fields = std::min(rowCount(), 200);
    int best = 8;
    for (int f = 0; f < fields && best < ResultTableModel::kSampleMaxChars; ++f) {
        const QString text = m_source->data(m_source->index(srcRow, f), Qt::DisplayRole).toString();
        best = std::max(best, static_cast<int>(text.size()));
    }
    return std::min(best, ResultTableModel::kSampleMaxChars);
}

namespace {

void appendTransposedField(QString &out, QString field)
{
    for (QChar &ch : field)
        if (ch == QLatin1Char('\t') || ch == QLatin1Char('\n') || ch == QLatin1Char('\r'))
            ch = QLatin1Char(' ');
    out += field;
}

} // namespace

QString TransposedTableModel::copyText(int top, int left, int bottom, int right) const
{
    const int rCount = rowCount();
    const int cCount = columnCount();
    if (!m_source || rCount <= 0 || cCount <= 0)
        return {};
    top = std::clamp(top, 0, rCount - 1);
    left = std::clamp(left, 0, cCount - 1);
    bottom = std::clamp(bottom, top, rCount - 1);
    right = std::clamp(right, left, cCount - 1);
    QString out;
    for (int r = top; r <= bottom; ++r) {
        if (r > top)
            out += QLatin1Char('\n');
        for (int c = left; c <= right; ++c) {
            if (c > left)
                out += QLatin1Char('\t');
            const QModelIndex idx = m_source->index(m_startRow + c, r);
            if (!m_source->data(idx, ResultTableModel::IsNullRole).toBool())
                appendTransposedField(out, m_source->data(idx, Qt::DisplayRole).toString());
        }
    }
    return out;
}

bool TransposedTableModel::copyToClipboard(int top, int left, int bottom, int right) const
{
    const qint64 cells = static_cast<qint64>(bottom - top + 1) * (right - left + 1);
    if (cells <= 0 || cells > ResultTableModel::kMaxCopyCells)
        return false;
    if (QClipboard *cb = QGuiApplication::clipboard())
        cb->setText(copyText(top, left, bottom, right));
    return true;
}

} // namespace quarry::ui
