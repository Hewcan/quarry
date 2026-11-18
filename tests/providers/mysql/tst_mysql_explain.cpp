#include "mysql_explain.h"

#include <QtTest>

#include <algorithm>

using namespace quarry;
using namespace quarry::mysql;

namespace {

QString detailOf(const ExplainNode &node, const QString &name)
{
    for (const ExplainProperty &property : node.details) {
        if (property.name == name)
            return property.value;
    }
    return {};
}

ExplainPlan mysqlOk(const QString &text, ExplainMode mode = ExplainMode::Plan)
{
    auto result = parseMysqlExplain(text, mode);
    if (!result)
        qWarning() << result.error().message << result.error().detail;
    Q_ASSERT(result);
    return std::move(result).value();
}

ExplainPlan mariadbOk(const QString &text, ExplainMode mode = ExplainMode::Plan)
{
    auto result = parseMariadbExplain(text, mode);
    if (!result)
        qWarning() << result.error().message << result.error().detail;
    Q_ASSERT(result);
    return std::move(result).value();
}

// A query block with `levels` + 1 operations of the same kind nested in each other: levels + 2
// nodes with the block.
QString nested(const QString &key, int levels)
{
    QString inner = QStringLiteral(R"json({"using_filesort": true})json");
    for (int i = 0; i < levels; ++i)
        inner = QStringLiteral(R"json({"%1": %2})json").arg(key, inner);
    return QStringLiteral(R"json({"query_block": {"select_id": 1, "%1": %2}})json").arg(key, inner);
}

QString manyTables(int count)
{
    QStringList tables;
    for (int i = 0; i < count; ++i)
        tables.append(
            QStringLiteral(R"json({"table": {"table_name": "t%1", "access_type": "ALL"}})json")
                .arg(i));
    return QStringLiteral(R"json({"query_block": {"select_id": 1, "nested_loop": [%1]}})json")
        .arg(tables.join(QLatin1Char(',')));
}

const QString kSingleAll = QStringLiteral(R"json({
  "query_block": {
    "select_id": 1,
    "cost_info": {"query_cost": "1.05"},
    "table": {
      "table_name": "t",
      "access_type": "ALL",
      "rows_examined_per_scan": 3,
      "rows_produced_per_join": 1,
      "filtered": "33.33",
      "cost_info": {
        "read_cost": "0.85",
        "eval_cost": "0.20",
        "prefix_cost": "1.05",
        "data_read_per_join": "24"
      },
      "used_columns": ["a", "b"],
      "attached_condition": "(`d`.`t`.`b` = 1)"
    }
  }
})json");

const QString kRef = QStringLiteral(R"json({
  "query_block": {
    "select_id": 1,
    "cost_info": {"query_cost": "2.10"},
    "table": {
      "table_name": "orders",
      "access_type": "ref",
      "possible_keys": ["idx_user", "idx_date"],
      "key": "idx_user",
      "used_key_parts": ["user_id"],
      "key_length": "4",
      "ref": ["const"],
      "rows_examined_per_scan": 5,
      "rows_produced_per_join": 5,
      "filtered": "100.00",
      "cost_info": {
        "read_cost": "1.60",
        "eval_cost": "0.50",
        "prefix_cost": "2.10",
        "data_read_per_join": "400"
      },
      "used_columns": ["id", "user_id"]
    }
  }
})json");

const QString kJoin = QStringLiteral(R"json({
  "query_block": {
    "select_id": 1,
    "cost_info": {"query_cost": "120.50"},
    "nested_loop": [
      {"table": {"table_name": "users", "access_type": "ALL",
                 "rows_examined_per_scan": 100, "rows_produced_per_join": 100,
                 "cost_info": {"read_cost": "10.00", "eval_cost": "10.00", "prefix_cost": "20.00"}}},
      {"table": {"table_name": "orders", "access_type": "ref", "key": "idx_user",
                 "rows_examined_per_scan": 2, "rows_produced_per_join": 200,
                 "cost_info": {"read_cost": "50.00", "eval_cost": "20.00", "prefix_cost": "90.00"}}},
      {"table": {"table_name": "items", "access_type": "eq_ref", "key": "PRIMARY",
                 "using_join_buffer": "hash join",
                 "rows_examined_per_scan": 1, "rows_produced_per_join": 200,
                 "cost_info": {"read_cost": "20.50", "eval_cost": "10.00", "prefix_cost": "120.50"}}}
    ]
  }
})json");

