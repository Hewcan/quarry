#include <quarry/dump.h>
#include <quarry/dump_runner.h>

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <sqlite3.h>

using namespace quarry;

namespace {

// Runs SQL on a database file (created if missing); fails the test on error.
bool exec(const QString &file, const QByteArray &sql)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open(file.toUtf8().constData(), &db) != SQLITE_OK)
        return false;
    char *message = nullptr;
    const int rc = sqlite3_exec(db, sql.constData(), nullptr, nullptr, &message);
    if (rc != SQLITE_OK)
        qWarning("%s", message);
    sqlite3_free(message);
    sqlite3_close(db);
    return rc == SQLITE_OK;
}

QStringList query(const QString &file, const QByteArray &sql)
{
    QStringList rows;
    sqlite3 *db = nullptr;
    sqlite3_open_v2(file.toUtf8().constData(), &db, SQLITE_OPEN_READONLY, nullptr);
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.constData(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            QStringList cells;
            for (int i = 0; i < sqlite3_column_count(stmt); ++i)
                cells << QString::fromUtf8(
                        reinterpret_cast<const char *>(sqlite3_column_text(stmt, i)));
            rows << cells.join(QLatin1Char('|'));
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rows;
}

// Everything that defines the database: schema and every row with its storage type.
QStringList snapshot(const QString &file)
{
    QStringList out = query(file, "SELECT type, name, tbl_name, sql FROM sqlite_master "
                                  "WHERE name NOT LIKE 'sqlite_autoindex%' ORDER BY type, name");
    const QStringList tables =
            query(file, "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name");
    for (const QString &table : tables) {
        out << QStringLiteral("== ") + table;
        const QByteArray name = table.toUtf8().replace('"', "\"\"");
        const QByteArray literal = table.toUtf8().replace('\'', "''");
        const QStringList columns = query(file, "SELECT name FROM pragma_table_xinfo('" + literal
                                                  + "') WHERE hidden IN (0, 2, 3)");
        QStringList quoted;
        for (const QString &column : columns)
            quoted << QStringLiteral("typeof(\"%1\") || ':' || quote(\"%1\")").arg(column);
        out << query(file, "SELECT " + quoted.join(QLatin1Char(',')).toUtf8() + " FROM \"" + name
                             + "\" ORDER BY 1, 2");
    }
    return out;
}

struct Outcome
{
    QString what; // "finished", "failed", "cancelled"
    Error error;
    QStringList log;
};

Outcome waitFor(DumpRunner &runner, quint64 id)
{
    Outcome outcome;
    bool done = false;
    QObject context;
    QObject::connect(&runner, &DumpRunner::finished, &context, [&](quint64 got) {
        if (got == id) {
            outcome.what = QStringLiteral("finished");
            done = true;
        }
    });
    QObject::connect(&runner, &DumpRunner::cancelled, &context, [&](quint64 got) {
        if (got == id) {
            outcome.what = QStringLiteral("cancelled");
            done = true;
        }
    });
    QObject::connect(&runner, &DumpRunner::failed, &context, [&](quint64 got, const Error &e) {
        if (got == id) {
            outcome.what = QStringLiteral("failed");
            outcome.error = e;
            done = true;
        }
    });
    QObject::connect(&runner, &DumpRunner::output, &context,
                     [&](quint64 got, const QString &text, bool) {
                         if (got == id)
                             outcome.log << text;
                     });
    if (!QTest::qWaitFor([&done] { return done; }, 30000))
        outcome.what = QStringLiteral("timeout");
    return outcome;
}

DumpTarget sqliteTarget(const QString &file)
{
    DumpTarget target;
    target.providerId = QStringLiteral("sqlite");
    target.database = file;
    return target;
}

DumpTarget postgresTarget()
{
    DumpTarget target;
    target.providerId = QStringLiteral("postgres");
    target.host = QStringLiteral("db.example");
    target.port = 6543;
    target.database = QStringLiteral("shop");
    target.user = QStringLiteral("app");
    target.password = QStringLiteral("s3cr:et");
    return target;
}

const QByteArray kSampleSchema = R"sql(
PRAGMA foreign_keys=ON;
CREATE TABLE customers(id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, photo BLOB,
                       score REAL, note);
CREATE TABLE orders(id INTEGER PRIMARY KEY, customer_id INTEGER REFERENCES customers(id),
                    total REAL, doubled REAL GENERATED ALWAYS AS (total * 2) VIRTUAL);
CREATE TABLE "odd ""name"""(k TEXT PRIMARY KEY, v) WITHOUT ROWID;
CREATE TABLE logs(id INTEGER PRIMARY KEY, message TEXT);
CREATE INDEX orders_customer ON orders(customer_id);
CREATE TRIGGER orders_log AFTER INSERT ON orders BEGIN
    INSERT INTO logs(message) VALUES ('order ' || NEW.id);
END;
CREATE VIEW big_orders AS SELECT * FROM orders WHERE total > 10;
)sql";

