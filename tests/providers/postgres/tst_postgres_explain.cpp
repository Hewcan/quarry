#include "postgres_explain.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include <algorithm>

using namespace quarry;
using namespace quarry::postgres;

namespace {

QString detailOf(const ExplainNode &node, const QString &name)
{
    for (const ExplainProperty &property : node.details) {
        if (property.name == name)
            return property.value;
    }
    return {};
}

bool hasDetail(const ExplainNode &node, const QString &name)
{
    return std::any_of(node.details.begin(), node.details.end(),
                       [&](const ExplainProperty &p) { return p.name == name; });
}

ExplainPlan parseOk(const QString &json, ExplainMode mode = ExplainMode::Plan)
{
    auto result = parseExplainJson(json, mode);
    if (!result)
        qWarning() << result.error().message << result.error().detail;
    Q_ASSERT(result);
    return std::move(result).value();
}

// A plan made of `levels` nested nodes (the leaf included).
QString chain(int levels)
{
    QString node = QStringLiteral(R"json({"Node Type": "Leaf"})json");
    for (int i = 1; i < levels; ++i)
        node = QStringLiteral(R"json({"Node Type": "N", "Plans": [%1]})json").arg(node);
    return QStringLiteral(R"json([{"Plan": %1}])json").arg(node);
}

const QString kSeqScan = QStringLiteral(R"json([
  {
    "Plan": {
      "Node Type": "Seq Scan",
      "Parallel Aware": false,
      "Relation Name": "orders",
      "Alias": "orders",
      "Startup Cost": 0.00,
      "Total Cost": 18.10,
      "Plan Rows": 810,
      "Plan Width": 36,
      "Filter": "(status = 'new'::text)"
    }
  }
])json");

const QString kIndexScan = QStringLiteral(R"json([
  {
    "Plan": {
      "Node Type": "Index Scan",
      "Parallel Aware": false,
      "Scan Direction": "Forward",
      "Index Name": "orders_pkey",
      "Relation Name": "orders",
      "Alias": "o",
      "Startup Cost": 0.15,
      "Total Cost": 8.17,
      "Plan Rows": 1,
      "Plan Width": 36,
      "Index Cond": "(id = 5)"
    }
  }
])json");

const QString kHashJoin = QStringLiteral(R"json([
  {
    "Plan": {
      "Node Type": "Hash Join",
      "Join Type": "Inner",
      "Startup Cost": 12.50,
      "Total Cost": 40.25,
      "Plan Rows": 100,
      "Hash Cond": "(o.customer_id = c.id)",
      "Plans": [
        {
          "Node Type": "Seq Scan",
          "Parent Relationship": "Outer",
          "Relation Name": "orders",
          "Alias": "o",
          "Startup Cost": 0.00,
          "Total Cost": 20.00,
          "Plan Rows": 1000
        },
        {
          "Node Type": "Hash",
          "Parent Relationship": "Inner",
          "Startup Cost": 10.00,
          "Total Cost": 10.00,
          "Plan Rows": 50,
          "Plans": [
            {
              "Node Type": "Seq Scan",
              "Parent Relationship": "Outer",
              "Relation Name": "customers",
              "Alias": "c",
              "Startup Cost": 0.00,
              "Total Cost": 10.00,
              "Plan Rows": 50
            }
          ]
        }
      ]
    }
  }
])json");

const QString kSortSpill = QStringLiteral(R"json([
  {
    "Plan": {
      "Node Type": "Sort",
      "Startup Cost": 1000.0,
      "Total Cost": 1100.5,
      "Plan Rows": 100000,
      "Actual Startup Time": 80.0,
      "Actual Total Time": 95.5,
      "Actual Rows": 100000,
      "Actual Loops": 1,
      "Sort Key": ["created_at DESC", "id"],
      "Sort Method": "external merge",
      "Sort Space Used": 3560,
      "Sort Space Type": "Disk",
      "Plans": [
        {
          "Node Type": "Seq Scan",
          "Parent Relationship": "Outer",
          "Relation Name": "events",
          "Alias": "events",
          "Startup Cost": 0.0,
          "Total Cost": 900.0,
          "Plan Rows": 100000,
          "Actual Total Time": 20.0,
          "Actual Rows": 100000,
          "Actual Loops": 1
        }
      ]
    },
    "Planning Time": 0.123,
    "Execution Time": 120.75
  }
])json");