const QString kOrderGroup = QStringLiteral(R"json({
  "query_block": {
    "select_id": 1,
    "cost_info": {"query_cost": "10.00"},
    "ordering_operation": {
      "using_filesort": true,
      "grouping_operation": {
        "using_temporary_table": true,
        "using_filesort": false,
        "table": {"table_name": "t", "access_type": "ALL", "rows_produced_per_join": 50,
                  "cost_info": {"prefix_cost": "10.00"}}
      }
    }
  }
})json");

const QString kSubquery = QStringLiteral(R"json({
  "query_block": {
    "select_id": 1,
    "table": {
      "table_name": "t",
      "access_type": "ALL",
      "attached_condition": "(t.a in (select u.id from u))",
      "attached_subqueries": [
        {
          "dependent": false,
          "cacheable": true,
          "query_block": {
            "select_id": 2,
            "table": {"table_name": "u", "access_type": "index", "key": "PRIMARY"}
          }
        }
      ]
    }
  }
})json");

const QString kDerived = QStringLiteral(R"json({
  "query_block": {
    "select_id": 1,
    "table": {
      "table_name": "<derived2>",
      "access_type": "ALL",
      "materialized_from_subquery": {
        "using_temporary_table": true,
        "dependent": false,
        "cacheable": true,
        "query_block": {
          "select_id": 2,
          "table": {"table_name": "u", "access_type": "ALL"}
        }
      }
    }
  }
})json");

const QString kUnion = QStringLiteral(R"json({
  "query_block": {
    "union_result": {
      "using_temporary_table": true,
      "table_name": "<union1,2>",
      "access_type": "ALL",
      "query_specifications": [
        {"dependent": false, "cacheable": true,
         "query_block": {"select_id": 1, "table": {"table_name": "a", "access_type": "ALL"}}},
        {"dependent": false, "cacheable": true,
         "query_block": {"select_id": 2, "table": {"table_name": "b", "access_type": "ALL"}}}
      ]
    }
  }
})json");

const QString kTree = QStringLiteral(
    "-> Nested loop inner join  (cost=1.15 rows=3) (actual time=0.052..0.080 rows=3 loops=1)\n"
    "    -> Table scan on a  (cost=0.35 rows=3) (actual time=0.030..0.035 rows=3 loops=1)\n"
    "    -> Index lookup on b using idx_a (a_id=a.id)  (cost=0.27 rows=1) (actual "
    "time=0.012..0.014 rows=1 loops=3)\n");

// MariaDB
const QString kMariaSingle = QStringLiteral(R"json({
  "query_block": {
    "select_id": 1,
    "table": {
      "table_name": "t",
      "access_type": "ALL",
      "rows": 3,
      "filtered": 100,
      "attached_condition": "t.b = 1"
    }
  }
})json");

const QString kMariaRef = QStringLiteral(R"json({
  "query_block": {
    "select_id": 1,
    "table": {
      "table_name": "t",
      "access_type": "ref",
      "possible_keys": ["idx_b"],
      "key": "idx_b",
      "key_length": "5",
      "used_key_parts": ["b"],
      "ref": ["const"],
      "rows": 2,
      "filtered": 100,
      "index_condition": "t.c > 1"
    }
  }
})json");

const QString kMariaJoin = QStringLiteral(R"json({
  "query_block": {
    "select_id": 1,
    "nested_loop": [
      {"table": {"table_name": "a", "access_type": "ALL", "rows": 10, "filtered": 100}},
      {"block-nl-join": {
        "table": {"table_name": "b", "access_type": "ALL", "rows": 20, "filtered": 100},
        "buffer_type": "flat",
        "buffer_size": "256Kb",
        "join_type": "BNL",
        "attached_condition": "b.a_id = a.id"
      }}
    ]
  }
})json");

const QString kMariaFilesort = QStringLiteral(R"json({
  "query_block": {
    "select_id": 1,
    "filesort": {
      "sort_key": "t.a",
      "temporary_table": {
        "table": {"table_name": "t", "access_type": "ALL", "rows": 100, "filtered": 100}
      }
    }
  }
})json");

