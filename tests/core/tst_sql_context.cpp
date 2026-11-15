#include <quarry/sql_context.h>

#include <QElapsedTimer>
#include <QtTest>

using namespace quarry;
using namespace Qt::StringLiterals;

namespace {

constexpr auto kNone = CompletionScope::None;
constexpr auto kKeyword = CompletionScope::Keyword;
constexpr auto kTable = CompletionScope::Table;
constexpr auto kColumn = CompletionScope::Column;
constexpr auto kMember = CompletionScope::Member;

QString describe(const TableReference &ref)
{
    return ref.parts.join(u'.') + (ref.alias.isEmpty() ? QString() : u" as "_s + ref.alias);
}

} // namespace

class TstSqlContext : public QObject
{
    Q_OBJECT

private:
    // `sql` holds the cursor as '|'. replaceStart -1: cursor minus the prefix length.
    static void row(const char *name, const QString &sql, CompletionScope scope,
                    const QString &prefix, const QString &qualifier, const QStringList &tables,
                    int replaceStart = -1)
    {
        QTest::newRow(name) << sql << static_cast<int>(scope) << prefix << qualifier << tables
                            << replaceStart;
    }

private slots:
    void analyze_data()
    {
        QTest::addColumn<QString>("sql");
        QTest::addColumn<int>("scope");
        QTest::addColumn<QString>("prefix");
        QTest::addColumn<QString>("qualifier"); // parts joined with '.'
        QTest::addColumn<QStringList>("tables"); // "a.b as x"
        QTest::addColumn<int>("replaceStart");

        const QStringList none;
        // Statement start and keywords.
        row("empty", u"|"_s, kKeyword, {}, {}, none);
        row("keyword-prefix", u"sel|"_s, kKeyword, u"sel"_s, {}, none);
        row("after-comment", u"-- x\nSEL|"_s, kKeyword, u"SEL"_s, {}, none);
        row("after-block-comment", u"/* x */ sel|"_s, kKeyword, u"sel"_s, {}, none);
        row("mid-word", u"SELECT * FROM us|ers"_s, kTable, u"us"_s, {}, none);
        row("unicode-prefix", u"SELECT ü|"_s, kColumn, u"ü"_s, {}, none);
        row("create", u"CREATE |"_s, kKeyword, {}, {}, none);

        // Scopes.
        row("select", u"SELECT |"_s, kColumn, {}, {}, none);
        row("select-comma", u"SELECT a, |"_s, kColumn, {}, {}, none);
        row("select-before-from", u"SELECT | FROM users"_s, kColumn, {}, {}, { u"users"_s });
        row("from", u"SELECT * FROM |"_s, kTable, {}, {}, none);
        row("from-prefix", u"SELECT * FROM us|"_s, kTable, u"us"_s, {}, none);
        row("after-table", u"SELECT * FROM users |"_s, kKeyword, {}, {}, { u"users"_s });
        row("alias-typed", u"SELECT * FROM users x|"_s, kKeyword, u"x"_s, {}, { u"users"_s });
        row("where", u"SELECT * FROM users WHERE |"_s, kColumn, {}, {}, { u"users"_s });
        row("and", u"SELECT * FROM t WHERE a = 1 AND |"_s, kColumn, {}, {}, { u"t"_s });
        row("group-by", u"SELECT a FROM t GROUP BY |"_s, kColumn, {}, {}, { u"t"_s });
        row("order-word", u"SELECT a FROM t ORDER |"_s, kKeyword, {}, {}, { u"t"_s });
        row("having", u"SELECT a FROM t GROUP BY a HAVING |"_s, kColumn, {}, {}, { u"t"_s });
        row("join", u"SELECT * FROM t LEFT JOIN |"_s, kTable, {}, {}, { u"t"_s });
        row("join-on", u"SELECT * FROM t JOIN u ON |"_s, kColumn, {}, {}, { u"t"_s, u"u"_s });
        row("comma-list", u"SELECT * FROM a, b c, |"_s, kTable, {}, {}, { u"a"_s, u"b as c"_s });
        row("comma-list-all", u"SELECT * FROM a, b c, d |"_s, kKeyword, {}, {},
            { u"a"_s, u"b as c"_s, u"d"_s });
        row("update", u"UPDATE |"_s, kTable, {}, {}, none);
        row("update-set", u"UPDATE t SET |"_s, kColumn, {}, {}, { u"t"_s });
        row("update-after-table", u"UPDATE t |"_s, kKeyword, {}, {}, { u"t"_s });
        row("insert-into", u"INSERT INTO |"_s, kTable, {}, {}, none);
        row("insert-columns", u"INSERT INTO t (|"_s, kColumn, {}, {}, { u"t"_s });
        row("insert-columns-second", u"INSERT INTO s.t (a, |"_s, kColumn, {}, {}, { u"s.t"_s });
        row("insert-values", u"INSERT INTO t (a) VALUES (|"_s, kColumn, {}, {}, { u"t"_s });
        row("delete-where", u"DELETE FROM t WHERE |"_s, kColumn, {}, {}, { u"t"_s });
        row("drop-table", u"DROP TABLE |"_s, kTable, {}, {}, none);
        row("drop-table-if-exists", u"DROP TABLE IF EXISTS |"_s, kTable, {}, {}, none);
        row("create-table-not-a-reference", u"CREATE TABLE foo (|"_s, kColumn, {}, {}, none);
        row("function-call", u"SELECT count(|"_s, kColumn, {}, {}, none);
        row("with-cte", u"WITH c AS (SELECT 1) SELECT |"_s, kColumn, {}, {}, none);
        row("union", u"SELECT a FROM t UNION |"_s, kKeyword, {}, {}, { u"t"_s });
        row("parameter", u"SELECT * FROM t WHERE x = $1 AND |"_s, kColumn, {}, {}, { u"t"_s });
        row("after-string", u"SELECT 'it''s' |"_s, kColumn, {}, {}, none);
        row("after-e-string", u"SELECT E'it\\'s' |"_s, kColumn, {}, {}, none);

        // Tables and aliases, also after the cursor.
        row("schema-qualified", u"SELECT * FROM public.users |"_s, kKeyword, {}, {},
            { u"public.users"_s });
        row("as-alias", u"SELECT * FROM a.b AS x WHERE |"_s, kColumn, {}, {}, { u"a.b as x"_s });
        row("bare-alias", u"SELECT * FROM t x WHERE |"_s, kColumn, {}, {}, { u"t as x"_s });
        row("alias-not-keyword", u"SELECT * FROM t WHERE |"_s, kColumn, {}, {}, { u"t"_s });
        row("quoted-names", u"SELECT * FROM \"my \"\"s\"\".t\" \"a b\" WHERE |"_s, kColumn, {}, {},
            { u"my \"s\".t as a b"_s });
        row("backtick-bracket", u"SELECT * FROM `a`.[b] c WHERE |"_s, kColumn, {}, {},
            { u"a.b as c"_s });
        row("tables-after-cursor", u"SELECT | FROM a x JOIN b y ON x.i = y.i"_s, kColumn, {}, {},
            { u"a as x"_s, u"b as y"_s });
        row("subquery-flat", u"SELECT * FROM (SELECT a FROM t) s WHERE |"_s, kColumn, {}, {},
            { u"t"_s });
        row("subquery-in", u"SELECT * FROM t WHERE a IN (SELECT b FROM u WHERE |)"_s, kColumn, {},
            {}, { u"t"_s, u"u"_s });
        row("subquery-start", u"SELECT * FROM (|"_s, kKeyword, {}, {}, none);
        row("for-update-of", u"SELECT * FROM t FOR UPDATE OF t2 |"_s, kKeyword, {}, {},
            { u"t"_s, u"t2"_s });

        // Member.
        row("member", u"SELECT * FROM users u WHERE u.|"_s, kMember, {}, u"u"_s,
            { u"users as u"_s });
        row("member-prefix", u"SELECT u.na| FROM users u"_s, kMember, u"na"_s, u"u"_s,
            { u"users as u"_s });
        row("member-schema", u"SELECT * FROM public.|"_s, kMember, {}, u"public"_s, none);
        row("member-schema-prefix", u"SELECT * FROM public.us|"_s, kMember, u"us"_s, u"public"_s,
            none);
        row("member-two-parts", u"SELECT x.y.|"_s, kMember, {}, u"x.y"_s, none);
        row("member-quoted", u"SELECT x.\"a b\".|"_s, kMember, {}, u"x.a b"_s, none);
        row("member-spaces", u"SELECT a . | FROM t"_s, kMember, {}, u"a"_s, { u"t"_s });
        row("member-dot-after-paren", u"SELECT (1).|"_s, kNone, {}, {}, none);
        row("member-second-alias", u"SELECT * FROM t x JOIN u y ON x.id = y.|"_s, kMember, {},
            u"y"_s, { u"t as x"_s, u"u as y"_s });

        // Quoted identifiers, including an unfinished quote at the cursor.
        row("dq-unfinished", u"SELECT \"us|"_s, kColumn, u"us"_s, {}, none, 7);
        row("dq-unfinished-tables-after", u"SELECT \"us| FROM t"_s, kColumn, u"us"_s, {},
            { u"t"_s }, 7);
        row("dq-closed", u"SELECT \"us|er\" FROM t"_s, kColumn, u"us"_s, {}, { u"t"_s }, 7);
        row("dq-empty", u"SELECT \"|"_s, kColumn, {}, {}, none, 7);
        row("dq-doubled", u"SELECT \"my \"\"col|"_s, kColumn, u"my \"col"_s, {}, none, 7);
        row("backtick-unfinished", u"SELECT `us|"_s, kColumn, u"us"_s, {}, none, 7);
        row("bracket-unfinished", u"SELECT [us|"_s, kColumn, u"us"_s, {}, none, 7);
        row("dq-after-dot", u"SELECT t.\"na|"_s, kMember, u"na"_s, u"t"_s, none, 9);
        row("dq-table", u"SELECT * FROM \"us|"_s, kTable, u"us"_s, {}, none, 14);
        row("subscript-is-not-quote", u"SELECT a[1], b|"_s, kColumn, u"b"_s, {}, none);

        // None: strings, comments, numbers.
        row("string-open", u"SELECT 'abc|"_s, kNone, {}, {}, none, 0);
        row("string-middle", u"SELECT 'a|bc'"_s, kNone, {}, {}, none, 0);
        row("line-comment", u"SELECT 1 -- com|"_s, kNone, {}, {}, none, 0);
        row("line-comment-middle", u"SELECT 1 -- co|m\nFROM t"_s, kNone, {}, {}, none, 0);
        row("block-comment", u"/* x| */ SELECT"_s, kNone, {}, {}, none, 0);
        row("block-comment-unfinished", u"SELECT /* x|"_s, kNone, {}, {}, none, 0);
        row("nested-block-comment", u"/* a /* b */ c| */"_s, kNone, {}, {}, none, 0);
        row("number", u"SELECT 12|"_s, kNone, {}, {}, none, 0);
        row("number-decimal", u"SELECT 1.5e3|"_s, kNone, {}, {}, none, 0);
        row("number-after-eq", u"SELECT * FROM t WHERE a = 1|"_s, kNone, {}, {}, none, 0);
        row("parameter-number", u"SELECT $1|"_s, kNone, {}, {}, none, 0);
        row("dollar-quote", u"SELECT $$ foo|"_s, kNone, {}, {}, none, 0);
        row("dollar-tag", u"SELECT $a$ x | y $a$"_s, kNone, {}, {}, none, 0);
        row("e-string", u"SELECT E'it\\'s |"_s, kNone, {}, {}, none, 0);
        row("word-with-digit", u"SELECT a1|"_s, kColumn, u"a1"_s, {}, none);
    }

