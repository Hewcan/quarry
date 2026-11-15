#include "ui/result_table_model.h"

#include <QAbstractItemModelTester>
#include <QClipboard>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QtTest>

#include <memory>

using namespace quarry;
using namespace quarry::ui;

namespace {

// Columns of the fixtures: id (Int64), name (Text, NULL on every 5th row), score (Double),
// mixed (an Int64 that turns into Text on some rows, so the buffer becomes mixed).
std::vector<ColumnInfo> fixtureColumns()
{
    return { { QStringLiteral("id"), ValueType::Int64, QStringLiteral("integer"), false, {} },
             { QStringLiteral("name"), ValueType::Text, QStringLiteral("text"), true, {} },
             { QStringLiteral("score"), ValueType::Double, QStringLiteral("real"), true, {} },
             { QStringLiteral("mixed"), ValueType::Other, QStringLiteral(""), true, {} } };
}

// `count` rows whose id starts at `first`.
std::shared_ptr<const ResultBatch> fixtureBatch(int first, int count)
{
    auto batch = std::make_shared<ResultBatch>();
    batch->rowCount = count;
    batch->columns.resize(4);
    for (int i = 0; i < count; ++i) {
        const int id = first + i;
        batch->columns[0].append(static_cast<int64_t>(id));
        if (id % 5 == 4)
            batch->columns[1].appendNull();
        else
            batch->columns[1].appendBytes(QByteArray("name ") + QByteArray::number(id),
                                          ValueType::Text);
        batch->columns[2].append(id + 0.5);
        if (id % 2 == 0)
            batch->columns[3].append(static_cast<int64_t>(id));
        else
            batch->columns[3].appendBytes(QByteArray("t") + QByteArray::number(id),
                                          ValueType::Text);
    }
    return batch;
}

QString textAt(const ResultTableModel &m, int row, int column)
{
    return m.data(m.index(row, column), Qt::DisplayRole).toString();
}

} // namespace

// ResultTableModel (H1.7): batches kept as they arrive, grouped inserts, fetchMore / Fetch all,
// the NULL role, and the TSV of a selection.
class TstResultTableModel : public QObject
{
    Q_OBJECT

private slots:
    void streamingKeepsTheModelValid()
    {
        ResultTableModel m;
        QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Fatal);
        QCOMPARE(m.rowCount(), 0);
        QCOMPARE(m.columnCount(), 0);
        m.begin(fixtureColumns());
        QCOMPARE(m.columnCount(), 4);
        QCOMPARE(m.rowCount(), 0);

        QSignalSpy inserted(&m, &QAbstractItemModel::rowsInserted);
        m.append(fixtureBatch(0, 500)); // the first rows are inserted at once
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(m.rowCount(), 500);
        m.append(fixtureBatch(500, 500)); // within the group window: waits for the timer
        m.append(fixtureBatch(1000, 7));
        QCOMPARE(m.rowCount(), 500);
        QTRY_COMPARE_WITH_TIMEOUT(m.rowCount(), 1007, 2000);
        QCOMPARE(inserted.count(), 2); // the two batches arrived as one insert
        QCOMPARE(m.rows(), 1007);

        // The boundaries of the batches: the last and first rows of each, and the very last.
        for (const int row : { 0, 499, 500, 999, 1000, 1006 })
            QCOMPARE(textAt(m, row, 0), QString::number(row));
        QCOMPARE(textAt(m, 1006, 1), QStringLiteral("name 1006"));
        QCOMPARE(textAt(m, 3, 2), QStringLiteral("3.5"));
        QVERIFY(!m.data(m.index(1007, 0)).isValid()); // out of range
        QVERIFY(!m.data(m.index(0, 4)).isValid());