const QString kAggregate = QStringLiteral(R"json([
  {
    "Plan": {
      "Node Type": "Aggregate",
      "Strategy": "Plain",
      "Startup Cost": 100.0,
      "Total Cost": 100.01,
      "Plan Rows": 1,
      "Plans": [
        {
          "Node Type": "Gather",
          "Parent Relationship": "Outer",
          "Startup Cost": 99.0,
          "Total Cost": 99.9,
          "Plan Rows": 2,
          "Workers Planned": 2,
          "Plans": [
            {
              "Node Type": "Aggregate",
              "Strategy": "Plain",
              "Partial Mode": "Partial",
              "Parent Relationship": "Outer",
              "Startup Cost": 90.0,
              "Total Cost": 90.01,
              "Plan Rows": 1,
              "Plans": [
                {
                  "Node Type": "Seq Scan",
                  "Parent Relationship": "Outer",
                  "Relation Name": "t",
                  "Alias": "t",
                  "Startup Cost": 0.0,
                  "Total Cost": 80.0,
                  "Plan Rows": 5000
                }
              ]
            }
          ]
        }
      ]
    }
  }
])json");

const QString kLoops = QStringLiteral(R"json([
  {
    "Plan": {
      "Node Type": "Nested Loop",
      "Join Type": "Inner",
      "Startup Cost": 0.30,
      "Total Cost": 120.0,
      "Plan Rows": 10,
      "Actual Startup Time": 0.05,
      "Actual Total Time": 6.5,
      "Actual Rows": 30,
      "Actual Loops": 1,
      "Plans": [
        {
          "Node Type": "Seq Scan",
          "Parent Relationship": "Outer",
          "Relation Name": "a",
          "Alias": "a",
          "Startup Cost": 0.0,
          "Total Cost": 1.10,
          "Plan Rows": 10,
          "Actual Startup Time": 0.01,
          "Actual Total Time": 0.02,
          "Actual Rows": 10,
          "Actual Loops": 1
        },
        {
          "Node Type": "Index Scan",
          "Parent Relationship": "Inner",
          "Index Name": "b_a_id",
          "Relation Name": "b",
          "Alias": "b",
          "Startup Cost": 0.15,
          "Total Cost": 8.0,
          "Plan Rows": 1,
          "Actual Startup Time": 0.004,
          "Actual Total Time": 0.5,
          "Actual Rows": 3,
          "Actual Loops": 10,
          "Index Cond": "(a_id = a.id)",
          "Rows Removed by Filter": 0,
          "Shared Hit Blocks": 42,
          "Shared Read Blocks": 2
        }
      ]
    },
    "Planning Time": 0.4,
    "Execution Time": 6.9
  }
])json");

} // namespace

class TstPostgresExplain : public QObject
{
    Q_OBJECT

private slots:
    void seqScan();
    void indexScanWithAliasAndCondition();
    void hashJoinKeepsChildren();
    void sortSpilledToDisk();
    void hashBatchesSpill();
    void aggregateNestsPlans();
    void analyzeWithLoops();
    void estimateOffWarning();
    void lateFilterWarning();
    void neverExecutedNodeHasNoEstimateWarning();
    void malformedInputIsAnError();
    void depthLimit();
    void nodeLimitTruncatesBreadthFirst();
};

void TstPostgresExplain::seqScan()
{
    const ExplainPlan plan = parseOk(kSeqScan);
    QCOMPARE(plan.engine, QStringLiteral("PostgreSQL"));
    QCOMPARE(plan.rawText, kSeqScan);
    QVERIFY(!plan.analyzed);
    QVERIFY(!plan.truncated);
    QCOMPARE(plan.nodeCount, qsizetype(1));
    QVERIFY(!plan.planningTimeMs && !plan.executionTimeMs);
    QCOMPARE(plan.root.label, QStringLiteral("Seq Scan on orders")); // alias equals the table
    QVERIFY(plan.root.startupCost && plan.root.totalCost && plan.root.estimatedRows);
    QCOMPARE(*plan.root.startupCost, 0.0);
    QCOMPARE(*plan.root.totalCost, 18.10);
    QCOMPARE(*plan.root.estimatedRows, 810.0);
    QVERIFY(!plan.root.actualRows && !plan.root.actualTimeMs && !plan.root.loops);
    QCOMPARE(detailOf(plan.root, QStringLiteral("Filter")),
             QStringLiteral("(status = 'new'::text)"));
    QCOMPARE(detailOf(plan.root, QStringLiteral("Plan Width")), QStringLiteral("36"));
    QVERIFY(!hasDetail(plan.root, QStringLiteral("Node Type")));
    QVERIFY(plan.root.warnings.isEmpty());
}