    void analyze()
    {
        QFETCH(QString, sql);
        QFETCH(int, scope);
        QFETCH(QString, prefix);
        QFETCH(QString, qualifier);
        QFETCH(QStringList, tables);
        QFETCH(int, replaceStart);

        const qsizetype cursor = sql.indexOf(u'|');
        QVERIFY(cursor >= 0);
        sql.remove(cursor, 1);
        const SqlContext ctx = analyzeSqlContext(sql, cursor);

        QCOMPARE(static_cast<int>(ctx.scope), scope);
        if (ctx.scope == kNone) {
            return; // nothing else is promised
        }
        QCOMPARE(ctx.prefix, prefix);
        QCOMPARE(ctx.replaceStart,
                 replaceStart >= 0 ? qsizetype(replaceStart) : cursor - prefix.size());
        QCOMPARE(ctx.qualifier.join(u'.'), qualifier);
        QStringList got;
        for (const TableReference &ref : ctx.tables) {
            got << describe(ref);
        }
        QCOMPARE(got, tables);
    }

    void cursorIsClamped()
    {
        const QString sql = u"select"_s;
        QCOMPARE(analyzeSqlContext(sql, 100).prefix, u"select"_s);
        QCOMPARE(analyzeSqlContext(sql, -5).prefix, QString());
        QCOMPARE(analyzeSqlContext(sql, 0).scope, kKeyword);
    }

