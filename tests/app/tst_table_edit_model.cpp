#include "ui/table_edit_model.h"

#include <QAbstractItemModelTester>
#include <QClipboard>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QtTest>

#include <memory>

using namespace quarry;
using namespace quarry::ui;

namespace {

// id (Int64, the primary key), name (Text, NULL on row 3), data (Bytes). With `hidden` the
// result starts with a hidden "rowid" column, as the select of a table without primary key does.
std::vector<ColumnInfo> fixtureColumns(bool hidden)
{
    std::vector<ColumnInfo> columns;
    if (hidden)
        columns.push_back(
            { QStringLiteral("rowid"), ValueType::Int64, QStringLiteral("integer"), false, {} });
    columns.push_back(
        { QStringLiteral("id"), ValueType::Int64, QStringLiteral("integer"), false, {} });
    columns.push_back(
        { QStringLiteral("name"), ValueType::Text, QStringLiteral("text"), true, {} });
    columns.push_back(
        { QStringLiteral("data"), ValueType::Bytes, QStringLiteral("blob"), true, {} });
    return columns;
}

std::shared_ptr<const ResultBatch> fixtureBatch(int first, int count, bool hidden)
{
    auto batch = std::make_shared<ResultBatch>();
    batch->rowCount = count;
    batch->columns.resize(hidden ? 4 : 3);
    for (int i = 0; i < count; ++i) {
        const int id = first + i;
        size_t c = 0;
        if (hidden)
            batch->columns[c++].append(1000 + static_cast<int64_t>(id));
        batch->columns[c++].append(static_cast<int64_t>(id));
        if (id == 3)
            batch->columns[c++].appendNull();
        else
            batch->columns[c++].appendBytes(QByteArray("name ") + QByteArray::number(id),
                                            ValueType::Text);
        batch->columns[c++].appendBytes(QByteArray("\x01\x02", 2), ValueType::Bytes);
    }
    return batch;
}

TableEditModel::Config pkConfig()
{
    TableEditModel::Config c;
    c.table
        = ObjectPath { { QStringLiteral("main"), QStringLiteral("people") }, ObjectKind::Table };
    c.keyKind = RowKey::Kind::PrimaryKey;
    c.keyColumns = { QStringLiteral("id") };
    return c;
}

} // namespace

// TableEditModel (H2): the deltas over a ResultTableModel, undo/redo, the RowEdits built from
// them and the choice of key.
class TstTableEditModel : public QObject
{
    Q_OBJECT

private:
    struct Fixture
    {
        ResultTableModel base;
        TableEditModel edits { &base };
    };

    static void load(Fixture &f, const TableEditModel::Config &config, int rows = 5,
                     bool hidden = false)
    {
        f.edits.configure(config);
        f.base.begin(fixtureColumns(hidden));
        f.base.append(fixtureBatch(0, rows, hidden));
        f.base.finishStream(true);
    }

    static QString text(const TableEditModel &m, int row, int column)
    {
        return m.data(m.index(row, column), Qt::DisplayRole).toString();
    }

private slots:
    void modelStaysValid()
    {
        Fixture f;
        QAbstractItemModelTester tester(&f.edits,
                                        QAbstractItemModelTester::FailureReportingMode::Fatal);
        load(f, pkConfig());
        QVERIFY(f.edits.editable());
        QCOMPARE(f.edits.rowCount(), 5);
        QCOMPARE(f.edits.columnCount(), 3);
        QVERIFY(f.edits.setCell(1, 1, QStringLiteral("Zed")));
        QCOMPARE(f.edits.insertRow(), 5);
        QVERIFY(f.edits.setCell(5, 1, QStringLiteral("Fresh")));
        QCOMPARE(f.edits.deleteRows(0, 2), 3);
        f.edits.undo();
        f.edits.undo();
        f.edits.redo();
        f.edits.discardAll();
        QCOMPARE(f.edits.rowCount(), 5);
        // More rows arriving from the stream while a new row exists: the new row moves down.
        QCOMPARE(f.edits.insertRow(), 5);
        f.base.finishStream(false);
        f.base.append(fixtureBatch(5, 3, false));
        f.base.finishStream(true);
        QCOMPARE(f.edits.rowCount(), 9);
        QCOMPARE(f.edits.data(f.edits.index(8, 0), TableEditModel::RowStateRole).toInt(),
                 int(TableEditModel::RowNew));
        // A new select drops everything.
        f.base.begin(fixtureColumns(false));
        QCOMPARE(f.edits.rowCount(), 0);
        QCOMPARE(f.edits.pendingCount(), 0);
    }