void TstPostgresExplain::indexScanWithAliasAndCondition()
{
    const ExplainPlan plan = parseOk(kIndexScan);
    QCOMPARE(plan.root.label, QStringLiteral("Index Scan on orders o using orders_pkey"));
    QCOMPARE(detailOf(plan.root, QStringLiteral("Index Cond")), QStringLiteral("(id = 5)"));
    QCOMPARE(detailOf(plan.root, QStringLiteral("Scan Direction")), QStringLiteral("Forward"));
    QCOMPARE(*plan.root.totalCost, 8.17);
    QCOMPARE(*plan.root.startupCost, 0.15);
}

void TstPostgresExplain::hashJoinKeepsChildren()
{
    const ExplainPlan plan = parseOk(kHashJoin);
    QCOMPARE(plan.nodeCount, qsizetype(4));
    QCOMPARE(plan.root.label, QStringLiteral("Hash Join"));
    QCOMPARE(detailOf(plan.root, QStringLiteral("Join Type")), QStringLiteral("Inner"));
    QCOMPARE(detailOf(plan.root, QStringLiteral("Hash Cond")),
             QStringLiteral("(o.customer_id = c.id)"));
    QCOMPARE(plan.root.children.size(), size_t(2));
    const ExplainNode &outer = plan.root.children[0];
    const ExplainNode &inner = plan.root.children[1];
    QCOMPARE(outer.label, QStringLiteral("Seq Scan on orders o"));
    QCOMPARE(*outer.estimatedRows, 1000.0);
    QVERIFY(!hasDetail(outer, QStringLiteral("Parent Relationship")));
    QCOMPARE(inner.label, QStringLiteral("Hash"));
    QCOMPARE(inner.children.size(), size_t(1));
    QCOMPARE(inner.children[0].label, QStringLiteral("Seq Scan on customers c"));
}

void TstPostgresExplain::sortSpilledToDisk()
{
    const ExplainPlan plan = parseOk(kSortSpill, ExplainMode::Analyze);
    QVERIFY(plan.analyzed);
    QCOMPARE(plan.root.label, QStringLiteral("Sort"));
    QCOMPARE(detailOf(plan.root, QStringLiteral("Sort Key")),
             QStringLiteral("created_at DESC, id"));
    QCOMPARE(detailOf(plan.root, QStringLiteral("Sort Method")), QStringLiteral("external merge"));
    QCOMPARE(plan.root.warnings, QStringList { QStringLiteral("Spilled to disk") });
    QVERIFY(plan.planningTimeMs && plan.executionTimeMs);
    QCOMPARE(*plan.planningTimeMs, 0.123);
    QCOMPARE(*plan.executionTimeMs, 120.75);
    QCOMPARE(*plan.root.actualRows, 100000.0);
    QCOMPARE(*plan.root.actualTimeMs, 95.5);
    QVERIFY(plan.root.children[0].warnings.isEmpty());
}

void TstPostgresExplain::hashBatchesSpill()
{
    const QString json = QStringLiteral(R"json([{"Plan": {"Node Type": "Hash", "Plan Rows": 10,
        "Actual Rows": 10, "Actual Loops": 1, "Hash Batches": 4, "Peak Memory Usage": 4096}}])json");
    const ExplainPlan plan = parseOk(json, ExplainMode::Analyze);
    QCOMPARE(plan.root.warnings, QStringList { QStringLiteral("Spilled to disk") });

    const QString inMemory = QStringLiteral(R"json([{"Plan": {"Node Type": "Hash", "Plan Rows": 10,
        "Actual Rows": 10, "Actual Loops": 1, "Hash Batches": 1}}])json");
    QVERIFY(parseOk(inMemory, ExplainMode::Analyze).root.warnings.isEmpty());
}

void TstPostgresExplain::aggregateNestsPlans()
{
    const ExplainPlan plan = parseOk(kAggregate);
    QCOMPARE(plan.nodeCount, qsizetype(4));
    QCOMPARE(plan.root.label, QStringLiteral("Aggregate"));
    QCOMPARE(detailOf(plan.root, QStringLiteral("Strategy")), QStringLiteral("Plain"));
    const ExplainNode &gather = plan.root.children.at(0);
    QCOMPARE(gather.label, QStringLiteral("Gather"));
    QCOMPARE(detailOf(gather, QStringLiteral("Workers Planned")), QStringLiteral("2"));
    const ExplainNode &partial = gather.children.at(0);
    QCOMPARE(partial.label, QStringLiteral("Aggregate"));
    QCOMPARE(partial.children.at(0).label, QStringLiteral("Seq Scan on t"));
    QCOMPARE(*partial.children.at(0).totalCost, 80.0);
}

