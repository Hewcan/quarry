#include "sqlite_explain.h"

#include <QtTest>

using namespace quarry;
using namespace quarry::sqlite;

namespace {

// One EXPLAIN QUERY PLAN row: id, parent, notused, detail.
QString row(int id, int parent, const QString &detail)
{
    return QStringLiteral("%1\t%2\t0\t%3").arg(id).arg(parent).arg(detail);
}

} // namespace

class TstSqliteExplain : public QObject
{
    Q_OBJECT

private slots:
    void searchWithIndex();
    void scanWarnsOnlyWithoutIndex();
    void joinWithSeveralTopLevelSteps();
    void nestedStepsBuildATree();
    void tempBtreeWarns();
    void windowsLineEndingsAndBlankLines();
    void analyzeModeOnlySetsTheFlag();
    void malformedInputIsAnError();
    void depthLimit();
    void nodeLimitTruncatesBreadthFirst();
    void topLevelFloodDoesNotCountTheHiddenRoot();
};

void TstSqliteExplain::searchWithIndex()
{
    const QString text = row(3, 0, QStringLiteral("SEARCH t USING INDEX idx_a (a=?)"));
    const auto result = parseExplainPlan(text, ExplainMode::Plan);
    QVERIFY(result);
    const ExplainPlan &plan = result.value();
    QCOMPARE(plan.engine, QStringLiteral("SQLite"));
    QCOMPARE(plan.rawText, text);
    QVERIFY(!plan.analyzed);
    QVERIFY(!plan.truncated);
    QCOMPARE(plan.nodeCount, qsizetype(1));
    QCOMPARE(plan.root.label, QStringLiteral("SEARCH t USING INDEX idx_a (a=?)"));
    QVERIFY(plan.root.children.empty());
    QVERIFY(plan.root.warnings.isEmpty());
    QVERIFY(!plan.root.totalCost && !plan.root.startupCost && !plan.root.estimatedRows);
    QVERIFY(!plan.root.actualRows && !plan.root.actualTimeMs && !plan.root.loops);
    QVERIFY(!plan.planningTimeMs && !plan.executionTimeMs);
}

void TstSqliteExplain::scanWarnsOnlyWithoutIndex()
{
    const QStringList warned = { QStringLiteral("SCAN t"), QStringLiteral("SCAN TABLE t"),
                                 QStringLiteral("SCAN orders AS o") };
    for (const QString &detail : warned) {
        const auto result = parseExplainPlan(row(2, 0, detail), ExplainMode::Plan);
        QVERIFY(result);
        QCOMPARE(result.value().root.warnings, QStringList { QStringLiteral("Full table scan") });
    }
    const QStringList quiet = { QStringLiteral("SCAN t USING COVERING INDEX idx_a"),
                                QStringLiteral("SCAN CONSTANT ROW"),
                                QStringLiteral("SEARCH t USING INTEGER PRIMARY KEY (rowid=?)") };
    for (const QString &detail : quiet) {
        const auto result = parseExplainPlan(row(2, 0, detail), ExplainMode::Plan);
        QVERIFY(result);
        QVERIFY2(result.value().root.warnings.isEmpty(), qPrintable(detail));
    }
}

void TstSqliteExplain::joinWithSeveralTopLevelSteps()
{
    const QString text = row(2, 0, QStringLiteral("SCAN o")) + QLatin1Char('\n')
        + row(5, 0, QStringLiteral("SEARCH c USING INTEGER PRIMARY KEY (rowid=?)"));
    const auto result = parseExplainPlan(text, ExplainMode::Plan);
    QVERIFY(result);
    const ExplainPlan &plan = result.value();
    QVERIFY(plan.root.label.isEmpty()); // hidden wrapper
    QCOMPARE(plan.root.children.size(), size_t(2));
    QCOMPARE(plan.root.children[0].label, QStringLiteral("SCAN o"));
    QCOMPARE(plan.root.children[1].label,
             QStringLiteral("SEARCH c USING INTEGER PRIMARY KEY (rowid=?)"));
    QCOMPARE(plan.nodeCount, qsizetype(2)); // the wrapper is not a node
}

