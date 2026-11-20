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
        q.sort = SortKey { QStringLiteral("we\"ird"), true };
        auto s = TableQueryBuilder::select(FakeDialect(true), q);
        QVERIFY(s);
        QCOMPARE(s->sql, QStringLiteral("SELECT * FROM \"public\".\"t\" ORDER BY \"we\"\"ird\" DESC"));
        q.sort->descending = false;
        s = TableQueryBuilder::select(FakeDialect(true), q);
        QVERIFY(s->sql.endsWith(QStringLiteral(" ASC")));
    }

    void filtersAreParameters_data()
    {
        QTest::addColumn<bool>("numbered");
        QTest::addColumn<QString>("expected");
        QTest::newRow("numbered") << true
                                  << QStringLiteral("SELECT * FROM \"public\".\"t\" WHERE \"a\" = $1 "
                                                    "AND \"b\" >= $2 AND \"c\" IS NULL ORDER BY "
                                                    "\"a\" ASC");
        QTest::newRow("question marks")
            << false
            << QStringLiteral("SELECT * FROM \"public\".\"t\" WHERE \"a\" = ? AND \"b\" >= ? AND "
                              "\"c\" IS NULL ORDER BY \"a\" ASC");
    }

    void filtersAreParameters()
    {
        QFETCH(bool, numbered);
        QFETCH(QString, expected);
        auto q = query();
        q.filters = { filter(QStringLiteral("a"), ColumnFilter::Op::Equals,
                             QStringLiteral("x'; DROP TABLE t; --")),
                      filter(QStringLiteral("b"), ColumnFilter::Op::GreaterOrEqual,
                             QStringLiteral("10")),
                      filter(QStringLiteral("c"), ColumnFilter::Op::IsNull,
                             QStringLiteral("ignored")) };
        q.sort = SortKey { QStringLiteral("a"), false };
        const auto s = TableQueryBuilder::select(FakeDialect(numbered), q);
        QVERIFY(s);
        QCOMPARE(s->sql, expected);
        QCOMPARE(s->parameters,
                 (QVariantList { QStringLiteral("x'; DROP TABLE t; --"), QStringLiteral("10") }));
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
        QCOMPARE(TableQueryBuilder::likePattern(u"50%_off!", false), QStringLiteral("%50!%!_off!!%"));
        QCOMPARE(TableQueryBuilder::likePattern(u"ab", true), QStringLiteral("ab%"));
        QCOMPARE(TableQueryBuilder::likePattern(u"", false), QStringLiteral("%%"));

        auto q = query();
        q.filters = { filter(QStringLiteral("a"), ColumnFilter::Op::Contains, QStringLiteral("5%")) };
        const auto s = TableQueryBuilder::select(FakeDialect(true), q);
        QVERIFY(s);
        QCOMPARE(s->parameters, QVariantList { QStringLiteral("%5!%%") });
    }

    void countKeepsFiltersAndDropsOrder()
    {
        auto q = query();
        q.hiddenColumn = QStringLiteral("rowid");
        q.filters = { filter(QStringLiteral("a"), ColumnFilter::Op::NotEquals, QStringLiteral("1")) };
        q.sort = SortKey { QStringLiteral("a"), true };
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
        q.sort = SortKey {};
        QVERIFY(!TableQueryBuilder::select(FakeDialect(true), q));
        q = query();
        q.hiddenColumn = QString();
        QVERIFY(!TableQueryBuilder::select(FakeDialect(true), q));
    }
};

QTEST_GUILESS_MAIN(TstTableQuery)
#include "tst_table_query.moc"
