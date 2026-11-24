#include "ui/connection_controller.h"
#include "ui/connection_form_model.h"
#include "ui/connection_list_model.h"
#include "ui/dump_controller.h"
#include "ui/schema_tree_model.h"

#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <sqlite3.h>

using namespace quarry;
using namespace quarry::ui;

namespace {

constexpr int kWaitMs = 10000;
constexpr int kSlowMs = 60000;
const QString kPassword = QStringLiteral("Tr0ub4dor&3-DUMP-PW");

// Stands in for the system keychain: a saved password reads back at once (queued, like the real
// one) and every call is recorded.
struct FakeKeychain
{
    explicit FakeKeychain(ConnectionController &controller)
        : c(controller)
    {
        CredentialBackend b;
        b.write = [this](QUuid id, const QString &) {
            calls << QStringLiteral("write");
            QMetaObject::invokeMethod(
                &c, [this, id] { c.onPasswordWritten(id); }, Qt::QueuedConnection);
        };
        b.read = [this](QUuid id) {
            calls << QStringLiteral("read");
            QMetaObject::invokeMethod(
                &c,
                [this, id] {
                    if (failRead)
                        c.onCredentialFailed(
                            id,
                            Error { ErrorKind::Unsupported,
                                    QStringLiteral("The system keychain is not available."),
                                    QStringLiteral("no backend"),
                                    {},
                                    {} });
                    else
                        c.onPasswordRead(id, kPassword);
                },
                Qt::QueuedConnection);
        };
        b.remove = [](QUuid) {};
        b.forget = [](QUuid) {};
        b.writeSecret = [](QUuid, SecretKind, const QString &) {};
        b.removeSecret = [](QUuid, SecretKind) {};
        c.setCredentialBackend(b);
    }

    ConnectionController &c;
    QStringList calls;
    bool failRead = false;
};

// The fields of a server engine reduced to what the dump needs: a file (the SQLite path, unused
// by the fake tools), the tunnel flag and the password.
std::vector<ConnectionField> serverFields()
{
    ConnectionField file;
    file.key = QStringLiteral("file");
    file.label = QStringLiteral("Database file");
    file.kind = ConnectionField::Kind::Path;
    ConnectionField ssh;
    ssh.key = QStringLiteral("ssh.enabled");
    ssh.label = QStringLiteral("SSH");
    ssh.kind = ConnectionField::Kind::Bool;
    ConnectionField pw;
    pw.key = QStringLiteral("password");
    pw.kind = ConnectionField::Kind::Secret;
    return { file, ssh, pw };
}

QString readText(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

} // namespace

// DumpController (H14 phase B): states, the keychain, errors, cancelling, the tools' paths and the
// table selector with its foreign-key closure, over the real SQLite plugin and the in-process
// SQLite dump, and over the fake pg_dump / psql / pg_restore of tests/core for PostgreSQL. No
// network, no real tool, no real keychain.
class TstDumpController : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QTemporaryDir m_settings;
    int m_files = 0;

    struct Env
    {
        std::unique_ptr<ConnectionController> connections;
        std::unique_ptr<FakeKeychain> keychain;
        std::unique_ptr<DumpController> dump;
    };

    QString path(const QString &name) const { return m_dir.filePath(name); }

