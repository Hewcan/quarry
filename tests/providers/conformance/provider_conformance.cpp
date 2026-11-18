#include "provider_conformance.h"

#include <quarry/cell_formatter.h>

#include <QtTest>

#include <atomic>
#include <future>
#include <mutex>

namespace quarry::conformance {

namespace {

// Qt Test macros return void, so the value-returning helpers record the failure by hand.
void recordFailure(const QString &text, int line)
{
    QTest::qFail(qPrintable(text), __FILE__, line);
}

bool containsName(const std::vector<SchemaNode> &nodes, ObjectKind kind, const QString &name)
{
    return std::any_of(nodes.begin(), nodes.end(), [&](const SchemaNode &node) {
        return node.path.kind == kind && node.name == name;
    });
}

// Depth-first walk of the whole tree below `parent`, appending every descendant to `out`.
bool collect(IConnection &connection, const ObjectPath &parent, int depth,
             std::vector<SchemaNode> &out)
{
    if (depth > 6)
        return false;
    auto children = connection.children(parent, {});
    if (!children)
        return false;
    for (const SchemaNode &node : children.value()) {
        out.push_back(node);
        if (node.hasChildren && !collect(connection, node.path, depth + 1, out))
            return false;
    }
    return true;
}

} // namespace

void ProviderConformanceTest::init()
{
    m_fixture = createFixture();
}

void ProviderConformanceTest::cleanup()
{
    m_fixture.reset();
}

std::unique_ptr<IConnection> ProviderConformanceTest::openConnection(bool withSchema)
{
    auto connection
        = fixture().provider().createConnection(fixture().config(), fixture().secrets());
    if (!connection) {
        recordFailure(QStringLiteral("createConnection returned null"), __LINE__);
        return nullptr;
    }
    auto info = connection->open({});
    if (!info) {
        recordFailure(info.error().message + QStringLiteral(" | ") + info.error().detail, __LINE__);
        return nullptr;
    }
    if (withSchema) {
        const QString script = fixture().sampleSchemaSql();
        for (const StatementRange &range : fixture().provider().dialect().splitStatements(script)) {
            const auto d = drain(*connection, script.mid(range.begin, range.end - range.begin));
            if (!d.ok) {
                recordFailure(QStringLiteral("schema setup failed: ") + d.errorText(), __LINE__);
                return nullptr;
            }
        }
    }
    return connection;
}

ProviderConformanceTest::Drained ProviderConformanceTest::drain(IConnection &connection,
                                                                const QString &sql,
                                                                const QVariantList &parameters)
{
    Drained out;
    auto stream = connection.execute(Statement { sql, parameters, 500 }, {});
    if (!stream) {
        out.error = stream.error();
        return out;
    }
    IResultStream &s = *stream.value();
    out.columns = s.columns();
    for (int guard = 0; !s.atEnd() && guard < 1'000'000; ++guard) {
        auto batch = s.fetch(500, {});
        if (!batch) {
            out.error = batch.error();
            return out;
        }
        out.rows += batch.value().rowCount;
        out.batches.push_back(std::move(batch).value());
    }
    out.affected = s.affectedRows();
    out.ok = true;
    return out;
}

QString ProviderConformanceTest::drainAsText(IConnection &connection, const QString &sql)
{
    const auto d = drain(connection, sql);
    if (!d.ok) {
        recordFailure(QStringLiteral("statement failed: ") + sql + QStringLiteral(" | ")
                          + d.errorText(),
                      __LINE__);
        return {};
    }
    QStringList rows;
    for (const ResultBatch &batch : d.batches) {
        for (qsizetype row = 0; row < batch.rowCount; ++row) {
            QStringList cells;
            for (const ColumnBuffer &column : batch.columns)
                cells.append(CellFormatter::display(column, row));
            rows.append(cells.join(QLatin1Char('\t')));
        }
    }
    return rows.join(QLatin1Char('\n'));
}

void ProviderConformanceTest::openCloseServerInfo()
{
    auto connection
        = fixture().provider().createConnection(fixture().config(), fixture().secrets());
    QVERIFY(connection);
    QVERIFY(!connection->isOpen());
    auto info = connection->open({});
    QVERIFY2(info, qPrintable(info ? QString() : info.error().detail));
    QVERIFY(!info.value().product.isEmpty());
    QVERIFY(!info.value().version.isEmpty());
    QVERIFY(connection->isOpen());
    connection->close();
    QVERIFY(!connection->isOpen());
    QVERIFY(connection->open({}));
    QVERIFY(connection->isOpen());
    const auto fields = fixture().provider().connectionFields();
    QVERIFY(!fields.empty());
    for (const auto &field : fields)
        QVERIFY(!field.key.isEmpty() && !field.label.isEmpty());
}

void ProviderConformanceTest::emptyResultKeepsColumns()
{
    auto connection = openConnection();
    QVERIFY(connection);
    const auto d = drain(*connection, QStringLiteral("SELECT id, title FROM books WHERE id < 0"));
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    QCOMPARE(d.columns.size(), size_t(2));
    QCOMPARE(d.columns[0].name, QStringLiteral("id"));
    QCOMPARE(d.columns[1].name, QStringLiteral("title"));
    QCOMPARE(d.rows, 0);
}

void ProviderConformanceTest::typesRoundTripWithNulls()
{
    auto connection = openConnection();
    QVERIFY(connection);
    QVERIFY(drain(*connection,
                  QStringLiteral("INSERT INTO authors(id, name) VALUES(%1, %2)")
                      .arg(fixture().placeholder(1), fixture().placeholder(2)),
                  { 1, QStringLiteral("Ann") })
                .ok);

    const QString insert
        = QStringLiteral("INSERT INTO books(id, title, author_id, price, cover) "
                         "VALUES(%1, %2, %3, %4, %5)")
              .arg(fixture().placeholder(1), fixture().placeholder(2), fixture().placeholder(3),
                   fixture().placeholder(4), fixture().placeholder(5));
    const qint64 bigId = 9'007'199'254'740'993LL; // beyond 2^53: must not go through a double
    const QString title = QStringLiteral("Ünï cødé ✓ 日本 😀");
    const QByteArray cover("\x00\xff\x10 z", 5);
    auto first = drain(*connection, insert,
                       { QVariant::fromValue(bigId), title, 1, 12.5, QVariant(cover) });
    QVERIFY2(first.ok, qPrintable(first.errorText()));
    auto second
        = drain(*connection, insert, { 2, QStringLiteral("plain"), 1, QVariant(), QVariant() });
    QVERIFY2(second.ok, qPrintable(second.errorText()));

    const auto d = drain(*connection,
                         QStringLiteral("SELECT id, title, price, cover FROM books ORDER BY id"));
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    QCOMPARE(d.rows, 2);
    QCOMPARE(d.columns.size(), size_t(4));
    const ResultBatch &b = d.batches.front();
    QCOMPARE(b.columns[0].typeAt(0), ValueType::Int64);
    QCOMPARE(b.columns[0].int64At(0), 2);
    QCOMPARE(b.columns[0].int64At(1), bigId);
    QCOMPARE(b.columns[1].typeAt(1), ValueType::Text);
    QCOMPARE(QString::fromUtf8(b.columns[1].bytesAt(1)), title);
    QCOMPARE(b.columns[2].typeAt(1), ValueType::Double);
    QCOMPARE(b.columns[2].doubleAt(1), 12.5);
    QCOMPARE(b.columns[3].typeAt(1), ValueType::Bytes);
    QCOMPARE(QByteArray(b.columns[3].bytesAt(1)), cover);
    QVERIFY(b.columns[2].isNull(0));
    QVERIFY(b.columns[3].isNull(0));
    QCOMPARE(b.columns[2].typeAt(0), ValueType::Null);

    // Literal values of every kind, including NULL, without any table.
    const auto lit
        = drain(*connection, QStringLiteral("SELECT 1 AS a, 'x' AS b, 2.5 AS c, NULL AS d"));
    QVERIFY2(lit.ok, qPrintable(lit.errorText()));
    QCOMPARE(lit.columns.size(), size_t(4));
    const ResultBatch &l = lit.batches.front();
    QCOMPARE(l.columns[0].typeAt(0), ValueType::Int64);
    QCOMPARE(l.columns[1].typeAt(0), ValueType::Text);
    QCOMPARE(l.columns[2].typeAt(0), fixture().fractionalLiteralType());
    QVERIFY(l.columns[3].isNull(0));
}

void ProviderConformanceTest::parametersAreBoundNotConcatenated()
{
    auto connection = openConnection();
    QVERIFY(connection);
    const QString hostile = QStringLiteral("Robert'); DROP TABLE authors;--");
    QVERIFY(drain(*connection,
                  QStringLiteral("INSERT INTO authors(id, name) VALUES(%1, %2)")
                      .arg(fixture().placeholder(1), fixture().placeholder(2)),
                  { 7, hostile })
                .ok);
    const auto d = drain(*connection,
                         QStringLiteral("SELECT id, name FROM authors WHERE name = %1")
                             .arg(fixture().placeholder(1)),
                         { hostile });
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    QCOMPARE(d.rows, 1);
    QCOMPARE(d.batches.front().columns[0].int64At(0), 7);
    QCOMPARE(QString::fromUtf8(d.batches.front().columns[1].bytesAt(0)), hostile);

    // Wrong number of parameters is an error, not a silent NULL.
    const auto bad = drain(*connection, QStringLiteral("SELECT %1").arg(fixture().placeholder(1)));
    QVERIFY(!bad.ok);
}

void ProviderConformanceTest::streamsInBatches()
{
    auto connection = openConnection(false);
    QVERIFY(connection);
    auto stream = connection->execute(Statement { fixture().seriesSql(1050), {}, 100 }, {});
    QVERIFY2(stream, qPrintable(stream ? QString() : stream.error().detail));
    IResultStream &s = *stream.value();
    QCOMPARE(s.columns().size(), size_t(1));

    qint64 total = 0;
    std::vector<qsizetype> sizes;
    for (int guard = 0; !s.atEnd() && guard < 50; ++guard) {
        auto batch = s.fetch(100, {});
        QVERIFY2(batch, qPrintable(batch ? QString() : batch.error().detail));
        const ResultBatch &b = batch.value();
        QVERIFY(b.rowCount <= 100);
        QCOMPARE(b.columns.size(), size_t(1));
        QCOMPARE(b.columns[0].size(), b.rowCount);
        for (qsizetype i = 0; i < b.rowCount; ++i)
            QCOMPARE(b.columns[0].int64At(i), total + i + 1);
        total += b.rowCount;
        sizes.push_back(b.rowCount);
        if (sizes.size() == 1)
            QVERIFY(!s.atEnd()); // 950 rows are still pending
    }
    QVERIFY(s.atEnd());
    QCOMPARE(total, 1050);
    QVERIFY(sizes.size() >= 11);
    for (size_t i = 0; i + 1 < sizes.size(); ++i)
        QVERIFY2(sizes[i] == 100 || sizes[i] == 0, "fetchSize not honored");

    // Fetching after the end keeps returning an empty batch.
    auto after = s.fetch(100, {});
    QVERIFY(after);
    QCOMPARE(after.value().rowCount, 0);
    QVERIFY(s.atEnd());
}

void ProviderConformanceTest::affectedRowsForUpdateAndDelete()
{
    auto connection = openConnection();
    QVERIFY(connection);
    for (int id = 1; id <= 3; ++id) {
        QVERIFY(drain(*connection,
                      QStringLiteral("INSERT INTO authors(id, name) VALUES(%1, %2)")
                          .arg(fixture().placeholder(1), fixture().placeholder(2)),
                      { id, QStringLiteral("author %1").arg(id) })
                    .ok);
    }
    auto update
        = drain(*connection, QStringLiteral("UPDATE authors SET id = id + 10 WHERE id <= 2"));
    QVERIFY2(update.ok, qPrintable(update.errorText()));
    QCOMPARE(update.affected.value_or(-1), 2);
    QVERIFY(update.columns.empty());

    auto none = drain(*connection, QStringLiteral("UPDATE authors SET name = 'x' WHERE id = 99"));
    QVERIFY(none.ok);
    QCOMPARE(none.affected.value_or(-1), 0);

    auto del = drain(*connection, QStringLiteral("DELETE FROM authors WHERE id = 3"));
    QVERIFY2(del.ok, qPrintable(del.errorText()));
    QCOMPARE(del.affected.value_or(-1), 1);
}

void ProviderConformanceTest::syntaxErrorHasPosition()
{
    auto connection = openConnection();
    QVERIFY(connection);
    const QString sql = QStringLiteral("SELEC 1");
    auto stream = connection->execute(Statement { sql, {}, 500 }, {});
    QVERIFY(!stream);
    const Error error = std::move(stream).error();
    QCOMPARE(error.kind, ErrorKind::Syntax);
    QVERIFY(!error.message.isEmpty());
    QVERIFY(!error.detail.isEmpty());
    if (fixture().reportsErrorPosition()) {
        const int position = error.position.value_or(-1);
        QVERIFY2(position >= 0 && position <= sql.size(), "missing or out-of-range position");
    } else {
        QVERIFY(!error.position.has_value());
    }
    // The connection is still usable after an error.
    QVERIFY(drain(*connection, QStringLiteral("SELECT 1")).ok);
}

void ProviderConformanceTest::constraintViolationKind()
{
    auto connection = openConnection();
    QVERIFY(connection);
    const QString insert = QStringLiteral("INSERT INTO authors(id, name) VALUES(%1, %2)")
                               .arg(fixture().placeholder(1), fixture().placeholder(2));
    QVERIFY(drain(*connection, insert, { 1, QStringLiteral("Ann") }).ok);
    const auto duplicate = drain(*connection, insert, { 1, QStringLiteral("Bob") });
    QVERIFY(!duplicate.ok);
    QCOMPARE(duplicate.error.kind, ErrorKind::Constraint);
    QVERIFY(!duplicate.error.message.isEmpty());
    const auto notNull = drain(*connection, insert, { 2, QVariant() });
    QVERIFY(!notNull.ok);
    QCOMPARE(notNull.error.kind, ErrorKind::Constraint);
    const auto foreignKey = drain(*connection,
                                  QStringLiteral("INSERT INTO books(id, title, author_id) "
                                                 "VALUES(1, 't', 999)"));
    QVERIFY(!foreignKey.ok);
    QCOMPARE(foreignKey.error.kind, ErrorKind::Constraint);
}

void ProviderConformanceTest::missingTableIsNotFound()
{
    auto connection = openConnection();
    QVERIFY(connection);
    const auto d = drain(*connection, QStringLiteral("SELECT * FROM no_such_table"));
    QVERIFY(!d.ok);
    QCOMPARE(d.error.kind, ErrorKind::NotFound);
}

void ProviderConformanceTest::cancelLongQueryIsFastAndConnectionSurvives()
{
    auto connection = openConnection(false);
    QVERIFY(connection);
    std::atomic<bool> started { false };
    const QString slow = fixture().slowQuerySql();
    auto worker = std::async(std::launch::async, [&]() -> std::optional<ErrorKind> {
        started = true;
        auto stream = connection->execute(Statement { slow, {}, 500 }, {});
        if (!stream)
            return stream.error().kind;
        auto batch = stream.value()->fetch(500, {});
        if (!batch)
            return batch.error().kind;
        return std::nullopt;
    });
    QTRY_VERIFY_WITH_TIMEOUT(started.load(), 5000);
    QThread::msleep(150); // let it get properly stuck inside the engine

    QElapsedTimer clock;
    clock.start();
    connection->requestCancel();
    QVERIFY2(worker.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
             "the query did not stop");
    const qint64 elapsed = clock.elapsed();
    const auto kind = worker.get();
    QVERIFY2(kind.has_value(), "the query finished instead of being cancelled");
    QCOMPARE(kind.value_or(ErrorKind::Internal), ErrorKind::Cancelled);
    QVERIFY2(elapsed < 200, qPrintable(QStringLiteral("cancel took %1 ms").arg(elapsed)));

    const auto d = drain(*connection, QStringLiteral("SELECT 1"));
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    QCOMPARE(d.rows, 1);
}

void ProviderConformanceTest::staleCancelWhileIdleIsIgnored()
{
    auto connection = openConnection(false);
    QVERIFY(connection);
    connection->requestCancel(); // nothing is running: must not poison the next statement
    const auto d = drain(*connection, QStringLiteral("SELECT 1"));
    QVERIFY2(d.ok, qPrintable(d.errorText()));
    QCOMPARE(d.rows, 1);
}

void ProviderConformanceTest::staleCancelBetweenFetchesIsIgnored()
{
    auto connection = openConnection(false);
    QVERIFY(connection);
    auto stream = connection->execute(Statement { fixture().seriesSql(1000), {}, 10 }, {});
    QVERIFY(stream);
    auto first = stream.value()->fetch(10, {});
    QVERIFY(first);
    QCOMPARE(first.value().rowCount, 10);
    connection->requestCancel(); // stream is idle between fetches
    auto second = stream.value()->fetch(10, {});
    QVERIFY2(second, qPrintable(second ? QString() : second.error().detail));
    QCOMPARE(second.value().rowCount, 10);
    QCOMPARE(second.value().columns[0].int64At(0), 11);
}

void ProviderConformanceTest::cancelledTokenIsHonored()
{
    auto connection = openConnection(false);
    QVERIFY(connection);
    CancellationSource source;
    source.cancel();
    auto rejected
        = connection->execute(Statement { QStringLiteral("SELECT 1"), {}, 500 }, source.token());
    QVERIFY(!rejected);
    QCOMPARE(std::move(rejected).error().kind, ErrorKind::Cancelled);

    {
        auto stream = connection->execute(Statement { fixture().seriesSql(1000), {}, 10 }, {});
        QVERIFY(stream);
        auto batch = stream.value()->fetch(10, source.token());
        QVERIFY(!batch);
        QCOMPARE(std::move(batch).error().kind, ErrorKind::Cancelled);
    }
    // And the connection is fine afterwards.
    QVERIFY(drain(*connection, QStringLiteral("SELECT 1")).ok);
}

void ProviderConformanceTest::introspectionTree()
{
    auto connection = openConnection();
    QVERIFY(connection);
    std::vector<SchemaNode> all;
    QVERIFY(collect(*connection, ObjectPath { {}, ObjectKind::Database }, 0, all));

    auto find = [&](const ObjectPath &target) {
        return std::find_if(all.begin(), all.end(), [&](const SchemaNode &node) {
            return node.path.kind == target.kind && node.path.parts == target.parts;
        });
    };
    const auto books = find(fixture().booksPath());
    QVERIFY2(books != all.end(), "books table not reachable from the root");
    QVERIFY(books->hasChildren);
    const auto view = find(fixture().viewPath());
    QVERIFY2(view != all.end(), "view not reachable from the root");
    QCOMPARE(view->path.kind, ObjectKind::View);

    std::vector<SchemaNode> tableNodes;
    QVERIFY(collect(*connection, fixture().booksPath(), 0, tableNodes));
    for (const QString &column :
         { QStringLiteral("id"), QStringLiteral("title"), QStringLiteral("author_id"),
           QStringLiteral("price"), QStringLiteral("cover") })
        QVERIFY2(containsName(tableNodes, ObjectKind::Column, column), qPrintable(column));
    QVERIFY(containsName(tableNodes, ObjectKind::Index, QStringLiteral("idx_books_title")));
    const auto fks = std::count_if(tableNodes.begin(), tableNodes.end(), [](const SchemaNode &n) {
        return n.path.kind == ObjectKind::ForeignKey;
    });
    QCOMPARE(fks, 1);
    for (const SchemaNode &node : tableNodes) {
        if (node.path.kind == ObjectKind::Column) {
            QVERIFY(!node.hasChildren);
            if (node.name == QLatin1String("title"))
                QVERIFY(!node.detail.isEmpty());
        }
    }

    std::vector<SchemaNode> viewNodes;
    QVERIFY(collect(*connection, fixture().viewPath(), 0, viewNodes));
    QVERIFY(containsName(viewNodes, ObjectKind::Column, QStringLiteral("title")));
}

// Autocompletion (ARCHITECTURE.md 6.5) reads columns from ObjectPath{table.parts + "Columns",
// Folder}.
void ProviderConformanceTest::columnsFolderPath()
{
    auto connection = openConnection();
    QVERIFY(connection);
    for (const ObjectPath &relation : { fixture().booksPath(), fixture().viewPath() }) {
        const ObjectPath folder { relation.parts + QStringList { QStringLiteral("Columns") },
                                  ObjectKind::Folder };
        // The relation itself offers that very folder.
        auto offered = connection->children(relation, {});
        QVERIFY2(offered, qPrintable(offered ? QString() : offered.error().detail));
        const auto it = std::find_if(
            offered.value().begin(), offered.value().end(), [&](const SchemaNode &n) {
                return n.path.kind == ObjectKind::Folder && n.path.parts == folder.parts;
            });
        QVERIFY2(it != offered.value().end(), "no Columns folder under the relation");

        auto columns = connection->children(folder, {});
        QVERIFY2(columns, qPrintable(columns ? QString() : columns.error().detail));
        QVERIFY(!columns.value().empty());
        for (const SchemaNode &node : columns.value()) {
            QCOMPARE(node.path.kind, ObjectKind::Column);
            QVERIFY(!node.name.isEmpty());
        }
        QVERIFY(containsName(columns.value(), ObjectKind::Column, QStringLiteral("title")));
    }
    // Every column of books, and only them.
    auto books = connection->children(
        ObjectPath { fixture().booksPath().parts + QStringList { QStringLiteral("Columns") },
                     ObjectKind::Folder },
        {});
    QVERIFY(books);
    QStringList names;
    for (const SchemaNode &node : books.value())
        names << node.name;
    names.sort();
    QCOMPARE(
        names,
        (QStringList { QStringLiteral("author_id"), QStringLiteral("cover"), QStringLiteral("id"),
                       QStringLiteral("price"), QStringLiteral("title") }));
}

void ProviderConformanceTest::describeTable()
{
    auto connection = openConnection();
    QVERIFY(connection);
    auto details = connection->describe(fixture().booksPath(), {});
    QVERIFY2(details, qPrintable(details ? QString() : details.error().detail));
    const TableDetails &t = details.value();

    QStringList names;
    for (const ColumnDef &c : t.columns)
        names << c.name;
    QCOMPARE(
        names,
        (QStringList { QStringLiteral("id"), QStringLiteral("title"), QStringLiteral("author_id"),
                       QStringLiteral("price"), QStringLiteral("cover") }));
    QCOMPARE(t.columns[0].type, ValueType::Int64);
    QCOMPARE(t.columns[1].type, ValueType::Text);
    QVERIFY(!t.columns[1].nullable);
    QVERIFY(t.columns[3].nullable);
    QVERIFY(!t.columns[1].nativeType.isEmpty());

    QVERIFY(t.primaryKey.has_value());
    QCOMPARE(t.primaryKey.value_or(QStringList()), QStringList { QStringLiteral("id") });

    const auto index = std::find_if(t.indexes.begin(), t.indexes.end(), [](const IndexDef &i) {
        return i.name == QLatin1String("idx_books_title");
    });
    QVERIFY(index != t.indexes.end());
    QCOMPARE(index->columns, QStringList { QStringLiteral("title") });
    QVERIFY(!index->unique);

    QCOMPARE(t.foreignKeys.size(), size_t(1));
    QCOMPARE(t.foreignKeys[0].columns, QStringList { QStringLiteral("author_id") });
    QCOMPARE(t.foreignKeys[0].referencedColumns, QStringList { QStringLiteral("id") });
    QVERIFY(!t.foreignKeys[0].referencedTable.parts.isEmpty());
    QCOMPARE(t.foreignKeys[0].referencedTable.parts.last(), QStringLiteral("authors"));

    auto missing = connection->describe(
        ObjectPath { fixture().booksPath().parts.mid(0, fixture().booksPath().parts.size() - 1)
                         + QStringList { QStringLiteral("no_such_table") },
                     ObjectKind::Table },
        {});
    QVERIFY(!missing);
    QCOMPARE(std::move(missing).error().kind, ErrorKind::NotFound);
}

void ProviderConformanceTest::quotingWorksInsideStatements()
{
    auto connection = openConnection(false);
    QVERIFY(connection);
    const ISqlDialect &dialect = fixture().provider().dialect();
    const QString table = QStringLiteral("we\"ird; name'x");
    const QString create
        = QStringLiteral("CREATE TABLE %1 (a INTEGER)").arg(dialect.quoteIdentifier(table));
    QVERIFY2(drain(*connection, create).ok, qPrintable(create));
    QVERIFY(drain(*connection,
                  QStringLiteral("INSERT INTO %1 VALUES (5)").arg(dialect.quoteIdentifier(table)))
                .ok);
    const auto count = drain(
        *connection, QStringLiteral("SELECT a FROM %1").arg(dialect.quoteIdentifier(table)));
    QVERIFY2(count.ok, qPrintable(count.errorText()));
    QCOMPARE(count.rows, 1);

    const QString text = QStringLiteral("it's; \"quoted\" -- not a comment");
    const auto literal
        = drain(*connection, QStringLiteral("SELECT %1").arg(dialect.quoteLiteral(text)));
    QVERIFY2(literal.ok, qPrintable(literal.errorText()));
    QCOMPARE(QString::fromUtf8(literal.batches.front().columns[0].bytesAt(0)), text);

    QVERIFY(!dialect.highlightingDefinition().isEmpty());
    QVERIFY(!dialect.keywords().isEmpty());
    QVERIFY(dialect.limitClause(10, 0).contains(QLatin1String("10")));
    QVERIFY(dialect.limitClause(10, 20).contains(QLatin1String("20")));
}

void ProviderConformanceTest::splitStatements()
{
    std::vector<SplitCase> cases {
        { QStringLiteral("SELECT 1; SELECT 2;"),
          { QStringLiteral("SELECT 1"), QStringLiteral("SELECT 2") } },
        { QStringLiteral("  SELECT 1  ;\n\n;; SELECT 2"),
          { QStringLiteral("SELECT 1"), QStringLiteral("SELECT 2") } },
        { QStringLiteral("SELECT 'a;b'; SELECT \"c;d\""),
          { QStringLiteral("SELECT 'a;b'"), QStringLiteral("SELECT \"c;d\"") } },
        { QStringLiteral("SELECT 1 -- ; not a split\n; SELECT 2"),
          { QStringLiteral("SELECT 1 -- ; not a split"), QStringLiteral("SELECT 2") } },
        { QStringLiteral("/* ; */ SELECT 1; /* x */"), { QStringLiteral("/* ; */ SELECT 1") } },
        { QStringLiteral("SELECT 'it''s;'; SELECT 2"),
          { QStringLiteral("SELECT 'it''s;'"), QStringLiteral("SELECT 2") } },
        { QStringLiteral("SELECT 'é;ü'; SELECT '日本;'"),
          { QStringLiteral("SELECT 'é;ü'"), QStringLiteral("SELECT '日本;'") } },
        { QString(), {} },
        { QStringLiteral("  ;  ; "), {} },
        { QStringLiteral("-- only a comment"), {} },
    };
    for (SplitCase &extra : fixture().extraSplitCases())
        cases.push_back(std::move(extra));

    const ISqlDialect &dialect = fixture().provider().dialect();
    for (const SplitCase &c : cases) {
        const auto ranges = dialect.splitStatements(c.script);
        QStringList actual;
        qsizetype previousEnd = 0;
        for (const StatementRange &r : ranges) {
            QVERIFY2(r.begin >= previousEnd && r.begin < r.end && r.end <= c.script.size(),
                     qPrintable(c.script));
            previousEnd = r.end;
            actual << c.script.mid(r.begin, r.end - r.begin);
        }
        QVERIFY2(actual == c.expected,
                 qPrintable(QStringLiteral("script: %1\nactual: %2")
                                .arg(c.script, actual.join(QLatin1String(" || ")))));
    }
}

namespace {

// Log of what the provider saw, written on the session thread and read after a signal arrived.
struct Events
{
    std::mutex mutex;
    QStringList list;
    QList<Statement> statements;
    void add(const QString &text)
    {
        std::lock_guard lock(mutex);
        list << text;
    }
    QStringList snapshot()
    {
        std::lock_guard lock(mutex);
        return list;
    }
};

// Forwards everything to a real connection and records transaction calls and statements.
class RecordingConnection : public IConnection
{
public:
    RecordingConnection(std::unique_ptr<IConnection> inner, std::shared_ptr<Events> events)
        : m_inner(std::move(inner))
        , m_events(std::move(events))
    { }
    Result<ServerInfo> open(const CancellationToken &t) override { return m_inner->open(t); }
    void close() override { m_inner->close(); }
    bool isOpen() const override { return m_inner->isOpen(); }
    Result<std::vector<SchemaNode>> children(const ObjectPath &p,
                                             const CancellationToken &t) override
    {
        return m_inner->children(p, t);
    }
    Result<TableDetails> describe(const ObjectPath &p, const CancellationToken &t) override
    {
        return m_inner->describe(p, t);
    }
    Result<std::unique_ptr<IResultStream>> execute(const Statement &s,
                                                   const CancellationToken &t) override
    {
        {
            std::lock_guard lock(m_events->mutex);
            m_events->list << QStringLiteral("sql");
            m_events->statements.push_back(s);
        }
        return m_inner->execute(s, t);
    }
    void requestCancel() override { m_inner->requestCancel(); }
    Result<void> begin() override
    {
        m_events->add(QStringLiteral("begin"));
        return m_inner->begin();
    }
    Result<void> commit() override
    {
        m_events->add(QStringLiteral("commit"));
        return m_inner->commit();
    }
    Result<void> rollback() override
    {
        m_events->add(QStringLiteral("rollback"));
        return m_inner->rollback();
    }

private:
    std::unique_ptr<IConnection> m_inner;
    std::shared_ptr<Events> m_events;
};

RowEdit updateEdit(const QVariant &id, std::vector<CellChange> cells)
{
    RowEdit e;
    e.kind = RowEdit::Kind::Update;
    e.key = { id };
    e.cells = std::move(cells);
    return e;
}

RowEdit insertEdit(std::vector<CellChange> cells)
{
    RowEdit e;
    e.kind = RowEdit::Kind::Insert;
    e.cells = std::move(cells);
    return e;
}

RowEdit deleteEdit(const QVariant &id)
{
    RowEdit e;
    e.kind = RowEdit::Kind::Delete;
    e.key = { id };
    return e;
}

// Column indexes of books in EditTarget::columns (see booksTarget()).
constexpr int kId = 0;
constexpr int kTitle = 1;
constexpr int kAuthor = 2;
constexpr int kPrice = 3;

} // namespace

QString ProviderConformanceTest::scalar(IConnection &connection, const QString &sql)
{
    const auto d = drain(connection, sql);
    if (!d.ok || d.batches.empty() || d.batches.front().rowCount < 1) {
        recordFailure(QStringLiteral("scalar failed: ") + sql + QStringLiteral(" | ")
                          + d.errorText(),
                      __LINE__);
        return {};
    }
    return CellFormatter::display(d.batches.front().columns[0], 0);
}

EditTarget ProviderConformanceTest::booksTarget() const
{
    EditTarget t;
    t.table = m_fixture->booksPath();
    t.columns = { QStringLiteral("id"), QStringLiteral("title"), QStringLiteral("author_id"),
                  QStringLiteral("price"), QStringLiteral("cover") };
    t.key = RowKey { RowKey::Kind::PrimaryKey, { QStringLiteral("id") } };
    return t;
}

bool ProviderConformanceTest::seedBooks(IConnection &connection)
{
    const QStringList sql {
        QStringLiteral("INSERT INTO authors(id, name) VALUES (1, 'x'), (2, 'y')"),
        QStringLiteral("INSERT INTO books(id, title, author_id) VALUES (1, 'a', 1), (2, 'b', 1), "
                       "(3, 'c', 2)"),
    };
    for (const QString &statement : sql) {
        const auto d = drain(connection, statement);
        if (!d.ok) {
            recordFailure(QStringLiteral("seed failed: ") + d.errorText(), __LINE__);
            return false;
        }
    }
    return true;
}

void ProviderConformanceTest::dialectEditingHelpers()
{
    const ISqlDialect &dialect = fixture().provider().dialect();
    QCOMPARE(dialect.placeholder(1), fixture().placeholder(1));
    QCOMPARE(dialect.placeholder(12), fixture().placeholder(12));
    const QString qualified = dialect.qualifiedName(fixture().booksPath());
    QVERIFY(qualified.endsWith(dialect.quoteIdentifier(QStringLiteral("books"))));
    if (fixture().hasRowIdentifier())
        QCOMPARE(dialect.rowIdentifier(), std::optional<QString>(fixture().rowIdentifierName()));
    else
        QVERIFY(!dialect.rowIdentifier().has_value());

    // The qualified name must work inside a statement, and describe() must report the key.
    auto connection = openConnection();
    QVERIFY(connection);
    QCOMPARE(scalar(*connection, QStringLiteral("SELECT count(*) FROM %1").arg(qualified)),
             QStringLiteral("0"));
    auto details = connection->describe(fixture().booksPath(), {});
    QVERIFY(details);
    QCOMPARE(details.value().primaryKey.value_or(QStringList()),
             QStringList { QStringLiteral("id") });
}

void ProviderConformanceTest::applyChangesCommitsBatch()
{
    auto raw = openConnection();
    QVERIFY(raw);
    QVERIFY(seedBooks(*raw));
    auto events = std::make_shared<Events>();
    Session session(std::make_unique<RecordingConnection>(std::move(raw), events));

    const std::vector<RowEdit> edits {
        updateEdit(1, { { kTitle, QStringLiteral("A") } }),
        insertEdit({ { kId, QStringLiteral("10") },
                     { kTitle, QStringLiteral("t") },
                     { kAuthor, QStringLiteral("1") },
                     { kPrice, QStringLiteral("2.5") } }),
        deleteEdit(3),
    };
    auto planned = ChangeSqlBuilder::build(fixture().provider().dialect(), booksTarget(), edits);
    QVERIFY(planned);
    QSignalSpy applied(&session, &Session::changesApplied);
    QSignalSpy failed(&session, &Session::changesFailed);
    session.applyChanges(std::move(planned).value());
    QVERIFY(applied.wait(15000) || !applied.isEmpty());
    QCOMPARE(failed.count(), 0);
    QCOMPARE(applied.takeFirst().value(0).toInt(), 3);

    auto check = openConnection(false);
    QVERIFY(check);
    QCOMPARE(scalar(*check, QStringLiteral("SELECT title FROM books WHERE id = 1")),
             QStringLiteral("A"));
    QCOMPARE(scalar(*check, QStringLiteral("SELECT title FROM books WHERE id = 10")),
             QStringLiteral("t"));
    QCOMPARE(scalar(*check, QStringLiteral("SELECT count(*) FROM books")), QStringLiteral("3"));
    QCOMPARE(scalar(*check, QStringLiteral("SELECT count(*) FROM books WHERE id = 3")),
             QStringLiteral("0"));
    const QStringList log = events->snapshot();
    QCOMPARE(log.value(0), QStringLiteral("begin"));
    QCOMPARE(log.value(log.size() - 1), QStringLiteral("commit"));
}

void ProviderConformanceTest::applyChangesRollsBackOnFailingStatement()
{
    auto raw = openConnection();
    QVERIFY(raw);
    QVERIFY(seedBooks(*raw));
    auto events = std::make_shared<Events>();
    Session session(std::make_unique<RecordingConnection>(std::move(raw), events));

    // The second statement repeats the primary key 2.
    const std::vector<RowEdit> edits {
        updateEdit(1, { { kTitle, QStringLiteral("Z") } }),
        insertEdit({ { kId, QStringLiteral("2") },
                     { kTitle, QStringLiteral("dup") },
                     { kAuthor, QStringLiteral("1") } }),
        deleteEdit(3),
    };
    auto planned = ChangeSqlBuilder::build(fixture().provider().dialect(), booksTarget(), edits);
    QVERIFY(planned);
    QSignalSpy applied(&session, &Session::changesApplied);
    QSignalSpy failed(&session, &Session::changesFailed);
    session.applyChanges(std::move(planned).value());
    QVERIFY(failed.wait(15000) || !failed.isEmpty());
    QCOMPARE(applied.count(), 0);
    QCOMPARE(failed.value(0).value(0).toInt(), 1);
    QCOMPARE(failed.value(0).value(1).value<Error>().kind, ErrorKind::Constraint);

    auto check = openConnection(false);
    QVERIFY(check);
    QCOMPARE(scalar(*check, QStringLiteral("SELECT title FROM books WHERE id = 1")),
             QStringLiteral("a"));
    QCOMPARE(scalar(*check, QStringLiteral("SELECT count(*) FROM books")), QStringLiteral("3"));
    QVERIFY(events->snapshot().contains(QStringLiteral("rollback")));
    QVERIFY(!events->snapshot().contains(QStringLiteral("commit")));
}

void ProviderConformanceTest::applyChangesRollsBackOnZeroRowUpdate()
{
    auto raw = openConnection();
    QVERIFY(raw);
    QVERIFY(seedBooks(*raw));
    Session session(std::move(raw));

    const std::vector<RowEdit> edits {
        updateEdit(1, { { kTitle, QStringLiteral("Z") } }),
        updateEdit(999, { { kTitle, QStringLiteral("Q") } }),
    };
    auto planned = ChangeSqlBuilder::build(fixture().provider().dialect(), booksTarget(), edits);
    QVERIFY(planned);
    QSignalSpy failed(&session, &Session::changesFailed);
    session.applyChanges(std::move(planned).value());
    QVERIFY(failed.wait(15000) || !failed.isEmpty());
    QCOMPARE(failed.value(0).value(0).toInt(), 1);

    auto check = openConnection(false);
    QVERIFY(check);
    QCOMPARE(scalar(*check, QStringLiteral("SELECT title FROM books WHERE id = 1")),
             QStringLiteral("a"));
}

void ProviderConformanceTest::applyChangesWithRowIdentifierKey()
{
    if (!fixture().hasRowIdentifier())
        QSKIP("the engine has no hidden row identifier");
    auto raw = openConnection(false);
    QVERIFY(raw);
    QVERIFY(drain(*raw, QStringLiteral("CREATE TABLE loose(a TEXT, b TEXT)")).ok);
    QVERIFY(drain(*raw, QStringLiteral("INSERT INTO loose VALUES ('one', 'x'), ('two', 'y')")).ok);

    const ISqlDialect &dialect = fixture().provider().dialect();
    const QString rowId = dialect.rowIdentifier().value_or(QString());
    QVERIFY(!rowId.isEmpty());
    ObjectPath path = fixture().booksPath();
    path.parts.last() = QStringLiteral("loose");
    auto details = raw->describe(path, {});
    QVERIFY(details);
    QVERIFY(!details.value().primaryKey.has_value());

    // The grid selects the hidden column first and keeps its text as the key value.
    const QString key
        = scalar(*raw,
                 QStringLiteral("SELECT %1 FROM %2 WHERE a = 'two'")
                     .arg(dialect.quoteIdentifier(rowId), dialect.qualifiedName(path)));
    QVERIFY(!key.isEmpty());

    EditTarget target;
    target.table = path;
    target.columns = { QStringLiteral("a"), QStringLiteral("b") };
    target.key = RowKey { RowKey::Kind::RowIdentifier, { rowId } };
    RowEdit edit;
    edit.key = { key };
    edit.cells = { { 1, QStringLiteral("changed") } };
    auto planned = ChangeSqlBuilder::build(dialect, target, { edit });
    QVERIFY(planned);

    Session session(std::move(raw));
    QSignalSpy applied(&session, &Session::changesApplied);
    QSignalSpy failed(&session, &Session::changesFailed);
    session.applyChanges(std::move(planned).value());
    QVERIFY(applied.wait(15000) || !applied.isEmpty());
    QCOMPARE(failed.count(), 0);

    auto check = openConnection(false);
    QVERIFY(check);
    QCOMPARE(scalar(*check, QStringLiteral("SELECT b FROM loose WHERE a = 'two'")),
             QStringLiteral("changed"));
    QCOMPARE(scalar(*check, QStringLiteral("SELECT b FROM loose WHERE a = 'one'")),
             QStringLiteral("x"));
}

void ProviderConformanceTest::applyChangesNullInSet()
{
    auto raw = openConnection();
    QVERIFY(raw);
    QVERIFY(seedBooks(*raw));
    QVERIFY(drain(*raw, QStringLiteral("UPDATE books SET price = 1.5 WHERE id = 1")).ok);
    Session session(std::move(raw));

    const std::vector<RowEdit> edits {
        updateEdit(1, { { kPrice, QVariant() } }),
        insertEdit({ { kId, QStringLiteral("11") },
                     { kTitle, QStringLiteral("n") },
                     { kAuthor, QStringLiteral("1") },
                     { kPrice, QVariant() } }),
    };
    auto planned = ChangeSqlBuilder::build(fixture().provider().dialect(), booksTarget(), edits);
    QVERIFY(planned);
    QSignalSpy applied(&session, &Session::changesApplied);
    QSignalSpy failed(&session, &Session::changesFailed);
    session.applyChanges(std::move(planned).value());
    QVERIFY(applied.wait(15000) || !applied.isEmpty());
    QCOMPARE(failed.count(), 0);

    auto check = openConnection(false);
    QVERIFY(check);
    QCOMPARE(scalar(*check, QStringLiteral("SELECT count(*) FROM books WHERE price IS NULL")),
             QStringLiteral("4"));
}

void ProviderConformanceTest::applyChangesPassesExactlyThePlannedStatements()
{
    auto raw = openConnection();
    QVERIFY(raw);
    QVERIFY(seedBooks(*raw));
    auto events = std::make_shared<Events>();
    Session session(std::make_unique<RecordingConnection>(std::move(raw), events));

    const std::vector<RowEdit> edits {
        updateEdit(1, { { kTitle, QStringLiteral("it's \"q\"") }, { kPrice, QVariant() } }),
        deleteEdit(3),
    };
    auto planned = ChangeSqlBuilder::build(fixture().provider().dialect(), booksTarget(), edits);
    QVERIFY(planned);
    const std::vector<PlannedStatement> preview = planned.value();
    QSignalSpy applied(&session, &Session::changesApplied);
    session.applyChanges(std::move(planned).value());
    QVERIFY(applied.wait(15000) || !applied.isEmpty());

    std::lock_guard lock(events->mutex);
    const QStringList expected { QStringLiteral("begin"), QStringLiteral("sql"),
                                 QStringLiteral("sql"), QStringLiteral("commit") };
    QCOMPARE(events->list, expected);
    QCOMPARE(events->statements.size(), static_cast<qsizetype>(preview.size()));
    for (size_t i = 0; i < preview.size(); ++i) {
        const Statement &seen = events->statements.value(static_cast<qsizetype>(i));
        QCOMPARE(seen.sql, preview[i].statement.sql);
        QCOMPARE(seen.parameters, preview[i].statement.parameters);
    }
}

void ProviderConformanceTest::applyChangesWhileBusyFailsWithoutTouchingAnything()
{
    auto raw = openConnection();
    QVERIFY(raw);
    auto events = std::make_shared<Events>();
    Session session(std::make_unique<RecordingConnection>(std::move(raw), events));
    QSignalSpy started(&session, &Session::started);
    QSignalSpy cancelled(&session, &Session::cancelled);
    QSignalSpy failed(&session, &Session::changesFailed);
    session.execute(Statement { fixture().slowQuerySql(), {}, 500 });
    QVERIFY(started.wait(15000) || !started.isEmpty());

    auto planned
        = ChangeSqlBuilder::build(fixture().provider().dialect(), booksTarget(), { deleteEdit(1) });
    QVERIFY(planned);
    session.applyChanges(std::move(planned).value());
    QVERIFY(failed.wait(15000) || !failed.isEmpty());
    QCOMPARE(failed.value(0).value(0).toInt(), -1);

    session.cancel();
    QVERIFY(cancelled.wait(15000) || !cancelled.isEmpty());
    const QStringList log = events->snapshot();
    QVERIFY(!log.contains(QStringLiteral("begin")));
    QCOMPARE(log.count(QStringLiteral("sql")), 1);
}

void ProviderConformanceTest::cancelDuringApplyRollsBack()
{
    auto raw = openConnection();
    QVERIFY(raw);
    QVERIFY(seedBooks(*raw));
    auto events = std::make_shared<Events>();
    Session session(std::make_unique<RecordingConnection>(std::move(raw), events));

    PlannedStatement change;
    change.statement.sql = QStringLiteral("UPDATE books SET title = 'Z' WHERE id = 1");
    PlannedStatement slow;
    slow.statement.sql = fixture().slowQuerySql();
    slow.expectOneRow = false;
    slow.editIndex = 1;
    QSignalSpy failed(&session, &Session::changesFailed);
    session.applyChanges({ change, slow });
    QTRY_VERIFY_WITH_TIMEOUT(events->snapshot().count(QStringLiteral("sql")) == 2, 15000);
    QTest::qWait(100); // let the provider enter the slow statement
    session.cancel();
    QVERIFY(failed.wait(15000) || !failed.isEmpty());
    QCOMPARE(failed.value(0).value(0).toInt(), 1);
    QCOMPARE(failed.value(0).value(1).value<Error>().kind, ErrorKind::Cancelled);
    const QStringList log = events->snapshot();
    QCOMPARE(log.value(log.size() - 1), QStringLiteral("rollback"));

    auto check = openConnection(false);
    QVERIFY(check);
    QCOMPARE(scalar(*check, QStringLiteral("SELECT title FROM books WHERE id = 1")),
             QStringLiteral("a"));
}

} // namespace quarry::conformance