const QByteArray kSampleData = R"sql(
INSERT INTO customers(name, photo, score, note) VALUES
    ('O''Brien', X'00FF10', 2.0, NULL),
    ('line
break', NULL, 0.1, 42),
    ('ñandú ✓', X'', -1.5e300, 'text');
INSERT INTO customers(name, note) VALUES (CAST(X'61006200' AS TEXT), 3.25);
DELETE FROM customers WHERE id = 4;
INSERT INTO orders(customer_id, total) VALUES (1, 5), (1, 12.5), (2, 99);
INSERT INTO "odd ""name"""(k, v) VALUES ('a', 1), ('b', X'01');
)sql";

} // namespace

class TstDump : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void closureFollowsReferencedTablesOnly();
    void pgArgumentsNeverCarryThePassword();
    void pgArgumentsForContentAndSelection();
    void pgPassLineEscapes();
    void resolvesConfiguredToolFileAndFolder();
    void targetFromProfile();
    void sqliteRoundTripReproducesTheDatabase();
    void sqliteSchemaOnlyThenDataOnly();
    void sqliteSelectedTables();
    void sqliteMissingTableFails();
    void sqliteRestoreFailureRollsBack();
    void sqliteCancelLeavesNoFile();
    void sqliteRestoreRejectsPostgresArchive();
    void postgresDumpUsesPassFile();
    void postgresDumpErrors_data();
    void postgresDumpErrors();
    void postgresCancelLeavesNoFile();
    void postgresRestorePicksTheTool();
    void refusesUnsupportedTargets();

private:
    QTemporaryDir m_dir;
    QString m_sample;
    DumpToolPaths m_fakeTools;
};

void TstDump::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_sample = m_dir.filePath(QStringLiteral("sample.db"));
    QVERIFY(exec(m_sample, kSampleSchema + kSampleData));
    m_fakeTools.pgDump = QStringLiteral(QUARRY_FAKE_PG_TOOL);
    m_fakeTools.psql = QStringLiteral(QUARRY_FAKE_PG_TOOL);
    m_fakeTools.pgRestore = QStringLiteral(QUARRY_FAKE_PG_TOOL);
    // Must never reach the tools: the runner removes it from their environment.
    qputenv("PGPASSWORD", "from-the-environment");
}

void TstDump::closureFollowsReferencedTablesOnly()
{
    const DumpTable orders { QStringLiteral("public"), QStringLiteral("orders") };
    const DumpTable customers { QStringLiteral("public"), QStringLiteral("customers") };
    const DumpTable countries { QStringLiteral("public"), QStringLiteral("countries") };
    const DumpTable payments { QStringLiteral("public"), QStringLiteral("payments") };
    const DumpTable employees { QStringLiteral("public"), QStringLiteral("employees") };
    const QList<DumpTableEdge> edges {
        { orders, customers },       { customers, countries }, { payments, orders },
        { employees, employees }, // self reference
        { countries, customers }, // cycle
    };
    QCOMPARE(dependencyClosure({ orders }, edges),
             (QList<DumpTable> { orders, customers, countries }));
    // Payments points at orders, so selecting orders never pulls payments in.
    QVERIFY(!dependencyClosure({ customers }, edges).contains(payments));
    QCOMPARE(dependencyClosure({ employees, employees }, edges), QList<DumpTable> { employees });
    QCOMPARE(dependencyClosure({}, edges), QList<DumpTable> {});
    // Same name in another schema is another table.
    const DumpTable otherOrders { QStringLiteral("archive"), QStringLiteral("orders") };
    QCOMPARE(dependencyClosure({ otherOrders }, edges), QList<DumpTable> { otherOrders });
}