    void cursorAtWordStartIsNotInsideIt()
    {
        const SqlContext ctx = analyzeSqlContext(u"SELECT name"_s, 7);
        QCOMPARE(ctx.prefix, QString());
        QCOMPARE(ctx.replaceStart, qsizetype(7));
        QCOMPARE(ctx.scope, kColumn);
    }

    void cursorRightAfterClosedStringIsNotInsideIt()
    {
        QCOMPARE(analyzeSqlContext(u"SELECT 'a'"_s, 10).scope, kColumn);
        QCOMPARE(analyzeSqlContext(u"SELECT 'a'"_s, 9).scope, kNone);
    }

    void everyCursorPositionIsHandled()
    {
        // No crash or out-of-range access at any cursor of a hostile statement.
        const QString sql
            = u"SELECT \"a, [b, `c, 'd, /* e, -- f\n $$g$$ $h$ é\U0001F600.. ((( ))) FROM x.y. z"_s;
        for (qsizetype i = 0; i <= sql.size(); ++i) {
            const SqlContext ctx = analyzeSqlContext(sql, i);
            QVERIFY(ctx.replaceStart >= 0 && ctx.replaceStart <= i);
        }
    }

    void linearTime()
    {
        QString sql
            = u"SELECT a, b FROM t1 x JOIN t2 y ON x.id = y.id WHERE x.v > (SELECT 1) AND "_s;
        QString big;
        for (int i = 0; i < 2000; ++i) {
            big += sql;
        }
        big += u"na"_s;
        QElapsedTimer timer;
        timer.start();
        const SqlContext ctx = analyzeSqlContext(big, big.size());
        const qint64 ms = timer.elapsed();
        qInfo() << big.size() << "chars analyzed in" << ms << "ms";
        QCOMPARE(ctx.scope, kColumn);
        QCOMPARE(ctx.tables.size(), size_t(4000));
        QVERIFY2(ms < 1000, "budget (generous for debug builds)");
    }
};

QTEST_GUILESS_MAIN(TstSqlContext)
#include "tst_sql_context.moc"
