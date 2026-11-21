// MigrationRunner against a fake `php` (tests/core/fake_php): no PHP, no database.
#include <quarry/migration_runner.h>
#include <quarry/migration_source.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace quarry;

namespace {

const QString kName = QStringLiteral("2024_01_01_000000_create_x_table");

bool touch(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write("<?php\n") > 0;
}

QStringList readLines(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

qint64 sizeOf(const QString &path)
{
    return QFileInfo(path).size();
}

// Every signal of a runner, in order, as text.
struct Recorder
{
    explicit Recorder(MigrationRunner &runner)
    {
        QObject::connect(&runner, &MigrationRunner::started, &runner, [this](quint64 id) {
            events << QStringLiteral("started");
            ids << id;
        });
        QObject::connect(&runner, &MigrationRunner::output, &runner,
                         [this](quint64 id, const QString &text, bool isError) {
                             events << QStringLiteral("output");
                             texts << text;
                             errors << isError;
                             ids << id;
                         });
        QObject::connect(&runner, &MigrationRunner::finished, &runner,
                         [this, &runner](quint64 id, int code) {
                             events << QStringLiteral("finished");
                             exitCode = code;
                             ids << id;
                             stillRunning = runner.isRunning(id);
                         });
        QObject::connect(&runner, &MigrationRunner::cancelled, &runner, [this](quint64 id) {
            events << QStringLiteral("cancelled");
            ids << id;
        });
        QObject::connect(&runner, &MigrationRunner::failed, &runner,
                         [this](quint64 id, const Error &e) {
                             events << QStringLiteral("failed");
                             error = e;
                             ids << id;
                         });
    }
    int count(const QString &event) const { return int(events.count(event)); }

    QStringList events;
    QStringList texts;
    QList<bool> errors;
    QList<quint64> ids;
    int exitCode = -1;
    bool stillRunning = false;
    Error error;
};

} // namespace

class TstMigrationRunner : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void argumentsInsideRoot();
    void argumentsOutsideRoot();
    void argumentsInvalid();
    void resolveOk();
    void resolveDerivedRoot();
    void resolveNoArtisan();
    void resolveNoPhp();
    void resolveNoFolder();

    void resolveTargetMysql();
    void resolveTargetHostArray();
    void resolveTargetSqlite();
    void resolveTargetNoCommand();
    void resolveTargetBrokenJson();
    void resolveTargetInvalidSettings();

    void executeOk();
    void executeFail();
    void executeArguments();
    void executeCwd();
    void executeBigOutputIsTruncatedOnce();
    void executeCancel();
    void executeTimeout();
    void executeInvalidName();
    void executePhpMissing();

    void sameDatabase_data();
    void sameDatabase();
    void sameDatabaseSqlite();

private:
    MigrationSettings settings() const
    {
        MigrationSettings s;
        s.connectionId = QUuid::createUuid();
        s.kind = QStringLiteral("laravel");
        s.folder = m_root->filePath(QStringLiteral("database/migrations"));
        s.phpPath = QStringLiteral(QUARRY_FAKE_PHP);
        return s;
    }
    QString logPath() const { return m_root->filePath(QStringLiteral("fake_php.log")); }
    MigrationSettings targetSettings(const QByteArray &json, const char *mode = "target")
    {
        qputenv("FAKE_PHP_MODE", mode);
        qputenv("FAKE_PHP_TARGET_JSON", json);
        return settings();
    }
    void useFake(const char *mode)
    {
        qputenv("FAKE_PHP_MODE", mode);
        qputenv("FAKE_PHP_LOG", logPath().toUtf8());
    }

    std::unique_ptr<QTemporaryDir> m_root;
};

void TstMigrationRunner::init()
{
    m_root = std::make_unique<QTemporaryDir>();
    QVERIFY(m_root->isValid());
    QVERIFY(QDir().mkpath(m_root->filePath(QStringLiteral("database/migrations"))));
    QVERIFY(touch(m_root->filePath(QStringLiteral("artisan"))));
    qputenv("FAKE_PHP_LOG", logPath().toUtf8());
}

void TstMigrationRunner::cleanup()
{
    qunsetenv("FAKE_PHP_MODE");
    qunsetenv("FAKE_PHP_LOG");
    qunsetenv("FAKE_PHP_TARGET_JSON");
    qunsetenv("FAKE_PHP_TICKS");
    qunsetenv("QUARRY_RUNNER_TIMEOUT_MS");
}

