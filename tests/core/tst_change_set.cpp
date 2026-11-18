#include <quarry/change_set.h>

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

EditTarget target(QStringList key = { QStringLiteral("id") },
                  RowKey::Kind kind = RowKey::Kind::PrimaryKey)
{
    EditTarget t;
    t.table = ObjectPath { { QStringLiteral("db"), QStringLiteral("public"), QStringLiteral("t") },
                           ObjectKind::Table };
    t.columns = { QStringLiteral("id"), QStringLiteral("a"), QStringLiteral("we\"ird col") };
    t.key = RowKey { kind, std::move(key) };
    return t;
}

RowEdit edit(RowEdit::Kind kind, QVariantList key, std::vector<CellChange> cells = {})
{
    RowEdit e;
    e.kind = kind;
    e.key = std::move(key);
    e.cells = std::move(cells);
    return e;
}

} // namespace

class TstChangeSet : public QObject
{
    Q_OBJECT

private slots:
    void updateNumbered()
    {
        const FakeDialect d(true);
        auto r
            = ChangeSqlBuilder::build(d, target(),
                                      { edit(RowEdit::Kind::Update, { 7 },
                                             { { 1, QStringLiteral("x") }, { 2, QVariant() } }) });
        QVERIFY(r);
        const auto &p = r.value().at(0);
        QCOMPARE(p.statement.sql,
                 QStringLiteral("UPDATE \"public\".\"t\" SET \"a\" = $1, \"we\"\"ird col\" = $2 "
                                "WHERE \"id\" = $3"));
        QCOMPARE(p.statement.parameters.size(), 3);
        QCOMPARE(p.statement.parameters.value(0), QVariant(QStringLiteral("x")));
        QVERIFY(p.statement.parameters.value(1).isNull());
        QCOMPARE(p.statement.parameters.value(2), QVariant(7));
        QVERIFY(p.expectOneRow);
        QCOMPARE(p.editIndex, 0);
    }
    void updateQuestionMarks()
    {
        const FakeDialect d(false);
        auto r = ChangeSqlBuilder::build(
            d, target(), { edit(RowEdit::Kind::Update, { 7 }, { { 1, QStringLiteral("x") } }) });
        QVERIFY(r);
        QCOMPARE(r.value().at(0).statement.sql,
                 QStringLiteral("UPDATE \"public\".\"t\" SET \"a\" = ? WHERE \"id\" = ?"));
    }
    void compositeKeyAndRowid()
    {
        const FakeDialect d(true);
        auto r
            = ChangeSqlBuilder::build(d, target({ QStringLiteral("id"), QStringLiteral("a") }),
                                      { edit(RowEdit::Kind::Delete, { 1, QStringLiteral("k") }) });
        QVERIFY(r);
        QCOMPARE(r.value().at(0).statement.sql,
                 QStringLiteral("DELETE FROM \"public\".\"t\" WHERE \"id\" = $1 AND \"a\" = $2"));
        QCOMPARE(r.value().at(0).statement.parameters.size(), 2);
        QVERIFY(r.value().at(0).expectOneRow);

        auto rid = ChangeSqlBuilder::build(
            d, target({ QStringLiteral("rowid") }, RowKey::Kind::RowIdentifier),
            { edit(RowEdit::Kind::Update, { 42 }, { { 0, 1 } }) });
        QVERIFY(rid);
        QCOMPARE(rid.value().at(0).statement.sql,
                 QStringLiteral("UPDATE \"public\".\"t\" SET \"id\" = $1 WHERE \"rowid\" = $2"));
    }
    void insertAndDefaultValues()
    {
        const FakeDialect d(true);
        auto r = ChangeSqlBuilder::build(
            d, target(),
            { edit(RowEdit::Kind::Insert, {}, { { 0, 1 }, { 1, QVariant() } }),
              edit(RowEdit::Kind::Insert, {}) });
        QVERIFY(r);
        QCOMPARE(r.value().size(), size_t(2));
        QCOMPARE(r.value().at(0).statement.sql,
                 QStringLiteral("INSERT INTO \"public\".\"t\" (\"id\", \"a\") VALUES ($1, $2)"));
        QVERIFY(!r.value().at(0).expectOneRow);
        QCOMPARE(r.value().at(1).statement.sql,
                 QStringLiteral("INSERT INTO \"public\".\"t\" DEFAULT VALUES"));
        QVERIFY(r.value().at(1).statement.parameters.isEmpty());
        QCOMPARE(r.value().at(1).editIndex, 1);
    }
    void numberingRestartsPerStatement()
    {
        const FakeDialect d(true);
        auto r = ChangeSqlBuilder::build(
            d, target(),
            { edit(RowEdit::Kind::Delete, { 1 }), edit(RowEdit::Kind::Delete, { 2 }) });
        QVERIFY(r);
        QCOMPARE(r.value().at(1).statement.sql,
                 QStringLiteral("DELETE FROM \"public\".\"t\" WHERE \"id\" = $1"));
    }
    void errors_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<QVariantList>("key");
        QTest::addColumn<int>("column"); // -1: no cell at all
        QTest::addColumn<bool>("emptyKey");
        QTest::newRow("null key") << int(RowEdit::Kind::Delete) << QVariantList { QVariant() } << -1
                                  << false;
        QTest::newRow("missing key")
            << int(RowEdit::Kind::Delete) << QVariantList {} << -1 << false;
        QTest::newRow("empty key columns")
            << int(RowEdit::Kind::Delete) << QVariantList { 1 } << -1 << true;
        QTest::newRow("update no cells")
            << int(RowEdit::Kind::Update) << QVariantList { 1 } << -1 << false;
        QTest::newRow("column too big")
            << int(RowEdit::Kind::Update) << QVariantList { 1 } << 3 << false;
        QTest::newRow("column negative")
            << int(RowEdit::Kind::Insert) << QVariantList {} << -2 << false;
    }
    void errors()
    {
        QFETCH(int, kind);
        QFETCH(QVariantList, key);
        QFETCH(int, column);
        QFETCH(bool, emptyKey);
        const FakeDialect d(true);
        std::vector<CellChange> cells;
        if (column != -1)
            cells.push_back({ column, 1 });
        auto r = ChangeSqlBuilder::build(d, emptyKey ? target(QStringList {}) : target(),
                                         { edit(static_cast<RowEdit::Kind>(kind), key, cells) });
        QVERIFY(!r);
        QCOMPARE(std::move(r).error().kind, ErrorKind::Internal);
    }
    void emptyEditsGiveEmptyPlan()
    {
        const FakeDialect d(true);
        auto r = ChangeSqlBuilder::build(d, target(), {});
        QVERIFY(r);
        QVERIFY(r.value().empty());
    }
};

QTEST_GUILESS_MAIN(TstChangeSet)
#include "tst_change_set.moc"