        QCOMPARE(m.headerData(1, Qt::Horizontal).toString(), QStringLiteral("name"));
        QCOMPARE(m.columnName(2), QStringLiteral("score"));
        QCOMPARE(m.nativeType(0), QStringLiteral("integer"));
        QCOMPARE(m.columnName(9), QString());
        QCOMPARE(m.roleNames().value(ResultTableModel::IsNullRole), QByteArray("isNull"));
    }

    void nullsMixedColumnsAndBlobs()
    {
        ResultTableModel m;
        QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Fatal);
        m.begin(fixtureColumns());
        m.append(fixtureBatch(0, 10));
        QTRY_COMPARE(m.rowCount(), 10);
        const auto isNull = [&](int r, int c) {
            return m.data(m.index(r, c), ResultTableModel::IsNullRole).toBool();
        };
        QVERIFY(isNull(4, 1));
        QVERIFY(!isNull(3, 1));
        QCOMPARE(textAt(m, 4, 1), QStringLiteral("NULL")); // painted in italics by the grid
        QVERIFY(!isNull(4, 0));
        // The mixed column: an int on the even rows, a text on the odd ones.
        QCOMPARE(textAt(m, 2, 3), QStringLiteral("2"));
        QCOMPARE(textAt(m, 3, 3), QStringLiteral("t3"));

        auto blob = std::make_shared<ResultBatch>();
        blob->rowCount = 1;
        blob->columns.resize(1);
        blob->columns[0].appendBytes(QByteArray(2400, 'x'), ValueType::Bytes);
        m.begin({ { QStringLiteral("b"), ValueType::Bytes, QStringLiteral("blob"), true, {} } });
        m.append(blob);
        QTRY_COMPARE(m.rowCount(), 1);
        QCOMPARE(textAt(m, 0, 0), QStringLiteral("<binary 2.3 KB>"));
    }

    void fetchMoreIsQueuedAndOnlyWhenThereIsMore()
    {
        ResultTableModel m;
        QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Fatal);
        QSignalSpy requested(&m, &ResultTableModel::fetchRequested);
        QVERIFY(!m.canFetchMore({})); // nothing yet
        m.begin(fixtureColumns());
        m.append(fixtureBatch(0, 500));
        QVERIFY(!m.canFetchMore({})); // the statement has not ended
        m.finishStream(false);
        QVERIFY(m.moreAvailable());
        QVERIFY(m.canFetchMore({}));
        QVERIFY(!m.canFetchMore(m.index(0, 0))); // a table has no children

        m.fetchMore({});
        QCOMPARE(requested.count(), 0); // never synchronous
        QVERIFY(m.fetching());
        QVERIFY(!m.canFetchMore({})); // one request at a time
        m.fetchMore({});
        QTRY_COMPARE(requested.count(), 1);
        QCOMPARE(requested.at(0).at(0).toLongLong(), ResultTableModel::kPageRows);

        // The page arrives; the result ends.
        m.append(fixtureBatch(500, 120));
        m.finishStream(true);
        QCOMPARE(m.rowCount(), 620); // the end flushes at once
        QVERIFY(!m.moreAvailable());
        QVERIFY(!m.canFetchMore({}));
        QVERIFY(!m.truncated());

        // The controller could not start the fetch: the model asks again later.
        m.begin(fixtureColumns());
        m.append(fixtureBatch(0, 500));
        m.finishStream(false);
        m.loadMore();
        QTRY_COMPARE(requested.count(), 2);
        m.fetchDeclined();
        QVERIFY(!m.fetching());
        QVERIFY(m.canFetchMore({}));
    }

    void aNewStatementDropsAQueuedRequest()
    {
        ResultTableModel m;
        QSignalSpy requested(&m, &ResultTableModel::fetchRequested);
        m.begin(fixtureColumns());
        m.append(fixtureBatch(0, 500));
        m.finishStream(false);
        m.fetchMore({});
        m.begin(fixtureColumns()); // before the queued request is delivered
        QTest::qWait(50);
        QCOMPARE(requested.count(), 0);
        QCOMPARE(m.rowCount(), 0);
        QVERIFY(!m.fetching());
    }

    void fetchAllChainsAndGroupsTheInserts()
    {
        ResultTableModel m;
        QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Fatal);
        QSignalSpy requested(&m, &ResultTableModel::fetchRequested);
        QSignalSpy inserted(&m, &QAbstractItemModel::rowsInserted);
        m.begin(fixtureColumns());
        m.append(fixtureBatch(0, 500));
        m.finishStream(false);
        m.fetchAll();
        QVERIFY(m.fetchingAll());
        QTRY_COMPARE(requested.count(), 1);
        QCOMPARE(requested.at(0).at(0).toLongLong(), ResultTableModel::kFetchAllRows);

        // 40 batches back to back, each ending a fetch: the model asks for the next one every
        // time, and the inserts are grouped (one at once, the rest when the window ends).
        int next = 500;
        QElapsedTimer clock;
        clock.start();
        for (int i = 0; i < 40; ++i) {
            const int before = static_cast<int>(requested.count());
            m.append(fixtureBatch(next, 500));
            next += 500;
            m.finishStream(false);
            QTRY_COMPARE(requested.count(), before + 1);
        }
        // At most one insert per group window, plus the first one and the one in progress.
        QVERIFY2(inserted.count() <= clock.elapsed() / ResultTableModel::kGroupMs + 2,
                 qPrintable(QString::number(inserted.count())));
        QVERIFY(m.fetchingAll());
        m.append(fixtureBatch(next, 10));
        m.finishStream(true); // the end flushes everything
        QVERIFY(!m.fetchingAll());
        QCOMPARE(m.rowCount(), next + 10);
        QCOMPARE(textAt(m, next + 9, 0), QString::number(next + 9));
    }

    void fetchAllCanBeStopped()
    {
        ResultTableModel m;
        QSignalSpy requested(&m, &ResultTableModel::fetchRequested);
        m.begin(fixtureColumns());
        m.append(fixtureBatch(0, 500));
        m.finishStream(false);
        m.fetchAll();
        QTRY_COMPARE(requested.count(), 1);
        m.cancelFetchAll();
        QVERIFY(!m.fetchingAll());
        m.append(fixtureBatch(500, 500));
        m.finishStream(false); // the page in flight still ends; nothing more is asked for
        QTest::qWait(50);
        QCOMPARE(requested.count(), 1);
        QVERIFY(m.moreAvailable());
        QVERIFY(m.canFetchMore({}));
        QTRY_COMPARE(m.rowCount(), 1000);
    }

    void aReplacedOrLostStreamIsTruncated()
    {
        ResultTableModel m;
        m.begin(fixtureColumns());
        m.append(fixtureBatch(0, 500));
        m.finishStream(false);
        m.detachStream(); // the next statement of the plan took the session
        QVERIFY(!m.moreAvailable());
        QVERIFY(m.truncated());
        QCOMPARE(m.rowCount(), 500);
        m.detachStream(); // idempotent

        m.begin(fixtureColumns());
        m.append(fixtureBatch(0, 500));
        m.finishStream(true);
        m.detachStream(); // a complete result stays complete
        QVERIFY(!m.truncated());

        m.begin(fixtureColumns());
        m.append(fixtureBatch(0, 500));
        m.finishStream(false);
        m.fetchAll();
        m.streamLost(); // the fetch failed
        QVERIFY(m.truncated());
        QVERIFY(!m.fetchingAll());
        QVERIFY(!m.canFetchMore({}));
    }

    void clearAndBeginResetTheModel()
    {
        ResultTableModel m;
        QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Fatal);
        m.begin(fixtureColumns());
        m.append(fixtureBatch(0, 500));
        QSignalSpy reset(&m, &QAbstractItemModel::modelReset);
        QSignalSpy rows(&m, &ResultTableModel::rowsChanged);
        m.clear();
        QCOMPARE(reset.count(), 1);
        QCOMPARE(m.rowCount(), 0);
        QCOMPARE(m.columnCount(), 0);
        QVERIFY(rows.count() >= 1);
        m.clear(); // nothing to clear: no reset
        QCOMPARE(reset.count(), 1);

        // Rows that arrive while no columns are known (late batches of an old plan) are ignored.
        m.append(fixtureBatch(0, 5));
        QCOMPARE(m.rowCount(), 0);
        // A pending group is dropped by begin().
        m.begin(fixtureColumns());
        m.append(fixtureBatch(0, 5));
        m.append(fixtureBatch(5, 5));
        m.begin(fixtureColumns());
        QTest::qWait(80);
        QCOMPARE(m.rowCount(), 0);
    }

    void sampleCharsFollowTheFirstRows()
    {
        ResultTableModel m;
        m.begin(fixtureColumns());
        QSignalSpy sample(&m, &ResultTableModel::sampleChanged);
        QCOMPARE(m.sampleChars(1), 4); // the header only
        m.append(fixtureBatch(0, 50));
        QCOMPARE(sample.count(), 1);
        QCOMPARE(m.sampleChars(1), 7); // "name 12"
        QCOMPARE(m.sampleChars(0), 2);
        QCOMPARE(m.sampleChars(99), 0);
    }

    void copyBuildsTsv()
    {
        ResultTableModel m;
        m.begin({ { QStringLiteral("a\tb"), ValueType::Int64, {}, true, {} },
                  { QStringLiteral("t"), ValueType::Text, {}, true, {} },
                  { QStringLiteral("blob"), ValueType::Bytes, {}, true, {} } });
        const auto mk = [](int first, int count) {
            auto b = std::make_shared<ResultBatch>();
            b->rowCount = count;
            b->columns.resize(3);
            for (int i = 0; i < count; ++i) {
                const int id = first + i;
                b->columns[0].append(static_cast<int64_t>(id));
                if (id == 1)
                    b->columns[1].appendNull();
                else if (id == 2)
                    b->columns[1].appendBytes(QByteArray("x\ty\nz\r\nw"), ValueType::Text);
                else
                    b->columns[1].appendBytes(QByteArray("v") + QByteArray::number(id),
                                              ValueType::Text);
                b->columns[2].appendBytes(QByteArray("\x01\xAB", 2), ValueType::Bytes);
            }
            return std::shared_ptr<const ResultBatch>(b);
        };
        m.append(mk(0, 2)); // two batches, so a selection crosses the boundary
        m.append(mk(2, 2));
        QTRY_COMPARE(m.rowCount(), 4);

        // NULL is empty; the line break and tab inside a value are spaces; no trailing newline.
        QCOMPARE(m.copyText(0, 0, 3, 1, false), QStringLiteral("0\tv0\n1\t\n2\tx y z  w\n3\tv3"));
        // With the header (its own tab is a space too).
        QCOMPARE(m.copyText(1, 1, 2, 1, true), QStringLiteral("t\n\nx y z  w"));
        QCOMPARE(m.copyText(0, 0, 0, 0, true), QStringLiteral("a b\n0"));
        QCOMPARE(m.copyText(3, 2, 3, 2, false), QStringLiteral("0x01ab")); // a blob copies as hex
        // Out of range is clamped; an empty range gives nothing.
        QCOMPARE(m.copyText(-5, -5, 99, 99, false).count(QLatin1Char('\n')), 3);
        QCOMPARE(m.copyText(2, 0, 1, 0, false), QString());
        QCOMPARE(m.copyText(0, 2, 0, 2, true), QStringLiteral("blob\n0x01ab"));
    }

    void aSelectionOverTheCapIsNotCopied()
    {
        ResultTableModel m;
        m.begin(fixtureColumns()); // 4 columns
        const int rows = static_cast<int>(ResultTableModel::kMaxCopyCells / 4) + 1;
        m.append(fixtureBatch(0, rows));
        QTRY_COMPARE(m.rowCount(), rows);
        QClipboard *clipboard = QGuiApplication::clipboard();
        const QString before = clipboard->text();
        clipboard->setText(QStringLiteral("untouched"));
        QVERIFY(m.copyText(0, 0, rows - 1, 3, false).isEmpty()); // not even built
        QVERIFY(!m.copyToClipboard(0, 0, rows - 1, 3, true));
        QCOMPARE(clipboard->text(), QStringLiteral("untouched"));
        // Exactly at the cap it is copied.
        QVERIFY(m.copyToClipboard(0, 0, rows - 2, 3, false));
        QVERIFY(clipboard->text().startsWith(QStringLiteral("0	name 0")));
        clipboard->setText(before);
    }

    void theClipboardGetsTheText()
    {
        ResultTableModel m;
        m.begin(fixtureColumns());
        m.append(fixtureBatch(0, 3));
        QTRY_COMPARE(m.rowCount(), 3);
        // The user's own clipboard is put back afterwards.
        QClipboard *clipboard = QGuiApplication::clipboard();
        const QString before = clipboard->text();
        m.copyToClipboard(0, 0, 1, 1, true);
        QCOMPARE(clipboard->text(), QStringLiteral("id\tname\n0\tname 0\n1\tname 1"));
        clipboard->setText(before);
    }
};

QTEST_MAIN(TstResultTableModel)
#include "tst_result_table_model.moc"
