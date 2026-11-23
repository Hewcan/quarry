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
    // While the table is read again the old rows are on show and nothing can change them; the
    // swap to the new rows ends it.
    void nothingChangesWhileTheRowsAreReplaced()
    {
        Fixture f;
        QAbstractItemModelTester tester(&f.edits,
                                        QAbstractItemModelTester::FailureReportingMode::Fatal);
        load(f, pkConfig());
        QVERIFY(f.edits.editable());
        QVERIFY(f.edits.blockReason(1, 1).isEmpty());
        QSignalSpy reloadingSpy(&f.edits, &TableEditModel::reloadingChanged);
        f.base.markStale();
        QVERIFY(f.edits.reloading());
        QCOMPARE(reloadingSpy.count(), 1);
        QVERIFY(f.edits.blockReason(1, 1).contains(QStringLiteral("read again")));
        QVERIFY(!f.edits.setCell(1, 1, QStringLiteral("x")));
        QVERIFY(!f.edits.setNull(1, 1));
        QCOMPARE(f.edits.insertRow(), -1);
        QCOMPARE(f.edits.duplicateRow(1), -1);
        QCOMPARE(f.edits.deleteRows(0, 1), -1);
        QCOMPARE(f.edits.pendingCount(), 0);
        // The old rows are still readable (a failed read leaves them).
        QCOMPARE(f.edits.rowCount(), 5);
        QCOMPARE(text(f.edits, 1, 1), QStringLiteral("name 1"));
        f.base.abandonStale();
        QVERIFY(!f.edits.reloading());
        QVERIFY(f.edits.setCell(1, 1, QStringLiteral("x")));

        // The new rows replace them and edits work again.
        f.edits.discardAll();
        f.base.markStale();
        f.base.begin(fixtureColumns(false));
        f.base.append(fixtureBatch(10, 2, false));
        QVERIFY(!f.edits.reloading());
        QCOMPARE(f.edits.rowCount(), 2);
        QVERIFY(f.edits.setCell(0, 1, QStringLiteral("y")));
    }

    // The record panel (H11 phase 3): the fields of a row with their pending values, NULL, the
    // viewer a value gets, the size of a BLOB, the cap on what is read, and Copy of the whole
    // value.
    void recordFieldsShowTheRowWithItsPendingValues()
    {
        Fixture f;
        QAbstractItemModelTester tester(&f.edits,
                                        QAbstractItemModelTester::FailureReportingMode::Fatal);
        load(f, pkConfig());
        QVariantList fields = f.edits.recordFields(1);
        const auto field = [&](int i) { return fields.at(i).toMap(); };
        QCOMPARE(fields.size(), 3);
        QCOMPARE(field(0).value(QStringLiteral("name")).toString(), QStringLiteral("id"));
        QCOMPARE(field(0).value(QStringLiteral("type")).toString(), QStringLiteral("integer"));
        QCOMPARE(field(0).value(QStringLiteral("text")).toString(), QStringLiteral("1"));
        QCOMPARE(field(0).value(QStringLiteral("kind")).toString(), QString());
        QVERIFY(!field(0).value(QStringLiteral("isNull")).toBool());
        QCOMPARE(field(0).value(QStringLiteral("state")).toString(), QString());
        QCOMPARE(field(1).value(QStringLiteral("text")).toString(), QStringLiteral("name 1"));
        QCOMPARE(field(2).value(QStringLiteral("kind")).toString(), QStringLiteral("blob"));
        QCOMPARE(field(2).value(QStringLiteral("size")).toLongLong(), qint64(2));
        QCOMPARE(field(1).value(QStringLiteral("size")).toLongLong(), qint64(-1));

        // NULL.
        fields = f.edits.recordFields(3);
        QVERIFY(field(1).value(QStringLiteral("isNull")).toBool());
        QCOMPARE(field(1).value(QStringLiteral("text")).toString(), QString());
        QCOMPARE(field(1).value(QStringLiteral("kind")).toString(), QString());

        // A pending change shows its value and says "modified".
        QVERIFY(f.edits.setCell(1, 1, QStringLiteral("Zed")));
        fields = f.edits.recordFields(1);
        QCOMPARE(field(1).value(QStringLiteral("text")).toString(), QStringLiteral("Zed"));
        QCOMPARE(field(1).value(QStringLiteral("state")).toString(), QStringLiteral("modified"));
        QCOMPARE(field(0).value(QStringLiteral("state")).toString(), QString());

        // A new row has NULL fields and says so; no row gives no fields.
        const int added = f.edits.insertRow();
        fields = f.edits.recordFields(added);
        QCOMPARE(fields.size(), 3);
        QVERIFY(field(1).value(QStringLiteral("isNull")).toBool());
        QCOMPARE(field(1).value(QStringLiteral("state")).toString(), QStringLiteral("new row"));
        QVERIFY(f.edits.recordFields(99).isEmpty());
        QVERIFY(f.edits.recordFields(-1).isEmpty());

        // A long JSON value: the list shows the first kRecordChars, the viewer all of it, indented.
        const QString json
            = QStringLiteral("{\"a\":\"") + QString(3000, QLatin1Char('x')) + QStringLiteral("\"}");
        QVERIFY(f.edits.setCell(2, 1, json));
        fields = f.edits.recordFields(2);
        QCOMPARE(field(1).value(QStringLiteral("kind")).toString(), QStringLiteral("json"));
        QCOMPARE(field(1).value(QStringLiteral("text")).toString().size(),
                 TableEditModel::kRecordChars);
        QVERIFY(field(1).value(QStringLiteral("cut")).toBool());
        const QVariantMap view = f.edits.fieldView(2, 1);
        QVERIFY(view.value(QStringLiteral("ok")).toBool());
        QCOMPARE(view.value(QStringLiteral("kind")).toString(), QStringLiteral("json"));
        QVERIFY(view.value(QStringLiteral("text"))
                    .toString()
                    .startsWith(QLatin1String("{\n  \"a\": \"xxx")));
        QVERIFY(view.value(QStringLiteral("text")).toString().size() > 3000);
        QCOMPARE(view.value(QStringLiteral("name")).toString(), QStringLiteral("name"));

        // Text with line breaks gets the text viewer; a short plain value none.
        QVERIFY(f.edits.setCell(0, 1, QStringLiteral("one\ntwo")));
        fields = f.edits.recordFields(0);
        QCOMPARE(field(1).value(QStringLiteral("kind")).toString(), QStringLiteral("text"));
        QCOMPARE(f.edits.fieldView(0, 1).value(QStringLiteral("text")).toString(),
                 QStringLiteral("one\ntwo"));

        // A BLOB: a hex dump and its size. NULL: said, not formatted.
        const QVariantMap blob = f.edits.fieldView(1, 2);
        QCOMPARE(blob.value(QStringLiteral("kind")).toString(), QStringLiteral("blob"));
        QVERIFY2(blob.value(QStringLiteral("text"))
                     .toString()
                     .startsWith(QLatin1String("00000000  01 02")),
                 qPrintable(blob.value(QStringLiteral("text")).toString()));
        QCOMPARE(blob.value(QStringLiteral("size")).toString(), QStringLiteral("2 bytes"));
        QVERIFY(blob.value(QStringLiteral("notice")).toString().isEmpty());
        const QVariantMap none = f.edits.fieldView(3, 1);
        QVERIFY(none.value(QStringLiteral("notice")).toString().contains(QLatin1String("NULL")));
        QVERIFY(f.edits.fieldView(99, 0).value(QStringLiteral("kind")).toString().isEmpty());

        // Copy takes the whole value: the text, an empty string for NULL, 0x... for a BLOB.
        QVERIFY(f.edits.copyField(2, 1));
        QCOMPARE(QGuiApplication::clipboard()->text(), json);
        QVERIFY(f.edits.copyField(3, 1));
        QCOMPARE(QGuiApplication::clipboard()->text(), QString());
        QVERIFY(f.edits.copyField(1, 2));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("0x0102"));
        QVERIFY(!f.edits.copyField(99, 0));
        QVERIFY(f.edits.isNullAt(3, 1));
        QVERIFY(!f.edits.isNullAt(1, 1));
        QVERIFY(f.edits.isNullAt(99, 0));
    }

    // What the base model reads for the panel never copies a huge value whole.
    void theBaseModelReadsAtMostWhatItIsAskedFor()
    {
        Fixture f;
        load(f, pkConfig());
        bool cut = false;
        QCOMPARE(f.base.textAt(1, 1, 3, &cut), QStringLiteral("nam"));
        QVERIFY(cut);
        QCOMPARE(f.base.textAt(1, 1, 100, &cut), QStringLiteral("name 1"));
        QVERIFY(!cut);
        QCOMPARE(f.base.textAt(1, 0, 100, &cut), QStringLiteral("1"));
        QCOMPARE(f.base.textAt(3, 1, 100, &cut), QString()); // NULL
        QCOMPARE(f.base.valueTypeAt(1, 1), ValueType::Text);
        QCOMPARE(f.base.valueTypeAt(3, 1), ValueType::Null);
        QCOMPARE(f.base.valueTypeAt(1, 2), ValueType::Bytes);
        qint64 total = 0;
        QCOMPARE(f.base.bytesAt(1, 2, 1, &total), QByteArray("\x01", 1));
        QCOMPARE(total, qint64(2));
        QCOMPARE(f.base.bytesAt(1, 1, 10, &total), QByteArray()); // not a BLOB
        QCOMPARE(f.base.columnValueType(2), ValueType::Bytes);
        QCOMPARE(f.base.valueTypeAt(99, 0), ValueType::Null);
    }

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
        QCOMPARE(f.edits.duplicateRow(1), -1);
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

    // A copy of a row: a new pending row with the same values, undoable like insertRow.
    void duplicateRowCopiesTheValuesExceptThePrimaryKey()
    {
        Fixture f;
        QAbstractItemModelTester tester(&f.edits,
                                        QAbstractItemModelTester::FailureReportingMode::Fatal);
        load(f, pkConfig());
        f.edits.setCell(1, 1, QStringLiteral("edited"));
        QCOMPARE(f.edits.duplicateRow(1), 5);
        QCOMPARE(f.edits.rowCount(), 6);
        QCOMPARE(f.edits.rowStateText(5), QStringLiteral("New"));
        QCOMPARE(f.edits.pendingCount(), 2); // the edit and the new row
        QCOMPARE(text(f.edits, 5, 1), QStringLiteral("edited")); // the pending value is copied
        QCOMPARE(text(f.edits, 5, 0), QStringLiteral("DEFAULT")); // the key is left to the server
        QCOMPARE(text(f.edits, 5, 2), QStringLiteral("DEFAULT")); // binary: not copied
        std::vector<int> rows;
        const auto edits = f.edits.buildEdits(&rows);
        QCOMPARE(edits.back().kind, RowEdit::Kind::Insert);
        QCOMPARE(edits.back().cells.size(), size_t(1));
        QCOMPARE(edits.back().cells[0].value.toString(), QStringLiteral("edited"));

        // A NULL is copied as NULL; a new row copies only the cells that were set.
        QCOMPARE(f.edits.duplicateRow(3), 6);
        QCOMPARE(text(f.edits, 6, 1), QStringLiteral("NULL"));
        QCOMPARE(f.edits.duplicateRow(5), 7);
        QCOMPARE(text(f.edits, 7, 1), QStringLiteral("edited"));
        QCOMPARE(text(f.edits, 7, 0), QStringLiteral("DEFAULT"));

        // One undo step per copy, announced with the row it came from.
        QCOMPARE(f.edits.undo(), QStringLiteral("Undone: duplicate of New row 1"));
        QCOMPARE(f.edits.rowCount(), 7);
        QCOMPARE(f.edits.redo(), QStringLiteral("Redone: duplicate of New row 1"));
        QCOMPARE(f.edits.rowCount(), 8);

        // Refused: no such row, a deleted row.
        QCOMPARE(f.edits.duplicateRow(-1), -1);
        QCOMPARE(f.edits.duplicateRow(99), -1);
        QCOMPARE(f.edits.deleteRows(0, 0), 1);
        QCOMPARE(f.edits.duplicateRow(0), -1);
        QCOMPARE(f.edits.rowCount(), 8);
    }

    // With a row identifier (no primary key) the key is hidden, so every value is copied.
    void duplicateRowWithARowIdentifierCopiesEverythingVisible()
    {
        Fixture f;
        TableEditModel::Config config = pkConfig();
        config.keyKind = RowKey::Kind::RowIdentifier;
        config.keyColumns = { QStringLiteral("rowid") };
        load(f, config, 5, true);
        QCOMPARE(f.edits.duplicateRow(2), 5);
        QCOMPARE(text(f.edits, 5, 0), QStringLiteral("2"));
        QCOMPARE(text(f.edits, 5, 1), QStringLiteral("name 2"));
    }

    void duplicateRowIsRefusedWhenReadOnly()
    {
        Fixture f;
        TableEditModel::Config config = pkConfig();
        config.readOnlyReason = QStringLiteral("a view");
        load(f, config);
        QCOMPARE(f.edits.duplicateRow(0), -1);
        QCOMPARE(f.edits.rowCount(), 5);
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
