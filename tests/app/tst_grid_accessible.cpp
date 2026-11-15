#include "ui/result_table_model.h"

#include <QAccessible>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest>

#include <memory>
#include <private/qaccessiblequickitem_p.h>

using namespace quarry;
using namespace quarry::ui;

// ARCHITECTURE section 13, spike plan A: can a QQuickItem (the QML grid) expose a custom
// QAccessibleInterface that implements QAccessibleTableInterface / QAccessibleTableCellInterface?
//
// What this test shows: yes. An interface factory installed by the application after Qt Quick's
// (the last installed is asked first) returns, for an item that declares `quarryTable: true`, a
// subclass of QAccessibleQuickItem (the interface Qt Quick would have made, so the name, state,
// parent and actions of the QML item stay) that reports the Table role and implements the table
// interface. Cells are lazily created interfaces (role Cell, name = the text, column header cells =
// the header of its column), so a bridge can ask "row 3, column 2" without enumerating a million
// rows.
//
// What it costs: QAccessibleQuickItem is a private Qt header (exported, and the Qt version is
// pinned); a proxy that only uses public API cannot work, because asking Qt for the interface to
// forward to registers it for the object, and QAccessibleCache asserts on a second registration.
// The factory needs the item to say it is a table in its declaration: Qt Quick asks for the
// interface while the item is still being created.
//
// What it does not show: that NVDA, VoiceOver or Orca speak the header and the value when the
// focus moves (needs the manual pass; the focus events on the cells are the missing piece), nor how
// the bridges cope with childCount() = rows * columns. So the grid ships plan B (an announcement
// per cell move) and this stays a test; the findings are in the report and in docs/UX.md section 7.

namespace {

class GridTable;

class CellInterface final : public QAccessibleInterface, public QAccessibleTableCellInterface
{
public:
    CellInterface(GridTable *table, int row, int column)
        : m_table(table)
        , m_row(row)
        , m_column(column)
    { }

    bool isValid() const override { return m_table != nullptr; }
    QObject *object() const override { return nullptr; }
    QAccessibleInterface *childAt(int, int) const override { return nullptr; }
    QAccessibleInterface *parent() const override;
    QAccessibleInterface *child(int) const override { return nullptr; }
    int childCount() const override { return 0; }
    int indexOfChild(const QAccessibleInterface *) const override { return -1; }
    QString text(QAccessible::Text t) const override;
    void setText(QAccessible::Text, const QString &) override { }
    QRect rect() const override { return {}; }
    QAccessible::Role role() const override
    {
        return m_row < 0 ? QAccessible::ColumnHeader : QAccessible::Cell;
    }
    QAccessible::State state() const override
    {
        QAccessible::State s;
        s.focusable = m_row >= 0;
        s.selectable = m_row >= 0;
        return s;
    }
    void *interface_cast(QAccessible::InterfaceType type) override
    {
        return type == QAccessible::TableCellInterface
            ? static_cast<QAccessibleTableCellInterface *>(this)
            : nullptr;
    }

    bool isSelected() const override { return false; }
    QList<QAccessibleInterface *> columnHeaderCells() const override;
    QList<QAccessibleInterface *> rowHeaderCells() const override { return {}; }
    int columnIndex() const override { return m_column; }
    int rowIndex() const override { return m_row; }
    int columnExtent() const override { return 1; }
    int rowExtent() const override { return 1; }
    QAccessibleInterface *table() const override { return parent(); }

private:
    GridTable *m_table;
    int m_row;
    int m_column;
};

class GridTable final : public QAccessibleQuickItem, public QAccessibleTableInterface
{
public:
    explicit GridTable(QQuickItem *item)
        : QAccessibleQuickItem(item)
    { }

    ~GridTable() override
    {
        for (const QAccessible::Id id : std::as_const(m_cells))
            QAccessible::deleteAccessibleInterface(id);
    }

    QAccessible::Role role() const override { return QAccessible::Table; }
    QAccessibleInterface *child(int index) const override
    {
        const int columns = mdl()->columns();
        return columns > 0 ? cellAt(index / columns, index % columns) : nullptr;
    }
    int childCount() const override { return mdl()->rows() * mdl()->columns(); }
    int indexOfChild(const QAccessibleInterface *c) const override
    {
        const auto *cell = static_cast<const CellInterface *>(c);
        return cell->rowIndex() < 0 ? -1
                                    : cell->rowIndex() * mdl()->columns() + cell->columnIndex();
    }
    void *interface_cast(QAccessible::InterfaceType type) override
    {
        if (type == QAccessible::TableInterface)
            return static_cast<QAccessibleTableInterface *>(this);
        return QAccessibleQuickItem::interface_cast(type);
    }