void TstDump::pgArgumentsNeverCarryThePassword()
{
    DumpTarget target = postgresTarget();
    target.database = QStringLiteral("-x=y 'q'");
    const QStringList all = pgDumpArguments(target, {}, QStringLiteral("out.sql"))
            + psqlRestoreArguments(target, QStringLiteral("in.sql"))
            + pgRestoreArguments(target, QStringLiteral("in.dump"));
    for (const QString &arg : all)
        QVERIFY2(!arg.contains(QLatin1String("s3cr")), qPrintable(arg));
    const QString conninfo = pgDumpArguments(target, {}, QStringLiteral("o")).first();
    QVERIFY(conninfo.startsWith(QLatin1String("--dbname=host='db.example' port='6543'")));
    QVERIFY(conninfo.contains(QLatin1String("dbname='-x=y \\'q\\''")));
    QVERIFY(conninfo.contains(QLatin1String("user='app'")));
    QVERIFY(conninfo.contains(QLatin1String("sslmode='prefer'")));
    QVERIFY(psqlRestoreArguments(target, QStringLiteral("in.sql"))
                    .contains(QLatin1String("--single-transaction")));
    QVERIFY(psqlRestoreArguments(target, QStringLiteral("in.sql"))
                    .contains(QLatin1String("--set=ON_ERROR_STOP=1")));
    QCOMPARE(pgRestoreArguments(target, QStringLiteral("in.dump")).last(),
             QStringLiteral("in.dump"));
}

void TstDump::pgArgumentsForContentAndSelection()
{
    DumpOptions options;
    options.content = DumpContent::SchemaOnly;
    options.format = DumpFormat::PostgresCustom;
    options.schemas = { QStringLiteral("Sales") };
    options.tables = { { QStringLiteral("public"), QStringLiteral("we\"ird*") },
                       { {}, QStringLiteral("plain") } };
    const QStringList args = pgDumpArguments(postgresTarget(), options, QStringLiteral("o.dump"));
    QVERIFY(args.contains(QLatin1String("--schema-only")));
    QVERIFY(args.contains(QLatin1String("--format=custom")));
    QVERIFY(args.contains(QLatin1String("--file=o.dump")));
    QVERIFY(args.contains(QLatin1String("--no-owner")));
    QVERIFY(args.contains(QLatin1String("--no-password")));
    QVERIFY(args.contains(QLatin1String("--schema=\"Sales\"")));
    QVERIFY(args.contains(QLatin1String("--table=\"public\".\"we\"\"ird*\"")));
    QVERIFY(args.contains(QLatin1String("--table=\"plain\"")));

    options = {};
    options.content = DumpContent::DataOnly;
    options.includeOwnership = true;
    const QStringList data = pgDumpArguments(postgresTarget(), options, QStringLiteral("o.sql"));
    QVERIFY(data.contains(QLatin1String("--data-only")));
    QVERIFY(data.contains(QLatin1String("--format=plain")));
    QVERIFY(!data.contains(QLatin1String("--no-owner")));
}

void TstDump::pgPassLineEscapes()
{
    DumpTarget target = postgresTarget();
    target.password = QStringLiteral("a:b\\c\nd");
    QCOMPARE(pgPassFileLine(target), QByteArray("*:*:*:*:a\\:b\\\\cd\n"));
}

void TstDump::resolvesConfiguredToolFileAndFolder()
{
    const Result<QString> file = resolveDumpTool(DumpTool::PgDump, m_fakeTools);
    QVERIFY(file);
    QCOMPARE(QFileInfo(*file).canonicalFilePath(),
             QFileInfo(QStringLiteral(QUARRY_FAKE_PG_TOOL)).canonicalFilePath());

    QTemporaryDir bin;
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("psql.exe");
#else
    const QString name = QStringLiteral("psql");
#endif
    QVERIFY(QFile::copy(QStringLiteral(QUARRY_FAKE_PG_TOOL), bin.filePath(name)));
    DumpToolPaths folder;
    folder.psql = bin.path();
    const Result<QString> inFolder = resolveDumpTool(DumpTool::Psql, folder);
    QVERIFY(inFolder);
    QCOMPARE(QFileInfo(*inFolder).fileName(), name);

    DumpToolPaths missing;
    missing.pgRestore = bin.filePath(QStringLiteral("nope"));
    const Result<QString> notThere = resolveDumpTool(DumpTool::PgRestore, missing);
    QVERIFY(!notThere);
    QCOMPARE(notThere.error().kind, ErrorKind::NotFound);
    QVERIFY(notThere.error().message.contains(QLatin1String("pg_restore")));
}