void TstMigrationRunner::argumentsInsideRoot()
{
    const MigrationSettings s = settings();
    const QString path
        = QStringLiteral("--path=database/migrations/") + kName + QStringLiteral(".php");
    const QStringList tail { QStringLiteral("--force"), QStringLiteral("--no-interaction"),
                             QStringLiteral("--no-ansi") };

    QCOMPARE(MigrationRunner::arguments(s, kName, MigrationAction::Run),
             (QStringList { QStringLiteral("artisan"), QStringLiteral("migrate"), path } + tail));
    QCOMPARE(MigrationRunner::arguments(s, kName, MigrationAction::Rollback),
             (QStringList { QStringLiteral("artisan"), QStringLiteral("migrate:rollback"), path }
              + tail));
    QCOMPARE(MigrationRunner::arguments(s, kName, MigrationAction::DryRun),
             (QStringList { QStringLiteral("artisan"), QStringLiteral("migrate"), path } + tail
              + QStringList { QStringLiteral("--pretend") }));
}

void TstMigrationRunner::argumentsOutsideRoot()
{
    QTemporaryDir other;
    MigrationSettings s = settings();
    s.projectRoot = other.path(); // the migrations folder is not under it
    const QStringList args = MigrationRunner::arguments(s, kName, MigrationAction::Run);
    QVERIFY(!args.isEmpty());
    QVERIFY(args.contains(QStringLiteral("--realpath")));
    const QString expected = QStringLiteral("--path=")
        + QDir::cleanPath(s.folder + QLatin1Char('/') + kName + QStringLiteral(".php"));
    QVERIFY2(args.contains(expected), qPrintable(args.join(QLatin1Char(' '))));
    QVERIFY(args.contains(QStringLiteral("--force")));

    const QStringList dry = MigrationRunner::arguments(s, kName, MigrationAction::DryRun);
    QVERIFY(dry.contains(QStringLiteral("--pretend")));
    QVERIFY(dry.contains(QStringLiteral("--realpath")));
}

void TstMigrationRunner::argumentsInvalid()
{
    const MigrationSettings s = settings();
    for (const QString &name : { QString(), QStringLiteral("../x"), QStringLiteral("a/b"),
                                 QStringLiteral("a\\b"), QStringLiteral("..") }) {
        QVERIFY2(MigrationRunner::arguments(s, name, MigrationAction::Run).isEmpty(),
                 qPrintable(name));
    }
    MigrationSettings noKind = s;
    noKind.kind = QStringLiteral("nope");
    QVERIFY(MigrationRunner::arguments(noKind, kName, MigrationAction::Run).isEmpty());
    noKind.kind.clear();
    QVERIFY(MigrationRunner::arguments(noKind, kName, MigrationAction::Run).isEmpty());
}

void TstMigrationRunner::resolveOk()
{
    MigrationSettings s = settings();
    s.projectRoot = m_root->path();
    const auto env = MigrationRunner::resolve(s);
    QVERIFY2(env.has_value(), qPrintable(env ? QString() : env.error().message));
    QCOMPARE(env->projectRoot, QDir::cleanPath(m_root->path()));
    QCOMPARE(QFileInfo(env->php).canonicalFilePath(),
             QFileInfo(QStringLiteral(QUARRY_FAKE_PHP)).canonicalFilePath());
}

void TstMigrationRunner::resolveDerivedRoot()
{
    const auto env = MigrationRunner::resolve(settings());
    QVERIFY(env.has_value());
    QCOMPARE(QFileInfo(env->projectRoot).canonicalFilePath(),
             QFileInfo(m_root->path()).canonicalFilePath());
}

void TstMigrationRunner::resolveNoArtisan()
{
    QTemporaryDir bare;
    MigrationSettings s = settings();
    s.projectRoot = bare.path();
    const auto env = MigrationRunner::resolve(s);
    QVERIFY(!env.has_value());
    QCOMPARE(env.error().kind, ErrorKind::NotFound);
    QVERIFY(env.error().message.contains(QLatin1String("artisan")));
    QVERIFY(env.error().detail.contains(QDir::cleanPath(bare.path())));
}

