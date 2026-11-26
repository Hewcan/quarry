#include <quarry/table_query.h>

#include <QtTest>

using namespace quarry;

namespace {

// Placeholder style and identifier quoting are the only things the builder asks of a dialect.
class FakeDialect : public ISqlDialect
{
public:
    explicit FakeDialect(bool numbered)
        : m_numbered(numbered)
    { }
    QString quoteIdentifier(QStringView name) const override
    {
        return QLatin1Char('"') + name.toString().replace(QLatin1Char('"'), QLatin1String("\"\""))
            + QLatin1Char('"');
    }
    QString quoteLiteral(QStringView text) const override { return text.toString(); }
    QString highlightingDefinition() const override { return {}; }
    QStringList keywords() const override { return {}; }
    QStringList builtinFunctions() const override { return {}; }
    std::vector<StatementRange> splitStatements(QStringView) const override { return {}; }
    QString limitClause(qint64, qint64) const override { return {}; }
    QString placeholder(int index) const override
    {
        return m_numbered ? QStringLiteral("$%1").arg(index) : QStringLiteral("?");
    }
    QString qualifiedName(const ObjectPath &table) const override
    {
        QStringList quoted;
        for (const QString &part : table.parts.mid(1))
            quoted << quoteIdentifier(part);
        return quoted.join(QLatin1Char('.'));
    }
    std::optional<QString> rowIdentifier() const override { return QStringLiteral("rowid"); }

private:
    bool m_numbered;
};

TableQuery query()
{
    TableQuery q;
    q.table = ObjectPath { { QStringLiteral("db"), QStringLiteral("public"), QStringLiteral("t") },
                           ObjectKind::Table };
    return q;
}

ColumnFilter filter(const QString &column, ColumnFilter::Op op, const QString &value = {})
{
    return ColumnFilter { column, op, value };
}

} // namespace

class TstTableQuery : public QObject
{
    Q_OBJECT

private slots:
    void plainSelect()
    {
        const auto s = TableQueryBuilder::select(FakeDialect(true), query());
        QVERIFY(s);
        QCOMPARE(s->sql, QStringLiteral("SELECT * FROM \"public\".\"t\""));
        QVERIFY(s->parameters.isEmpty());
    }

    void hiddenColumnGoesFirst()
    {
        auto q = query();
        q.hiddenColumn = QStringLiteral("ctid");
        const auto s = TableQueryBuilder::select(FakeDialect(true), q);
        QVERIFY(s);
        QCOMPARE(s->sql, QStringLiteral("SELECT \"ctid\", * FROM \"public\".\"t\""));
    }

    void sortQuotesTheColumn()
    {
        auto q = query();
        q.sorts = { SortKey { QStringLiteral("we\"ird"), true } };
        auto s = TableQueryBuilder::select(FakeDialect(true), q);
        QVERIFY(s);
        const QString expected = QStringLiteral("SELECT * FROM \"public\".\"t\" ORDER BY ")
            + QStringLiteral("\"we\"\"ird\" DESC");
        QCOMPARE(s->sql, expected);
        q.sorts = { SortKey { QStringLiteral("we\"ird"), false } };
        s = TableQueryBuilder::select(FakeDialect(true), q);
        QVERIFY(s->sql.endsWith(QStringLiteral(" ASC")));
    }

    void multiColumnSort()
    {
        auto q = query();
        q.sorts = { SortKey { QStringLiteral("a"), false }, SortKey { QStringLiteral("b"), true },
                    SortKey { QStringLiteral("we\"ird"), false } };
        const auto s = TableQueryBuilder::select(FakeDialect(true), q);
        QVERIFY(s);
        QCOMPARE(s->sql,
                 QStringLiteral("SELECT * FROM \"public\".\"t\" ORDER BY \"a\" ASC, \"b\" DESC, "
                                "\"we\"\"ird\" ASC"));
    }

    void filtersAreParameters_data()
    {
        QTest::addColumn<bool>("numbered");
        QTest::addColumn<QString>("expected");
        const QString head = QStringLiteral("SELECT * FROM \"public\".\"t\" WHERE ");
        const QString tail = QStringLiteral(" AND \"c\" IS NULL ORDER BY \"a\" ASC");
        const QString numbered = QStringLiteral("\"a\" = $1 AND \"b\" >= $2");
        const QString marks = QStringLiteral("\"a\" = ? AND \"b\" >= ?");
        QTest::newRow("numbered") << true << head + numbered + tail;
        QTest::newRow("question marks") << false << head + marks + tail;
    }

    void filtersAreParameters()
    {
        QFETCH(bool, numbered);
        QFETCH(QString, expected);
        const QString attack = QStringLiteral("x'; DROP TABLE t; --");
        auto q = query();
        q.filters.push_back(filter(QStringLiteral("a"), ColumnFilter::Op::Equals, attack));
        q.filters.push_back(
            filter(QStringLiteral("b"), ColumnFilter::Op::GreaterOrEqual, QStringLiteral("10")));
        q.filters.push_back(
            filter(QStringLiteral("c"), ColumnFilter::Op::IsNull, QStringLiteral("ignored")));
        q.sorts = { SortKey { QStringLiteral("a"), false } };
        const auto s = TableQueryBuilder::select(FakeDialect(numbered), q);
        QVERIFY(s);
        QCOMPARE(s->sql, expected);
        QCOMPARE(s->parameters, (QVariantList { attack, QStringLiteral("10") }));
    }

