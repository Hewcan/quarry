#include "result_table_model.h"

#include <quarry/cell_formatter.h>

#include <QClipboard>
#include <QGuiApplication>

#include <algorithm>

namespace quarry::ui {

ResultTableModel::ResultTableModel(QObject *parent)
    : QAbstractTableModel(parent)
{
    m_flushTimer.setSingleShot(true);
    connect(&m_flushTimer, &QTimer::timeout, this, &ResultTableModel::flush);
}

int ResultTableModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows);
}

int ResultTableModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : columns();
}

// Binary search over the first row of each batch: the cumulative row index.
const ResultTableModel::Chunk *ResultTableModel::chunkOf(qsizetype row) const
{
    if (row < 0 || row >= m_rows)
        return nullptr;
    const auto it = std::ranges::upper_bound(m_chunks, row, {}, &Chunk::start);
    return it == m_chunks.begin() ? nullptr : &*std::prev(it);
}

QVariant ResultTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.column() >= columnCount())
        return {};
    if (role == CellStateRole || role == RowStateRole)
        return 0;
    if (role != Qt::DisplayRole && role != IsNullRole)
        return {};
    const Chunk *chunk = chunkOf(index.row());
    const auto source = sourceIndex(index.column());
    if (chunk == nullptr || source >= chunk->batch->columns.size())
        return {};
    const ColumnBuffer &buffer = chunk->batch->columns[source];
    const qsizetype row = index.row() - chunk->start;
    if (role == IsNullRole)
        return CellFormatter::isNull(buffer, row);
    return CellFormatter::display(buffer, row);
}

QVariant ResultTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole && section >= 0
        && section < columnCount())
        return m_columns[sourceIndex(section)].name;
    return QAbstractTableModel::headerData(section, orientation, role);
}

QHash<int, QByteArray> ResultTableModel::roleNames() const
{
    QHash<int, QByteArray> roles = QAbstractTableModel::roleNames();
    roles.insert(IsNullRole, "isNull");
    roles.insert(CellStateRole, "cellState");
    roles.insert(RowStateRole, "rowState");
    return roles;
}

QString ResultTableModel::columnName(int column) const
{
    return column >= 0 && column < columnCount() ? m_columns[sourceIndex(column)].name : QString();
}

QString ResultTableModel::nativeType(int column) const
{
    return column >= 0 && column < columnCount() ? m_columns[sourceIndex(column)].nativeType
                                                 : QString();
}

int ResultTableModel::sampleChars(int column) const
{
    return column >= 0 && column < columns() ? m_sample[sourceIndex(column)] : 0;
}

QString ResultTableModel::cellText(int row, int column) const
{
    return data(index(row, column), Qt::DisplayRole).toString();
}

QString ResultTableModel::sourceText(int row, int sourceColumn) const
{
    const Chunk *chunk = chunkOf(row);
    if (chunk == nullptr || sourceColumn < 0
        || sourceColumn >= static_cast<int>(chunk->batch->columns.size()))
        return {};
    return CellFormatter::copy(chunk->batch->columns[static_cast<size_t>(sourceColumn)],
                               row - chunk->start);
}

bool ResultTableModel::isBinary(int row, int column) const
{
    const Chunk *chunk = chunkOf(row);
    const auto source = sourceIndex(column);
    if (chunk == nullptr || column < 0 || source >= chunk->batch->columns.size())
        return false;
    return m_columns[source].type == ValueType::Bytes
        || chunk->batch->columns[source].typeAt(row - chunk->start) == ValueType::Bytes;
}

bool ResultTableModel::sourceIsNull(int row, int sourceColumn) const
{
    const Chunk *chunk = chunkOf(row);
    return chunk == nullptr || sourceColumn < 0
        || sourceColumn >= static_cast<int>(chunk->batch->columns.size())
        || chunk->batch->columns[static_cast<size_t>(sourceColumn)].isNull(row - chunk->start);
}