const QString kMariaAnalyze = QStringLiteral(R"json({
  "query_block": {
    "select_id": 1,
    "r_loops": 1,
    "r_total_time_ms": 1.234,
    "table": {
      "table_name": "t",
      "access_type": "ALL",
      "r_loops": 1,
      "rows": 1000,
      "r_rows": 3,
      "r_table_rows": 1000,
      "r_total_time_ms": 0.8,
      "filtered": 100,
      "r_filtered": 0.3,
      "attached_condition": "t.b = 1"
    }
  }
})json");

const QString kMariaUnion = QStringLiteral(R"json({
  "query_block": {
    "union_result": {
      "table_name": "<union1,2>",
      "access_type": "ALL",
      "query_specifications": [
        {"query_block": {"select_id": 1, "table": {"table_name": "a", "access_type": "ALL", "rows": 1}}},
        {"query_block": {"select_id": 2, "table": {"table_name": "b", "access_type": "ALL", "rows": 2}}}
      ]
    }
  }
})json");

const QString kMariaSubquery = QStringLiteral(R"json({
  "query_block": {
    "select_id": 1,
    "table": {
      "table_name": "t",
      "access_type": "ALL",
      "rows": 5,
      "attached_condition": "t.a in (subquery)"
    },
    "subqueries": [
      {"expression_cache": {
        "state": "uninitialized",
        "query_block": {
          "select_id": 2,
          "table": {"table_name": "u", "access_type": "index", "key": "PRIMARY", "rows": 7}
        }
      }}
    ]
  }
})json");

} // namespace

class TstMysqlExplain : public QObject
{
    Q_OBJECT

private slots:
    // MySQL 8 JSON
    void mysqlSingleTableAll();
    void mysqlRefAccess();
    void mysqlNestedLoop();
    void mysqlOrderingAndGrouping();
    void mysqlSubqueries();
    void mysqlUnion();
    void mysqlMalformedInputIsAnError();
    void mysqlDepthLimit();
    void mysqlNodeLimit();

    // MySQL EXPLAIN ANALYZE (TREE)
    void treeWithAnalyzeData();
    void treeWarnings();
    void treeWithoutMetricsAndSeveralRoots();
    void treeMalformedAndLimits();

    // MariaDB JSON
    void mariadbSimpleTable();
    void mariadbNestedLoopWithJoinBuffer();
    void mariadbFilesortAndTemporaryTable();
    void mariadbAnalyze();
    void mariadbUnionAndSubqueries();
    void mariadbMalformedInputIsAnError();
    void mariadbLimits();
};

void TstMysqlExplain::mysqlSingleTableAll()
{
    const ExplainPlan plan = mysqlOk(kSingleAll);
    QCOMPARE(plan.engine, QStringLiteral("MySQL"));
    QCOMPARE(plan.rawText, kSingleAll);
    QVERIFY(!plan.analyzed);
    QVERIFY(!plan.truncated);
    QCOMPARE(plan.nodeCount, qsizetype(2));
    QCOMPARE(plan.root.label, QStringLiteral("Query block #1"));
    QVERIFY(plan.root.totalCost);
    QCOMPARE(*plan.root.totalCost, 1.05);
    QCOMPARE(plan.root.children.size(), size_t(1));
    const ExplainNode &table = plan.root.children[0];
    QCOMPARE(table.label, QStringLiteral("Table t (ALL)"));
    QVERIFY(table.totalCost && table.estimatedRows);
    QCOMPARE(*table.totalCost, 1.05);
    QCOMPARE(*table.estimatedRows, 1.0);
    QVERIFY(!table.startupCost && !table.actualRows && !table.actualTimeMs && !table.loops);
    QCOMPARE(table.warnings, QStringList { QStringLiteral("Full table scan") });
    QCOMPARE(detailOf(table, QStringLiteral("Access type")), QStringLiteral("ALL"));
    QCOMPARE(detailOf(table, QStringLiteral("Rows examined per scan")), QStringLiteral("3"));
    QCOMPARE(detailOf(table, QStringLiteral("Filtered")), QStringLiteral("33.33"));
    QCOMPARE(detailOf(table, QStringLiteral("Used columns")), QStringLiteral("a, b"));
    QCOMPARE(detailOf(table, QStringLiteral("Attached condition")),
             QStringLiteral("(`d`.`t`.`b` = 1)"));
    QCOMPARE(detailOf(table, QStringLiteral("Read cost")), QStringLiteral("0.85"));
    QCOMPARE(detailOf(table, QStringLiteral("Prefix cost")), QStringLiteral("1.05"));
}