    void everyComparison()
    {
        const std::pair<ColumnFilter::Op, QString> ops[] = {
            { ColumnFilter::Op::Equals, QStringLiteral("\"a\" = $1") },
            { ColumnFilter::Op::NotEquals, QStringLiteral("\"a\" <> $1") },
            { ColumnFilter::Op::Less, QStringLiteral("\"a\" < $1") },
            { ColumnFilter::Op::LessOrEqual, QStringLiteral("\"a\" <= $1") },
            { ColumnFilter::Op::Greater, QStringLiteral("\"a\" > $1") },
            { ColumnFilter::Op::GreaterOrEqual, QStringLiteral("\"a\" >= $1") },
            { ColumnFilter::Op::IsNotNull, QStringLiteral("\"a\" IS NOT NULL") },
            { ColumnFilter::Op::Contains, QStringLiteral("\"a\" LIKE $1 ESCAPE '!'") },
            { ColumnFilter::Op::StartsWith, QStringLiteral("\"a\" LIKE $1 ESCAPE '!'") },
        };
        for (const auto &[op, term] : ops) {
            auto q = query();
            q.filters = { filter(QStringLiteral("a"), op, QStringLiteral("v")) };
            const auto s = TableQueryBuilder::select(FakeDialect(true), q);
            QVERIFY(s);
            QCOMPARE(s->sql, QStringLiteral("SELECT * FROM \"public\".\"t\" WHERE ") + term);
        }
    }

    void likePatternEscapesWildcards()
    {
        const QString escaped = TableQueryBuilder::likePattern(u"50%_off!", false);
        QCOMPARE(escaped, QStringLiteral("%50!%!_off!!%"));
        QCOMPARE(TableQueryBuilder::likePattern(u"ab", true), QStringLiteral("ab%"));
        QCOMPARE(TableQueryBuilder::likePattern(QStringView(), false), QStringLiteral("%%"));

        auto q = query();
        q.filters.push_back(
            filter(QStringLiteral("a"), ColumnFilter::Op::Contains, QStringLiteral("5%")));
        const auto s = TableQueryBuilder::select(FakeDialect(true), q);
        QVERIFY(s);
        QCOMPARE(s->parameters, QVariantList { QStringLiteral("%5!%%") });
    }

    void countKeepsFiltersAndDropsOrder()
    {
        auto q = query();
        q.hiddenColumn = QStringLiteral("rowid");
        q.filters.push_back(
            filter(QStringLiteral("a"), ColumnFilter::Op::NotEquals, QStringLiteral("1")));
        q.sorts = { SortKey { QStringLiteral("a"), true } };
        const auto s = TableQueryBuilder::count(FakeDialect(false), q);
        QVERIFY(s);
        QCOMPARE(s->sql, QStringLiteral("SELECT COUNT(*) FROM \"public\".\"t\" WHERE \"a\" <> ?"));
        QCOMPARE(s->parameters, QVariantList { QStringLiteral("1") });
    }

    void emptyNamesAreErrors()
    {
        auto q = query();
        q.filters = { filter({}, ColumnFilter::Op::Equals, QStringLiteral("1")) };
        QVERIFY(!TableQueryBuilder::select(FakeDialect(true), q));
        QVERIFY(!TableQueryBuilder::count(FakeDialect(true), q));
        q = query();
        q.sorts = { SortKey {} };
        QVERIFY(!TableQueryBuilder::select(FakeDialect(true), q));
        q = query();
        q.hiddenColumn = QString();
        QVERIFY(!TableQueryBuilder::select(FakeDialect(true), q));
    }