    void editingACellMarksItAndRevertingClearsIt()
    {
        Fixture f;
        load(f, pkConfig());
        QCOMPARE(text(f.edits, 1, 1), QStringLiteral("name 1"));
        QVERIFY(f.edits.setCell(1, 1, QStringLiteral("Zed")));
        QCOMPARE(text(f.edits, 1, 1), QStringLiteral("Zed"));
        QCOMPARE(f.edits.data(f.edits.index(1, 1), TableEditModel::CellStateRole).toInt(),
                 int(TableEditModel::CellChanged));
        QCOMPARE(f.edits.pendingCount(), 1);
        QCOMPARE(f.edits.pendingText(), QStringLiteral("1 pending change"));
        QCOMPARE(f.edits.cellStateText(1, 1), QStringLiteral("modified"));
        // Back to the original text: nothing is pending any more.
        QVERIFY(f.edits.setCell(1, 1, QStringLiteral("name 1")));
        QCOMPARE(f.edits.pendingCount(), 0);
        QCOMPARE(f.edits.pendingText(), QStringLiteral("No pending changes"));
        // NULL, and an empty string, are different things.
        QVERIFY(f.edits.setNull(1, 1));
        QCOMPARE(text(f.edits, 1, 1), QStringLiteral("NULL"));
        QVERIFY(f.edits.data(f.edits.index(1, 1), TableEditModel::IsNullRole).toBool());
        QVERIFY(f.edits.setCell(2, 1, QString()));
        std::vector<int> rows;
        const auto edits = f.edits.buildEdits(&rows);
        QCOMPARE(edits.size(), size_t(2));
        QVERIFY(edits[0].cells[0].value.isNull());
        QVERIFY(!edits[1].cells[0].value.isNull()); // "" is not NULL
        QCOMPARE(edits[1].cells[0].value.toString(), QString());
    }

    void undoAndRedoWorkPerAction()
    {
        Fixture f;
        load(f, pkConfig());
        QVERIFY(!f.edits.canUndo());
        QCOMPARE(f.edits.undo(), QString());
        f.edits.setCell(0, 1, QStringLiteral("A"));
        f.edits.insertRow();
        QCOMPARE(f.edits.deleteRows(1, 3), 3); // one step for three rows
        QCOMPARE(f.edits.pendingCount(), 5); // 1 edit + 3 deleted + 1 new
        QCOMPARE(f.edits.undo(), QStringLiteral("Undone: deletion of 3 rows"));
        QCOMPARE(f.edits.pendingCount(), 2);
        QCOMPARE(f.edits.undo(), QStringLiteral("Undone: insert of a new row"));
        QCOMPARE(f.edits.rowCount(), 5);
        QVERIFY(f.edits.canRedo());
        QCOMPARE(f.edits.redo(), QStringLiteral("Redone: insert of a new row"));
        QCOMPARE(f.edits.rowCount(), 6);
        // A new action drops what could be redone.
        f.edits.undo();
        f.edits.setCell(4, 1, QStringLiteral("B"));
        QVERIFY(!f.edits.canRedo());
        QCOMPARE(f.edits.undo(), QStringLiteral("Undone: edit of name, Row 5"));
        QCOMPARE(f.edits.undo(), QStringLiteral("Undone: edit of name, Row 1"));
        QCOMPARE(f.edits.pendingCount(), 0);
    }

