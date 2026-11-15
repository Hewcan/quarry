#include <quarry/completion.h>
#include <quarry/metadata_cache.h>
#include <quarry/session.h>
#include <quarry/sql_context.h>

#include <QtTest>

#include <memory>

using namespace quarry;
using namespace Qt::StringLiterals;

namespace {

// A PostgreSQL-shaped tree in memory: database app -> schemas public, sales -> relations ->
// Columns folder -> Column nodes.
class FakeConnection : public IConnection
{
public:
    FakeConnection()
    {
        const QString db = u"app"_s;
        m_tree[{}] = { node({ db }, ObjectKind::Database, true) };
        m_tree[{ db }] = { node({ db, u"public"_s }, ObjectKind::Schema, true),
                           node({ db, u"sales"_s }, ObjectKind::Schema, true) };
        relation(
            u"public"_s, u"users"_s, ObjectKind::Table,
            { { u"id"_s, u"integer PK"_s }, { u"name"_s, u"text"_s }, { u"email"_s, u"text"_s } });
        relation(u"public"_s, u"orders"_s, ObjectKind::Table,
                 { { u"id"_s, u"integer PK"_s },
                   { u"user_id"_s, u"integer"_s },
                   { u"order"_s, u"integer"_s },
                   { u"total"_s, u"numeric"_s } });
        relation(u"public"_s, u"user_roles"_s, ObjectKind::Table,
                 { { u"user_id"_s, u"integer"_s }, { u"role"_s, u"text"_s } });
        relation(u"public"_s, u"Mixed Case"_s, ObjectKind::Table, { { u"a"_s, u"integer"_s } });
        relation(u"public"_s, u"select"_s, ObjectKind::Table, { { u"x"_s, u"integer"_s } });
        relation(u"public"_s, u"v_active_users"_s, ObjectKind::View,
                 { { u"id"_s, u"integer"_s }, { u"name"_s, u"text"_s } });
        relation(u"public"_s, u"mv_totals"_s, ObjectKind::MaterializedView,
                 { { u"total"_s, u"numeric"_s } });
        relation(u"sales"_s, u"invoices"_s, ObjectKind::Table,
                 { { u"id"_s, u"integer PK"_s }, { u"amount"_s, u"numeric"_s } });
        relation(u"sales"_s, u"users"_s, ObjectKind::Table,
                 { { u"id"_s, u"integer"_s }, { u"note"_s, u"text"_s } });
    }

    Result<ServerInfo> open(const CancellationToken &) override
    {
        m_open = true;
        return ServerInfo { u"Fake"_s, u"1"_s };
    }
    void close() override { m_open = false; }
    bool isOpen() const override { return m_open; }
    Result<std::vector<SchemaNode>> children(const ObjectPath &parent,
                                             const CancellationToken &) override
    {
        const auto it = m_tree.find(parent.parts);
        return it == m_tree.end() ? std::vector<SchemaNode> {} : it->second;
    }
    Result<TableDetails> describe(const ObjectPath &, const CancellationToken &) override
    {
        return fail(Error { ErrorKind::Unsupported, u"Unsupported."_s, {}, {}, {} });
    }
    Result<std::unique_ptr<IResultStream>> execute(const Statement &,
                                                   const CancellationToken &) override
    {
        return fail(Error { ErrorKind::Unsupported, u"Unsupported."_s, {}, {}, {} });
    }
    void requestCancel() override { }
    Result<void> begin() override { return {}; }
    Result<void> commit() override { return {}; }
    Result<void> rollback() override { return {}; }

private:
    static SchemaNode node(const QStringList &parts, ObjectKind kind, bool children,
                           const QString &detail = {})
    {
        return SchemaNode { ObjectPath { parts, kind }, parts.last(), children, detail };
    }

    void relation(const QString &schema, const QString &name, ObjectKind kind,
                  const std::vector<std::pair<QString, QString>> &columns)
    {
        const QStringList path { u"app"_s, schema, name };
        m_tree[{ u"app"_s, schema }].push_back(node(path, kind, true));
        std::vector<SchemaNode> nodes;
        nodes.reserve(columns.size());
        for (const auto &[column, type] : columns) {
            nodes.push_back(node(path + QStringList { column }, ObjectKind::Column, false, type));
        }
        m_tree[path + QStringList { u"Columns"_s }] = std::move(nodes);
    }