void TstMysqlExplain::mysqlRefAccess()
{
    const ExplainPlan plan = mysqlOk(kRef);
    const ExplainNode &table = plan.root.children.at(0);
    QCOMPARE(table.label, QStringLiteral("Table orders (ref via idx_user)"));
    QVERIFY(table.warnings.isEmpty());
    QCOMPARE(*table.totalCost, 2.10);
    QCOMPARE(*table.estimatedRows, 5.0);
    QCOMPARE(detailOf(table, QStringLiteral("Key")), QStringLiteral("idx_user"));
    QCOMPARE(detailOf(table, QStringLiteral("Possible keys")),
             QStringLiteral("idx_user, idx_date"));
    QCOMPARE(detailOf(table, QStringLiteral("Used key parts")), QStringLiteral("user_id"));
    QCOMPARE(detailOf(table, QStringLiteral("Ref")), QStringLiteral("const"));
}

void TstMysqlExplain::mysqlNestedLoop()
{
    const ExplainPlan plan = mysqlOk(kJoin);
    QCOMPARE(plan.nodeCount, qsizetype(5)); // block, nested loop, 3 tables
    QCOMPARE(*plan.root.totalCost, 120.50);
    const ExplainNode &loop = plan.root.children.at(0);
    QCOMPARE(loop.label, QStringLiteral("Nested loop"));
    QCOMPARE(loop.children.size(), size_t(3));
    QVERIFY(loop.totalCost);
    QCOMPARE(*loop.totalCost, 120.50); // accumulated cost of the last table
    QCOMPARE(loop.children[0].label, QStringLiteral("Table users (ALL)"));
    QCOMPARE(loop.children[1].label, QStringLiteral("Table orders (ref via idx_user)"));
    QCOMPARE(loop.children[2].label, QStringLiteral("Table items (eq_ref via PRIMARY)"));
    QCOMPARE(*loop.children[1].totalCost, 90.0);
    QCOMPARE(*loop.children[1].estimatedRows, 200.0);
    QCOMPARE(loop.children[0].warnings, QStringList { QStringLiteral("Full table scan") });
    QCOMPARE(loop.children[2].warnings, QStringList { QStringLiteral("Uses a join buffer") });
    QCOMPARE(detailOf(loop.children[2], QStringLiteral("Using join buffer")),
             QStringLiteral("hash join"));
}

void TstMysqlExplain::mysqlOrderingAndGrouping()
{
    const ExplainPlan plan = mysqlOk(kOrderGroup);
    QCOMPARE(plan.nodeCount, qsizetype(4));
    const ExplainNode &ordering = plan.root.children.at(0);
    QCOMPARE(ordering.label, QStringLiteral("Ordering (filesort)"));
    QCOMPARE(ordering.warnings, QStringList { QStringLiteral("Uses filesort") });
    const ExplainNode &grouping = ordering.children.at(0);
    QCOMPARE(grouping.label, QStringLiteral("Grouping (temporary table)"));
    QCOMPARE(grouping.warnings, QStringList { QStringLiteral("Uses a temporary table") });
    QCOMPARE(grouping.children.at(0).label, QStringLiteral("Table t (ALL)"));
}

void TstMysqlExplain::mysqlSubqueries()
{
    const ExplainPlan plan = mysqlOk(kSubquery);
    QCOMPARE(plan.nodeCount, qsizetype(4));
    const ExplainNode &table = plan.root.children.at(0);
    QCOMPARE(table.label, QStringLiteral("Table t (ALL)"));
    QCOMPARE(table.children.size(), size_t(1));
    QCOMPARE(table.children[0].label, QStringLiteral("Subquery #2"));
    QCOMPARE(table.children[0].children.at(0).label, QStringLiteral("Table u (index via PRIMARY)"));

    const ExplainPlan derived = mysqlOk(kDerived);
    QCOMPARE(derived.nodeCount, qsizetype(4));
    const ExplainNode &derivedTable = derived.root.children.at(0);
    QCOMPARE(derivedTable.label, QStringLiteral("Table <derived2> (ALL)"));
    QCOMPARE(derivedTable.children.at(0).label, QStringLiteral("Subquery #2"));
    QCOMPARE(derivedTable.children[0].children.at(0).label, QStringLiteral("Table u (ALL)"));
}