    void insertThenDeleteCancelsAndEditThenDeleteIsJustTheDelete()
    {
        Fixture f;
        load(f, pkConfig());
        const int created = f.edits.insertRow();
        f.edits.setCell(created, 1, QStringLiteral("x"));
        QCOMPARE(f.edits.pendingCount(), 1);
        QCOMPARE(f.edits.deleteRows(created, created), 1);
        QCOMPARE(f.edits.pendingCount(), 0); // cancels out
        QCOMPARE(f.edits.rowCount(), 5);
        f.edits.undo(); // the row comes back with its text
        QCOMPARE(text(f.edits, created, 1), QStringLiteral("x"));

        f.edits.discardAll();
        f.edits.setCell(2, 1, QStringLiteral("edited"));
        QCOMPARE(f.edits.deleteRows(2, 2), 1);
        std::vector<int> rows;
        const auto edits = f.edits.buildEdits(&rows);
        QCOMPARE(edits.size(), size_t(1));
        QCOMPARE(edits[0].kind, RowEdit::Kind::Delete);
        QVERIFY(edits[0].cells.empty());
        QCOMPARE(f.edits.rowStateText(2), QStringLiteral("Deleted"));
        QVERIFY(!f.edits.blockReason(2, 1).isEmpty()); // a deleted row is not editable
        f.edits.undo(); // the edit is back
        QCOMPARE(text(f.edits, 2, 1), QStringLiteral("edited"));
        QCOMPARE(f.edits.rowStateText(2), QStringLiteral("Modified"));
        QCOMPARE(f.edits.deleteRows(2, 2), 1);
        QCOMPARE(f.edits.deleteRows(2, 2), 0); // already deleted
    }

    void buildEditsAndTheRowOfEachEdit()
    {
        Fixture f;
        load(f, pkConfig());
        f.edits.insertRow(); // row 5
        f.edits.setCell(5, 1, QStringLiteral("new"));
        f.edits.setCell(4, 1, QStringLiteral("upd"));
        f.edits.deleteRows(1, 1);
        f.edits.setNull(0, 1);
        std::vector<int> rows;
        const auto edits = f.edits.buildEdits(&rows);
        // Updates and deletes by base row, then the inserts.
        QCOMPARE(edits.size(), size_t(4));
        QCOMPARE(rows, (std::vector<int> { 0, 1, 4, 5 }));
        QCOMPARE(edits[0].kind, RowEdit::Kind::Update);
        QCOMPARE(edits[0].key, QVariantList { QStringLiteral("0") });
        QVERIFY(edits[0].cells[0].value.isNull());
        QCOMPARE(edits[0].cells[0].column, 1);
        QCOMPARE(edits[1].kind, RowEdit::Kind::Delete);
        QCOMPARE(edits[1].key, QVariantList { QStringLiteral("1") });
        QCOMPARE(edits[2].key, QVariantList { QStringLiteral("4") });
        QCOMPARE(edits[3].kind, RowEdit::Kind::Insert);
        QVERIFY(edits[3].key.isEmpty());
        QCOMPARE(edits[3].cells[0].value.toString(), QStringLiteral("new"));
        QCOMPARE(f.edits.rowLabel(5), QStringLiteral("New row 1"));
        QCOMPARE(f.edits.rowLabel(4), QStringLiteral("Row 5"));
        const EditTarget t = f.edits.target();
        QCOMPARE(
            t.columns,
            (QStringList { QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("data") }));
        QCOMPARE(t.key.columns, QStringList { QStringLiteral("id") });
    }

    void primaryKeyMustBeInTheResult()
    {
        Fixture f;
        TableEditModel::Config c = pkConfig();
        c.keyColumns = { QStringLiteral("missing") };
        load(f, c);
        QVERIFY(!f.edits.editable());
        QVERIFY(!f.edits.readOnlyReason().isEmpty());
        QCOMPARE(f.edits.insertRow(), -1);
        QVERIFY(!f.edits.setCell(0, 1, QStringLiteral("x")));
    }