    std::map<QStringList, std::vector<SchemaNode>> m_tree;
    bool m_open = false;
};

class FakeDialect : public ISqlDialect
{
public:
    QString quoteIdentifier(QStringView name) const override
    {
        return u"\""_s + name.toString().replace(u'"', u"\"\""_s) + u"\""_s;
    }
    QString quoteLiteral(QStringView text) const override { return text.toString(); }
    QString highlightingDefinition() const override { return {}; }
    QStringList keywords() const override
    {
        return { u"SELECT"_s, u"FROM"_s,   u"WHERE"_s,  u"ORDER"_s, u"BY"_s,   u"GROUP"_s,
                 u"AND"_s,    u"OR"_s,     u"JOIN"_s,   u"ON"_s,    u"AS"_s,   u"USER"_s,
                 u"SET"_s,    u"UPDATE"_s, u"INSERT"_s, u"INTO"_s,  u"NULL"_s, u"SELF"_s };
    }
    QStringList builtinFunctions() const override
    {
        return { u"coalesce"_s, u"concat"_s, u"count"_s, u"lower"_s, u"max"_s,
                 u"min"_s,      u"now"_s,    u"sum"_s,   u"upper"_s };
    }
    std::vector<StatementRange> splitStatements(QStringView) const override { return {}; }
    QString limitClause(qint64, qint64) const override { return {}; }
    QString explainPrefix(ExplainMode) const override { return {}; }
    QString placeholder(int) const override { return u"?"_s; }
    QString qualifiedName(const ObjectPath &) const override { return {}; }
    std::optional<QString> rowIdentifier() const override { return std::nullopt; }
};

QStringList labels(const std::vector<Suggestion> &list)
{
    QStringList out;
    for (const Suggestion &s : list) {
        out << s.label;
    }
    return out;
}

QStringList texts(const std::vector<Suggestion> &list)
{
    QStringList out;
    for (const Suggestion &s : list) {
        out << s.text;
    }
    return out;
}

} // namespace

class TstCompletion : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<Session> m_session;
    std::unique_ptr<MetadataCache> m_cache;
    FakeDialect m_dialect;

    // `sql` has the cursor as '|'.
    static SqlContext context(QString sql)
    {
        const qsizetype cursor = sql.indexOf(u'|');
        sql.remove(cursor, 1);
        return analyzeSqlContext(sql, cursor);
    }

    std::vector<Suggestion> complete(const QString &sql, int limit = 200) const
    {
        return CompletionEngine::complete(context(sql), *m_cache, m_dialect, limit);
    }

    // What the app does: ask for the missing columns and wait until they are in.
    void loadColumnsFor(const QString &sql)
    {
        const std::vector<ObjectPath> missing
            = CompletionEngine::missingColumns(context(sql), *m_cache);
        for (const ObjectPath &table : missing) {
            m_cache->ensureColumns(table);
        }
        for (const ObjectPath &table : missing) {
            QTRY_VERIFY_WITH_TIMEOUT(m_cache->columns(table) != nullptr, 10000);
        }
    }