void TstMysqlExplain::mysqlUnion()
{
    const ExplainPlan plan = mysqlOk(kUnion);
    QCOMPARE(plan.nodeCount, qsizetype(6));
    QCOMPARE(plan.root.label, QStringLiteral("Query block"));
    const ExplainNode &result = plan.root.children.at(0);
    QCOMPARE(result.label, QStringLiteral("Union result (temporary table)"));
    QCOMPARE(detailOf(result, QStringLiteral("Table name")), QStringLiteral("<union1,2>"));
    QCOMPARE(result.children.size(), size_t(2));
    QCOMPARE(result.children[0].label, QStringLiteral("Union branch #1"));
    QCOMPARE(result.children[1].children.at(0).label, QStringLiteral("Table b (ALL)"));
}

void TstMysqlExplain::mysqlMalformedInputIsAnError()
{
    const QStringList bad = {
        QString(),
        QStringLiteral(" \n "),
        QStringLiteral("garbage"),
        QStringLiteral(R"json({"query_block": )json"),
        QStringLiteral("[]"),
        QStringLiteral("{}"),
        QStringLiteral(R"json({"foo": 1})json"),
        QStringLiteral(R"json({"query_block": 5})json"),
        QStringLiteral(R"json([{"query_block": {}}])json"),
    };
    for (const QString &text : bad) {
        const auto result = parseMysqlExplain(text, ExplainMode::Plan);
        QVERIFY2(!result, qPrintable(text));
        QCOMPARE(result.error().kind, ErrorKind::Syntax);
        QVERIFY(!result.error().message.isEmpty());
    }
    const auto invalid = parseMysqlExplain(QStringLiteral("{"), ExplainMode::Plan);
    QVERIFY(!invalid.error().detail.isEmpty());
}

void TstMysqlExplain::mysqlDepthLimit()
{
    QCOMPARE(mysqlOk(nested(QStringLiteral("ordering_operation"), 198)).nodeCount, qsizetype(200));
    const auto deep
        = parseMysqlExplain(nested(QStringLiteral("ordering_operation"), 199), ExplainMode::Plan);
    QVERIFY(!deep);
    QCOMPARE(deep.error().kind, ErrorKind::Syntax);
    // Far deeper than the JSON parser accepts: an error as well, not a crash.
    QVERIFY(
        !parseMysqlExplain(nested(QStringLiteral("ordering_operation"), 3000), ExplainMode::Plan));
}

void TstMysqlExplain::mysqlNodeLimit()
{
    const ExplainPlan plan = mysqlOk(manyTables(6000));
    QVERIFY(plan.truncated);
    QCOMPARE(plan.nodeCount, kMaxExplainNodes);
    const ExplainNode &loop = plan.root.children.at(0);
    QCOMPARE(loop.children.size(), size_t(kMaxExplainNodes - 2));
    QCOMPARE(loop.children.front().label, QStringLiteral("Table t0 (ALL)"));

    const ExplainPlan fits = mysqlOk(manyTables(kMaxExplainNodes - 2));
    QVERIFY(!fits.truncated);
    QCOMPARE(fits.nodeCount, kMaxExplainNodes);
}

void TstMysqlExplain::treeWithAnalyzeData()
{
    const ExplainPlan plan = mysqlOk(kTree, ExplainMode::Analyze);
    QCOMPARE(plan.engine, QStringLiteral("MySQL"));
    QCOMPARE(plan.rawText, kTree);
    QVERIFY(plan.analyzed);
    QCOMPARE(plan.nodeCount, qsizetype(3));
    QCOMPARE(plan.root.label, QStringLiteral("Nested loop inner join"));
    QVERIFY(plan.root.totalCost && plan.root.estimatedRows && plan.root.actualRows
            && plan.root.loops && plan.root.actualTimeMs);
    QCOMPARE(*plan.root.totalCost, 1.15);
    QCOMPARE(*plan.root.estimatedRows, 3.0);
    QCOMPARE(*plan.root.actualRows, 3.0);
    QCOMPARE(*plan.root.loops, 1.0);
    QCOMPARE(*plan.root.actualTimeMs, 0.080);
    QCOMPARE(detailOf(plan.root, QStringLiteral("Actual time per loop")),
             QStringLiteral("0.052..0.080 ms"));
    QVERIFY(!plan.root.startupCost);
    QCOMPARE(plan.root.children.size(), size_t(2));
    const ExplainNode &lookup = plan.root.children[1];
    QCOMPARE(lookup.label, QStringLiteral("Index lookup on b using idx_a (a_id=a.id)"));
    QCOMPARE(*lookup.loops, 3.0);
    QCOMPARE(*lookup.actualRows, 1.0);
    QCOMPARE(*lookup.actualTimeMs, 0.042); // 0.014 ms per loop, 3 loops
    QVERIFY(lookup.warnings.isEmpty());
}