void TstSqliteExplain::nestedStepsBuildATree()
{
    const QString text = row(2, 0, QStringLiteral("MULTI-INDEX OR")) + QLatin1Char('\n')
        + row(3, 2, QStringLiteral("INDEX 1")) + QLatin1Char('\n')
        + row(4, 3, QStringLiteral("SEARCH t USING INDEX i (a=?)")) + QLatin1Char('\n')
        + row(5, 2, QStringLiteral("INDEX 2"));
    const auto result = parseExplainPlan(text, ExplainMode::Plan);
    QVERIFY(result);
    const ExplainPlan &plan = result.value();
    QCOMPARE(plan.root.label, QStringLiteral("MULTI-INDEX OR")); // single top level: no wrapper
    QCOMPARE(plan.nodeCount, qsizetype(4));
    QCOMPARE(plan.root.children.size(), size_t(2));
    QCOMPARE(plan.root.children[0].label, QStringLiteral("INDEX 1"));
    QCOMPARE(plan.root.children[0].children.size(), size_t(1));
    QCOMPARE(plan.root.children[0].children[0].label,
             QStringLiteral("SEARCH t USING INDEX i (a=?)"));
    QCOMPARE(plan.root.children[1].label, QStringLiteral("INDEX 2"));
    QVERIFY(plan.root.children[1].children.empty());

    // A co-routine and the scan that reads it are two top-level steps.
    const QString sub = row(3, 0, QStringLiteral("CO-ROUTINE sub")) + QLatin1Char('\n')
        + row(5, 3, QStringLiteral("SCAN t")) + QLatin1Char('\n')
        + row(9, 0, QStringLiteral("SCAN sub"));
    const auto second = parseExplainPlan(sub, ExplainMode::Plan);
    QVERIFY(second);
    QVERIFY(second.value().root.label.isEmpty());
    QCOMPARE(second.value().root.children.size(), size_t(2));
    QCOMPARE(second.value().root.children[0].children.size(), size_t(1));
    QCOMPARE(second.value().root.children[0].children[0].label, QStringLiteral("SCAN t"));
    QCOMPARE(second.value().nodeCount, qsizetype(3));
}

void TstSqliteExplain::tempBtreeWarns()
{
    const QString text = row(2, 0, QStringLiteral("SCAN t")) + QLatin1Char('\n')
        + row(7, 0, QStringLiteral("USE TEMP B-TREE FOR ORDER BY"));
    const auto result = parseExplainPlan(text, ExplainMode::Plan);
    QVERIFY(result);
    const auto &steps = result.value().root.children;
    QCOMPARE(steps.size(), size_t(2));
    QCOMPARE(steps[0].warnings, QStringList { QStringLiteral("Full table scan") });
    QCOMPARE(steps[1].warnings, QStringList { QStringLiteral("Uses a temporary b-tree") });
}

void TstSqliteExplain::windowsLineEndingsAndBlankLines()
{
    const QString text = QStringLiteral("2\t0\t0\tSCAN a\r\n\r\n3\t2\t0\tSEARCH b\r\n\n");
    const auto result = parseExplainPlan(text, ExplainMode::Plan);
    QVERIFY(result);
    QCOMPARE(result.value().root.label, QStringLiteral("SCAN a"));
    QCOMPARE(result.value().root.children.size(), size_t(1));
    QCOMPARE(result.value().root.children[0].label, QStringLiteral("SEARCH b"));
    QCOMPARE(result.value().rawText, text);
}

void TstSqliteExplain::analyzeModeOnlySetsTheFlag()
{
    const auto result = parseExplainPlan(row(2, 0, QStringLiteral("SCAN t")), ExplainMode::Analyze);
    QVERIFY(result);
    QVERIFY(result.value().analyzed);
}