ValueType ResultTableModel::valueTypeAt(int row, int column) const
{
    const Chunk *chunk = chunkOf(row);
    const auto source = sourceIndex(column);
    if (chunk == nullptr || column < 0 || column >= columns()
        || source >= chunk->batch->columns.size())
        return ValueType::Null;
    return chunk->batch->columns[source].typeAt(row - chunk->start);
}

ValueType ResultTableModel::columnValueType(int column) const
{
    return column >= 0 && column < columns() ? m_columns[sourceIndex(column)].type
                                             : ValueType::Other;
}

QString ResultTableModel::textAt(int row, int column, qsizetype maxChars, bool *cut) const
{
    if (cut != nullptr)
        *cut = false;
    const Chunk *chunk = chunkOf(row);
    const auto source = sourceIndex(column);
    if (chunk == nullptr || column < 0 || column >= columns()
        || source >= chunk->batch->columns.size())
        return {};
    const ColumnBuffer &buffer = chunk->batch->columns[source];
    const qsizetype r = row - chunk->start;
    switch (buffer.typeAt(r)) {
    case ValueType::Null:
    case ValueType::Bytes:
        return {};
    case ValueType::Bool:
    case ValueType::Int64:
    case ValueType::Double:
        return CellFormatter::copy(buffer, r); // small: a number
    default:
        break;
    }
    // 4 bytes per character is the UTF-8 maximum, so this window always holds `maxChars`
    // characters.
    const QByteArrayView bytes = buffer.bytesAt(r);
    const qsizetype window = maxChars * 4 + 4;
    QString text = QString::fromUtf8(bytes.first(std::min<qsizetype>(bytes.size(), window)));
    if (text.size() > maxChars || bytes.size() > window) {
        text.truncate(maxChars);
        if (cut != nullptr)
            *cut = true;
    }
    return text;
}

QByteArray ResultTableModel::bytesAt(int row, int column, qsizetype maxBytes, qint64 *total) const
{
    if (total != nullptr)
        *total = 0;
    const Chunk *chunk = chunkOf(row);
    const auto source = sourceIndex(column);
    if (chunk == nullptr || column < 0 || column >= columns()
        || source >= chunk->batch->columns.size())
        return {};
    const ColumnBuffer &buffer = chunk->batch->columns[source];
    const qsizetype r = row - chunk->start;
    if (buffer.typeAt(r) != ValueType::Bytes)
        return {};
    const QByteArrayView bytes = buffer.bytesAt(r);
    if (total != nullptr)
        *total = bytes.size();
    return bytes.first(std::min<qsizetype>(bytes.size(), maxBytes)).toByteArray();
}

// ---- stream ------------------------------------------------------------------------------------

void ResultTableModel::reset(std::vector<ColumnInfo> columns)
{
    m_flushTimer.stop();
    beginResetModel();
    ++m_generation;
    m_chunks.clear();
    m_pending.clear();
    m_pendingRows = 0;
    m_rows = 0;
    m_columns = std::move(columns);
    m_sample.assign(m_columns.size(), 0);
    for (size_t c = 0; c < m_columns.size(); ++c)
        m_sample[c]
            = static_cast<int>(std::min<qsizetype>(m_columns[c].name.size(), kSampleMaxChars));
    m_more = false;
    m_fetching = false;
    m_fetchAll = false;
    m_truncated = false;
    m_lastFlush.invalidate(); // the first rows are inserted at once
    endResetModel();
    emit columnsChanged();
    emit rowsChanged();
    emit stateChanged();
    emit sampleChanged();
}

std::vector<ColumnInfo> ResultTableModel::exportColumns() const
{
    const auto hidden = static_cast<std::ptrdiff_t>(m_hidden);
    if (hidden >= std::ssize(m_columns))
        return {};
    return { m_columns.begin() + hidden, m_columns.end() };
}