    // Read when asked: the QML property is assigned after the item (and its interface) exist.
    ResultTableModel *mdl() const
    {
        static ResultTableModel none;
        auto *model = qobject_cast<ResultTableModel *>(
            qvariant_cast<QObject *>(object()->property("quarryModel")));
        return model != nullptr ? model : &none;
    }
    QAccessibleInterface *self() const { return QAccessible::queryAccessibleInterface(object()); }

    // The header cells have row -1; every cell is made once and kept until the table goes.
    QAccessibleInterface *cellAt(int row, int column) const override
    {
        if (row < -1 || row >= mdl()->rows() || column < 0 || column >= mdl()->columns())
            return nullptr;
        const qint64 key = (static_cast<qint64>(row) + 1) * mdl()->columns() + column;
        QAccessible::Id id = m_cells.value(key);
        if (id == 0) {
            id = QAccessible::registerAccessibleInterface(
                new CellInterface(const_cast<GridTable *>(this), row, column));
            m_cells.insert(key, id);
        }
        return QAccessible::accessibleInterface(id);
    }
    QAccessibleInterface *caption() const override { return nullptr; }
    QAccessibleInterface *summary() const override { return nullptr; }
    int selectedCellCount() const override { return 0; }
    QList<QAccessibleInterface *> selectedCells() const override { return {}; }
    QString columnDescription(int column) const override { return mdl()->columnName(column); }
    QString rowDescription(int) const override { return {}; }
    int selectedColumnCount() const override { return 0; }
    int selectedRowCount() const override { return 0; }
    int columnCount() const override { return mdl()->columns(); }
    int rowCount() const override { return mdl()->rows(); }
    QList<int> selectedColumns() const override { return {}; }
    QList<int> selectedRows() const override { return {}; }
    bool isColumnSelected(int) const override { return false; }
    bool isRowSelected(int) const override { return false; }
    bool selectRow(int) override { return false; }
    bool selectColumn(int) override { return false; }
    bool unselectRow(int) override { return false; }
    bool unselectColumn(int) override { return false; }
    void modelChange(QAccessibleTableModelChangeEvent *) override { }

private:
    mutable QHash<qint64, QAccessible::Id> m_cells;
};

QAccessibleInterface *CellInterface::parent() const
{
    return m_table != nullptr ? m_table->self() : nullptr;
}

QString CellInterface::text(QAccessible::Text t) const
{
    if (m_row < 0)
        return t == QAccessible::Name ? m_table->mdl()->columnName(m_column) : QString();
    if (t == QAccessible::Name)
        return m_table->mdl()->cellText(m_row, m_column);
    if (t == QAccessible::Description)
        return m_table->mdl()->columnName(m_column);
    return {};
}

QList<QAccessibleInterface *> CellInterface::columnHeaderCells() const
{
    return { m_table->cellAt(-1, m_column) };
}

// Asked before Qt Quick's factory: an item that says it is a table gets the table interface.
QAccessibleInterface *tableFactory(const QString &, QObject *object)
{
    auto *item = qobject_cast<QQuickItem *>(object);
    return item != nullptr && item->property("quarryTable").toBool() ? new GridTable(item)
                                                                     : nullptr;
}

// An item the way the grid is declared in QML: a role and a name from Accessible. `quarryTable` is
// a literal in the declaration, because Qt Quick asks for the interface while the item is still
// being created (before any later assignment); the model is read by the interface when needed.
QQuickItem *makeItem(QQmlEngine &engine, ResultTableModel *model, const QString &name,
                     QQuickWindow &window)
{
    QQmlComponent component(&engine);
    component.setData(("import QtQuick\n"
                       "Item { property bool quarryTable: "
                       + QByteArray(model != nullptr ? "true" : "false")
                       + "; property QtObject quarryModel: null;"
                         " Accessible.role: Accessible.Pane; Accessible.name: \""
                       + name.toUtf8() + "\" }"),
                      QUrl());
    auto *item = qobject_cast<QQuickItem *>(component.create());
    if (item == nullptr)
        return nullptr;
    item->setProperty("quarryModel", QVariant::fromValue<QObject *>(model));
    item->setParent(&window);
    item->setParentItem(window.contentItem());
    return item;
}

std::shared_ptr<const ResultBatch> batch(int first, int count)
{
    auto b = std::make_shared<ResultBatch>();
    b->rowCount = count;
    b->columns.resize(2);
    for (int i = 0; i < count; ++i) {
        b->columns[0].append(static_cast<int64_t>(first) + i);
        if (i % 2 == 0)
            b->columns[1].appendNull();
        else
            b->columns[1].appendBytes("v" + QByteArray::number(first + i), ValueType::Text);
    }
    return b;
}

} // namespace