void TstMigrationRunner::resolveNoPhp()
{
    MigrationSettings s = settings();
    s.phpPath = m_root->filePath(QStringLiteral("no/such/php.exe"));
    const auto env = MigrationRunner::resolve(s);
    QVERIFY(!env.has_value());
    QCOMPARE(env.error().kind, ErrorKind::NotFound);
    QCOMPARE(
        env.error().message,
        QStringLiteral("PHP was not found. Set its path in the connection's migrations settings."));
    QCOMPARE(env.error().detail, s.phpPath);

    // A file that is not executable (a plain text file) is not php either (Unix only: on Windows
    // the extension decides).
#ifndef Q_OS_WIN
    s.phpPath = m_root->filePath(QStringLiteral("artisan"));
    QVERIFY(!MigrationRunner::resolve(s).has_value());
#endif
}

void TstMigrationRunner::resolveNoFolder()
{
    MigrationSettings s = settings();
    s.folder.clear();
    const auto env = MigrationRunner::resolve(s);
    QVERIFY(!env.has_value());
    QCOMPARE(env.error().kind, ErrorKind::Syntax);
}

void TstMigrationRunner::resolveTargetMysql()
{
    MigrationRunner runner;
    QSignalSpy resolved(&runner, &MigrationRunner::targetResolved);
    QSignalSpy failed(&runner, &MigrationRunner::failed);
    const quint64 id = runner.resolveTarget(
        targetSettings(R"({"platform":{"config":{"driver":"mysql","host":"db.local","port":"3307",)"
                       R"("database":"app","name":"mysql"}},"tables":[]})"));
    QVERIFY(id > 0);
    QTRY_COMPARE(resolved.count(), 1);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(resolved.at(0).at(0).value<quint64>(), id);
    const auto target = resolved.at(0).at(1).value<ArtisanTarget>();
    QCOMPARE(target.driver, QStringLiteral("mysql"));
    QCOMPARE(target.host, QStringLiteral("db.local"));
    QCOMPARE(target.port, 3307);
    QCOMPARE(target.database, QStringLiteral("app"));
    QCOMPARE(target.connectionName, QStringLiteral("mysql"));
    QVERIFY(!runner.isRunning(id));

    // The command is read-only and exactly this one.
    QCOMPARE(readLines(logPath()),
             (QStringList { QStringLiteral("artisan"), QStringLiteral("db:show"),
                            QStringLiteral("--json"), QStringLiteral("--no-interaction"),
                            QStringLiteral("--no-ansi") }));

    // A numeric port.
    const quint64 id2 = runner.resolveTarget(targetSettings(
        R"({"platform":{"config":{"driver":"pgsql","host":"h","port":5433,"database":"d"}}})"));
    QTRY_COMPARE(resolved.count(), 2);
    QCOMPARE(resolved.at(1).at(0).value<quint64>(), id2);
    const auto second = resolved.at(1).at(1).value<ArtisanTarget>();
    QCOMPARE(second.port, 5433);
    QCOMPARE(second.driver, QStringLiteral("pgsql"));
}

void TstMigrationRunner::resolveTargetHostArray()
{
    MigrationRunner runner;
    QSignalSpy resolved(&runner, &MigrationRunner::targetResolved);
    runner.resolveTarget(targetSettings(
        R"({"platform":{"config":{"driver":"mariadb","host":["10.0.0.5","10.0.0.6"],)"
        R"("port":3306,"database":"app"}}})"));
    QTRY_COMPARE(resolved.count(), 1);
    QCOMPARE(resolved.at(0).at(1).value<ArtisanTarget>().host, QStringLiteral("10.0.0.5"));

    // Host inside read/write (read-replica setups).
    runner.resolveTarget(targetSettings(
        R"({"platform":{"config":{"driver":"mysql","read":{"host":["10.1.1.1","10.1.1.2"]},)"
        R"("write":{"host":"10.2.2.2"},"database":"app"}}})"));
    QTRY_COMPARE(resolved.count(), 2);
    QCOMPARE(resolved.at(1).at(1).value<ArtisanTarget>().host, QStringLiteral("10.2.2.2"));
}

