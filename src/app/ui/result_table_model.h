#pragma once

#include <quarry/result_batch.h>

#include <QAbstractTableModel>
#include <QElapsedTimer>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace quarry::ui {

// The rows of the Results grid (ARCHITECTURE sections 3, 5 and 6.2). It keeps the batches as they
// arrive from the Session (shared, never copied, no QVariant per cell) with the index of the first
// row of each, and turns a cell into text only in data(), for the cells the view shows.
//
// Rows arrive through append(); they are inserted into the model in groups, at most once every
// kGroupMs, so "Fetch all" does not flood the event loop with beginInsertRows. The first rows
// after begin() are inserted at once. QueryController drives it: begin() with the columns of a
// statement, append() per batch, finishStream() when the statement ends.
class ResultTableModel : public QAbstractTableModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by QueryState")

    // Rows in the model (what the view shows), and columns.
    Q_PROPERTY(int rows READ rows NOTIFY rowsChanged FINAL)
    Q_PROPERTY(int columns READ columns NOTIFY columnsChanged FINAL)
    // The statement ended with atEnd = false: more rows exist and can be asked for.
    Q_PROPERTY(bool moreAvailable READ moreAvailable NOTIFY stateChanged FINAL)
    // A request for rows is in flight.
    Q_PROPERTY(bool fetching READ fetching NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool fetchingAll READ fetchingAll NOTIFY stateChanged FINAL)
    // Not every row of the result is here and no more can be asked for (a later statement of the
    // plan replaced the stream, or reading failed).
    Q_PROPERTY(bool truncated READ truncated NOTIFY stateChanged FINAL)
    Q_PROPERTY(qint64 maxCopyCells READ maxCopyCells CONSTANT FINAL)

public:
    enum Role {
        IsNullRole = Qt::UserRole + 1, // true for a NULL cell: the grid paints it in italics
        // Edit state of a cell and of its row (TableEditModel). Always 0 here: the grid's
        // delegate reads the same roles from either model.
        CellStateRole,
        RowStateRole,
    };
    Q_ENUM(Role)

    static constexpr int kPageRows = 500; // what the view asks for when it gets close to the end
    static constexpr int kFetchAllRows = 5000; // "Fetch all" reads in bigger steps
    static constexpr int kGroupMs = 50; // inserts are grouped at most this often
    static constexpr int kSampleRows = 500; // rows that decide the first column widths
    static constexpr int kSampleMaxChars = 60;
    static constexpr qint64 kMaxCopyCells = 100000; // a bigger selection is not copied

    explicit ResultTableModel(QObject *parent = nullptr);

    int rows() const { return static_cast<int>(m_rows); }
    int columns() const { return std::max(0, static_cast<int>(m_columns.size()) - m_hidden); }
    bool moreAvailable() const { return m_more; }
    bool fetching() const { return m_fetching; }
    bool fetchingAll() const { return m_fetchAll; }
    bool truncated() const { return m_truncated; }
    static qint64 maxCopyCells() { return kMaxCopyCells; }

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;

    // Controller side.
    // The first `count` columns of the next begin() hold the hidden row key of an editable table
    // (rowid / ctid): they are not shown, counted or copied, but sourceText() reads them.
    void setHiddenColumns(int count) { m_hidden = std::max(count, 0); }
    // A statement that returns columns: the model starts again with them.
    void begin(const std::vector<ColumnInfo> &columns);
    void clear(); // no columns, no rows
    void append(std::shared_ptr<const ResultBatch> batch);
    // The statement (or a fetch) ended; `atEnd` false means more rows can be asked for.
    void finishStream(bool atEnd);
    // The next statement of the plan replaced the stream: no more rows can be asked for.
    void detachStream();
    // Reading failed or was cancelled: what is here is all there will be.
    void streamLost();
    // The controller could not start the fetch (a plan is running): ask again later.
    void fetchDeclined();

    // Export side (Exporter::startBatches): everything received so far, the rows still waiting to
    // be inserted included, without the hidden key columns. The batches are shared, not copied,
    // unless a hidden column has to be cut out of them.
    std::vector<ColumnInfo> exportColumns() const;
    std::vector<std::shared_ptr<const ResultBatch>> exportBatches() const;
    qsizetype exportRows() const { return m_rows + m_pendingRows; }

    // View side. loadMore() is for the view when it nears the end (QML TableView does not call
    // fetchMore itself).
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE void fetchAll();
    Q_INVOKABLE void cancelFetchAll();
    Q_INVOKABLE QString columnName(int column) const;
    Q_INVOKABLE QString nativeType(int column) const;
    // Characters the column needs to show its header and the first rows (capped), for the width.
    Q_INVOKABLE int sampleChars(int column) const;
    // The text the grid shows for a cell ("NULL" for NULL).
    Q_INVOKABLE QString cellText(int row, int column) const;
    // The whole value of a cell as text (CellFormatter::copy: NULL is empty); `column` counts the
    // hidden columns too, so 0 is the hidden key when there is one.
    QString sourceText(int row, int sourceColumn) const;
    // The cell holds binary data (not editable as text). `column` is a shown column.
    bool isBinary(int row, int column) const;
    bool sourceIsNull(int row, int sourceColumn) const;
    // TSV of the cells [top..bottom] x [left..right] (inclusive, clamped to the model), rows
    // separated by "\n". NULL is empty, tabs and line breaks inside a value become spaces. With
    // `withHeader` the first line holds the column names.
    Q_INVOKABLE QString copyText(int top, int left, int bottom, int right, bool withHeader) const;
    // The same, put on the system clipboard.
    // false (and the clipboard untouched) when the range has more than kMaxCopyCells cells.
    Q_INVOKABLE bool copyToClipboard(int top, int left, int bottom, int right,
                                     bool withHeader) const;

signals:
    void rowsChanged();
    void columnsChanged();
    void stateChanged();
    // The first rows arrived: widths from sampleChars() can be computed.
    void sampleChanged();
    // Ask the session for `rows` more rows. Queued: never emitted from inside fetchMore().
    void fetchRequested(qsizetype rows);

private:
    struct Chunk
    {
        qsizetype start = 0; // first row of the batch in the model
        std::shared_ptr<const ResultBatch> batch;
    };

    // Index of a shown column among all the columns (the hidden ones come first).
    size_t sourceIndex(int column) const
    {
        return static_cast<size_t>(column) + static_cast<size_t>(m_hidden);
    }
    const Chunk *chunkOf(qsizetype row) const;
    void requestFetch(qsizetype rows);
    void flush();
    void scheduleFlush();
    void computeSample();
    void reset(std::vector<ColumnInfo> columns);

    std::vector<ColumnInfo> m_columns;
    std::vector<Chunk> m_chunks;
    qsizetype m_rows = 0;
    std::vector<std::shared_ptr<const ResultBatch>> m_pending;
    qsizetype m_pendingRows = 0;
    QTimer m_flushTimer;
    QElapsedTimer m_lastFlush;
    std::vector<int> m_sample;
    bool m_more = false;
    bool m_fetching = false;
    bool m_fetchAll = false;
    bool m_truncated = false;
    int m_hidden = 0;
    quint64 m_generation = 0; // bumped by every reset: a queued fetch request of an older one dies
};

} // namespace quarry::ui