class TstGridAccessible : public QObject
{
    Q_OBJECT

private slots:
    void aFactoryCanMakeAQuickItemATable()
    {
        // Qt Quick installs its own factory when its types are registered (the first QML engine);
        // ours must come after it, since the last installed is asked first.
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nItem {}", QUrl());
        std::unique_ptr<QObject> warm(component.create());
        QVERIFY(warm != nullptr);
        QAccessible::installFactory(&tableFactory);
        QAccessible::setActive(true);
        QQuickWindow window;
        window.resize(300, 200);
        window.show();
        ResultTableModel model;
        model.begin(
            { { QStringLiteral("id"), ValueType::Int64, QStringLiteral("integer"), false, {} },
              { QStringLiteral("name"), ValueType::Text, QStringLiteral("text"), true, {} } });
        model.append(batch(10, 6));
        QTRY_COMPARE(model.rows(), 6);

        // An ordinary item stays what Qt Quick makes it.
        QQuickItem *plain = makeItem(engine, nullptr, QStringLiteral("Plain"), window);
        QAccessibleInterface *plainIface = QAccessible::queryAccessibleInterface(plain);
        QVERIFY(plainIface != nullptr);
        QCOMPARE(plainIface->tableInterface(), nullptr);
        QCOMPARE(plainIface->role(), QAccessible::Pane);

        QQuickItem *grid = makeItem(engine, &model, QStringLiteral("Results grid"), window);
        QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(grid);
        QVERIFY(iface != nullptr);
        QCOMPARE(iface->role(), QAccessible::Table);
        QCOMPARE(iface->text(QAccessible::Name), QStringLiteral("Results grid")); // forwarded
        QAccessibleTableInterface *table = iface->tableInterface();
        QVERIFY(table != nullptr);
        QCOMPARE(table->rowCount(), 6);
        QCOMPARE(table->columnCount(), 2);
        QCOMPARE(table->columnDescription(1), QStringLiteral("name"));
        QCOMPARE(iface->childCount(), 12);
        // The parent is the window's tree, as for any Qt Quick item.
        QVERIFY(iface->parent() != nullptr);

        // Row 3, column 0: the value, its column header, its position.
        QAccessibleInterface *cell = table->cellAt(3, 0);
        QVERIFY(cell != nullptr);
        QCOMPARE(cell->role(), QAccessible::Cell);
        QCOMPARE(cell->text(QAccessible::Name), QStringLiteral("13"));
        QAccessibleTableCellInterface *cellInfo = cell->tableCellInterface();
        QVERIFY(cellInfo != nullptr);
        QCOMPARE(cellInfo->rowIndex(), 3);
        QCOMPARE(cellInfo->columnIndex(), 0);
        QCOMPARE(cellInfo->table(), iface);
        QCOMPARE(cell->parent(), iface);
        QCOMPARE(cellInfo->columnHeaderCells().size(), 1);
        QAccessibleInterface *header = cellInfo->columnHeaderCells().at(0);
        QCOMPARE(header->role(), QAccessible::ColumnHeader);
        QCOMPARE(header->text(QAccessible::Name), QStringLiteral("id"));
        // A NULL is the word, like the grid paints it.
        QCOMPARE(table->cellAt(2, 1)->text(QAccessible::Name), QStringLiteral("NULL"));
        QCOMPARE(table->cellAt(3, 1)->text(QAccessible::Name), QStringLiteral("v13"));
        QCOMPARE(table->cellAt(3, 1)->text(QAccessible::Description), QStringLiteral("name"));
        // The same cell is the same interface, and an index maps back to its cell.
        QCOMPARE(table->cellAt(3, 0), cell);
        QCOMPARE(iface->child(7), table->cellAt(3, 1));
        QCOMPARE(iface->indexOfChild(table->cellAt(3, 1)), 7);
        QVERIFY(table->cellAt(6, 0) == nullptr);
        QVERIFY(table->cellAt(0, 2) == nullptr);

        // 1 000 000 rows: nothing is enumerated or created until a bridge asks for a cell.
        ResultTableModel big;
        big.begin({ { QStringLiteral("a"), ValueType::Int64, {}, false, {} },
                    { QStringLiteral("b"), ValueType::Text, {}, true, {} } });
        for (int i = 0; i < 200; ++i)
            big.append(batch(i * 5000, 5000));
        big.finishStream(true);
        QTRY_COMPARE_WITH_TIMEOUT(big.rows(), 1'000'000, 20000);
        QQuickItem *bigGrid = makeItem(engine, &big, QStringLiteral("Big grid"), window);
        QAccessibleTableInterface *bigTable
            = QAccessible::queryAccessibleInterface(bigGrid)->tableInterface();
        QCOMPARE(bigTable->rowCount(), 1'000'000);
        QCOMPARE(bigTable->cellAt(999'999, 0)->text(QAccessible::Name), QStringLiteral("999999"));
    }
};

QTEST_MAIN(TstGridAccessible)
#include "tst_grid_accessible.moc"