void TstDump::targetFromProfile()
{
    ConnectionProfile profile;
    profile.providerId = QStringLiteral("postgres");
    profile.production = true;
    profile.values = { { QStringLiteral("host"), QStringLiteral("h") },
                       { QStringLiteral("port"), 5433 },
                       { QStringLiteral("database"), QStringLiteral("d") },
                       { QStringLiteral("user"), QStringLiteral("u") },
                       { QStringLiteral("sslmode"), QStringLiteral("require") },
                       { QStringLiteral("ssh.enabled"), true } };
    DumpTarget target = dumpTargetFromProfile(profile, QStringLiteral("pw"));
    QCOMPARE(target.host, QStringLiteral("h"));
    QCOMPARE(target.port, 5433);
    QCOMPARE(target.database, QStringLiteral("d"));
    QCOMPARE(target.user, QStringLiteral("u"));
    QCOMPARE(target.password, QStringLiteral("pw"));
    QCOMPARE(target.sslmode, QStringLiteral("require"));
    QVERIFY(target.production);
    QVERIFY(target.sshTunnel);
    QCOMPARE(dumpTargetFromProfile(profile, {}, QStringLiteral("other")).database,
             QStringLiteral("other"));

    profile.providerId = QStringLiteral("sqlite");
    profile.values = { { QStringLiteral("file"), QStringLiteral("C:/x.db") } };
    QCOMPARE(dumpTargetFromProfile(profile, {}).database, QStringLiteral("C:/x.db"));
    QVERIFY(dumpSupported(QStringLiteral("sqlite")));
    QVERIFY(dumpSupported(QStringLiteral("postgres")));
    QVERIFY(!dumpSupported(QStringLiteral("mysql")));
}

void TstDump::sqliteRoundTripReproducesTheDatabase()
{
    DumpRunner runner;
    QSignalSpy started(&runner, &DumpRunner::started);
    QSignalSpy progress(&runner, &DumpRunner::progress);
    DumpOptions options;
    options.outputPath = m_dir.filePath(QStringLiteral("full.sql"));
    const quint64 id = runner.dump(sqliteTarget(m_sample), options);
    QVERIFY(id > 0);
    const Outcome dumped = waitFor(runner, id);
    QCOMPARE(dumped.what, QStringLiteral("finished"));
    QCOMPARE(started.count(), 1);
    QVERIFY(progress.count() > 0);
    QVERIFY(QFile::exists(options.outputPath));
    QVERIFY(dumped.log.join(QLatin1Char('\n')).contains(QLatin1String("customers: 3 rows")));

    const QString copy = m_dir.filePath(QStringLiteral("copy.db"));
    RestoreOptions restore;
    restore.inputPath = options.outputPath;
    const Outcome restored = waitFor(runner, runner.restore(sqliteTarget(copy), restore));
    QCOMPARE(restored.what, QStringLiteral("finished"));
    QCOMPARE(snapshot(copy), snapshot(m_sample));
    QCOMPARE(query(copy, "SELECT name, seq FROM sqlite_sequence"),
             query(m_sample, "SELECT name, seq FROM sqlite_sequence"));
    // The trigger fired while the sample was built, but not again during the restore.
    QCOMPARE(query(copy, "SELECT count(*) FROM logs"), QStringList { QStringLiteral("3") });
}