void TstMysqlExplain::treeWarnings()
{
    const QString text = QStringLiteral(
        "-> Filter: (t.x > 1)  (cost=10.5 rows=1000) (actual time=0.100..5.000 rows=2 loops=1)\n"
        "    -> Table scan on t  (cost=10.5 rows=1000) (actual time=0.050..4.000 rows=1000 "
        "loops=1)\n"
        "    -> Index lookup on c using i (id=t.id)  (cost=0.5 rows=1) (never executed)\n");
    const ExplainPlan plan = mysqlOk(text, ExplainMode::Analyze);
    QCOMPARE(plan.root.warnings, QStringList { QStringLiteral("Row estimate off by 500x") });
    QCOMPARE(plan.root.children.at(0).warnings, QStringList { QStringLiteral("Full table scan") });
    const ExplainNode &never = plan.root.children.at(1);
    QCOMPARE(never.label, QStringLiteral("Index lookup on c using i (id=t.id)"));
    QVERIFY(never.loops);
    QCOMPARE(*never.loops, 0.0);
    QVERIFY(!never.actualRows && !never.actualTimeMs);
    QVERIFY(never.warnings.isEmpty());
}

void TstMysqlExplain::treeWithoutMetricsAndSeveralRoots()
{
    const QString text = QStringLiteral("-> Sort: t.a\n"
                                        "    -> Filter: (t.a > 1)\n"
                                        "        -> Table scan on t\n"
                                        "    -> Rows fetched before execution\r\n");
    const ExplainPlan plan = mysqlOk(text, ExplainMode::Plan);
    QCOMPARE(plan.nodeCount, qsizetype(4));
    QCOMPARE(plan.root.label, QStringLiteral("Sort: t.a"));
    QVERIFY(!plan.root.totalCost && !plan.root.estimatedRows && !plan.root.actualRows);
    QCOMPARE(plan.root.children.size(), size_t(2));
    QCOMPARE(plan.root.children[0].children.at(0).label, QStringLiteral("Table scan on t"));
    QCOMPARE(plan.root.children[1].label, QStringLiteral("Rows fetched before execution"));

    const ExplainPlan twoRoots = mysqlOk(QStringLiteral("-> One\n-> Two\n    -> Three\n"));
    QVERIFY(twoRoots.root.label.isEmpty());
    QCOMPARE(twoRoots.root.children.size(), size_t(2));
    QCOMPARE(twoRoots.nodeCount, qsizetype(3));
}

void TstMysqlExplain::treeMalformedAndLimits()
{
    const QStringList bad = {
        QStringLiteral("-> ok\nnot an arrow line"),
        QStringLiteral("-> ok\n    - missing arrow"),
    };
    for (const QString &text : bad) {
        const auto result = parseMysqlExplain(text, ExplainMode::Analyze);
        QVERIFY2(!result, qPrintable(text));
        QCOMPARE(result.error().kind, ErrorKind::Syntax);
    }

    const auto chain = [](int levels) {
        QStringList lines;
        for (int i = 0; i < levels; ++i)
            lines.append(QString(qsizetype(i) * 2, QLatin1Char(' '))
                         + QStringLiteral("-> step %1").arg(i));
        return lines.join(QLatin1Char('\n'));
    };
    QCOMPARE(mysqlOk(chain(200), ExplainMode::Analyze).nodeCount, qsizetype(200));
    QVERIFY(!parseMysqlExplain(chain(201), ExplainMode::Analyze));

    QStringList flood { QStringLiteral("-> root") };
    for (int i = 0; i < 6000; ++i)
        flood.append(QStringLiteral("    -> child %1").arg(i));
    const ExplainPlan plan = mysqlOk(flood.join(QLatin1Char('\n')), ExplainMode::Analyze);
    QVERIFY(plan.truncated);
    QCOMPARE(plan.nodeCount, kMaxExplainNodes);
    QCOMPARE(plan.root.children.size(), size_t(kMaxExplainNodes - 1));
    QCOMPARE(plan.root.children.front().label, QStringLiteral("child 0"));
}