void TstPostgresExplain::analyzeWithLoops()
{
    const ExplainPlan plan = parseOk(kLoops, ExplainMode::Analyze);
    QVERIFY(plan.analyzed);
    QCOMPARE(plan.nodeCount, qsizetype(3));
    QCOMPARE(*plan.planningTimeMs, 0.4);
    QCOMPARE(*plan.executionTimeMs, 6.9);
    const ExplainNode &inner = plan.root.children.at(1);
    QCOMPARE(inner.label, QStringLiteral("Index Scan on b using b_a_id"));
    // Actual Rows is per loop (as PostgreSQL reports it); the time is multiplied by the loops.
    QCOMPARE(*inner.actualRows, 3.0);
    QCOMPARE(*inner.loops, 10.0);
    QCOMPARE(*inner.actualTimeMs, 5.0);
    QCOMPARE(detailOf(inner, QStringLiteral("Shared Hit Blocks")), QStringLiteral("42"));
    QCOMPARE(detailOf(inner, QStringLiteral("Shared Read Blocks")), QStringLiteral("2"));
    QVERIFY(inner.warnings.isEmpty()); // 1 estimated vs 3 actual is not 10x
    QCOMPARE(*plan.root.actualTimeMs, 6.5);
}

void TstPostgresExplain::estimateOffWarning()
{
    const QString json = QStringLiteral(R"json([{"Plan": {"Node Type": "Seq Scan",
        "Relation Name": "t", "Alias": "t", "Plan Rows": 2, "Actual Rows": 500,
        "Actual Loops": 1, "Actual Total Time": 1.0}}])json");
    const ExplainPlan plan = parseOk(json, ExplainMode::Analyze);
    QCOMPARE(plan.root.warnings, QStringList { QStringLiteral("Row estimate off by 250x") });

    // Also when the planner overestimates.
    const QString over
        = QStringLiteral(R"json([{"Plan": {"Node Type": "Seq Scan", "Plan Rows": 1000,
        "Actual Rows": 10, "Actual Loops": 1}}])json");
    QCOMPARE(parseOk(over, ExplainMode::Analyze).root.warnings,
             QStringList { QStringLiteral("Row estimate off by 100x") });

    // No ANALYZE data, no warning.
    const QString planOnly = QStringLiteral(R"json([{"Plan": {"Node Type": "Seq Scan",
        "Plan Rows": 1000}}])json");
    QVERIFY(parseOk(planOnly).root.warnings.isEmpty());
}

void TstPostgresExplain::lateFilterWarning()
{
    const QString json = QStringLiteral(R"json([{"Plan": {"Node Type": "Seq Scan",
        "Relation Name": "t", "Alias": "t", "Plan Rows": 10, "Actual Rows": 10,
        "Actual Loops": 1, "Filter": "(a = 1)", "Rows Removed by Filter": 50000}}])json");
    const ExplainPlan plan = parseOk(json, ExplainMode::Analyze);
    QCOMPARE(plan.root.warnings,
             QStringList {
                 QStringLiteral("Filter removed 50000 rows after reading them to return 10") });
    QCOMPARE(detailOf(plan.root, QStringLiteral("Rows Removed by Filter")),
             QStringLiteral("50000"));

    // Below 1000 removed rows, or less than 10x the returned rows: nothing to say.
    const QString few = QStringLiteral(R"json([{"Plan": {"Node Type": "Seq Scan", "Plan Rows": 10,
        "Actual Rows": 10, "Actual Loops": 1, "Rows Removed by Filter": 900}}])json");
    QVERIFY(parseOk(few, ExplainMode::Analyze).root.warnings.isEmpty());
    const QString balanced
        = QStringLiteral(R"json([{"Plan": {"Node Type": "Seq Scan", "Plan Rows": 5000,
        "Actual Rows": 5000, "Actual Loops": 1, "Rows Removed by Filter": 20000}}])json");
    QVERIFY(parseOk(balanced, ExplainMode::Analyze).root.warnings.isEmpty());
}

void TstPostgresExplain::neverExecutedNodeHasNoEstimateWarning()
{
    const QString json
        = QStringLiteral(R"json([{"Plan": {"Node Type": "Index Scan", "Plan Rows": 1000,
        "Actual Rows": 0, "Actual Loops": 0, "Actual Total Time": 0.0}}])json");
    const ExplainPlan plan = parseOk(json, ExplainMode::Analyze);
    QVERIFY(plan.root.warnings.isEmpty());
    QCOMPARE(*plan.root.actualTimeMs, 0.0);
    QCOMPARE(*plan.root.loops, 0.0);
}

