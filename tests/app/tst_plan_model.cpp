#include "ui/plan_model.h"

#include <QAbstractItemModelTester>
#include <QElapsedTimer>
#include <QLocale>
#include <QSignalSpy>
#include <QtTest>

using namespace quarry;
using namespace quarry::ui;

namespace {

ExplainNode node(const QString &label, std::optional<double> totalCost = std::nullopt,
                 std::optional<double> timeMs = std::nullopt)
{
    ExplainNode n;
    n.label = label;
    n.totalCost = totalCost;
    n.actualTimeMs = timeMs;
    return n;
}

// A plan whose top-level steps hang from the unlabelled wrapper, like SQLite's.
ExplainPlan wrapped(std::vector<ExplainNode> tops)
{
    ExplainPlan p;
    p.root.children = std::move(tops);
    p.engine = QStringLiteral("Test");
    return p;
}

QVariant role(const PlanModel &m, const QModelIndex &i, PlanModel::Role r)
{
    return m.data(i, r);
}

} // namespace

// PlanModel (H9): the tree over ExplainPlan, the shares of cost and time, the formatting and the
// states the Plan tab shows.
class TstPlanModel : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates)); }

    void statesFollowTheRequest()
    {
        PlanModel m;
        QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Fatal);
        QSignalSpy changed(&m, &PlanModel::changed);
        QVERIFY(!m.requested());
        QCOMPARE(m.state(), QStringLiteral("idle"));

        m.begin(false);
        QVERIFY(m.requested());
        QVERIFY(m.running());
        QCOMPARE(m.state(), QStringLiteral("running"));
        QVERIFY(!m.analyzed());

        m.setPlan(wrapped({ node(QStringLiteral("SCAN t")) }));
        QCOMPARE(m.state(), QStringLiteral("ready"));
        QCOMPARE(m.nodeCount(), 1);

        m.begin(true);
        QCOMPARE(m.rowCount(), 0); // the old plan goes at once
        QVERIFY(m.analyzed());
        m.setCancelled();
        QCOMPARE(m.state(), QStringLiteral("cancelled"));
        QVERIFY(m.requested());

        m.begin(false);
        m.setFailure(QStringLiteral("Could not read it"), QStringLiteral("engine text"),
                     QStringLiteral("raw output"));
        QCOMPARE(m.state(), QStringLiteral("failed"));
        QCOMPARE(m.errorMessage(), QStringLiteral("Could not read it"));
        QCOMPARE(m.errorDetail(), QStringLiteral("engine text"));
        QCOMPARE(m.rawText(), QStringLiteral("raw output"));
        QCOMPARE(m.rowCount(), 0);

        m.clear();
        QVERIFY(!m.requested());
        QCOMPARE(m.state(), QStringLiteral("idle"));
        QVERIFY(m.rawText().isEmpty());
        QVERIFY(changed.count() >= 6);
    }

    void aRefusedRequestKeepsThePlan()
    {
        PlanModel m;
        m.setPlan(wrapped({ node(QStringLiteral("SCAN t")) }));
        QSignalSpy changed(&m, &PlanModel::changed);
        m.setNotice(QStringLiteral("Not available"));
        QCOMPARE(m.notice(), QStringLiteral("Not available"));
        QCOMPARE(m.state(), QStringLiteral("ready"));
        QCOMPARE(m.rowCount(), 1);
        QCOMPARE(changed.count(), 1);
        m.begin(false); // the next request drops the notice
        QVERIFY(m.notice().isEmpty());
    }

    void theHiddenRootIsNotARow()
    {
        PlanModel m;
        QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Fatal);
        ExplainNode a = node(QStringLiteral("A"));
        ExplainNode b = node(QStringLiteral("B"));
        ExplainNode c = node(QStringLiteral("C"));
        ExplainNode d = node(QStringLiteral("D"));
        c.children.push_back(d);
        a.children.push_back(b);
        a.children.push_back(c);
        m.setPlan(wrapped({ a, node(QStringLiteral("E")) }));

        QCOMPARE(m.rowCount(), 2); // A and E, not the wrapper
        QCOMPARE(m.nodeCount(), 5);
        const QModelIndex ia = m.index(0, 0);
        const QModelIndex ie = m.index(1, 0);
        QCOMPARE(role(m, ia, PlanModel::LabelRole).toString(), QStringLiteral("A"));
        QCOMPARE(role(m, ie, PlanModel::LabelRole).toString(), QStringLiteral("E"));
        QVERIFY(!ia.parent().isValid());
        QVERIFY(m.hasChildren(ia));
        QVERIFY(!m.hasChildren(ie));
        QCOMPARE(m.rowCount(ia), 2);
        const QModelIndex ic = m.index(1, 0, ia);
        QCOMPARE(role(m, ic, PlanModel::LabelRole).toString(), QStringLiteral("C"));
        QCOMPARE(ic.parent(), ia);
        const QModelIndex id = m.index(0, 0, ic);
        QCOMPARE(role(m, id, PlanModel::LabelRole).toString(), QStringLiteral("D"));
        QCOMPARE(id.parent(), ic);
        QCOMPARE(role(m, id, PlanModel::LevelRole).toInt(), 2);
        QCOMPARE(role(m, ia, PlanModel::LevelRole).toInt(), 0);
        // Position among the siblings.
        QCOMPARE(role(m, ie, PlanModel::PositionRole).toInt(), 2);
        QCOMPARE(role(m, ie, PlanModel::SetSizeRole).toInt(), 2);
        QCOMPARE(role(m, ic, PlanModel::PositionRole).toInt(), 2);
        QCOMPARE(role(m, ic, PlanModel::SetSizeRole).toInt(), 2);
        QCOMPARE(role(m, m.index(0, 0, ia), PlanModel::PositionRole).toInt(), 1);
        QCOMPARE(m.columnCount(), 1);
    }

    void aLabelledRootIsTheOnlyTopLevelStep()
    {
        PlanModel m;
        QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Fatal);
        ExplainNode root = node(QStringLiteral("Aggregate"), 100);
        root.children.push_back(node(QStringLiteral("Seq Scan"), 60));
        ExplainPlan p;
        p.root = root;
        m.setPlan(p);
        QCOMPARE(m.rowCount(), 1);
        QCOMPARE(m.nodeCount(), 2);
        QCOMPARE(role(m, m.index(0, 0), PlanModel::LabelRole).toString(),
                 QStringLiteral("Aggregate"));
        QCOMPARE(m.rowCount(m.index(0, 0)), 1);
        QCOMPARE(role(m, m.index(0, 0, m.index(0, 0)), PlanModel::LevelRole).toInt(), 1);
    }

    void anEmptyPlanHasNoRows()
    {
        PlanModel m;
        QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Fatal);
        m.setPlan(ExplainPlan {});
        QCOMPARE(m.rowCount(), 0);
        QCOMPARE(m.nodeCount(), 0);
        QCOMPARE(m.state(), QStringLiteral("ready"));
    }

    void costSharesAreTheNodesOwnCost()
    {
        PlanModel m;
        QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Fatal);
        // A 80 (B 50, C 10) and E 20: the total of the plan is 100.
        ExplainNode a = node(QStringLiteral("A"), 80);
        a.children.push_back(node(QStringLiteral("B"), 50));
        a.children.push_back(node(QStringLiteral("C"), 10));
        m.setPlan(wrapped({ a, node(QStringLiteral("E"), 20) }));
        QCOMPARE(m.barBasis(), QStringLiteral("cost"));
        QVERIFY(m.hasBars());
        const QModelIndex ia = m.index(0, 0);
        QCOMPARE(role(m, ia, PlanModel::CostPercentTextRole).toString(), QStringLiteral("20%"));
        QCOMPARE(role(m, ia, PlanModel::CostFractionRole).toDouble(), 0.2);
        QCOMPARE(role(m, m.index(0, 0, ia), PlanModel::CostPercentTextRole).toString(),
                 QStringLiteral("50%"));
        QCOMPARE(role(m, m.index(1, 0, ia), PlanModel::CostFractionRole).toDouble(), 0.1);
        QCOMPARE(role(m, m.index(1, 0), PlanModel::CostPercentTextRole).toString(),
                 QStringLiteral("20%"));
        QVERIFY(role(m, ia, PlanModel::HasBarRole).toBool());
        QCOMPARE(role(m, ia, PlanModel::ShareTextRole).toString(),
                 QStringLiteral("20% of the total estimated cost"));
    }

    void ownCostNeverGoesBelowZeroAndSmallSharesSayLessThanOne()
    {
        PlanModel m;
        ExplainNode a = node(QStringLiteral("A"), 10);
        a.children.push_back(node(QStringLiteral("B"), 30)); // more than its parent
        ExplainNode tiny = node(QStringLiteral("Tiny"), 0.4);
        m.setPlan(wrapped({ a, tiny, node(QStringLiteral("Big"), 100) }));
        QCOMPARE(role(m, m.index(0, 0), PlanModel::CostPercentTextRole).toString(),
                 QStringLiteral("0%"));
        QCOMPARE(role(m, m.index(1, 0), PlanModel::CostPercentTextRole).toString(),
                 QStringLiteral("<1%"));
        // A labelled root's own total is the denominator.
        ExplainNode root = node(QStringLiteral("Root"), 200);
        root.children.push_back(node(QStringLiteral("Child"), 150));
        ExplainPlan p;
        p.root = root;
        m.setPlan(p);
        QCOMPARE(role(m, m.index(0, 0), PlanModel::CostPercentTextRole).toString(),
                 QStringLiteral("25%"));
        QCOMPARE(role(m, m.index(0, 0, m.index(0, 0)), PlanModel::CostPercentTextRole).toString(),
                 QStringLiteral("75%"));
    }

    void realTimeReplacesCostAfterAnalyze()
    {
        PlanModel m;
        QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Fatal);
        // The costs say 50/50; the times say 25/75: the bars follow the times.
        ExplainNode a = node(QStringLiteral("A"), 100, 60);
        a.children.push_back(node(QStringLiteral("B"), 50, 45));
        ExplainPlan p = wrapped({ a });
        p.analyzed = true;
        m.setPlan(p);
        QCOMPARE(m.barBasis(), QStringLiteral("time"));
        const QModelIndex ia = m.index(0, 0);
        QCOMPARE(role(m, ia, PlanModel::CostPercentTextRole).toString(), QStringLiteral("25%"));
        QCOMPARE(role(m, m.index(0, 0, ia), PlanModel::CostPercentTextRole).toString(),
                 QStringLiteral("75%"));
        QCOMPARE(role(m, ia, PlanModel::ShareTextRole).toString(),
                 QStringLiteral("25% of the total execution time"));

        // Analyzed, but the engine gave no times: back to the cost.
        ExplainPlan noTimes = wrapped({ node(QStringLiteral("A"), 100) });
        noTimes.analyzed = true;
        m.setPlan(noTimes);
        QCOMPARE(m.barBasis(), QStringLiteral("cost"));

        // Times of a plan that was not analyzed are not used.
        ExplainPlan estimated = wrapped({ node(QStringLiteral("A"), 100, 5) });
        m.setPlan(estimated);
        QCOMPARE(m.barBasis(), QStringLiteral("cost"));
    }

    void withoutCostOrTimeThereIsNoBar()
    {
        PlanModel m;
        QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Fatal);
        m.setPlan(wrapped({ node(QStringLiteral("SCAN t")), node(QStringLiteral("SEARCH u")) }));
        QCOMPARE(m.barBasis(), QStringLiteral("none"));
        QVERIFY(!m.hasBars());
        const QModelIndex i = m.index(0, 0);
        QVERIFY(!role(m, i, PlanModel::HasBarRole).toBool());
        QCOMPARE(role(m, i, PlanModel::CostPercentTextRole).toString(), QString());
        QCOMPARE(role(m, i, PlanModel::ShareTextRole).toString(), QString());
        QCOMPARE(role(m, i, PlanModel::CostFractionRole).toDouble(), 0.0);
        QCOMPARE(role(m, i, PlanModel::CostTextRole).toString(), QString());
        // All costs zero: nothing to divide by.
        m.setPlan(wrapped({ node(QStringLiteral("A"), 0) }));
        QVERIFY(!m.hasBars());
    }

    void numbersAreWrittenWithUnits()
    {
        PlanModel m;
        ExplainNode n = node(QStringLiteral("Seq Scan on orders"), 12.5, 0.42);
        n.estimatedRows = 1234;
        n.actualRows = 1200;
        n.loops = 3;
        ExplainNode plain = node(QStringLiteral("Index Scan"), 1234.56);
        plain.estimatedRows = 1234;
        ExplainNode one = node(QStringLiteral("One"), 0.5);
        one.estimatedRows = 1;
        ExplainNode slow = node(QStringLiteral("Slow"), std::nullopt, 2500);
        ExplainNode mid = node(QStringLiteral("Mid"), std::nullopt, 12.34);
        ExplainPlan p = wrapped({ n, plain, one, slow, mid });
        p.analyzed = true;
        p.planningTimeMs = 0.12;
        p.executionTimeMs = 1500;
        m.setPlan(p);
        QCOMPARE(role(m, m.index(0, 0), PlanModel::CostTextRole).toString(),
                 QStringLiteral("cost 12.5"));
        QCOMPARE(role(m, m.index(0, 0), PlanModel::TimeTextRole).toString(),
                 QStringLiteral("0.42 ms"));
        QCOMPARE(role(m, m.index(0, 0), PlanModel::RowsTextRole).toString(),
                 QStringLiteral("1,200 rows × 3 loops"));
        QCOMPARE(role(m, m.index(1, 0), PlanModel::RowsTextRole).toString(),
                 QStringLiteral("est. 1,234 rows"));
        QCOMPARE(role(m, m.index(1, 0), PlanModel::CostTextRole).toString(),
                 QStringLiteral("cost 1,235"));
        QCOMPARE(role(m, m.index(1, 0), PlanModel::TimeTextRole).toString(), QString());
        QCOMPARE(role(m, m.index(2, 0), PlanModel::RowsTextRole).toString(),
                 QStringLiteral("est. 1 row"));
        QCOMPARE(role(m, m.index(2, 0), PlanModel::CostTextRole).toString(),
                 QStringLiteral("cost 0.5"));
        QCOMPARE(role(m, m.index(3, 0), PlanModel::TimeTextRole).toString(),
                 QStringLiteral("2.5 s"));
        QCOMPARE(role(m, m.index(4, 0), PlanModel::TimeTextRole).toString(),
                 QStringLiteral("12.3 ms"));
        QCOMPARE(m.planningTimeText(), QStringLiteral("0.12 ms"));
        QCOMPARE(m.executionTimeText(), QStringLiteral("1.5 s"));
        QCOMPARE(m.planningTime(), 0.12);
        QCOMPARE(m.executionTime(), 1500.0);
    }

    void numbersFollowTheLocale()
    {
        PlanModel m;
        ExplainNode n = node(QStringLiteral("A"), 12.5);
        n.estimatedRows = 1234;
        QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
        m.setPlan(wrapped({ n }));
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        QCOMPARE(role(m, m.index(0, 0), PlanModel::CostTextRole).toString(),
                 QStringLiteral("cost 12,5"));
        QCOMPARE(role(m, m.index(0, 0), PlanModel::RowsTextRole).toString(),
                 QStringLiteral("est. 1.234 rows"));
    }

    void warningsDetailsAndTheDescription()
    {
        PlanModel m;
        ExplainNode n = node(QStringLiteral("Seq Scan on t"), 100, 5);
        n.details.push_back(ExplainProperty { QStringLiteral("Filter"), QStringLiteral("a > 1") });
        n.details.push_back(ExplainProperty { QStringLiteral("Index"), QStringLiteral("t_a") });
        n.warnings << QStringLiteral("Full table scan")
                   << QStringLiteral("Rows estimate off by 10x");
        n.actualRows = 7;
        ExplainPlan p = wrapped({ n });
        p.analyzed = true;
        m.setPlan(p);
        const QModelIndex i = m.index(0, 0);
        QCOMPARE(role(m, i, PlanModel::WarningsRole).toStringList(),
                 (QStringList { QStringLiteral("Full table scan"),
                                QStringLiteral("Rows estimate off by 10x") }));
        const QVariantList details = role(m, i, PlanModel::DetailsRole).toList();
        QCOMPARE(details.size(), 2);
        QCOMPARE(details.at(0).toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Filter"));
        QCOMPARE(details.at(1).toMap().value(QStringLiteral("value")).toString(),
                 QStringLiteral("t_a"));
        // Everything a screen reader needs, in one sentence.
        QCOMPARE(
            role(m, i, PlanModel::AccessibleDescriptionRole).toString(),
            QStringLiteral("100% of the total execution time, cost 100, 7 rows, 5 ms, warning: "
                           "Full table scan, warning: Rows estimate off by 10x, level 1, 1 of 1"));
    }

    void theRawTextAndTheEngineComeFromThePlan()
    {
        PlanModel m;
        ExplainPlan p = wrapped({ node(QStringLiteral("A")) });
        p.rawText = QStringLiteral("raw\tplan");
        p.engine = QStringLiteral("SQLite");
        p.truncated = true;
        m.setPlan(p);
        QCOMPARE(m.rawText(), QStringLiteral("raw\tplan"));
        QCOMPARE(m.engine(), QStringLiteral("SQLite"));
        QVERIFY(m.truncated());
        QVERIFY(!m.analyzed());
        QCOMPARE(m.planningTime(), -1.0);
        QCOMPARE(m.planningTimeText(), QString());
        QCOMPARE(m.executionTimeText(), QString());
    }

    void aResetTellsTheViews()
    {
        PlanModel m;
        QSignalSpy reset(&m, &QAbstractItemModel::modelReset);
        m.setPlan(wrapped({ node(QStringLiteral("A")) }));
        QCOMPARE(reset.count(), 1);
        m.begin(false);
        QCOMPARE(reset.count(), 2);
    }

    void fiveThousandNodesStayFast()
    {
        // 50 steps with 99 children each: the 5000 nodes the parsers allow.
        std::vector<ExplainNode> tops;
        for (int i = 0; i < 50; ++i) {
            ExplainNode top = node(QStringLiteral("Top %1").arg(i), 1000, 10);
            for (int k = 0; k < 99; ++k) {
                ExplainNode c = node(QStringLiteral("Child %1.%2").arg(i).arg(k), 5, 1);
                c.estimatedRows = 10;
                c.details.push_back(
                    ExplainProperty { QStringLiteral("Cond"), QStringLiteral("x") });
                top.children.push_back(std::move(c));
            }
            tops.push_back(std::move(top));
        }
        ExplainPlan p = wrapped(std::move(tops));
        p.analyzed = true;
        PlanModel m;
        QElapsedTimer clock;
        clock.start();
        m.setPlan(p);
        const qint64 loadMs = clock.elapsed();
        QCOMPARE(m.nodeCount(), 5000);

        clock.restart();
        qsizetype visited = 0;
        const int roles[] = { PlanModel::LabelRole,
                              PlanModel::CostTextRole,
                              PlanModel::RowsTextRole,
                              PlanModel::TimeTextRole,
                              PlanModel::CostFractionRole,
                              PlanModel::CostPercentTextRole,
                              PlanModel::AccessibleDescriptionRole };
        for (int top = 0; top < m.rowCount(); ++top) {
            const QModelIndex it = m.index(top, 0);
            for (int k = 0; k < m.rowCount(it); ++k) {
                const QModelIndex ic = m.index(k, 0, it);
                for (int r : roles)
                    visited += m.data(ic, r).isValid() ? 1 : 0;
            }
        }
        const qint64 readMs = clock.elapsed();
        QVERIFY(visited > 0);
        // Generous: a debug build on a busy machine. The design target is milliseconds.
        QVERIFY2(loadMs < 1000, qPrintable(QStringLiteral("load %1 ms").arg(loadMs)));
        QVERIFY2(readMs < 1000, qPrintable(QStringLiteral("read %1 ms").arg(readMs)));
        qInfo() << "5000 nodes: load" << loadMs << "ms, read" << readMs << "ms";
    }

    void aSmallTreeOfManyLevelsPassesTheTester()
    {
        PlanModel m;
        QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::Fatal);
        ExplainNode deepest = node(QStringLiteral("L0"), 1);
        for (int i = 1; i < 30; ++i) {
            ExplainNode parent = node(QStringLiteral("L%1").arg(i), i + 1);
            parent.children.push_back(std::move(deepest));
            parent.children.push_back(node(QStringLiteral("S%1").arg(i), 1));
            deepest = std::move(parent);
        }
        m.setPlan(wrapped({ std::move(deepest) }));
        QCOMPARE(m.nodeCount(), 1 + 2 * 29);
    }
};

QTEST_MAIN(TstPlanModel)
#include "tst_plan_model.moc"