private slots:
    void init()
    {
        m_session = std::make_unique<Session>(std::make_unique<FakeConnection>());
        m_session->open();
        m_cache = std::make_unique<MetadataCache>();
        m_cache->attach(m_session.get());
        m_cache->load();
        QTRY_COMPARE_WITH_TIMEOUT(m_cache->state(), MetadataCache::State::Ready, 10000);
    }
    void cleanup()
    {
        m_cache.reset();
        m_session.reset();
    }

    void noneScopeGivesNothing()
    {
        QVERIFY(complete(u"SELECT 'a|b'"_s).empty());
        QVERIFY(complete(u"SELECT 1|"_s).empty());
    }

    void limitIsApplied()
    {
        QCOMPARE(complete(u"SELECT * FROM |"_s, 3).size(), size_t(3));
        QCOMPARE(complete(u"SELECT * FROM |"_s, 1).size(), size_t(1));
        QVERIFY(complete(u"SELECT * FROM |"_s, 0).empty());
        QVERIFY(complete(u"SELECT * FROM |"_s, -4).empty());
        const auto all = complete(u"SELECT * FROM |"_s, 1000);
        QVERIFY(all.size() > 3);
        // The limit keeps the head of the full ordering.
        QCOMPARE(labels(complete(u"SELECT * FROM |"_s, 4)), labels(all).mid(0, 4));
    }

    void tableScopeOrdersTablesViewsThenSchemas()
    {
        const auto list = complete(u"SELECT * FROM |"_s);
        QCOMPARE(labels(list),
                 (QStringList { u"invoices"_s, u"Mixed Case"_s, u"orders"_s, u"select"_s,
                                u"user_roles"_s, u"users"_s, u"users"_s, u"mv_totals"_s,
                                u"v_active_users"_s, u"public"_s, u"sales"_s }));
        QCOMPARE(list[0].kind, SuggestionKind::Table);
        QCOMPARE(list[7].kind, SuggestionKind::View); // materialized views are views
        QCOMPARE(list[9].kind, SuggestionKind::Schema);
    }

    void sameNameInTwoSchemasShowsTheSchema()
    {
        const auto list = complete(u"SELECT * FROM users|"_s);
        QCOMPARE(list.size(), size_t(3)); // and the view that contains the text
        QCOMPARE(list[0].detail, u"public"_s); // first in load order
        QCOMPARE(list[1].detail, u"sales"_s);
        QCOMPARE(list[2].label, u"v_active_users"_s);
    }

    void prefixBeforeSubstringThenKindThenAlphabetical()
    {
        // Prefix: orders. Substring: tables (invoices, user_roles) before the view (mv_totals).
        QCOMPARE(labels(complete(u"SELECT * FROM o|"_s)),
                 (QStringList { u"orders"_s, u"invoices"_s, u"user_roles"_s, u"mv_totals"_s }));
    }

    void matchingIgnoresCase()
    {
        QCOMPARE(labels(complete(u"SELECT * FROM USER|"_s)),
                 (QStringList { u"user_roles"_s, u"users"_s, u"users"_s, u"v_active_users"_s }));
        QCOMPARE(labels(complete(u"SELECT * FROM mixed|"_s)), QStringList { u"Mixed Case"_s });
    }

    void substringGroupIsSkippedWhenPrefixFillsTheLimit()
    {
        const auto list = complete(u"SELECT * FROM u|"_s, 3);
        QCOMPARE(labels(list), (QStringList { u"user_roles"_s, u"users"_s, u"users"_s }));
    }

    void quotesWhenNeeded()
    {
        const auto list = complete(u"SELECT * FROM |"_s);
        QCOMPARE(texts(list).mid(0, 5),
                 (QStringList { u"invoices"_s, u"\"Mixed Case\""_s, u"orders"_s, u"\"select\""_s,
                                u"user_roles"_s }));
        QCOMPARE(labels(list)[1], u"Mixed Case"_s); // the label has no quotes
        QCOMPARE(labels(list)[3], u"select"_s);
    }

    void userTypedQuoteReplacesFromTheQuote()
    {
        const SqlContext ctx = context(u"SELECT * FROM \"us|"_s);
        QCOMPARE(ctx.replaceStart, qsizetype(14));
        const auto list = CompletionEngine::complete(ctx, *m_cache, m_dialect);
        QCOMPARE(texts(list).mid(0, 2), (QStringList { u"user_roles"_s, u"users"_s }));
    }

    void keywordsAreInsertedInUppercase()
    {
        const auto list = complete(u"sel|"_s);
        QVERIFY(!list.empty());
        QCOMPARE(list[0].kind, SuggestionKind::Keyword);
        QCOMPARE(list[0].text, u"SELECT"_s);
        QCOMPARE(list[0].label, u"SELECT"_s);
        // SELF too, then the table called select (quoted: it is a keyword).
        QCOMPARE(texts(list), (QStringList { u"SELECT"_s, u"SELF"_s, u"\"select\""_s }));
        QCOMPARE(list[2].kind, SuggestionKind::Table);
    }

    void keywordScopeOrder()
    {
        const auto list = complete(u"|"_s, 1000);
        // Keywords, then functions, then tables and views; no columns (no table in the statement).
        QCOMPARE(list.front().kind, SuggestionKind::Keyword);
        QCOMPARE(labels(list).mid(0, 3), (QStringList { u"AND"_s, u"AS"_s, u"BY"_s }));
        int lastRank = -1;
        for (const Suggestion &s : list) {
            const int rank = s.kind == SuggestionKind::Keyword ? 0
                : s.kind == SuggestionKind::Function           ? 1
                : s.kind == SuggestionKind::Column             ? 2
                                                               : 3;
            QVERIFY2(rank >= lastRank, qPrintable(s.label));
            lastRank = rank;
            QVERIFY(s.kind != SuggestionKind::Schema);
        }
    }

    void columnScopeNeedsColumnsLoaded()
    {
        const QString sql = u"SELECT na| FROM users"_s;
        const SqlContext ctx = context(sql);
        QCOMPARE(ctx.scope, CompletionScope::Column);
        // Not loaded yet: no columns, and the engine says what to load.
        QVERIFY(labels(CompletionEngine::complete(ctx, *m_cache, m_dialect)).isEmpty());
        const auto missing = CompletionEngine::missingColumns(ctx, *m_cache);
        QCOMPARE(missing.size(), size_t(1));
        QCOMPARE(missing[0].parts, (QStringList { u"app"_s, u"public"_s, u"users"_s }));

        loadColumnsFor(sql);
        QVERIFY(CompletionEngine::missingColumns(ctx, *m_cache).empty());
        const auto list = CompletionEngine::complete(ctx, *m_cache, m_dialect);
        QVERIFY(!list.empty());
        QCOMPARE(list[0].label, u"name"_s);
        QCOMPARE(list[0].kind, SuggestionKind::Column);
        QCOMPARE(list[0].detail, u"users: text"_s);
    }

    void columnScopeMixesColumnsFunctionsAndKeywords()
    {
        const QString sql = u"SELECT o| FROM orders"_s;
        loadColumnsFor(sql);
        const auto list = complete(sql);
        // Prefix: column "order" (quoted: keyword), keyword ORDER, then substring: columns
        // (total), functions (coalesce... no 'o' prefix), keywords.
        QCOMPARE(list[0].kind, SuggestionKind::Column);
        QCOMPARE(list[0].label, u"order"_s);
        QCOMPARE(list[0].text, u"\"order\""_s);
        QCOMPARE(texts(list).mid(1, 3), (QStringList { u"ON"_s, u"OR"_s, u"ORDER"_s }));
        QCOMPARE(list[1].kind, SuggestionKind::Keyword);
    }

    void columnsOfEveryReferencedTable()
    {
        const QString sql = u"SELECT i| FROM users u JOIN sales.invoices i ON 1 = 1"_s;
        loadColumnsFor(sql);
        const auto list = complete(sql);
        // id of sales.invoices and of public.users; then substring matches.
        QCOMPARE(list[0].label, u"id"_s);
        QCOMPARE(list[0].detail, u"invoices: integer PK"_s);
        QCOMPARE(list[1].label, u"id"_s);
        QCOMPARE(list[1].detail, u"users: integer PK"_s);
    }

    void memberScopeWithAlias()
    {
        const QString sql = u"SELECT u.| FROM users u"_s;
        loadColumnsFor(sql);
        const auto list = complete(sql);
        QCOMPARE(labels(list), (QStringList { u"email"_s, u"id"_s, u"name"_s }));
        QCOMPARE(list[1].detail, u"integer PK"_s); // the type alone
        QCOMPARE(labels(complete(u"SELECT u.na| FROM users u"_s)), QStringList { u"name"_s });
        // Substring after prefix.
        QCOMPARE(labels(complete(u"SELECT u.m| FROM users u"_s)),
                 (QStringList { u"email"_s, u"name"_s }));
    }

    void aliasWinsOverTableName()
    {
        // `users` is an alias of orders here.
        const QString sql = u"SELECT users.| FROM orders users"_s;
        loadColumnsFor(sql);
        QCOMPARE(labels(complete(sql)),
                 (QStringList { u"id"_s, u"order"_s, u"total"_s, u"user_id"_s }));
    }

    void memberScopeWithTableName()
    {
        const QString sql = u"SELECT orders.| FROM orders"_s;
        loadColumnsFor(sql);
        QCOMPARE(complete(sql).size(), size_t(4));
        // The qualifier alone also asks for its columns.
        const auto missing
            = CompletionEngine::missingColumns(context(u"SELECT user_roles.|"_s), *m_cache);
        QCOMPARE(missing.size(), size_t(1));
        QCOMPARE(missing[0].parts.last(), u"user_roles"_s);
    }

    void memberScopeWithSchemaQualifiedTable()
    {
        loadColumnsFor(u"SELECT sales.users.| FROM sales.users"_s);
        QCOMPARE(labels(complete(u"SELECT sales.users.| FROM sales.users"_s)),
                 (QStringList { u"id"_s, u"note"_s }));
    }

    void memberScopeWithSchemaGivesItsRelations()
    {
        QCOMPARE(labels(complete(u"SELECT * FROM sales.|"_s)),
                 (QStringList { u"invoices"_s, u"users"_s }));
        QCOMPARE(labels(complete(u"SELECT * FROM PUBLIC.us|"_s)),
                 (QStringList { u"user_roles"_s, u"users"_s, u"v_active_users"_s }));
        QCOMPARE(labels(complete(u"SELECT * FROM app.public.mv|"_s)),
                 QStringList { u"mv_totals"_s });
        QVERIFY(complete(u"SELECT * FROM nope.|"_s).empty());
    }

    void memberScopeUnloadedColumnsGivesNothing()
    {
        QVERIFY(complete(u"SELECT u.| FROM users u"_s).empty());
        QCOMPARE(CompletionEngine::missingColumns(context(u"SELECT u.| FROM users u"_s), *m_cache)
                     .size(),
                 size_t(1));
    }

    void missingColumnsDedupesAndSkipsUnknownTables()
    {
        const SqlContext ctx
            = context(u"SELECT | FROM users a, users b JOIN ghost g ON 1 = 1 JOIN orders o"_s);
        const auto missing = CompletionEngine::missingColumns(ctx, *m_cache);
        QCOMPARE(missing.size(), size_t(2)); // public.users once, orders; ghost is unknown
        QCOMPARE(missing[0].parts.last(), u"users"_s);
        QCOMPARE(missing[1].parts.last(), u"orders"_s);
    }

    void emptyCacheStillOffersKeywordsAndFunctions()
    {
        MetadataCache empty;
        const SqlContext ctx = context(u"SELECT co|"_s);
        const auto list = CompletionEngine::complete(ctx, empty, m_dialect);
        QCOMPARE(labels(list), (QStringList { u"coalesce"_s, u"concat"_s, u"count"_s }));
        QCOMPARE(list[0].kind, SuggestionKind::Function);
        QCOMPARE(list[0].text, u"coalesce"_s);
        QVERIFY(CompletionEngine::missingColumns(ctx, empty).empty());
        QVERIFY(!complete(u"SELECT * FROM |"_s).empty());
        QVERIFY(
            CompletionEngine::complete(context(u"SELECT * FROM |"_s), empty, m_dialect).empty());
    }

    void invalidatedCacheStopsSuggestingTables()
    {
        QVERIFY(!complete(u"SELECT * FROM |"_s).empty());
        m_cache->invalidate();
        QVERIFY(complete(u"SELECT * FROM |"_s).empty());
    }
};

QTEST_GUILESS_MAIN(TstCompletion)
#include "tst_completion.moc"