void TstMigrationRunner::resolveTargetSqlite()
{
    MigrationRunner runner;
    QSignalSpy resolved(&runner, &MigrationRunner::targetResolved);
    runner.resolveTarget(targetSettings(
        R"({"platform":{"config":{"driver":"sqlite","database":"C:/x/db.sqlite"}}})"));
    QTRY_COMPARE(resolved.count(), 1);
    const auto target = resolved.at(0).at(1).value<ArtisanTarget>();
    QCOMPARE(target.driver, QStringLiteral("sqlite"));
    QCOMPARE(target.database, QStringLiteral("C:/x/db.sqlite"));
    QCOMPARE(target.port, 0);
    QVERIFY(target.host.isEmpty());
}

void TstMigrationRunner::resolveTargetNoCommand()
{
    MigrationRunner runner;
    QSignalSpy resolved(&runner, &MigrationRunner::targetResolved);
    QSignalSpy failed(&runner, &MigrationRunner::failed);
    const quint64 id = runner.resolveTarget(targetSettings("{}", "nocommand"));
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(resolved.count(), 0);
    QCOMPARE(failed.at(0).at(0).value<quint64>(), id);
    const auto error = failed.at(0).at(1).value<Error>();
    QCOMPARE(error.message, QStringLiteral("The database artisan uses could not be verified."));
    QCOMPARE(error.detail, QStringLiteral("Command \"db:show\" is not defined."));
}

void TstMigrationRunner::resolveTargetBrokenJson()
{
    MigrationRunner runner;
    QSignalSpy resolved(&runner, &MigrationRunner::targetResolved);
    QSignalSpy failed(&runner, &MigrationRunner::failed);
    runner.resolveTarget(targetSettings(R"({"platform":{"config":{"driver":"mysql",)"));
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(failed.at(0).at(1).value<Error>().message,
             QStringLiteral("The database artisan uses could not be verified."));

    // Valid JSON without a driver.
    runner.resolveTarget(targetSettings(R"({"platform":{"config":{}}})"));
    QTRY_COMPARE(failed.count(), 2);
    // No JSON at all.
    runner.resolveTarget(targetSettings("not json"));
    QTRY_COMPARE(failed.count(), 3);
    QCOMPARE(resolved.count(), 0);
    // The detail is one line, cut to 500 characters, never the whole output.
    QVERIFY(failed.at(2).at(1).value<Error>().detail.size() <= 500);
}

void TstMigrationRunner::resolveTargetInvalidSettings()
{
    MigrationRunner runner;
    QSignalSpy failed(&runner, &MigrationRunner::failed);
    MigrationSettings s = settings();
    s.phpPath = m_root->filePath(QStringLiteral("missing.exe"));
    QCOMPARE(runner.resolveTarget(s), quint64(0));
    QCOMPARE(failed.count(), 1);
    QCOMPARE(failed.at(0).at(0).value<quint64>(), quint64(0));
    QCOMPARE(failed.at(0).at(1).value<Error>().kind, ErrorKind::NotFound);
    QVERIFY(!QFileInfo::exists(logPath()));
}

void TstMigrationRunner::executeOk()
{
    useFake("ok");
    MigrationRunner runner;
    Recorder rec(runner);
    const quint64 id = runner.execute(settings(), kName, MigrationAction::Run);
    QVERIFY(id > 0);
    QVERIFY(runner.isRunning(id));
    QTRY_COMPARE(rec.count(QStringLiteral("finished")), 1);

    QCOMPARE(rec.events.first(), QStringLiteral("started"));
    QCOMPARE(rec.events.last(), QStringLiteral("finished"));
    QCOMPARE(rec.count(QStringLiteral("started")), 1);
    QCOMPARE(rec.count(QStringLiteral("failed")), 0);
    QCOMPARE(rec.count(QStringLiteral("cancelled")), 0);
    QCOMPARE(rec.exitCode, 0);
    QVERIFY(!rec.stillRunning); // the id is gone before the terminal signal
    QVERIFY(!runner.isRunning(id));
    for (const quint64 seen : std::as_const(rec.ids))
        QCOMPARE(seen, id);

    QCOMPARE(rec.texts.size(), 3);
    const qsizetype first = rec.texts.indexOf(QStringLiteral("Migrating: 2024_01_01_000000_x"));
    const qsizetype second
        = rec.texts.indexOf(QStringLiteral("Migrated:  2024_01_01_000000_x (1.00ms)"));
    const qsizetype warning = rec.texts.indexOf(QStringLiteral("a warning"));
    QVERIFY(first >= 0 && second > first && warning >= 0);
    QVERIFY(!rec.errors.at(first));
    QVERIFY(!rec.errors.at(second));
    QVERIFY(rec.errors.at(warning));
}