std::vector<std::shared_ptr<const ResultBatch>> ResultTableModel::exportBatches() const
{
    std::vector<std::shared_ptr<const ResultBatch>> out;
    out.reserve(m_chunks.size() + m_pending.size());
    const auto hidden = static_cast<std::ptrdiff_t>(m_hidden);
    const auto add = [&](const std::shared_ptr<const ResultBatch> &batch) {
        if (hidden == 0) {
            out.push_back(batch);
            return;
        }
        // shortcut: a table edited through its rowid copies its cells once to drop the key column;
        // Exporter could take a column offset instead if exports of big editable tables matter.
        auto cut = std::make_shared<ResultBatch>();
        cut->rowCount = batch->rowCount;
        if (hidden < std::ssize(batch->columns))
            cut->columns.assign(batch->columns.begin() + hidden, batch->columns.end());
        out.push_back(std::move(cut));
    };
    for (const Chunk &chunk : m_chunks)
        add(chunk.batch);
    for (const auto &batch : m_pending)
        add(batch);
    return out;
}

void ResultTableModel::begin(const std::vector<ColumnInfo> &columns)
{
    reset(columns);
}

void ResultTableModel::clear()
{
    if (m_columns.empty() && m_rows == 0 && m_pending.empty())
        return;
    reset({});
}

void ResultTableModel::append(std::shared_ptr<const ResultBatch> batch)
{
    if (!batch || batch->rowCount <= 0 || m_columns.empty())
        return;
    m_pendingRows += batch->rowCount;
    m_pending.push_back(std::move(batch));
    scheduleFlush();
}

void ResultTableModel::scheduleFlush()
{
    if (m_flushTimer.isActive())
        return;
    if (!m_lastFlush.isValid() || m_lastFlush.elapsed() >= kGroupMs) {
        flush();
        return;
    }
    m_flushTimer.start(static_cast<int>(kGroupMs - m_lastFlush.elapsed()));
}

void ResultTableModel::flush()
{
    m_flushTimer.stop();
    if (m_pending.empty())
        return;
    const bool first = m_rows == 0;
    const qsizetype added = m_pendingRows;
    beginInsertRows({}, static_cast<int>(m_rows), static_cast<int>(m_rows + added - 1));
    for (auto &batch : m_pending) {
        const qsizetype count = batch->rowCount;
        m_chunks.push_back({ m_rows, std::move(batch) });
        m_rows += count;
    }
    m_pending.clear();
    m_pendingRows = 0;
    endInsertRows();
    m_lastFlush.restart();
    if (first) {
        computeSample();
        emit sampleChanged();
    }
    emit rowsChanged();
}

void ResultTableModel::computeSample()
{
    if (m_chunks.empty())
        return;
    const ResultBatch &batch = *m_chunks.front().batch;
    const qsizetype rows = std::min<qsizetype>(batch.rowCount, kSampleRows);
    const size_t columns = std::min(m_columns.size(), batch.columns.size());
    for (size_t c = 0; c < columns; ++c) {
        int best = m_sample[c];
        for (qsizetype r = 0; r < rows && best < kSampleMaxChars; ++r)
            best = std::max(best,
                            static_cast<int>(CellFormatter::display(batch.columns[c], r).size()));
        m_sample[c] = std::min(best, kSampleMaxChars);
    }
}

void ResultTableModel::finishStream(bool atEnd)
{
    m_fetching = false;
    m_more = !atEnd;
    // A page ends: insert what is pending, or a view that still sees the old row count asks for
    // yet another page (and so on until the end). Fetch all keeps its 50 ms grouping.
    if (atEnd || !m_fetchAll)
        flush();
    if (atEnd) {
        m_fetchAll = false;
    } else if (m_fetchAll) {
        requestFetch(kFetchAllRows);
    }
    emit stateChanged();
}

void ResultTableModel::detachStream()
{
    if (!m_more && !m_fetching && !m_fetchAll)
        return;
    m_truncated = m_truncated || m_more;
    m_more = false;
    m_fetching = false;
    m_fetchAll = false;
    flush();
    emit stateChanged();
}