    void rowIdentifierKeyComesFromTheHiddenColumn()
    {
        Fixture f;
        TableEditModel::Config c = pkConfig();
        c.keyKind = RowKey::Kind::RowIdentifier;
        c.keyColumns = { QStringLiteral("rowid") };
        c.warning = QStringLiteral("no primary key");
        load(f, c, 5, true);
        QVERIFY(f.edits.editable());
        QCOMPARE(f.edits.columnCount(), 3); // the hidden column is not shown
        QCOMPARE(f.base.columnName(0), QStringLiteral("id"));
        QCOMPARE(f.edits.keyWarning(), QStringLiteral("no primary key"));
        f.edits.setCell(2, 1, QStringLiteral("x"));
        std::vector<int> rows;
        const auto edits = f.edits.buildEdits(&rows);
        QCOMPARE(edits.size(), size_t(1));
        QCOMPARE(edits[0].key, QVariantList { QStringLiteral("1002") });
        QCOMPARE(int(f.edits.target().columns.size()), 3); // column indexes skip the hidden one
        QCOMPARE(f.edits.target().key.kind, RowKey::Kind::RowIdentifier);
    }

    void viewsAndBinaryCellsAreReadOnly()
    {
        Fixture view;
        TableEditModel::Config c = pkConfig();
        c.table.kind = ObjectKind::View;
        c.readOnlyReason = QStringLiteral("This is a view, so it is read-only.");
        load(view, c);
        QVERIFY(!view.edits.editable());
        QCOMPARE(view.edits.blockReason(0, 1), c.readOnlyReason);
        QCOMPARE(view.edits.deleteRows(0, 0), -1);

        Fixture f;
        load(f, pkConfig());
        QCOMPARE(f.edits.blockReason(0, 2), QStringLiteral("Binary values cannot be edited."));
        QVERIFY(!f.edits.setCell(0, 2, QStringLiteral("x")));
        QVERIFY(f.edits.blockReason(0, 1).isEmpty());
    }

    void deleteRangeIsClampedToTheRows()
    {
        Fixture f;
        load(f, pkConfig(), 10);
        QCOMPARE(f.edits.deleteRows(0, TableEditModel::kMaxDeleteRows + 5),
                 10); // clamped to the rows
        f.edits.discardAll();
        QCOMPARE(f.edits.pendingCount(), 0);
    }

    void copyUsesThePendingValues()
    {
        Fixture f;
        load(f, pkConfig());
        QCOMPARE(f.edits.copyText(0, 0, 0, 1, false), QStringLiteral("0\tname 0"));
        f.edits.setCell(0, 1, QStringLiteral("a\tb"));
        f.edits.setNull(1, 1);
        QCOMPARE(f.edits.copyText(0, 0, 1, 1, true), QStringLiteral("id\tname\n0\ta b\n1\t"));
    }

    void everyMutatorIsRefusedWhileSaving()
    {
        Fixture f;
        load(f, pkConfig());
        f.edits.setCell(0, 1, QStringLiteral("A"));
        f.edits.insertRow();
        QCOMPARE(f.edits.pendingCount(), 2);
        QSignalSpy spy(&f.edits, &TableEditModel::savingChanged);
        f.edits.setSaving(true);
        QCOMPARE(spy.count(), 1);
        QVERIFY(f.edits.saving());
        QVERIFY(!f.edits.setCell(1, 1, QStringLiteral("B")));
        QVERIFY(!f.edits.setNull(1, 1));
        QCOMPARE(f.edits.insertRow(), -1);
        QCOMPARE(f.edits.deleteRows(1, 2), -1);
        QCOMPARE(f.edits.undo(), QString());
        QCOMPARE(f.edits.redo(), QString());
        f.edits.discardAll();
        QCOMPARE(f.edits.pendingCount(), 2);
        QCOMPARE(f.edits.rowCount(), 6);
        QCOMPARE(f.edits.blockReason(0, 1), QStringLiteral("Saving changes…"));
        f.edits.setSaving(false);
        QVERIFY(f.edits.setCell(1, 1, QStringLiteral("B")));
        QCOMPARE(f.edits.pendingCount(), 3);
        f.edits.discardAll();
        QCOMPARE(f.edits.pendingCount(), 0);
    }

    void emptyEditsBuildNothing()
    {
        Fixture f;
        load(f, pkConfig());
        std::vector<int> rows;
        QVERIFY(f.edits.buildEdits(&rows).empty());
        QVERIFY(rows.empty());
        QVERIFY(!QVariant(QStringLiteral("")).isNull()); // what the builder relies on
    }
};

QTEST_MAIN(TstTableEditModel)
#include "tst_table_edit_model.moc"