void TstMysqlExplain::mariadbSimpleTable()
{
    const ExplainPlan plan = mariadbOk(kMariaSingle);
    QCOMPARE(plan.engine, QStringLiteral("MariaDB"));
    QCOMPARE(plan.rawText, kMariaSingle);
    QVERIFY(!plan.analyzed);
    QCOMPARE(plan.nodeCount, qsizetype(2));
    QCOMPARE(plan.root.label, QStringLiteral("Query block #1"));
    QVERIFY(!plan.root.totalCost); // MariaDB gives no cost: nullopt, not 0
    const ExplainNode &table = plan.root.children.at(0);
    QCOMPARE(table.label, QStringLiteral("Table t (ALL)"));
    QVERIFY(!table.totalCost && !table.startupCost);
    QVERIFY(table.estimatedRows);
    QCOMPARE(*table.estimatedRows, 3.0);
    QVERIFY(!table.actualRows && !table.actualTimeMs && !table.loops);
    QCOMPARE(table.warnings, QStringList { QStringLiteral("Full table scan") });
    QCOMPARE(detailOf(table, QStringLiteral("Filtered")), QStringLiteral("100"));
    QCOMPARE(detailOf(table, QStringLiteral("Attached condition")), QStringLiteral("t.b = 1"));

    const ExplainPlan ref = mariadbOk(kMariaRef);
    const ExplainNode &refTable = ref.root.children.at(0);
    QCOMPARE(refTable.label, QStringLiteral("Table t (ref via idx_b)"));
    QVERIFY(refTable.warnings.isEmpty());
    QCOMPARE(detailOf(refTable, QStringLiteral("Index condition")), QStringLiteral("t.c > 1"));
    QCOMPARE(detailOf(refTable, QStringLiteral("Used key parts")), QStringLiteral("b"));
}

void TstMysqlExplain::mariadbNestedLoopWithJoinBuffer()
{
    const ExplainPlan plan = mariadbOk(kMariaJoin);
    QCOMPARE(plan.nodeCount, qsizetype(5));
    const ExplainNode &loop = plan.root.children.at(0);
    QCOMPARE(loop.label, QStringLiteral("Nested loop"));
    QCOMPARE(loop.children.size(), size_t(2));
    QCOMPARE(loop.children[0].label, QStringLiteral("Table a (ALL)"));
    QCOMPARE(*loop.children[0].estimatedRows, 10.0);
    const ExplainNode &bnl = loop.children[1];
    QCOMPARE(bnl.label, QStringLiteral("Block nested loop join"));
    QCOMPARE(bnl.warnings, QStringList { QStringLiteral("Uses a join buffer") });
    QCOMPARE(detailOf(bnl, QStringLiteral("Buffer type")), QStringLiteral("flat"));
    QCOMPARE(detailOf(bnl, QStringLiteral("Join type")), QStringLiteral("BNL"));
    QCOMPARE(bnl.children.at(0).label, QStringLiteral("Table b (ALL)"));
    QCOMPARE(*bnl.children[0].estimatedRows, 20.0);
}

void TstMysqlExplain::mariadbFilesortAndTemporaryTable()
{
    const ExplainPlan plan = mariadbOk(kMariaFilesort);
    QCOMPARE(plan.nodeCount, qsizetype(4));
    const ExplainNode &sort = plan.root.children.at(0);
    QCOMPARE(sort.label, QStringLiteral("Filesort"));
    QCOMPARE(sort.warnings, QStringList { QStringLiteral("Uses filesort") });
    QCOMPARE(detailOf(sort, QStringLiteral("Sort key")), QStringLiteral("t.a"));
    const ExplainNode &temporary = sort.children.at(0);
    QCOMPARE(temporary.label, QStringLiteral("Temporary table"));
    QCOMPARE(temporary.warnings, QStringList { QStringLiteral("Uses a temporary table") });
    QCOMPARE(temporary.children.at(0).label, QStringLiteral("Table t (ALL)"));
}