void TstSqliteExplain::malformedInputIsAnError()
{
    const QStringList bad = {
        QString(),
        QStringLiteral("   \n\t\n"),
        QStringLiteral("not a plan"),
        QStringLiteral("a\tb\tc\tSCAN t"),
        QStringLiteral("2\t0\tSCAN t"), // a cell is missing
        QStringLiteral("2\t9\t0\tSCAN t"), // parent that does not exist
    };
    for (const QString &text : bad) {
        const auto result = parseExplainPlan(text, ExplainMode::Plan);
        QVERIFY2(!result, qPrintable(text));
        QCOMPARE(result.error().kind, ErrorKind::Syntax);
        QVERIFY(!result.error().message.isEmpty());
    }
    // The reason goes in `detail`, the message stays plain.
    const auto result = parseExplainPlan(QStringLiteral("2\t9\t0\tSCAN t"), ExplainMode::Plan);
    QVERIFY(result.error().detail.contains(QLatin1String("parent")));
}

void TstSqliteExplain::depthLimit()
{
    const auto chain = [](int levels) {
        QStringList rows;
        for (int i = 1; i <= levels; ++i)
            rows.append(row(i, i - 1, QStringLiteral("step %1").arg(i)));
        return rows.join(QLatin1Char('\n'));
    };
    const auto ok = parseExplainPlan(chain(200), ExplainMode::Plan);
    QVERIFY(ok);
    QCOMPARE(ok.value().nodeCount, qsizetype(200));

    const auto deep = parseExplainPlan(chain(201), ExplainMode::Plan);
    QVERIFY(!deep);
    QCOMPARE(deep.error().kind, ErrorKind::Syntax);
}

void TstSqliteExplain::nodeLimitTruncatesBreadthFirst()
{
    // One step with 6000 children: only its first 4999 children fit with it in 5000 nodes.
    QStringList rows { row(1, 0, QStringLiteral("root")) };
    for (int i = 2; i <= 6000; ++i)
        rows.append(row(i, 1, QStringLiteral("child %1").arg(i)));
    const auto result = parseExplainPlan(rows.join(QLatin1Char('\n')), ExplainMode::Plan);
    QVERIFY(result);
    const ExplainPlan &plan = result.value();
    QVERIFY(plan.truncated);
    QCOMPARE(plan.nodeCount, kMaxExplainNodes);
    QCOMPARE(plan.root.children.size(), size_t(kMaxExplainNodes - 1));
    QCOMPARE(plan.root.children.front().label, QStringLiteral("child 2"));
}

void TstSqliteExplain::topLevelFloodDoesNotCountTheHiddenRoot()
{
    QStringList rows;
    for (int i = 1; i <= 6000; ++i)
        rows.append(row(i, 0, QStringLiteral("SCAN t%1").arg(i)));
    const auto result = parseExplainPlan(rows.join(QLatin1Char('\n')), ExplainMode::Plan);
    QVERIFY(result);
    const ExplainPlan &plan = result.value();
    QVERIFY(plan.truncated);
    QVERIFY(plan.root.label.isEmpty());
    QCOMPARE(plan.nodeCount, kMaxExplainNodes);
    QCOMPARE(plan.root.children.size(), size_t(kMaxExplainNodes));

    // Exactly at the limit: nothing is dropped.
    QStringList exact;
    for (int i = 1; i <= kMaxExplainNodes; ++i)
        exact.append(row(i, 0, QStringLiteral("SCAN t%1").arg(i)));
    const auto fits = parseExplainPlan(exact.join(QLatin1Char('\n')), ExplainMode::Plan);
    QVERIFY(fits);
    QVERIFY(!fits.value().truncated);
    QCOMPARE(fits.value().nodeCount, kMaxExplainNodes);
}

QTEST_GUILESS_MAIN(TstSqliteExplain)
#include "tst_sqlite_explain.moc"