void TstDump::sqliteSchemaOnlyThenDataOnly()
{
    DumpRunner runner;
    DumpOptions schema;
    schema.content = DumpContent::SchemaOnly;
    schema.outputPath = m_dir.filePath(QStringLiteral("schema.sql"));
    QCOMPARE(waitFor(runner, runner.dump(sqliteTarget(m_sample), schema)).what,
             QStringLiteral("finished"));
    DumpOptions data;
    data.content = DumpContent::DataOnly;
    data.outputPath = m_dir.filePath(QStringLiteral("data.sql"));
    QCOMPARE(waitFor(runner, runner.dump(sqliteTarget(m_sample), data)).what,
             QStringLiteral("finished"));

    const QString copy = m_dir.filePath(QStringLiteral("split.db"));
    RestoreOptions restore;
    restore.inputPath = schema.outputPath;
    QCOMPARE(waitFor(runner, runner.restore(sqliteTarget(copy), restore)).what,
             QStringLiteral("finished"));
    QCOMPARE(query(copy, "SELECT count(*) FROM customers"), QStringList { QStringLiteral("0") });
    QCOMPARE(query(copy, "SELECT count(*) FROM sqlite_master WHERE type='view'"),
             QStringList { QStringLiteral("1") });

    restore.inputPath = data.outputPath;
    QCOMPARE(waitFor(runner, runner.restore(sqliteTarget(copy), restore)).what,
             QStringLiteral("finished"));
    QCOMPARE(snapshot(copy), snapshot(m_sample));
}

void TstDump::sqliteSelectedTables()
{
    DumpRunner runner;
    DumpOptions options;
    options.tables = { { {}, QStringLiteral("ORDERS") } };
    options.outputPath = m_dir.filePath(QStringLiteral("orders.sql"));
    QCOMPARE(waitFor(runner, runner.dump(sqliteTarget(m_sample), options)).what,
             QStringLiteral("finished"));
    const QString copy = m_dir.filePath(QStringLiteral("orders.db"));
    RestoreOptions restore;
    restore.inputPath = options.outputPath;
    QCOMPARE(waitFor(runner, runner.restore(sqliteTarget(copy), restore)).what,
             QStringLiteral("finished"));
    // The table, its index and its trigger; no other table and no view.
    QCOMPARE(query(copy, "SELECT type || ':' || name FROM sqlite_master ORDER BY name"),
             (QStringList { QStringLiteral("table:orders"), QStringLiteral("index:orders_customer"),
                            QStringLiteral("trigger:orders_log") }));
    QCOMPARE(query(copy, "SELECT count(*) FROM orders"), QStringList { QStringLiteral("3") });
}

void TstDump::sqliteMissingTableFails()
{
    DumpRunner runner;
    DumpOptions options;
    options.tables = { { {}, QStringLiteral("ghost") } };
    options.outputPath = m_dir.filePath(QStringLiteral("ghost.sql"));
    const Outcome outcome = waitFor(runner, runner.dump(sqliteTarget(m_sample), options));
    QCOMPARE(outcome.what, QStringLiteral("failed"));
    QCOMPARE(outcome.error.kind, ErrorKind::NotFound);
    QCOMPARE(outcome.error.detail, QStringLiteral("ghost"));
    QVERIFY(!QFile::exists(options.outputPath));
}

void TstDump::sqliteRestoreFailureRollsBack()
{
    DumpRunner runner;
    DumpOptions options;
    options.outputPath = m_dir.filePath(QStringLiteral("twice.sql"));
    QCOMPARE(waitFor(runner, runner.dump(sqliteTarget(m_sample), options)).what,
             QStringLiteral("finished"));
    const QString copy = m_dir.filePath(QStringLiteral("twice.db"));
    RestoreOptions restore;
    restore.inputPath = options.outputPath;
    QCOMPARE(waitFor(runner, runner.restore(sqliteTarget(copy), restore)).what,
             QStringLiteral("finished"));
    // Again into the same, no longer empty, database: the first CREATE fails.
    QVERIFY(exec(copy, "INSERT INTO logs(message) VALUES ('marker')"));
    const QStringList before = snapshot(copy);
    const Outcome again = waitFor(runner, runner.restore(sqliteTarget(copy), restore));
    QCOMPARE(again.what, QStringLiteral("failed"));
    QVERIFY(again.error.message.contains(QLatin1String("rolled back")));
    QVERIFY(again.error.detail.contains(QLatin1String("already exists")));
    QVERIFY(again.error.detail.startsWith(QLatin1String("Line 3:")));
    QCOMPARE(snapshot(copy), before);
}

