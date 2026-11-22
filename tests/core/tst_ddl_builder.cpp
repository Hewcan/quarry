#include <quarry/ddl_builder.h>

#include <QtTest>

using namespace quarry;

namespace {

class FakeDialect : public ISqlDialect
{
public:
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
    QString placeholder(int) const override { return QStringLiteral("?"); }
    QString qualifiedName(const ObjectPath &table) const override
    {
        QStringList quoted;
        for (const QString &part : table.parts.mid(1))
            quoted << quoteIdentifier(part);
        return quoted.join(QLatin1Char('.'));
    }
    std::optional<QString> rowIdentifier() const override { return std::nullopt; }
};

ObjectPath path(const QString &schema, const QString &name)
{
    return ObjectPath { { QStringLiteral("db"), schema, name }, ObjectKind::Table };
}

ObjectPath tablePath()
{
    // Two parts: the fake dialect drops the first (the database), so the name is just "t".
    return ObjectPath { { QStringLiteral("db"), QStringLiteral("t") }, ObjectKind::Table };
}

ColumnDef column(const QString &name, const QString &type, bool nullable = true,
                 const QString &def = {})
{
    ColumnDef c;
    c.name = name;
    c.nativeType = type;
    c.nullable = nullable;
    c.defaultExpression = def;
    return c;
}

QString build(const TableDetails &d, bool isView = false)
{
    return DdlBuilder::build(FakeDialect(), tablePath(), d, isView);
}

QString body(const TableDetails &d)
{
    const QString header = DdlBuilder::header() + QStringLiteral("\n\n");
    const QString text = build(d);
    return text.startsWith(header) ? text.mid(header.size()) : text;
}

TableDetails simple()
{
    TableDetails d;
    d.columns.push_back(column(QStringLiteral("id"), QStringLiteral("integer")));
    d.columns.push_back(column(QStringLiteral("name"), QStringLiteral("text")));
    return d;
}

} // namespace

class TstDdlBuilder : public QObject
{
    Q_OBJECT

private slots:
    void headerIsFirstLine()
    {
        const QString text = build(simple());
        QVERIFY(text.startsWith(DdlBuilder::header() + QStringLiteral("\n\n")));
        QVERIFY(DdlBuilder::header().startsWith(QStringLiteral("-- Reconstructed by Quarry")));
        QVERIFY(!DdlBuilder::header().contains(QLatin1Char('\n')));
    }

    void simplePrimaryKey()
    {
        auto d = simple();
        d.primaryKey = QStringList { QStringLiteral("id") };
        QCOMPARE(body(d),
                 QStringLiteral("CREATE TABLE \"t\" (\n    \"id\" integer,\n    \"name\" text,\n"
                                "    PRIMARY KEY (\"id\")\n);\n"));
    }

    void compositePrimaryKey()
    {
        auto d = simple();
        d.primaryKey = QStringList { QStringLiteral("id"), QStringLiteral("name") };
        QVERIFY(body(d).contains(QStringLiteral("    PRIMARY KEY (\"id\", \"name\")\n);")));
    }

    void noPrimaryKey()
    {
        QCOMPARE(
            body(simple()),
            QStringLiteral("CREATE TABLE \"t\" (\n    \"id\" integer,\n    \"name\" text\n);\n"));
    }

    void notNullAndDefault()
    {
        TableDetails d;
        d.columns.push_back(column(QStringLiteral("a"), QStringLiteral("int"), false));
        d.columns.push_back(
            column(QStringLiteral("b"), QStringLiteral("text"), true, QStringLiteral("'x'::text")));
        d.columns.push_back(column(QStringLiteral("c"), QStringLiteral("int"), false,
                                   QStringLiteral("nextval('s')")));
        QCOMPARE(body(d),
                 QStringLiteral("CREATE TABLE \"t\" (\n    \"a\" int NOT NULL,\n"
                                "    \"b\" text DEFAULT 'x'::text,\n"
                                "    \"c\" int NOT NULL DEFAULT nextval('s')\n);\n"));
    }