void TstMigrationRunner::executeFail()
{
    useFake("fail");
    MigrationRunner runner;
    Recorder rec(runner);
    runner.execute(settings(), kName, MigrationAction::Rollback);
    QTRY_COMPARE(rec.count(QStringLiteral("finished")), 1);
    QCOMPARE(rec.exitCode, 3); // a failing migration is a result, not a runner failure
    QCOMPARE(rec.count(QStringLiteral("failed")), 0);
    QVERIFY(rec.texts.contains(QStringLiteral("Migration failed: something went wrong")));
    QVERIFY(
        rec.errors.at(rec.texts.indexOf(QStringLiteral("Migration failed: something went wrong"))));
}

void TstMigrationRunner::executeArguments()
{
    useFake("ok");
    MigrationRunner runner;
    Recorder rec(runner);
    const MigrationSettings s = settings();
    runner.execute(s, kName, MigrationAction::DryRun);
    QTRY_COMPARE(rec.count(QStringLiteral("finished")), 1);
    QCOMPARE(readLines(logPath()), MigrationRunner::arguments(s, kName, MigrationAction::DryRun));
}

void TstMigrationRunner::executeCwd()
{
    useFake("cwd");
    MigrationRunner runner;
    Recorder rec(runner);
    runner.execute(settings(), kName, MigrationAction::DryRun);
    QTRY_COMPARE(rec.count(QStringLiteral("finished")), 1);
    QCOMPARE(rec.texts.size(), 1);
    QCOMPARE(QFileInfo(rec.texts.first()).canonicalFilePath(),
             QFileInfo(m_root->path()).canonicalFilePath());
}

void TstMigrationRunner::executeBigOutputIsTruncatedOnce()
{
    useFake("big");
    MigrationRunner runner;
    Recorder rec(runner);
    runner.execute(settings(), kName, MigrationAction::Run);
    QTRY_COMPARE_WITH_TIMEOUT(rec.count(QStringLiteral("finished")), 1, 20000);
    QCOMPARE(rec.exitCode, 0);
    QCOMPARE(int(rec.texts.count(QStringLiteral("[output truncated]"))), 1);
    QCOMPARE(rec.texts.last(), QStringLiteral("[output truncated]"));
    qint64 bytes = 0;
    for (const QString &text : std::as_const(rec.texts)) {
        if (text != QLatin1String("[output truncated]"))
            bytes += text.toUtf8().size() + 1;
    }
    // Every text counts one line end, but the line that is cut has none: one byte of slack.
    QVERIFY2(bytes <= MigrationRunner::kMaxOutputBytes + 1, qPrintable(QString::number(bytes)));
    QVERIFY(bytes > MigrationRunner::kMaxOutputBytes * 9 / 10); // CRLF on Windows: a bit less
}

void TstMigrationRunner::executeCancel()
{
    const QString ticks = m_root->filePath(QStringLiteral("ticks"));
    qputenv("FAKE_PHP_TICKS", ticks.toUtf8());
    useFake("slow");
    MigrationRunner runner;
    Recorder rec(runner);
    const quint64 id = runner.execute(settings(), kName, MigrationAction::Run);
    QTRY_VERIFY(rec.count(QStringLiteral("output")) >= 1);
    QVERIFY(runner.isRunning(id));

    runner.cancel(id);
    QCOMPARE(rec.count(QStringLiteral("cancelled")), 1);
    QVERIFY(!runner.isRunning(id));
    runner.cancel(id); // unknown now: nothing happens
    runner.cancel(9999);

    const qint64 size = sizeOf(ticks);
    QVERIFY(size >= 1);
    QTest::qWait(2500); // more than the one-second period of the fake
    QCOMPARE(sizeOf(ticks), size); // it died: no more ticks
    QCOMPARE(rec.count(QStringLiteral("cancelled")), 1);
    QCOMPARE(rec.count(QStringLiteral("finished")), 0);
    QCOMPARE(rec.count(QStringLiteral("failed")), 0);
}