void TstPostgresExplain::malformedInputIsAnError()
{
    const QStringList bad = {
        QString(),
        QStringLiteral("  \n "),
        QStringLiteral("Seq Scan on orders  (cost=0.00..18.10 rows=810 width=36)"), // text format
        QStringLiteral(R"json([{"Plan": )json"), // truncated
        QStringLiteral("{}"),
        QStringLiteral("[]"),
        QStringLiteral("[1, 2]"),
        QStringLiteral(R"json({"foo": 1})json"),
        QStringLiteral(R"json([{"Plan": 5}])json"),
        QStringLiteral(R"json([{"Plan": [1]}])json"),
        QStringLiteral(R"json("text")json"),
    };
    for (const QString &text : bad) {
        const auto result = parseExplainJson(text, ExplainMode::Plan);
        QVERIFY2(!result, qPrintable(text));
        QCOMPARE(result.error().kind, ErrorKind::Syntax);
        QVERIFY(!result.error().message.isEmpty());
    }
    const auto invalid = parseExplainJson(QStringLiteral("[{"), ExplainMode::Plan);
    QVERIFY(!invalid.error().detail.isEmpty());
}

void TstPostgresExplain::depthLimit()
{
    const ExplainPlan ok = parseOk(chain(200));
    QCOMPARE(ok.nodeCount, qsizetype(200));

    const auto deep = parseExplainJson(chain(201), ExplainMode::Plan);
    QVERIFY(!deep);
    QCOMPARE(deep.error().kind, ErrorKind::Syntax);

    // Hostile nesting, well past what the JSON parser itself accepts: also an error, no crash.
    const auto hostile = parseExplainJson(chain(2000), ExplainMode::Plan);
    QVERIFY(!hostile);
}

void TstPostgresExplain::nodeLimitTruncatesBreadthFirst()
{
    // Root with two children: A has 5000 children, B has one. Breadth first keeps the root, A and
    // B, then fills the rest with A's children; B's child is dropped.
    QJsonArray manyChildren;
    for (int i = 0; i < 5000; ++i)
        manyChildren.append(
            QJsonObject { { QStringLiteral("Node Type"), QStringLiteral("A%1").arg(i) } });
    const QJsonObject a { { QStringLiteral("Node Type"), QStringLiteral("A") },
                          { QStringLiteral("Plans"), manyChildren } };
    const QJsonObject b {
        { QStringLiteral("Node Type"), QStringLiteral("B") },
        { QStringLiteral("Plans"),
          QJsonArray { QJsonObject { { QStringLiteral("Node Type"), QStringLiteral("B0") } } } }
    };
    const QJsonObject root { { QStringLiteral("Node Type"), QStringLiteral("Root") },
                             { QStringLiteral("Plans"), QJsonArray { a, b } } };
    const QString json = QString::fromUtf8(
        QJsonDocument(QJsonArray { QJsonObject { { QStringLiteral("Plan"), root } } })
            .toJson(QJsonDocument::Compact));

    const ExplainPlan plan = parseOk(json);
    QVERIFY(plan.truncated);
    QCOMPARE(plan.nodeCount, kMaxExplainNodes);
    QCOMPARE(plan.root.children.size(), size_t(2));
    QCOMPARE(plan.root.children[0].children.size(), size_t(kMaxExplainNodes - 3));
    QVERIFY(plan.root.children[1].children.empty());
    QCOMPARE(plan.rawText, json);

    // Exactly 5000 nodes (root + 4999 children): nothing is dropped.
    QJsonArray fits;
    for (int i = 0; i < kMaxExplainNodes - 1; ++i)
        fits.append(QJsonObject { { QStringLiteral("Node Type"), QStringLiteral("X") } });
    const QJsonObject flat { { QStringLiteral("Node Type"), QStringLiteral("Root") },
                             { QStringLiteral("Plans"), fits } };
    const QString flatJson = QString::fromUtf8(
        QJsonDocument(QJsonArray { QJsonObject { { QStringLiteral("Plan"), flat } } })
            .toJson(QJsonDocument::Compact));
    const ExplainPlan exact = parseOk(flatJson);
    QVERIFY(!exact.truncated);
    QCOMPARE(exact.nodeCount, kMaxExplainNodes);
}

QTEST_GUILESS_MAIN(TstPostgresExplain)
#include "tst_postgres_explain.moc"