    void foreignKeyWithoutName()
    {
        auto d = simple();
        d.foreignKeys.push_back(ForeignKeyDef { {},
                                                { QStringLiteral("id") },
                                                path(QStringLiteral("public"), QStringLiteral("o")),
                                                { QStringLiteral("oid") } });
        QVERIFY(body(d).contains(QStringLiteral("    \"name\" text,\n    FOREIGN KEY (\"id\") "
                                                "REFERENCES \"public\".\"o\" (\"oid\")\n);")));
        QVERIFY(!body(d).contains(QStringLiteral("CONSTRAINT")));
    }

    void foreignKeyWithName()
    {
        auto d = simple();
        d.foreignKeys.push_back(ForeignKeyDef { QStringLiteral("fk_o"),
                                                { QStringLiteral("id") },
                                                path(QStringLiteral("public"), QStringLiteral("o")),
                                                { QStringLiteral("oid") } });
        QVERIFY(body(d).contains(QStringLiteral("    CONSTRAINT \"fk_o\" FOREIGN KEY (\"id\") "
                                                "REFERENCES \"public\".\"o\" (\"oid\")")));
    }

    void compositeForeignKeyOtherSchema()
    {
        auto d = simple();
        d.foreignKeys.push_back(ForeignKeyDef { QStringLiteral("fk"),
                                                { QStringLiteral("id"), QStringLiteral("name") },
                                                path(QStringLiteral("other"), QStringLiteral("p")),
                                                { QStringLiteral("a"), QStringLiteral("b") } });
        QVERIFY(body(d).contains(
            QStringLiteral("CONSTRAINT \"fk\" FOREIGN KEY (\"id\", \"name\") REFERENCES "
                           "\"other\".\"p\" (\"a\", \"b\")")));
    }

    void plainIndex()
    {
        auto d = simple();
        d.indexes.push_back(IndexDef { QStringLiteral("ix"), { QStringLiteral("name") }, false });
        QVERIFY(
            body(d).endsWith(QStringLiteral(");\n\nCREATE INDEX \"ix\" ON \"t\" (\"name\");\n")));
    }

    void uniqueIndex()
    {
        auto d = simple();
        d.indexes.push_back(IndexDef {
            QStringLiteral("ux"), { QStringLiteral("name"), QStringLiteral("id") }, true });
        QVERIFY(body(d).endsWith(
            QStringLiteral("\n\nCREATE UNIQUE INDEX \"ux\" ON \"t\" (\"name\", \"id\");\n")));
    }

    void primaryKeyIndexNotRepeated()
    {
        auto d = simple();
        d.primaryKey = QStringList { QStringLiteral("id"), QStringLiteral("name") };
        d.indexes.push_back(IndexDef {
            QStringLiteral("pk"), { QStringLiteral("id"), QStringLiteral("name") }, true });
        // same columns in another order, and the same columns but not unique, are real indexes
        d.indexes.push_back(IndexDef {
            QStringLiteral("rev"), { QStringLiteral("name"), QStringLiteral("id") }, true });
        d.indexes.push_back(IndexDef {
            QStringLiteral("nu"), { QStringLiteral("id"), QStringLiteral("name") }, false });
        const QString text = body(d);
        QVERIFY(!text.contains(QStringLiteral("\"pk\"")));
        QVERIFY(text.contains(QStringLiteral("CREATE UNIQUE INDEX \"rev\"")));
        QVERIFY(text.contains(QStringLiteral("CREATE INDEX \"nu\"")));
    }

    void quotesAreEscaped()
    {
        TableDetails d;
        d.columns.push_back(column(QStringLiteral("we\"ird"), QStringLiteral("int")));
        d.primaryKey = QStringList { QStringLiteral("we\"ird") };
        d.indexes.push_back(
            IndexDef { QStringLiteral("i\"x"), { QStringLiteral("we\"ird") }, false });
        const QString text = DdlBuilder::build(
            FakeDialect(),
            ObjectPath { { QStringLiteral("db"), QStringLiteral("ta\"ble") }, ObjectKind::Table },
            d);
        QVERIFY(text.contains(QStringLiteral("CREATE TABLE \"ta\"\"ble\" (")));
        QVERIFY(text.contains(QStringLiteral("    \"we\"\"ird\" int,")));
        QVERIFY(text.contains(QStringLiteral("PRIMARY KEY (\"we\"\"ird\")")));
        QVERIFY(text.contains(
            QStringLiteral("CREATE INDEX \"i\"\"x\" ON \"ta\"\"ble\" (\"we\"\"ird\");")));
    }