void TstMigrationRunner::executeTimeout()
{
    const QString ticks = m_root->filePath(QStringLiteral("ticks"));
    qputenv("FAKE_PHP_TICKS", ticks.toUtf8());
    qputenv("QUARRY_RUNNER_TIMEOUT_MS", "1200");
    useFake("slow");
    MigrationRunner runner;
    Recorder rec(runner);
    const quint64 id = runner.execute(settings(), kName, MigrationAction::Run);
    QTRY_COMPARE_WITH_TIMEOUT(rec.count(QStringLiteral("failed")), 1, 15000);
    QCOMPARE(rec.error.kind, ErrorKind::Timeout);
    QCOMPARE(rec.error.message, QStringLiteral("The migration took too long and was stopped."));
    QVERIFY(!runner.isRunning(id));

    const qint64 size = sizeOf(ticks);
    QTest::qWait(2500);
    QCOMPARE(sizeOf(ticks), size);
    QCOMPARE(rec.count(QStringLiteral("failed")), 1);
    QCOMPARE(rec.count(QStringLiteral("finished")), 0);
    QCOMPARE(rec.count(QStringLiteral("cancelled")), 0);
}

void TstMigrationRunner::executeInvalidName()
{
    useFake("ok");
    MigrationRunner runner;
    Recorder rec(runner);
    for (const QString &name : { QString(), QStringLiteral("../evil"), QStringLiteral("a/b") }) {
        QCOMPARE(runner.execute(settings(), name, MigrationAction::Run), quint64(0));
    }
    QCOMPARE(rec.count(QStringLiteral("failed")), 3);
    QCOMPARE(rec.ids, (QList<quint64> { 0, 0, 0 }));
    QTest::qWait(100);
    QCOMPARE(rec.events.size(), 3); // nothing else: no process
    QVERIFY(!QFileInfo::exists(logPath()));
}

void TstMigrationRunner::executePhpMissing()
{
    MigrationRunner runner;
    Recorder rec(runner);
    MigrationSettings s = settings();
    s.phpPath = m_root->filePath(QStringLiteral("missing.exe"));
    QCOMPARE(runner.execute(s, kName, MigrationAction::Run), quint64(0));
    QCOMPARE(rec.events, QStringList { QStringLiteral("failed") });
    QCOMPARE(rec.error.kind, ErrorKind::NotFound);
}

void TstMigrationRunner::sameDatabase_data()
{
    QTest::addColumn<QString>("driver");
    QTest::addColumn<QString>("host");
    QTest::addColumn<int>("port");
    QTest::addColumn<QString>("database");
    QTest::addColumn<QString>("providerId");
    QTest::addColumn<QString>("connHost");
    QTest::addColumn<int>("connPort");
    QTest::addColumn<QString>("connDatabase");
    QTest::addColumn<bool>("expected");
    QTest::addColumn<QStringList>("whyHas");

    const auto row = [](const char *name, const char *driver, const char *host, int port,
                        const char *db, const char *provider, const char *connHost, int connPort,
                        const char *connDb, bool expected, const QStringList &whyHas = {}) {
        QTest::newRow(name) << QString::fromLatin1(driver) << QString::fromLatin1(host) << port
                            << QString::fromLatin1(db) << QString::fromLatin1(provider)
                            << QString::fromLatin1(connHost) << connPort
                            << QString::fromLatin1(connDb) << expected << whyHas;
    };
    row("mysql same", "mysql", "db.example", 3306, "app", "mysql", "db.example", 3306, "app", true);
    row("mysql host case", "mysql", "DB.Example", 3306, "app", "mysql", "db.example", 3306, "app",
        true);
    row("mariadb is mysql", "mariadb", "db", 3306, "app", "mysql", "db", 3306, "app", true);
    row("pgsql is postgres", "pgsql", "pg", 5432, "app", "postgres", "pg", 5432, "app", true);
    row("sqlsrv is mssql", "sqlsrv", "ms", 1433, "app", "mssql", "ms", 1433, "app", true);
    row("localhost = 127.0.0.1", "mysql", "localhost", 3306, "app", "mysql", "127.0.0.1", 3306,
        "app", true);
    row("127.0.0.1 = ::1", "pgsql", "127.0.0.1", 5432, "app", "postgres", "::1", 5432, "app", true);
    row("localhost = ::1", "sqlsrv", "::1", 1433, "app", "mssql", "localhost", 1433, "app", true);
    row("default port mysql", "mysql", "db", 0, "app", "mysql", "db", 3306, "app", true);
    row("default port pg", "pgsql", "db", 5432, "app", "postgres", "db", 0, "app", true);
    row("default port mssql", "sqlsrv", "db", 0, "app", "mssql", "db", 0, "app", true);
    row("different database", "mysql", "db", 3306, "appone", "mysql", "db", 3306, "apptwo", false,
        { QStringLiteral("appone"), QStringLiteral("apptwo") });
    row("database is case sensitive", "pgsql", "db", 5432, "App", "postgres", "db", 5432, "app",
        false, { QStringLiteral("App"), QStringLiteral("app") });
    row("different host", "mysql", "hostone", 3306, "app", "mysql", "hosttwo", 3306, "app", false,
        { QStringLiteral("hostone"), QStringLiteral("hosttwo") });
    row("different port", "mysql", "db", 3307, "app", "mysql", "db", 0, "app", false,
        { QStringLiteral("3307"), QStringLiteral("3306") });
    row("mysql vs postgres", "mysql", "db", 3306, "app", "postgres", "db", 3306, "app", false,
        { QStringLiteral("mysql"), QStringLiteral("postgres") });
    row("pgsql vs mssql", "pgsql", "db", 5432, "app", "mssql", "db", 5432, "app", false,
        { QStringLiteral("pgsql"), QStringLiteral("mssql") });
    row("odbc never", "mysql", "db", 3306, "app", "odbc", "db", 3306, "app", false,
        { QStringLiteral("ODBC") });
    row("unknown driver", "oracle", "db", 1521, "app", "mysql", "db", 1521, "app", false,
        { QStringLiteral("oracle") });
}