void TstDump::sqliteCancelLeavesNoFile()
{
    const QString big = m_dir.filePath(QStringLiteral("big.db"));
    QVERIFY(exec(big, "CREATE TABLE t(x);"
                      "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<20000)"
                      "INSERT INTO t SELECT i FROM n;"));
    DumpRunner runner;
    DumpOptions options;
    options.outputPath = m_dir.filePath(QStringLiteral("big.sql"));
    const quint64 id = runner.dump(sqliteTarget(big), options);
    runner.cancel(id);
    QCOMPARE(waitFor(runner, id).what, QStringLiteral("cancelled"));
    QVERIFY(!runner.isRunning(id));
    QVERIFY(!QFile::exists(options.outputPath));
}

void TstDump::sqliteRestoreRejectsPostgresArchive()
{
    const QString archive = m_dir.filePath(QStringLiteral("archive.dump"));
    QFile file(archive);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("PGDMP\x01\x0e");
    file.close();
    QVERIFY(isPostgresCustomArchive(archive));
    DumpRunner runner;
    RestoreOptions restore;
    restore.inputPath = archive;
    const Outcome outcome =
            waitFor(runner, runner.restore(sqliteTarget(m_dir.filePath(QStringLiteral("x.db"))),
                                           restore));
    QCOMPARE(outcome.what, QStringLiteral("failed"));
    QCOMPARE(outcome.error.kind, ErrorKind::Unsupported);
}

void TstDump::postgresDumpUsesPassFile()
{
    const QString log = m_dir.filePath(QStringLiteral("pg-ok.log"));
    qputenv("FAKE_PG_LOG", log.toUtf8());
    qputenv("FAKE_PG_MODE", "ok");
    DumpRunner runner;
    DumpOptions options;
    options.outputPath = m_dir.filePath(QStringLiteral("pg.sql"));
    const Outcome outcome = waitFor(runner, runner.dump(postgresTarget(), options, m_fakeTools));
    QCOMPARE(outcome.what, QStringLiteral("finished"));
    QVERIFY(outcome.log.contains(
            QStringLiteral("pg_dump: dumping contents of table \"public.items\"")));
    QFile dump(options.outputPath);
    QVERIFY(dump.open(QIODevice::ReadOnly));
    QCOMPARE(dump.readAll(), QByteArray("-- fake dump\n"));
    QVERIFY(!QFile::exists(options.outputPath + QStringLiteral(".part")));

    QFile file(log);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    QVERIFY(lines.contains(QStringLiteral("PGPASSFILE:*:*:*:*:s3cr\\:et")));
    QVERIFY(lines.contains(QStringLiteral("PGPASSWORD:")));
    QVERIFY(lines.contains(QStringLiteral("--file=") + options.outputPath
                           + QStringLiteral(".part")));
    for (const QString &line : lines) {
        if (!line.startsWith(QLatin1String("PGPASSFILE:")))
            QVERIFY2(!line.contains(QLatin1String("s3cr")), qPrintable(line));
    }
    // The temporary password file is gone with the process.
    QString passPath;
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1String("PGPASSFILEPATH:")))
            passPath = line.mid(15);
    }
    QVERIFY(passPath.contains(QLatin1String("quarry-pgpass-")));
    QTRY_VERIFY(!QFile::exists(passPath));
}

void TstDump::postgresDumpErrors_data()
{
    QTest::addColumn<QByteArray>("mode");
    QTest::addColumn<int>("kindValue");
    QTest::newRow("auth") << QByteArray("authfail") << int(ErrorKind::Authentication);
    QTest::newRow("version") << QByteArray("mismatch") << int(ErrorKind::Unsupported);
}

void TstDump::postgresDumpErrors()
{
    QFETCH(QByteArray, mode);
    QFETCH(int, kindValue);
    const auto kind = static_cast<ErrorKind>(kindValue);
    qputenv("FAKE_PG_LOG", QByteArray());
    qputenv("FAKE_PG_MODE", mode);
    DumpRunner runner;
    DumpOptions options;
    options.outputPath = m_dir.filePath(QStringLiteral("pg-error.sql"));
    const Outcome outcome = waitFor(runner, runner.dump(postgresTarget(), options, m_fakeTools));
    QCOMPARE(outcome.what, QStringLiteral("failed"));
    QCOMPARE(outcome.error.kind, kind);
    QVERIFY(!outcome.error.detail.isEmpty());
    QVERIFY(!QFile::exists(options.outputPath));
    QVERIFY(!QFile::exists(options.outputPath + QStringLiteral(".part")));
}