void ResultTableModel::streamLost()
{
    if (m_columns.empty())
        return;
    m_truncated = true;
    m_more = false;
    m_fetching = false;
    m_fetchAll = false;
    flush();
    emit stateChanged();
}

void ResultTableModel::fetchDeclined()
{
    if (!m_fetching && !m_fetchAll)
        return;
    m_fetching = false;
    m_fetchAll = false;
    emit stateChanged();
}

// ---- fetching ----------------------------------------------------------------------------------

bool ResultTableModel::canFetchMore(const QModelIndex &parent) const
{
    return !parent.isValid() && m_more && !m_fetching;
}

void ResultTableModel::fetchMore(const QModelIndex &parent)
{
    if (canFetchMore(parent))
        requestFetch(m_pageRows);
}

void ResultTableModel::loadMore()
{
    fetchMore({});
}

// Only flags change here; the signal to the controller is queued, as in SchemaTreeModel, because a
// view may call fetchMore() from inside one of its own signals.
void ResultTableModel::requestFetch(qsizetype rows)
{
    m_fetching = true;
    emit stateChanged();
    const quint64 generation = m_generation;
    QMetaObject::invokeMethod(
        this,
        [this, rows, generation] {
            if (generation == m_generation && m_fetching)
                emit fetchRequested(rows);
        },
        Qt::QueuedConnection);
}

void ResultTableModel::fetchAll()
{
    if (!m_more || m_fetchAll)
        return;
    m_fetchAll = true;
    if (m_fetching)
        emit stateChanged(); // the page in flight ends, and finishStream() carries on
    else
        requestFetch(kFetchAllRows);
}

void ResultTableModel::cancelFetchAll()
{
    if (!m_fetchAll)
        return;
    m_fetchAll = false;
    emit stateChanged();
}

// ---- copy --------------------------------------------------------------------------------------

namespace {

void appendField(QString &out, QString field)
{
    for (QChar &ch : field)
        if (ch == QLatin1Char('\t') || ch == QLatin1Char('\n') || ch == QLatin1Char('\r'))
            ch = QLatin1Char(' ');
    out += field;
}

} // namespace

QString ResultTableModel::copyText(int top, int left, int bottom, int right, bool withHeader) const
{
    top = std::max(top, 0);
    left = std::max(left, 0);
    bottom = std::min(bottom, rows() - 1);
    right = std::min(right, columns() - 1);
    if (static_cast<qint64>(bottom - top + 1) * (right - left + 1) > kMaxCopyCells)
        return {}; // not built: see copyToClipboard()
    QString out;
    if (withHeader && left <= right) {
        for (int c = left; c <= right; ++c) {
            if (c > left)
                out += QLatin1Char('\t');
            appendField(out, m_columns[sourceIndex(c)].name);
        }
        if (top <= bottom)
            out += QLatin1Char('\n');
    }
    for (int r = top; r <= bottom; ++r) {
        const Chunk *chunk = chunkOf(r);
        if (chunk == nullptr)
            break;
        for (int c = left; c <= right; ++c) {
            if (c > left)
                out += QLatin1Char('\t');
            if (c + m_hidden < static_cast<int>(chunk->batch->columns.size()))
                appendField(
                    out,
                    CellFormatter::copy(chunk->batch->columns[sourceIndex(c)], r - chunk->start));
        }
        if (r < bottom)
            out += QLatin1Char('\n');
    }
    return out;
}

bool ResultTableModel::copyToClipboard(int top, int left, int bottom, int right,
                                       bool withHeader) const
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    const qint64 height = std::max(0, std::min(bottom, rows() - 1) - std::max(top, 0) + 1);
    const qint64 width = std::max(0, std::min(right, columns() - 1) - std::max(left, 0) + 1);
    if (clipboard == nullptr || height * width > kMaxCopyCells)
        return false;
    clipboard->setText(copyText(top, left, bottom, right, withHeader));
    return true;
}

} // namespace quarry::ui