void TstMysqlExplain::mariadbAnalyze()
{
    const ExplainPlan plan = mariadbOk(kMariaAnalyze, ExplainMode::Analyze);
    QVERIFY(plan.analyzed);
    QVERIFY(plan.root.actualTimeMs && plan.root.loops);
    QCOMPARE(*plan.root.actualTimeMs, 1.234);
    QCOMPARE(*plan.root.loops, 1.0);
    const ExplainNode &table = plan.root.children.at(0);
    QVERIFY(table.actualRows && table.actualTimeMs && table.estimatedRows && table.loops);
    QCOMPARE(*table.estimatedRows, 1000.0);
    QCOMPARE(*table.actualRows, 3.0); // r_rows
    QCOMPARE(*table.actualTimeMs, 0.8); // r_total_time_ms
    QCOMPARE(*table.loops, 1.0);
    QVERIFY(!table.totalCost);
    QCOMPARE(detailOf(table, QStringLiteral("R filtered")), QStringLiteral("0.3"));
    QVERIFY(table.warnings.contains(QStringLiteral("Row estimate off by 333x")));
    QVERIFY(table.warnings.contains(QStringLiteral("Full table scan")));
}

void TstMysqlExplain::mariadbUnionAndSubqueries()
{
    const ExplainPlan plan = mariadbOk(kMariaUnion);
    QCOMPARE(plan.nodeCount, qsizetype(6));
    const ExplainNode &result = plan.root.children.at(0);
    QCOMPARE(result.label, QStringLiteral("Union result"));
    QCOMPARE(result.children.size(), size_t(2));
    QCOMPARE(result.children[0].label, QStringLiteral("Union branch #1"));
    QCOMPARE(result.children[1].children.at(0).label, QStringLiteral("Table b (ALL)"));

    const ExplainPlan sub = mariadbOk(kMariaSubquery);
    QCOMPARE(sub.nodeCount, qsizetype(5));
    QCOMPARE(sub.root.children.size(), size_t(2));
    QCOMPARE(sub.root.children[0].label, QStringLiteral("Table t (ALL)"));
    const ExplainNode &cache = sub.root.children[1];
    QCOMPARE(cache.label, QStringLiteral("Expression cache"));
    QCOMPARE(detailOf(cache, QStringLiteral("State")), QString()); // not a listed property
    QCOMPARE(cache.children.at(0).label, QStringLiteral("Subquery #2"));
    QCOMPARE(cache.children[0].children.at(0).label, QStringLiteral("Table u (index via PRIMARY)"));
}

void TstMysqlExplain::mariadbMalformedInputIsAnError()
{
    const QStringList bad = {
        QString(),
        QStringLiteral("  "),
        QStringLiteral("-> a tree is MySQL's, not MariaDB's"),
        QStringLiteral(R"json({"query_block": )json"),
        QStringLiteral("[]"),
        QStringLiteral(R"json({"foo": 1})json"),
        QStringLiteral(R"json({"query_block": "x"})json"),
    };
    for (const QString &text : bad) {
        const auto result = parseMariadbExplain(text, ExplainMode::Plan);
        QVERIFY2(!result, qPrintable(text));
        QCOMPARE(result.error().kind, ErrorKind::Syntax);
        QVERIFY(!result.error().message.isEmpty());
    }
    QVERIFY(!parseMariadbExplain(QStringLiteral("{"), ExplainMode::Plan).error().detail.isEmpty());
}

void TstMysqlExplain::mariadbLimits()
{
    QCOMPARE(mariadbOk(nested(QStringLiteral("filesort"), 198)).nodeCount, qsizetype(200));
    const auto deep
        = parseMariadbExplain(nested(QStringLiteral("filesort"), 199), ExplainMode::Plan);
    QVERIFY(!deep);
    QCOMPARE(deep.error().kind, ErrorKind::Syntax);
    QVERIFY(!parseMariadbExplain(nested(QStringLiteral("filesort"), 3000), ExplainMode::Plan));

    const ExplainPlan plan = mariadbOk(manyTables(6000));
    QVERIFY(plan.truncated);
    QCOMPARE(plan.nodeCount, kMaxExplainNodes);
    QCOMPARE(plan.root.children.at(0).children.size(), size_t(kMaxExplainNodes - 2));
}

QTEST_GUILESS_MAIN(TstMysqlExplain)
#include "tst_mysql_explain.moc"