    void validateWhereAccepts_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        const auto row = [](const char *name, const QString &in, const QString &out) {
            QTest::newRow(name) << in << out;
        };
        const QString nested = QStringLiteral("a > 1 AND (b = 'x' OR c IS NULL)");
        row("nested", QStringLiteral("  ") + nested + QStringLiteral(" \n"), nested);
        row("semicolonInLiteral", QStringLiteral("a = 'a;b'"), QStringLiteral("a = 'a;b'"));
        row("dashesInLiteral", QStringLiteral("a = 'x--y'"), QStringLiteral("a = 'x--y'"));
        row("slashStarInLiteral", QStringLiteral("a = '/*'"), QStringLiteral("a = '/*'"));
        row("questionInLiteral", QStringLiteral("a = 'is it?'"), QStringLiteral("a = 'is it?'"));
        row("escapedQuote", QStringLiteral("a = 'it''s;'"), QStringLiteral("a = 'it''s;'"));
        row("dollarInLiteral", QStringLiteral("a = '$1'"), QStringLiteral("a = '$1'"));
        row("parenInLiteral", QStringLiteral("a = ')('"), QStringLiteral("a = ')('"));
        row("weirdIdentifier", QStringLiteral("\"we\"\"ird\" = 1"),
            QStringLiteral("\"we\"\"ird\" = 1"));
        row("backtickIdentifier", QStringLiteral("`a;b` = 1"), QStringLiteral("`a;b` = 1"));
        row("postgresCast", QStringLiteral("a::int > 1"), QStringLiteral("a::int > 1"));
        row("blank", QStringLiteral(" \t\n"), QString());
        row("empty", QString(), QString());
    }

    void validateWhereAccepts()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        const auto r = TableQueryBuilder::validateWhere(input);
        QVERIFY(r);
        QCOMPARE(*r, expected);
    }

    void validateWhereRejects_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<int>("position");
        QTest::newRow("semicolon") << QStringLiteral("a = 1; DROP TABLE t") << 5;
        QTest::newRow("lineComment") << QStringLiteral(" a = 1 -- x") << 7;
        QTest::newRow("blockComment") << QStringLiteral("a = 1 /* x */") << 6;
        QTest::newRow("question") << QStringLiteral("a = ?") << 4;
        QTest::newRow("dollarDigit") << QStringLiteral("a = $1") << 4;
        QTest::newRow("strayClose") << QStringLiteral("a = 1)") << 5;
        QTest::newRow("unclosedOpen") << QStringLiteral("(a = 1") << 0;
        QTest::newRow("unclosedLiteral") << QStringLiteral("a = 'x") << 4;
        QTest::newRow("unclosedIdentifier") << QStringLiteral("\"a = 1") << 0;
        QTest::newRow("unclosedBacktick") << QStringLiteral("x = 1 AND `a") << 10;
        QTest::newRow("nul") << QString(QStringLiteral("a = 'x") + QChar(0) + QLatin1Char('\''))
                             << 6;
    }

    void validateWhereRejects()
    {
        QFETCH(QString, input);
        QFETCH(int, position);
        const auto r = TableQueryBuilder::validateWhere(input);
        QVERIFY(!r);
        QCOMPARE(r.error().kind, ErrorKind::Syntax);
        QVERIFY(!r.error().message.isEmpty());
        QCOMPARE(r.error().position.value_or(-1), position);
    }

    void validateWhereMessagesDiffer()
    {
        QSet<QString> messages;
        const QStringList inputs { QStringLiteral("a;"),  QStringLiteral("a --"),
                                   QStringLiteral("a ?"), QStringLiteral("a $1"),
                                   QStringLiteral("a)"),  QStringLiteral("(a"),
                                   QStringLiteral("'a") };
        for (const QString &in : inputs) {
            const auto r = TableQueryBuilder::validateWhere(in);
            QVERIFY(!r);
            messages.insert(r.error().message);
        }
        QCOMPARE(messages.size(), inputs.size());
    }

    void expressionJoinsFiltersAndPrecedesOrder()
    {
        auto q = query();
        q.filters.push_back(
            filter(QStringLiteral("a"), ColumnFilter::Op::Equals, QStringLiteral("1")));
        q.whereExpression = QStringLiteral(" b > 2 OR c IS NULL ");
        q.sorts = { SortKey { QStringLiteral("a"), false } };
        const auto s = TableQueryBuilder::select(FakeDialect(true), q);
        QVERIFY(s);
        const QString expected = QStringLiteral("SELECT * FROM \"public\".\"t\" WHERE \"a\" = $1")
            + QStringLiteral(" AND (b > 2 OR c IS NULL) ORDER BY \"a\" ASC");
        QCOMPARE(s->sql, expected);
        QCOMPARE(s->parameters, QVariantList { QStringLiteral("1") });
        const auto n = TableQueryBuilder::count(FakeDialect(true), q);
        QVERIFY(n);
        const QString expectedCount = QStringLiteral("SELECT COUNT(*) FROM \"public\".\"t\" ")
            + QStringLiteral("WHERE \"a\" = $1 AND (b > 2 OR c IS NULL)");
        QCOMPARE(n->sql, expectedCount);
    }

    void expressionAloneAndBlank()
    {
        auto q = query();
        q.whereExpression = QStringLiteral("a > 1");
        auto s = TableQueryBuilder::select(FakeDialect(true), q);
        QVERIFY(s);
        QCOMPARE(s->sql, QStringLiteral("SELECT * FROM \"public\".\"t\" WHERE (a > 1)"));
        q.whereExpression = QStringLiteral("   ");
        s = TableQueryBuilder::select(FakeDialect(true), q);
        QVERIFY(s);
        QCOMPARE(s->sql, QStringLiteral("SELECT * FROM \"public\".\"t\""));
    }

    void invalidExpressionGivesNoSql()
    {
        auto q = query();
        q.whereExpression = QStringLiteral("a = 1; DROP TABLE t");
        const auto s = TableQueryBuilder::select(FakeDialect(true), q);
        QVERIFY(!s);
        QCOMPARE(s.error().kind, ErrorKind::Syntax);
        const auto n = TableQueryBuilder::count(FakeDialect(true), q);
        QVERIFY(!n);
        QCOMPARE(n.error().kind, ErrorKind::Syntax);
    }
};

QTEST_GUILESS_MAIN(TstTableQuery)
#include "tst_table_query.moc"