    void makeSample(const QString &file) const
    {
        sqlite3 *db = nullptr;
        QVERIFY(sqlite3_open(file.toUtf8().constData(), &db) == SQLITE_OK);
        const char *sql = "CREATE TABLE customers(id INTEGER PRIMARY KEY, name TEXT);"
                          "CREATE TABLE invoices(id INTEGER PRIMARY KEY, customer_id INTEGER "
                          "REFERENCES customers(id), total REAL);"
                          "CREATE TABLE items(id INTEGER PRIMARY KEY, invoice_id INTEGER "
                          "REFERENCES invoices(id));"
                          "CREATE TABLE products(id INTEGER PRIMARY KEY, title TEXT);"
                          "INSERT INTO customers VALUES (1, 'Ada');"
                          "INSERT INTO invoices VALUES (1, 1, 9.5);"
                          "INSERT INTO items VALUES (1, 1);"
                          "INSERT INTO products VALUES (1, 'Widget');";
        QVERIFY(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
    }

    Env start()
    {
        Env e;
        e.connections = std::make_unique<ConnectionController>();
        e.keychain = std::make_unique<FakeKeychain>(*e.connections);
        e.connections->setDatabasePath(path(QStringLiteral("quarry-%1.db").arg(++m_files)));
        e.connections->setProviderDirectory(QStringLiteral(QUARRY_PLUGIN_DIR));
        e.connections->start();
        [&] { QTRY_VERIFY_WITH_TIMEOUT(!e.connections->model()->loading(), kWaitMs); }();
        e.dump = std::make_unique<DumpController>();
        e.dump->setConnections(e.connections.get());
        return e;
    }

    QString saveDraft(ConnectionController &c, const QString &engine, const QString &name,
                      bool production, int passwordMode, bool ssh = false)
    {
        c.prepare(QString());
        c.form()->setFields(serverFields());
        c.form()->setValue(0, path(QStringLiteral("server-%1.db").arg(++m_files)));
        c.form()->setValue(1, ssh);
        QSignalSpy saved(&c, &ConnectionController::connectionSaved);
        const bool ok = c.save({ { QStringLiteral("id"), QString() },
                                 { QStringLiteral("providerId"), engine },
                                 { QStringLiteral("name"), name },
                                 { QStringLiteral("colorIndex"), 1 },
                                 { QStringLiteral("production"), production },
                                 { QStringLiteral("passwordMode"), passwordMode },
                                 { QStringLiteral("password"), kPassword },
                                 { QStringLiteral("passwordTouched"), true } });
        if (!ok || !saved.wait(kWaitMs))
            return {};
        return saved.at(0).at(0).toString();
    }

    // A SQLite connection on `file`.
    QString saveSqlite(ConnectionController &c, const QString &name, const QString &file,
                       bool production = false)
    {
        c.prepare(QString());
        c.form()->setValue(0, file);
        QSignalSpy saved(&c, &ConnectionController::connectionSaved);
        const bool ok = c.save({ { QStringLiteral("id"), QString() },
                                 { QStringLiteral("providerId"), QStringLiteral("sqlite") },
                                 { QStringLiteral("name"), name },
                                 { QStringLiteral("colorIndex"), 2 },
                                 { QStringLiteral("production"), production },
                                 { QStringLiteral("passwordMode"), 1 } });
        if (!ok || !saved.wait(kWaitMs))
            return {};
        return saved.at(0).at(0).toString();
    }

    static QVariantMap dumpRequest(const QString &id, const QString &file)
    {
        return { { QStringLiteral("connectionId"), id },
                 { QStringLiteral("filePath"), file },
                 { QStringLiteral("content"), QStringLiteral("both") } };
    }

    static void waitState(DumpController &d, const QString &state)
    {
        QTRY_COMPARE_WITH_TIMEOUT(d.state(), state, kSlowMs);
    }

private slots:
    void initTestCase()
    {
        QCoreApplication::setApplicationName(QStringLiteral("tst_dump_controller"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
        QVERIFY(m_dir.isValid());
    }

    void init()
    {
        QSettings().clear();
        qputenv("FAKE_PG_MODE", "ok");
        qputenv("FAKE_PG_LOG", QByteArray());
    }

    void infoDescribesTheConnection()
    {
        Env e = start();
        const QString file = path(QStringLiteral("info.db"));
        makeSample(file);
        const QString id = saveSqlite(*e.connections, QStringLiteral("Local"), file, true);
        QVERIFY(!id.isEmpty());
        const QVariantMap info = e.dump->infoFor(id);
        QVERIFY(info.value(QStringLiteral("valid")).toBool());
        QVERIFY(info.value(QStringLiteral("supported")).toBool());
        QVERIFY(info.value(QStringLiteral("production")).toBool());
        QVERIFY(!info.value(QStringLiteral("isPostgres")).toBool());
        QCOMPARE(info.value(QStringLiteral("engine")).toString(), QStringLiteral("SQLite"));
        QCOMPARE(info.value(QStringLiteral("database")).toString(), QStringLiteral("info.db"));
        QVERIFY(info.value(QStringLiteral("canPickTables")).toBool());
        // Another database of the connection: no table selector.
        QVERIFY(!e.dump->infoFor(id, QStringLiteral("other"))
                     .value(QStringLiteral("canPickTables"))
                     .toBool());
        QVERIFY(!e.dump->infoFor(QStringLiteral("{00000000-0000-0000-0000-000000000000}"))
                     .value(QStringLiteral("valid"))
                     .toBool());
    }

    void unsupportedEnginesAndTunnelsAreRefused()
    {
        Env e = start();
        const QString mysql
            = saveDraft(*e.connections, QStringLiteral("mysql"), QStringLiteral("My"), false, 1);
        QVERIFY(!mysql.isEmpty());
        const QVariantMap info = e.dump->infoFor(mysql);
        QVERIFY(!info.value(QStringLiteral("supported")).toBool());
        QVERIFY(!info.value(QStringLiteral("reason")).toString().isEmpty());
        QVERIFY(!e.dump->startDump(dumpRequest(mysql, path(QStringLiteral("x.sql")))).isEmpty());
        QCOMPARE(e.dump->state(), QStringLiteral("idle"));

        const QString tunnel = saveDraft(*e.connections, QStringLiteral("postgres"),
                                         QStringLiteral("Tunnel"), false, 1, true);
        QVERIFY(!tunnel.isEmpty());
        const QVariantMap viaSsh = e.dump->infoFor(tunnel);
        QVERIFY(!viaSsh.value(QStringLiteral("supported")).toBool());
        QVERIFY(viaSsh.value(QStringLiteral("reason")).toString().contains(QStringLiteral("SSH")));
        const QString problem
            = e.dump->startDump(dumpRequest(tunnel, path(QStringLiteral("x.sql"))));
        QVERIFY(problem.contains(QStringLiteral("SSH")));
        QCOMPARE(e.dump->state(), QStringLiteral("idle"));
    }

    void sqliteDumpWritesTheFileAndEndsDone()
    {
        Env e = start();
        const QString file = path(QStringLiteral("d1.db"));
        makeSample(file);
        const QString id = saveSqlite(*e.connections, QStringLiteral("D1"), file);
        const QString out = path(QStringLiteral("d1.sql"));
        QSignalSpy states(e.dump.get(), &DumpController::stateChanged);
        QSignalSpy notes(e.dump.get(), &DumpController::announcement);
        QVERIFY(!e.dump->busy());
        QCOMPARE(e.dump->startDump(dumpRequest(id, out)), QString());
        QVERIFY(e.dump->busy());
        QCOMPARE(e.dump->mode(), QStringLiteral("dump"));
        waitState(*e.dump, QStringLiteral("done"));
        QVERIFY(!e.dump->busy());
        QVERIFY(states.count() >= 2); // running, done
        QVERIFY(e.dump->resultText().contains(QStringLiteral("d1.sql")));
        const QString sql = readText(out);
        QVERIFY(sql.contains(QStringLiteral("CREATE TABLE")));
        QVERIFY(sql.contains(QStringLiteral("Widget")));
        // Announced: the start and the end, never the progress.
        QVERIFY(notes.count() >= 2);
        QVERIFY(!notes.last().at(1).toBool());
        e.dump->reset();
        QCOMPARE(e.dump->state(), QStringLiteral("idle"));
        QVERIFY(e.dump->log().isEmpty());
    }

    void startRejectsBadPathsAndASecondJob()
    {
        Env e = start();
        const QString file = path(QStringLiteral("d2.db"));
        makeSample(file);
        const QString id = saveSqlite(*e.connections, QStringLiteral("D2"), file);
        QVERIFY(!e.dump->startDump(dumpRequest(id, QString())).isEmpty());
        QVERIFY(!e.dump->startDump(dumpRequest(id, m_dir.path())).isEmpty()); // a folder
        QVERIFY(!e.dump->startDump(dumpRequest(id, path(QStringLiteral("nope/x.sql")))).isEmpty());
        QCOMPARE(e.dump->state(), QStringLiteral("idle"));
        QVERIFY(!e.dump->startDump(dumpRequest(QStringLiteral("{}"), path(QStringLiteral("y.sql"))))
                     .isEmpty());

        QCOMPARE(e.dump->startDump(dumpRequest(id, path(QStringLiteral("d2.sql")))), QString());
        QVERIFY(!e.dump->startDump(dumpRequest(id, path(QStringLiteral("d2b.sql")))).isEmpty());
        waitState(*e.dump, QStringLiteral("done"));
        QVERIFY(e.dump->fileExists(path(QStringLiteral("d2.sql"))));
        QVERIFY(!e.dump->fileExists(path(QStringLiteral("d2b.sql"))));
    }

    void tableSelectorListsTablesAndClosesOverForeignKeys()
    {
        Env e = start();
        const QString file = path(QStringLiteral("d3.db"));
        makeSample(file);
        const QString id = saveSqlite(*e.connections, QStringLiteral("D3"), file);
        QSignalSpy changed(e.dump.get(), &DumpController::tablesChanged);
        e.dump->loadTables(id);
        QTRY_COMPARE_WITH_TIMEOUT(e.dump->tablesState(), QStringLiteral("ready"), kSlowMs);
        QVERIFY(!e.dump->tablesTruncated());
        const QVariantList tables = e.dump->tables();
        QCOMPARE(tables.size(), 4);
        const auto rowOf = [&](const QString &name) {
            for (int i = 0; i < tables.size(); ++i)
                if (tables.at(i).toMap().value(QStringLiteral("name")).toString() == name)
                    return i;
            return -1;
        };
        const int customers = rowOf(QStringLiteral("customers"));
        const int invoices = rowOf(QStringLiteral("invoices"));
        const int items = rowOf(QStringLiteral("items"));
        QVERIFY(customers >= 0 && invoices >= 0 && items >= 0);

        // invoices points at customers; items points at invoices: pulling goes one way only.
        QCOMPARE(e.dump->closureOf({ invoices }), (QVariantList { invoices, customers }));
        QCOMPARE(e.dump->closureOf({ customers }), (QVariantList { customers }));
        QCOMPARE(e.dump->closureOf({ items }).size(), 3);

        // Only the selected table and what it references reach the file.
        const QString out = path(QStringLiteral("d3.sql"));
        QVariantMap request = dumpRequest(id, out);
        request.insert(QStringLiteral("tables"), QVariantList { invoices });
        request.insert(QStringLiteral("includeReferenced"), true);
        QCOMPARE(e.dump->startDump(request), QString());
        waitState(*e.dump, QStringLiteral("done"));
        const QString sql = readText(out);
        QVERIFY(sql.contains(QStringLiteral("customers")));
        QVERIFY(sql.contains(QStringLiteral("invoices")));
        QVERIFY(!sql.contains(QStringLiteral("products")));
        QVERIFY(!sql.contains(QStringLiteral("items")));

        // Without the box, just the table.
        e.dump->reset();
        const QString out2 = path(QStringLiteral("d3b.sql"));
        request = dumpRequest(id, out2);
        request.insert(QStringLiteral("tables"), QVariantList { customers });
        QCOMPARE(e.dump->startDump(request), QString());
        waitState(*e.dump, QStringLiteral("done"));
        QVERIFY(!readText(out2).contains(QStringLiteral("invoices")));

        // A row outside the list is refused; clearing the list forgets it.
        e.dump->reset();
        request.insert(QStringLiteral("tables"), QVariantList { 99 });
        QVERIFY(!e.dump->startDump(request).isEmpty());
        e.dump->clearTables();
        QCOMPARE(e.dump->tablesState(), QStringLiteral("idle"));
        QVERIFY(e.dump->tables().isEmpty());
        request.insert(QStringLiteral("tables"), QVariantList { 0 });
        QVERIFY(!e.dump->startDump(request).isEmpty()); // the list is not loaded
    }

    void tableSelectorIsUnavailableForUnsupportedConnections()
    {
        Env e = start();
        const QString mysql
            = saveDraft(*e.connections, QStringLiteral("mysql"), QStringLiteral("My"), false, 1);
        e.dump->loadTables(mysql);
        QCOMPARE(e.dump->tablesState(), QStringLiteral("unavailable"));
        QVERIFY(e.dump->tables().isEmpty());
    }

    void restoreConfirmationNeedsThePhraseAndForProductionTheName()
    {
        Env e = start();
        const QString file = path(QStringLiteral("prod.db"));
        makeSample(file);
        const QString plain = saveSqlite(*e.connections, QStringLiteral("Plain"), file);
        const QString prod = saveSqlite(*e.connections, QStringLiteral("Prod"),
                                        path(QStringLiteral("prod.db")), true);
        QVERIFY(!e.dump->needsDatabaseName(plain));
        QVERIFY(e.dump->needsDatabaseName(prod));
        QCOMPARE(e.dump->restorePhrase(), QStringLiteral("RESTORE"));

        QVERIFY(e.dump->restoreConfirmed(plain, {}, QStringLiteral("RESTORE"), {}));
        QVERIFY(e.dump->restoreConfirmed(plain, {}, QStringLiteral(" RESTORE "), {}));
        QVERIFY(!e.dump->restoreConfirmed(plain, {}, QStringLiteral("restore"), {}));
        QVERIFY(!e.dump->restoreConfirmed(plain, {}, QString(), {}));
        QVERIFY(!e.dump->restoreConfirmed(plain, {}, QStringLiteral("RESTOR"), {}));

        QVERIFY(!e.dump->restoreConfirmed(prod, {}, QStringLiteral("RESTORE"), {}));
        QVERIFY(!e.dump->restoreConfirmed(prod, {}, QStringLiteral("RESTORE"),
                                          QStringLiteral("other.db")));
        QVERIFY(e.dump->restoreConfirmed(prod, {}, QStringLiteral("RESTORE"),
                                         QStringLiteral("prod.db")));
        QVERIFY(
            !e.dump->restoreConfirmed(prod, {}, QStringLiteral("nope"), QStringLiteral("prod.db")));
        QVERIFY(!e.dump->restoreConfirmed(QStringLiteral("{}"), {}, QStringLiteral("RESTORE"), {}));
    }

    void sqliteRestoreRunsOnlyWithTheConfirmation()
    {
        Env e = start();
        const QString source = path(QStringLiteral("src.db"));
        makeSample(source);
        const QString from = saveSqlite(*e.connections, QStringLiteral("Src"), source);
        const QString script = path(QStringLiteral("src.sql"));
        QCOMPARE(e.dump->startDump(dumpRequest(from, script)), QString());
        waitState(*e.dump, QStringLiteral("done"));
        e.dump->reset();

        const QString target = path(QStringLiteral("empty.db"));
        sqlite3 *db = nullptr;
        QVERIFY(sqlite3_open(target.toUtf8().constData(), &db) == SQLITE_OK);
        sqlite3_close(db);
        const QString into = saveSqlite(*e.connections, QStringLiteral("Into"), target, true);

        QVariantMap request { { QStringLiteral("connectionId"), into },
                              { QStringLiteral("filePath"), script },
                              { QStringLiteral("phrase"), QStringLiteral("RESTORE") },
                              { QStringLiteral("typedName"), QString() } };
        QVERIFY(!e.dump->startRestore(request).isEmpty()); // production: the name is missing
        QCOMPARE(e.dump->state(), QStringLiteral("idle"));
        request.insert(QStringLiteral("typedName"), QStringLiteral("empty.db"));
        request.insert(QStringLiteral("filePath"), path(QStringLiteral("missing.sql")));
        QVERIFY(!e.dump->startRestore(request).isEmpty()); // the file does not exist
        request.insert(QStringLiteral("filePath"), script);
        QCOMPARE(e.dump->startRestore(request), QString());
        QCOMPARE(e.dump->mode(), QStringLiteral("restore"));
        waitState(*e.dump, QStringLiteral("done"));
        QVERIFY(e.dump->resultText().contains(QStringLiteral("src.sql")));

        // The restored database now dumps the same tables.
        e.dump->reset();
        const QString again = path(QStringLiteral("again.sql"));
        QCOMPARE(e.dump->startDump(dumpRequest(into, again)), QString());
        waitState(*e.dump, QStringLiteral("done"));
        QVERIFY(readText(again).contains(QStringLiteral("Widget")));
    }

    void toolPathsArePersistedAndTested()
    {
        Env e = start();
        QVERIFY(e.dump->pgDumpPath().isEmpty());
        QSignalSpy changed(e.dump.get(), &DumpController::toolPathsChanged);
        e.dump->setPgDumpPath(QStringLiteral(QUARRY_FAKE_PG_TOOL));
        e.dump->setPgDumpPath(QStringLiteral(QUARRY_FAKE_PG_TOOL)); // same: no signal
        QCOMPARE(changed.count(), 1);
        e.dump->setPsqlPath(path(QStringLiteral("no-such-psql")));
        QCOMPARE(QSettings().value(QStringLiteral("dump/pgDumpPath")).toString(),
                 QStringLiteral(QUARRY_FAKE_PG_TOOL));

        const QVariantMap good = e.dump->testTool(QStringLiteral("pg_dump"));
        QVERIFY(good.value(QStringLiteral("ok")).toBool());
        QCOMPARE(QFileInfo(good.value(QStringLiteral("path")).toString()).canonicalFilePath(),
                 QFileInfo(QStringLiteral(QUARRY_FAKE_PG_TOOL)).canonicalFilePath());
        const QVariantMap bad = e.dump->testTool(QStringLiteral("psql"));
        QVERIFY(!bad.value(QStringLiteral("ok")).toBool());
        QVERIFY(!bad.value(QStringLiteral("message")).toString().isEmpty());
        QVERIFY(!e.dump->testTool(QStringLiteral("nothing")).value(QStringLiteral("ok")).toBool());

        // A new controller reads them back.
        DumpController other;
        QCOMPARE(other.pgDumpPath(), QStringLiteral(QUARRY_FAKE_PG_TOOL));
        QCOMPARE(other.psqlPath(), path(QStringLiteral("no-such-psql")));
    }

    void postgresDumpReadsTheKeychainAndNeverPutsThePasswordInTheArguments()
    {
        Env e = start();
        const QString log = path(QStringLiteral("pg-log-%1.txt").arg(++m_files));
        qputenv("FAKE_PG_LOG", log.toUtf8());
        e.dump->setPgDumpPath(QStringLiteral(QUARRY_FAKE_PG_TOOL));
        const QString id
            = saveDraft(*e.connections, QStringLiteral("postgres"), QStringLiteral("Pg"), false, 1);
        QVERIFY(!id.isEmpty());
        QVERIFY(e.dump->infoFor(id).value(QStringLiteral("isPostgres")).toBool());
        QVariantMap request = dumpRequest(id, path(QStringLiteral("pg.dump")));
        request.insert(QStringLiteral("format"), QStringLiteral("custom"));
        request.insert(QStringLiteral("schemas"), QStringList { QStringLiteral("public") });
        QSignalSpy states(e.dump.get(), &DumpController::stateChanged);
        QCOMPARE(e.dump->startDump(request), QString());
        QCOMPARE(e.dump->state(), QStringLiteral("preparing")); // the keychain answers later
        waitState(*e.dump, QStringLiteral("done"));
        QVERIFY(e.keychain->calls.contains(QStringLiteral("read")));
        QVERIFY(e.dump->fileExists(path(QStringLiteral("pg.dump"))));

        const QString logged = readText(log);
        QVERIFY(logged.contains(QStringLiteral("--format=custom")));
        QVERIFY(logged.contains(QStringLiteral("PGPASSFILE:"))); // the file held the line...
        QVERIFY(logged.contains(kPassword)); // ...with the password...
        QVERIFY(!logged.contains(QStringLiteral("PGPASSWORD:") + kPassword)); // ...not the env
        for (const QString &line : logged.split(u'\n'))
            if (!line.startsWith(QStringLiteral("PGPASSFILE:")))
                QVERIFY2(!line.contains(kPassword), qPrintable(line));
        // The tool's output reaches the log.
        QTRY_VERIFY_WITH_TIMEOUT(!e.dump->log().isEmpty(), kWaitMs);
        QVERIFY(e.dump->log().join(u'\n').contains(QStringLiteral("dumping contents")));
        QClipboard *clipboard = QGuiApplication::clipboard();
        if (clipboard != nullptr) {
            e.dump->copyLog();
            QVERIFY(clipboard->text().contains(QStringLiteral("dumping contents")));
        }
    }

    void keychainFailureEndsFailed()
    {
        Env e = start();
        e.dump->setPgDumpPath(QStringLiteral(QUARRY_FAKE_PG_TOOL));
        const QString id
            = saveDraft(*e.connections, QStringLiteral("postgres"), QStringLiteral("Pg"), false, 1);
        e.keychain->failRead = true;
        QSignalSpy notes(e.dump.get(), &DumpController::announcement);
        QCOMPARE(e.dump->startDump(dumpRequest(id, path(QStringLiteral("k.sql")))), QString());
        waitState(*e.dump, QStringLiteral("failed"));
        QVERIFY(!e.dump->errorMessage().isEmpty());
        QCOMPARE(e.dump->errorDetail(), QStringLiteral("no backend"));
        QVERIFY(notes.last().at(1).toBool()); // an error is assertive
        QVERIFY(!QFileInfo::exists(path(QStringLiteral("k.sql"))));
    }

    void askEveryTimeNeedsThePasswordInMemory()
    {
        Env e = start();
        e.dump->setPgDumpPath(QStringLiteral(QUARRY_FAKE_PG_TOOL));
        const QString id = saveDraft(*e.connections, QStringLiteral("postgres"),
                                     QStringLiteral("Ask"), false, 2);
        QVERIFY(!id.isEmpty());
        const QString problem = e.dump->startDump(dumpRequest(id, path(QStringLiteral("a.sql"))));
        QVERIFY(!problem.isEmpty());
        QCOMPARE(e.dump->state(), QStringLiteral("idle"));
        QVERIFY(!e.keychain->calls.contains(QStringLiteral("read")));
    }

    void aToolThatFailsEndsFailedWithItsText()
    {
        Env e = start();
        e.dump->setPgDumpPath(QStringLiteral(QUARRY_FAKE_PG_TOOL));
        const QString id
            = saveDraft(*e.connections, QStringLiteral("postgres"), QStringLiteral("Pg"), false, 1);
        qputenv("FAKE_PG_MODE", "authfail");
        QCOMPARE(e.dump->startDump(dumpRequest(id, path(QStringLiteral("f.sql")))), QString());
        waitState(*e.dump, QStringLiteral("failed"));
        QVERIFY(!e.dump->errorMessage().isEmpty());
        QVERIFY(!QFileInfo::exists(path(QStringLiteral("f.sql"))));
        e.dump->reset();
        QCOMPARE(e.dump->state(), QStringLiteral("idle"));
        QVERIFY(e.dump->errorMessage().isEmpty());
    }

    void aMissingToolEndsFailedWithoutRunningAnything()
    {
        Env e = start();
        e.dump->setPgDumpPath(path(QStringLiteral("not-a-tool")));
        const QString id
            = saveDraft(*e.connections, QStringLiteral("postgres"), QStringLiteral("Pg"), false, 1);
        const QString problem = e.dump->startDump(dumpRequest(id, path(QStringLiteral("m.sql"))));
        if (problem.isEmpty())
            waitState(*e.dump, QStringLiteral("failed")); // the runner says so, queued
        QVERIFY(e.dump->state() == QLatin1String("failed")
                || e.dump->state() == QLatin1String("idle"));
        if (e.dump->state() == QLatin1String("failed"))
            QVERIFY(!e.dump->errorMessage().isEmpty());
    }

    void cancelEndsCancelledAndLeavesNoFile()
    {
        Env e = start();
        e.dump->setPgDumpPath(QStringLiteral(QUARRY_FAKE_PG_TOOL));
        const QString id
            = saveDraft(*e.connections, QStringLiteral("postgres"), QStringLiteral("Pg"), false, 1);
        qputenv("FAKE_PG_MODE", "slow");
        const QString out = path(QStringLiteral("slow.sql"));
        QSignalSpy notes(e.dump.get(), &DumpController::announcement);
        QCOMPARE(e.dump->startDump(dumpRequest(id, out)), QString());
        QTRY_COMPARE_WITH_TIMEOUT(e.dump->state(), QStringLiteral("running"), kSlowMs);
        QTRY_VERIFY_WITH_TIMEOUT(!e.dump->log().isEmpty(), kSlowMs); // it is alive
        e.dump->cancel();
        e.dump->cancel(); // twice is harmless
        waitState(*e.dump, QStringLiteral("cancelled"));
        QVERIFY(!e.dump->busy());
        QVERIFY(!e.dump->cancelling());
        QTest::qWait(500); // the killed process reports its death before the runner goes away
        QVERIFY(!QFileInfo::exists(out));
        QVERIFY(notes.last().at(0).toString().contains(QStringLiteral("cancelled")));
        // Idle controllers ignore cancel and reset leaves a finished job alone only when busy.
        e.dump->cancel();
        QCOMPARE(e.dump->state(), QStringLiteral("cancelled"));
        e.dump->reset();
        QCOMPARE(e.dump->state(), QStringLiteral("idle"));
    }

    void cancelWhilePreparingEndsCancelled()
    {
        Env e = start();
        e.dump->setPgDumpPath(QStringLiteral(QUARRY_FAKE_PG_TOOL));
        const QString id
            = saveDraft(*e.connections, QStringLiteral("postgres"), QStringLiteral("Pg"), false, 1);
        QCOMPARE(e.dump->startDump(dumpRequest(id, path(QStringLiteral("p.sql")))), QString());
        QCOMPARE(e.dump->state(), QStringLiteral("preparing"));
        e.dump->cancel();
        QCOMPARE(e.dump->state(), QStringLiteral("cancelled"));
        // The keychain answer that was still in flight changes nothing.
        QTest::qWait(100);
        QCOMPARE(e.dump->state(), QStringLiteral("cancelled"));
        QVERIFY(!QFileInfo::exists(path(QStringLiteral("p.sql"))));
    }

    void postgresRestoreUsesPsqlAndTheConfirmation()
    {
        Env e = start();
        const QString log = path(QStringLiteral("pg-restore-log-%1.txt").arg(++m_files));
        qputenv("FAKE_PG_LOG", log.toUtf8());
        e.dump->setPsqlPath(QStringLiteral(QUARRY_FAKE_PG_TOOL));
        const QString id = saveDraft(*e.connections, QStringLiteral("postgres"),
                                     QStringLiteral("PgProd"), true, 1);
        const QString script = path(QStringLiteral("in.sql"));
        QFile f(script);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("SELECT 1;\n");
        f.close();
        QVariantMap request { { QStringLiteral("connectionId"), id },
                              { QStringLiteral("filePath"), script },
                              { QStringLiteral("phrase"), QStringLiteral("RESTORE") },
                              { QStringLiteral("typedName"), QStringLiteral("wrong") } };
        e.keychain->calls.clear(); // saving the connection wrote the password
        const QString shown = e.dump->infoFor(id).value(QStringLiteral("database")).toString();
        QCOMPARE(shown, QStringLiteral("PgProd")); // no database in the profile: its name
        QVERIFY(!e.dump->startRestore(request).isEmpty());
        QVERIFY(e.keychain->calls.isEmpty());
        request.insert(QStringLiteral("typedName"), shown);
        QCOMPARE(e.dump->startRestore(request), QString());
        waitState(*e.dump, QStringLiteral("done"));
        QVERIFY(readText(log).contains(QStringLiteral("--single-transaction")));
        QVERIFY(e.dump->resultText().contains(QStringLiteral("in.sql")));
    }

    void filesHelpers()
    {
        Env e = start();
        const QString file = path(QStringLiteral("h.db"));
        makeSample(file);
        const QString id = saveSqlite(*e.connections, QStringLiteral("H"), file);
        const QString sql = e.dump->defaultPath(id, {}, false);
        QVERIFY(sql.endsWith(QStringLiteral("h.sql")));
        const QString custom = e.dump->defaultPath(id, {}, true);
        QVERIFY(custom.endsWith(QStringLiteral("h.dump")));
        QCOMPARE(e.dump->retarget(sql, true), custom);
        QCOMPARE(e.dump->retarget(custom, false), sql);
        QCOMPARE(e.dump->retarget(QStringLiteral("x.txt"), true), QStringLiteral("x.txt"));
        QCOMPARE(e.dump->retarget(QString(), true), QString());
        QVERIFY(e.dump->folderUrl(sql).isLocalFile());
        QVERIFY(!e.dump->fileExists(QString()));
        QVERIFY(e.dump->fileExists(file));
    }
};

QTEST_MAIN(TstDumpController)
#include "tst_dump_controller.moc"