    void noColumns()
    {
        QCOMPARE(build({}),
                 DdlBuilder::header()
                     + QStringLiteral("\n\n-- No columns are known for this table.\n"));
    }

    void view()
    {
        auto d = simple();
        d.columns.push_back(column(QStringLiteral("n"), {}));
        d.columns.back().type = ValueType::Int64;
        d.primaryKey = QStringList { QStringLiteral("id") };
        QCOMPARE(build(d, true),
                 DdlBuilder::header()
                     + QStringLiteral("\n\n-- The definition of a view is not available here.\n"
                                      "--   id integer\n--   name text\n--   n INT64\n"));
    }

    void emptyNativeTypeUsesValueTypeName()
    {
        TableDetails d;
        d.columns.push_back(column(QStringLiteral("a"), {}));
        d.columns.back().type = ValueType::DateTimeTz;
        d.columns.push_back(column(QStringLiteral("b"), {}));
        d.columns.back().type = ValueType::Uuid;
        d.columns.push_back(column(QStringLiteral("c"), {}));
        QCOMPARE(body(d),
                 QStringLiteral("CREATE TABLE \"t\" (\n    \"a\" DATETIMETZ,\n    \"b\" UUID,\n"
                                "    \"c\" OTHER\n);\n"));
    }

    void deterministic()
    {
        auto d = simple();
        d.primaryKey = QStringList { QStringLiteral("id") };
        d.indexes.push_back(IndexDef { QStringLiteral("ix"), { QStringLiteral("name") }, false });
        QCOMPARE(build(d), build(d));
    }

    void fullOutput()
    {
        TableDetails d;
        d.columns.push_back(column(QStringLiteral("id"), QStringLiteral("integer"), false));
        d.columns.push_back(column(QStringLiteral("email"), QStringLiteral("varchar(80)"), false));
        d.columns.push_back(column(QStringLiteral("org_id"), QStringLiteral("integer")));
        d.columns.push_back(column(QStringLiteral("active"), QStringLiteral("boolean"), false,
                                   QStringLiteral("true")));
        d.primaryKey = QStringList { QStringLiteral("id") };
        d.foreignKeys.push_back(ForeignKeyDef { QStringLiteral("fk_org"),
                                                { QStringLiteral("org_id") },
                                                path(QStringLiteral("hr"), QStringLiteral("orgs")),
                                                { QStringLiteral("id") } });
        d.indexes.push_back(IndexDef { QStringLiteral("t_pkey"), { QStringLiteral("id") }, true });
        d.indexes.push_back(
            IndexDef { QStringLiteral("t_email_key"), { QStringLiteral("email") }, true });
        d.indexes.push_back(
            IndexDef { QStringLiteral("t_org_idx"), { QStringLiteral("org_id") }, false });
        const QString expected = QStringLiteral(
            "-- Reconstructed by Quarry from the table's structure. CHECK constraints, triggers "
            "and engine options are not included.\n"
            "\n"
            "CREATE TABLE \"t\" (\n"
            "    \"id\" integer NOT NULL,\n"
            "    \"email\" varchar(80) NOT NULL,\n"
            "    \"org_id\" integer,\n"
            "    \"active\" boolean NOT NULL DEFAULT true,\n"
            "    PRIMARY KEY (\"id\"),\n"
            "    CONSTRAINT \"fk_org\" FOREIGN KEY (\"org_id\") REFERENCES \"hr\".\"orgs\" "
            "(\"id\")\n"
            ");\n"
            "\n"
            "CREATE UNIQUE INDEX \"t_email_key\" ON \"t\" (\"email\");\n"
            "\n"
            "CREATE INDEX \"t_org_idx\" ON \"t\" (\"org_id\");\n");
        QCOMPARE(build(d), expected);
    }
};

QTEST_GUILESS_MAIN(TstDdlBuilder)
#include "tst_ddl_builder.moc"