void TstMigrationRunner::sameDatabase()
{
    QFETCH(QString, driver);
    QFETCH(QString, host);
    QFETCH(int, port);
    QFETCH(QString, database);
    QFETCH(QString, providerId);
    QFETCH(QString, connHost);
    QFETCH(int, connPort);
    QFETCH(QString, connDatabase);
    QFETCH(bool, expected);
    QFETCH(QStringList, whyHas);

    ArtisanTarget artisan;
    artisan.driver = driver;
    artisan.host = host;
    artisan.port = port;
    artisan.database = database;
    ConnectionTarget connection;
    connection.providerId = providerId;
    connection.host = connHost;
    connection.port = connPort;
    connection.database = connDatabase;

    QString why;
    QCOMPARE(quarry::sameDatabase(artisan, connection, &why), expected);
    QCOMPARE(quarry::sameDatabase(artisan, connection), expected); // `why` is optional
    if (!expected) {
        QVERIFY(!why.isEmpty());
        for (const QString &part : whyHas)
            QVERIFY2(why.contains(part), qPrintable(why));
    }
}

void TstMigrationRunner::sameDatabaseSqlite()
{
    const QString file = m_root->filePath(QStringLiteral("data.sqlite"));
    QVERIFY(touch(file));
    ArtisanTarget artisan;
    artisan.driver = QStringLiteral("sqlite");
    ConnectionTarget connection;
    connection.providerId = QStringLiteral("sqlite");

    // The same file written in another way.
    artisan.database = file;
    connection.database = m_root->filePath(QStringLiteral("database/../data.sqlite"));
    QString why;
    QVERIFY2(quarry::sameDatabase(artisan, connection, &why), qPrintable(why));
    connection.database = QDir::toNativeSeparators(file);
    QVERIFY(quarry::sameDatabase(artisan, connection));

    // A file that does not exist yet: the cleaned path decides.
    artisan.database = m_root->filePath(QStringLiteral("new.sqlite"));
    connection.database = m_root->filePath(QStringLiteral("database/../new.sqlite"));
    QVERIFY(quarry::sameDatabase(artisan, connection));

    // Another file.
    artisan.database = file;
    connection.database = m_root->filePath(QStringLiteral("other.sqlite"));
    QVERIFY(!quarry::sameDatabase(artisan, connection, &why));
    QVERIFY(why.contains(QStringLiteral("data.sqlite")));
    QVERIFY(why.contains(QStringLiteral("other.sqlite")));

    // A server engine does not match a SQLite connection.
    artisan.driver = QStringLiteral("mysql");
    QVERIFY(!quarry::sameDatabase(artisan, connection, &why));
    QVERIFY(!why.isEmpty());
}

QTEST_MAIN(TstMigrationRunner)
#include "tst_migration_runner.moc"