void TstDump::postgresCancelLeavesNoFile()
{
    qputenv("FAKE_PG_LOG", QByteArray());
    qputenv("FAKE_PG_MODE", "slow");
    DumpRunner runner;
    QSignalSpy started(&runner, &DumpRunner::started);
    DumpOptions options;
    options.outputPath = m_dir.filePath(QStringLiteral("pg-slow.sql"));
    const quint64 id = runner.dump(postgresTarget(), options, m_fakeTools);
    QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 10000);
    QTRY_VERIFY(QFile::exists(options.outputPath + QStringLiteral(".part")));
    QSignalSpy cancelled(&runner, &DumpRunner::cancelled);
    runner.cancel(id);
    QCOMPARE(cancelled.count(), 1);
    QVERIFY(!runner.isRunning(id));
    QTRY_VERIFY(!QFile::exists(options.outputPath + QStringLiteral(".part")));
    QVERIFY(!QFile::exists(options.outputPath));
}

void TstDump::postgresRestorePicksTheTool()
{
    qputenv("FAKE_PG_MODE", "ok");
    const QString plain = m_dir.filePath(QStringLiteral("plain.sql"));
    QFile file(plain);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("SELECT 1;\n");
    file.close();
    const QString archive = m_dir.filePath(QStringLiteral("pg.dump"));
    QFile archiveFile(archive);
    QVERIFY(archiveFile.open(QIODevice::WriteOnly));
    archiveFile.write("PGDMP...");
    archiveFile.close();

    DumpRunner runner;
    DumpToolPaths onlyPsql;
    onlyPsql.psql = m_fakeTools.psql;
    onlyPsql.pgRestore = m_dir.filePath(QStringLiteral("missing-pg_restore"));
    RestoreOptions restore;
    restore.inputPath = plain;
    const QString log = m_dir.filePath(QStringLiteral("pg-restore.log"));
    qputenv("FAKE_PG_LOG", log.toUtf8());
    QCOMPARE(waitFor(runner, runner.restore(postgresTarget(), restore, onlyPsql)).what,
             QStringLiteral("finished"));
    QFile logFile(log);
    QVERIFY(logFile.open(QIODevice::ReadOnly));
    const QString logged = QString::fromUtf8(logFile.readAll());
    QVERIFY(logged.contains(QLatin1String("--single-transaction")));
    QVERIFY(logged.contains(QLatin1String("--no-psqlrc")));

    // A custom archive needs pg_restore, which is missing here.
    restore.inputPath = archive;
    const Outcome outcome = waitFor(runner, runner.restore(postgresTarget(), restore, onlyPsql));
    QCOMPARE(outcome.what, QStringLiteral("failed"));
    QCOMPARE(outcome.error.kind, ErrorKind::NotFound);
    QVERIFY(outcome.error.message.contains(QLatin1String("pg_restore")));
}

void TstDump::refusesUnsupportedTargets()
{
    DumpRunner runner;
    DumpOptions options;
    options.outputPath = m_dir.filePath(QStringLiteral("never.sql"));
    DumpTarget mysql = postgresTarget();
    mysql.providerId = QStringLiteral("mysql");
    QCOMPARE(runner.dump(mysql, options), quint64(0));
    QCOMPARE(waitFor(runner, 0).error.kind, ErrorKind::Unsupported);

    DumpTarget tunnel = postgresTarget();
    tunnel.sshTunnel = true;
    QCOMPARE(runner.dump(tunnel, options, m_fakeTools), quint64(0));
    QCOMPARE(waitFor(runner, 0).error.kind, ErrorKind::Unsupported);

    options.outputPath.clear();
    QCOMPARE(runner.dump(sqliteTarget(m_sample), options), quint64(0));
    QCOMPARE(waitFor(runner, 0).what, QStringLiteral("failed"));
    QVERIFY(!QFile::exists(m_dir.filePath(QStringLiteral("never.sql"))));
}

QTEST_GUILESS_MAIN(TstDump)
#include "tst_dump.moc"
