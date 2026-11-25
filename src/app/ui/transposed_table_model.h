#pragma once

#include <QAbstractTableModel>
#include <QPointer>
#include <QtQml/qqmlregistration.h>

namespace quarry::ui {

// Transposed read-only projection over a contiguous range of rows [startRow..endRow] from a
// ResultTableModel or TableEditModel: source columns become rows (fields) and each selected source
// row becomes a column. Virtualized and zero-copy so even wide tables or large selections stay
// responsive.
class TransposedTableModel : public QAbstractTableModel
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QAbstractItemModel *source READ source WRITE setSource NOTIFY sourceChanged FINAL)
    Q_PROPERTY(int startRow READ startRow WRITE setStartRow NOTIFY rangeChanged FINAL)
    Q_PROPERTY(int endRow READ endRow WRITE setEndRow NOTIFY rangeChanged FINAL)
    Q_PROPERTY(int rows READ rows NOTIFY layoutUpdated FINAL)
    Q_PROPERTY(int columns READ columns NOTIFY layoutUpdated FINAL)

public:
    explicit TransposedTableModel(QObject *parent = nullptr);

    QAbstractItemModel *source() const { return m_source; }
    void setSource(QAbstractItemModel *source);

    int startRow() const { return m_startRow; }
    void setStartRow(int row);

    int endRow() const { return m_endRow; }
    void setEndRow(int row);

    Q_INVOKABLE void setRange(int startRow, int endRow);

    int rows() const { return rowCount(); }
    int columns() const { return columnCount(); }

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Sample character width for a value column (one selected source row), capped like
    // ResultTableModel.
    Q_INVOKABLE int sampleChars(int column) const;
    // TSV copy of [top..bottom] x [left..right] (inclusive, clamped to the model).
    Q_INVOKABLE QString copyText(int top, int left, int bottom, int right) const;
    Q_INVOKABLE bool copyToClipboard(int top, int left, int bottom, int right) const;

signals:
    void sourceChanged();
    void rangeChanged();
    void layoutUpdated();

private:
    void reconnectSource();
    void resetFromSource();
    void onSourceDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight,
                             const QList<int> &roles);
    int clampedCount() const;

    QPointer<QAbstractItemModel> m_source;
    int m_startRow = 0;
    int m_endRow = -1;
};

} // namespace quarry::ui
