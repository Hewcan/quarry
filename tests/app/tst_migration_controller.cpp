#include "migration_controller.h"

#include <quarry/connection_profile.h>
#include <quarry/connection_store.h>

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <sqlite3.h>

using namespace quarry;
using namespace quarry::ui;

class TstMigrationController : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    int m_files = 0;

    // A controller that knows `id` as a Laravel project in `folder`. The connection does not exist
    // in the (temporary) quarry.db, so saving the settings fails there; what is asked here only
    // needs the settings in memory. Never the real quarry.db.
    void configure(ConnectionController &connections, MigrationController &c, const QString &id,
                   const QString &folder)
    {
        connections.setDatabasePath(m_dir.filePath(QStringLiteral("q-%1.db").arg(++m_files)));
        c.setConnections(&connections);
        c.saveSettings(id, {}, QStringLiteral("laravel"), folder, {});
    }

    static void write(const QString &path, const QByteArray &content)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(content);
    }

    struct Env;
    void open(Env &e, bool production = false);
    static void useFake(const Env &e, const char *mode, const QString &targetDb = {});
    static QStringList logOf(const Env &e);
    static QString stateOf(const Env &e, const QString &name);
    static QStringList kindsOf(const QueryMessagesModel &m);

private slots:
    void cleanup();
    void kindsStartWithNone();
    void validateWithoutDisk();
    void checkFolder();
    void readSourceGivesTheCodeOfTheFile();
    void readSourceFailsInPlainLanguage();
    void readSourceWithoutSettingsFails();
    void settingsAreKeptPerDatabase();
    void readSourceUsesTheSettingsOfItsDatabase();
    void aDatabaseWithoutSettingsCannotRun();
    void releaseSourceForgetsTheText();
    void entryOfAnUnreadConnectionIsNotFound();

    void settingsKeepPhpAndProjectRoot();
    void checkEnvironmentSaysWhatIsEffectiveAndMissing();
    void canRunSaysWhyNot();
    void canRunActionEnforcesMigrationState();
    void dryRunOnTheSameDatabaseRunsWithoutAsking();
    void dryRunOnAnotherDatabaseWarnsButRuns();
    void dryRunGoesOnWhenTheDatabaseCannotBeVerified();
    void runOnAnotherDatabaseIsBlockedAndNeverExecutes();
    void aDatabaseThatCannotBeVerifiedBlocksRunAndRollback();
    void runOnTheSameDatabaseWaitsForConfirmationAndThenRuns();
    void runAllPendingWaitsForConfirmationAndRunsFolderInOrder();
    void rollbackAsksWithItsOwnWords();
    void decliningRunsNothing();
    void confirmWithoutAQuestionDoesNothing();
    void aProductionConnectionIsMarkedInTheConfirmation();
    void cancellingWhileItRunsKillsTheProcess();
    void cancellingARunWarnsThatTheDatabaseMayBeHalfMigrated();
    void cancellingTheCheckStopsIt();
    void aFailedMigrationShowsTheErrorAndRefreshes();
    void theOutputKeepsTheFirstFiveThousandLines();
    void leavingATabForgetsItsRunUnlessItRuns();
};

void TstMigrationController::kindsStartWithNone()
{
    MigrationController c;
    const QVariantList kinds = c.kinds();
    QVERIFY(kinds.size() >= 2);
    QCOMPARE(kinds.at(0).toMap().value(QStringLiteral("id")).toString(), QString());
    QCOMPARE(kinds.at(1).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("laravel"));
    QCOMPARE(kinds.at(1).toMap().value(QStringLiteral("defaultTable")).toString(),
             QStringLiteral("migrations"));
}

void TstMigrationController::validateWithoutDisk()
{
    MigrationController c;
    QCOMPARE(c.validate({}, {}, {}), QString());
    QVERIFY(!c.validate(QStringLiteral("laravel"), {}, {}).isEmpty());
    QVERIFY(!c.validate(QStringLiteral("laravel"), QStringLiteral("rel/path"), {}).isEmpty());
    const QString laravel = QStringLiteral("laravel");
    QVERIFY(!c.validate(laravel, QDir::tempPath(), QStringLiteral("a;b")).isEmpty());
    QCOMPARE(c.validate(QStringLiteral("laravel"), QDir::tempPath(), {}), QString());
}

void TstMigrationController::checkFolder()
{
    QTemporaryDir folder;
    MigrationController c; // the folder check never opens quarry.db
    c.checkFolder(QStringLiteral("laravel"), folder.path(), {});
    QCOMPARE(c.folderState(), QStringLiteral("checking"));
    QTRY_COMPARE(c.folderState(), QStringLiteral("ok"));
    c.checkFolder(QStringLiteral("laravel"),
                  QDir::tempPath() + QStringLiteral("/quarry-missing-migrations-4242"), {});
    QTRY_COMPARE(c.folderState(), QStringLiteral("failed"));
    QVERIFY(!c.folderMessage().isEmpty());
    c.resetFolderCheck();
    QCOMPARE(c.folderState(), QStringLiteral("idle"));
}

void TstMigrationController::readSourceGivesTheCodeOfTheFile()
{
    QTemporaryDir folder;
    const QString name = QStringLiteral("2024_01_01_000000_create_users_table");
    write(folder.filePath(name + QStringLiteral(".php")),
          "<?php\nSchema::create('users', function () {});\n");
    ConnectionController connections;
    MigrationController c;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    configure(connections, c, id, folder.path());
    QVERIFY(c.isConfigured(id, {}));
    QCOMPARE(c.sourceOf(id, {}, name).value(QStringLiteral("state")).toString(),
             QStringLiteral("none"));

    QSignalSpy changed(&c, &MigrationController::sourceChanged);
    c.readSource(id, {}, name);
    QCOMPARE(c.sourceOf(id, {}, name).value(QStringLiteral("state")).toString(),
             QStringLiteral("loading"));
    c.readSource(id, {}, name); // already loading: not a second request
    QTRY_COMPARE(c.sourceOf(id, {}, name).value(QStringLiteral("state")).toString(),
                 QStringLiteral("ready"));
    const QVariantMap source = c.sourceOf(id, {}, name);
    QVERIFY(
        source.value(QStringLiteral("text")).toString().contains(QStringLiteral("Schema::create")));
    QCOMPARE(source.value(QStringLiteral("file")).toString(), name + QStringLiteral(".php"));
    QCOMPARE(source.value(QStringLiteral("definition")).toString(), QStringLiteral("PHP"));
    QCOMPARE(changed.size(), 2); // loading, ready
    QCOMPARE(changed.at(1).at(0).toString(), id);
    QCOMPARE(changed.at(1).at(2).toString(), name); // (connectionId, database, name)
}

void TstMigrationController::readSourceFailsInPlainLanguage()
{
    QTemporaryDir folder;
    const QString big = QStringLiteral("2024_02_01_000000_huge");
    write(folder.filePath(big + QStringLiteral(".php")),
          QByteArray(int(MigrationService::kMaxSourceBytes) + 1, 'x'));
    ConnectionController connections;
    MigrationController c;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    configure(connections, c, id, folder.path());

    const QString missing = QStringLiteral("2024_03_01_000000_not_there");
    for (const QString &name : { missing, big, QStringLiteral("../escape") }) {
        c.readSource(id, {}, name);
        QTRY_COMPARE(c.sourceOf(id, {}, name).value(QStringLiteral("state")).toString(),
                     QStringLiteral("failed"));
        const QVariantMap source = c.sourceOf(id, {}, name);
        QVERIFY(!source.value(QStringLiteral("message")).toString().isEmpty());
        QVERIFY(source.value(QStringLiteral("text")).toString().isEmpty());
    }
    // Asking again is a new request (the file may exist by now).
    write(folder.filePath(missing + QStringLiteral(".php")), "<?php\n");
    c.readSource(id, {}, missing);
    QTRY_COMPARE(c.sourceOf(id, {}, missing).value(QStringLiteral("state")).toString(),
                 QStringLiteral("ready"));
}

void TstMigrationController::readSourceWithoutSettingsFails()
{
    MigrationController c;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSignalSpy changed(&c, &MigrationController::sourceChanged);
    c.readSource(id, {}, QStringLiteral("x"));
    QCOMPARE(changed.size(), 1);
    const QVariantMap source = c.sourceOf(id, {}, QStringLiteral("x"));
    QCOMPARE(source.value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
    QVERIFY(!source.value(QStringLiteral("message")).toString().isEmpty());
}

// Two databases of one server connection are two projects: nothing of one leaks into the other.
void TstMigrationController::settingsAreKeptPerDatabase()
{
    QTemporaryDir shop;
    QTemporaryDir blog;
    ConnectionController connections;
    MigrationController c;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    configure(connections, c, id, shop.path()); // database "": the connection's own
    c.saveSettings(id, QStringLiteral("shop"), QStringLiteral("laravel"), shop.path(), {});
    c.saveSettings(id, QStringLiteral("blog"), QStringLiteral("laravel"), blog.path(),
                   QStringLiteral("blogs"));
    QVERIFY(c.isConfigured(id, {}));
    QVERIFY(c.isConfigured(id, QStringLiteral("shop")));
    QVERIFY(c.isConfigured(id, QStringLiteral("blog")));
    QVERIFY(!c.isConfigured(id, QStringLiteral("other")));
    QVERIFY(c.isConfiguredAny(id));
    QVERIFY(!c.isConfiguredAny(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    const auto folderOf = [&](const QString &database) {
        return QDir::fromNativeSeparators(
            c.settingsOf(id, database).value(QStringLiteral("folder")).toString());
    };
    QCOMPARE(folderOf(QStringLiteral("shop")), shop.path());
    QCOMPARE(folderOf(QStringLiteral("blog")), blog.path());
    QCOMPARE(c.settingsOf(id, QStringLiteral("blog")).value(QStringLiteral("table")).toString(),
             QStringLiteral("blogs"));
    QCOMPARE(c.settingsOf(id, QStringLiteral("shop")).value(QStringLiteral("table")).toString(),
             QString());
    QCOMPARE(c.settingsOf(id, QStringLiteral("other")).value(QStringLiteral("kind")).toString(),
             QString());

    // Removing the settings of one database leaves the others alone.
    QSignalSpy settings(&c, &MigrationController::settingsChanged);
    c.saveSettings(id, QStringLiteral("shop"), {}, {}, {});
    QCOMPARE(settings.size(), 1);
    QVERIFY(!c.isConfigured(id, QStringLiteral("shop")));
    QVERIFY(c.isConfigured(id, QStringLiteral("blog")));
    QVERIFY(c.isConfigured(id, {}));
    QCOMPARE(folderOf(QStringLiteral("blog")), blog.path());
    // The name of a database is trimmed, like the rest of the settings.
    c.saveSettings(id, QStringLiteral(" shop "), QStringLiteral("laravel"), shop.path(), {});
    QVERIFY(c.isConfigured(id, QStringLiteral("shop")));
    // Settings that cannot be saved as typed are dropped for that database only.
    c.saveSettings(id, QStringLiteral("blog"), QStringLiteral("laravel"), QStringLiteral("rel"),
                   {});
    QVERIFY(!c.isConfigured(id, QStringLiteral("blog")));
    QVERIFY(c.isConfigured(id, QStringLiteral("shop")));
}

void TstMigrationController::readSourceUsesTheSettingsOfItsDatabase()
{
    QTemporaryDir shop;
    QTemporaryDir blog;
    const QString name = QStringLiteral("2024_01_01_000000_create_users_table");
    write(shop.filePath(name + QStringLiteral(".php")), "<?php // shop\n");
    write(blog.filePath(name + QStringLiteral(".php")), "<?php // blog\n");
    ConnectionController connections;
    MigrationController c;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    configure(connections, c, id, shop.path());
    c.saveSettings(id, QStringLiteral("shop"), QStringLiteral("laravel"), shop.path(), {});
    c.saveSettings(id, QStringLiteral("blog"), QStringLiteral("laravel"), blog.path(), {});
    QSignalSpy changed(&c, &MigrationController::sourceChanged);
    c.readSource(id, QStringLiteral("shop"), name);
    c.readSource(id, QStringLiteral("blog"), name);
    const auto textOf = [&](const QString &database) {
        return c.sourceOf(id, database, name).value(QStringLiteral("text")).toString();
    };
    QTRY_COMPARE(
        c.sourceOf(id, QStringLiteral("shop"), name).value(QStringLiteral("state")).toString(),
        QStringLiteral("ready"));
    QTRY_COMPARE(
        c.sourceOf(id, QStringLiteral("blog"), name).value(QStringLiteral("state")).toString(),
        QStringLiteral("ready"));
    QVERIFY(textOf(QStringLiteral("shop")).contains(QStringLiteral("shop")));
    QVERIFY(textOf(QStringLiteral("blog")).contains(QStringLiteral("blog")));
    // The signal says which database changed.
    QVERIFY(changed.size() >= 2);
    QStringList databases;
    for (const auto &args : changed)
        databases << args.at(1).toString();
    QVERIFY(databases.contains(QStringLiteral("shop")));
    QVERIFY(databases.contains(QStringLiteral("blog")));
    // The connection's own database has no settings here: nothing to read.
    c.releaseSource(id, QStringLiteral("shop"), name);
    QCOMPARE(c.sourceOf(id, QStringLiteral("shop"), name).value(QStringLiteral("state")).toString(),
             QStringLiteral("none"));
    QVERIFY(!textOf(QStringLiteral("blog")).isEmpty()); // the other tab keeps its code
    c.readSource(id, QStringLiteral("elsewhere"), name);
    QCOMPARE(
        c.sourceOf(id, QStringLiteral("elsewhere"), name).value(QStringLiteral("state")).toString(),
        QStringLiteral("failed"));
}

void TstMigrationController::aDatabaseWithoutSettingsCannotRun()
{
    QTemporaryDir shop;
    ConnectionController connections;
    MigrationController c;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    configure(connections, c, id, shop.path());
    const QString name = QStringLiteral("2024_01_01_000000_a");
    c.saveSettings(id, QStringLiteral("shop"), QStringLiteral("laravel"), shop.path(), {});
    const QVariantMap other = c.canRun(id, QStringLiteral("blog"), name);
    QVERIFY(!other.value(QStringLiteral("ok")).toBool());
    QVERIFY(
        other.value(QStringLiteral("reason")).toString().contains(QStringLiteral("configured")));
    QVERIFY(!c.start(id, QStringLiteral("blog"), name, QStringLiteral("dryRun")));
    QVERIFY(!c.isBusy(id, QStringLiteral("blog")));
    QCOMPARE(c.pendingOf(id, QStringLiteral("shop")), -1); // not inspected
    QVERIFY(c.pendingCounts().isEmpty());
    // The entry of an unread database is not found, whatever the other one has.
    QVERIFY(!c.entryOf(id, QStringLiteral("shop"), name).value(QStringLiteral("found")).toBool());
}

void TstMigrationController::releaseSourceForgetsTheText()
{
    QTemporaryDir folder;
    const QString name = QStringLiteral("2024_01_01_000000_a");
    write(folder.filePath(name + QStringLiteral(".php")), "<?php\n");
    ConnectionController connections;
    MigrationController c;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    configure(connections, c, id, folder.path());
    c.readSource(id, {}, name);
    QTRY_COMPARE(c.sourceOf(id, {}, name).value(QStringLiteral("state")).toString(),
                 QStringLiteral("ready"));
    c.releaseSource(id, {}, name);
    QCOMPARE(c.sourceOf(id, {}, name).value(QStringLiteral("state")).toString(),
             QStringLiteral("none"));
    // A late answer for a tab that is gone is dropped.
    c.readSource(id, {}, name);
    c.releaseSource(id, {}, name);
    QTest::qWait(200);
    QCOMPARE(c.sourceOf(id, {}, name).value(QStringLiteral("state")).toString(),
             QStringLiteral("none"));
}

void TstMigrationController::entryOfAnUnreadConnectionIsNotFound()
{
    MigrationController c;
    const QVariantMap e
        = c.entryOf(QUuid::createUuid().toString(QUuid::WithoutBraces), {}, QStringLiteral("x"));
    QVERIFY(!e.value(QStringLiteral("found")).toBool());
}

// ---- running migrations (H12 phase C)
// ------------------------------------------------------------- Always the fake `php` of
// tests/core/fake_php: it logs its arguments and prints what the mode says. No PHP, no migration
// and no database of a project is ever touched.

namespace {

QString applied()
{
    return QStringLiteral("2024_01_01_000000_create_users_table");
}
QString pending()
{
    return QStringLiteral("2024_02_01_000000_create_posts_table");
}
QString second()
{
    return QStringLiteral("2024_03_01_000000_create_tags_table");
}

bool sql(const QString &file, const char *statements)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open(file.toUtf8().constData(), &db) != SQLITE_OK)
        return false;
    const bool ok = sqlite3_exec(db, statements, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(db);
    return ok;
}

QByteArray targetJson(const QString &db)
{
    return QByteArray(R"({"platform":{"config":{"driver":"sqlite","database":")")
        + QDir::fromNativeSeparators(db).toUtf8() + QByteArray(R"("}},"name":"sqlite"})");
}

} // namespace

struct TstMigrationController::Env
{
    ConnectionController connections;
    MigrationController c;
    QueryMessagesModel messages;
    QString root;
    QString folder;
    QString db;
    QString log;
    QString id;
};

void TstMigrationController::open(Env &e, bool production)
{
    e.root = m_dir.filePath(QStringLiteral("project-%1").arg(++m_files));
    e.folder = e.root + QStringLiteral("/database/migrations");
    e.db = e.root + QStringLiteral("/app.db");
    e.log = e.root + QStringLiteral("/fake_php.log");
    QVERIFY(QDir().mkpath(e.folder));
    write(e.root + QStringLiteral("/artisan"), "<?php\n");
    for (const QString &name : { applied(), pending(), second() })
        write(e.folder + QLatin1Char('/') + name + QStringLiteral(".php"), "<?php\n");
    QVERIFY(sql(e.db,
                "CREATE TABLE migrations(id INTEGER PRIMARY KEY, migration TEXT NOT NULL, batch "
                "INTEGER NOT NULL);"
                "INSERT INTO migrations(migration, batch) VALUES "
                "('2024_01_01_000000_create_users_table', 1);"));

    e.connections.setDatabasePath(m_dir.filePath(QStringLiteral("q-%1.db").arg(++m_files)));
    e.connections.setProviderDirectory(QStringLiteral(QUARRY_PLUGIN_DIR));
    e.connections.start();
    QTRY_VERIFY_WITH_TIMEOUT(!e.connections.model()->loading(), 10000);
    e.c.setConnections(&e.connections);
    ConnectionProfile profile;
    profile.id = QUuid::createUuid();
    profile.name = QStringLiteral("Laravel app");
    profile.providerId = QStringLiteral("sqlite");
    profile.values = { { QStringLiteral("file"), e.db } };
    profile.production = production;
    profile.passwordMode = PasswordMode::None;
    e.connections.model()->store()->save(profile);
    e.id = profile.id.toString(QUuid::WithoutBraces);
    QTRY_VERIFY_WITH_TIMEOUT(e.connections.model()->indexOf(e.id) >= 0, 10000);
    e.c.saveSettings(e.id, {}, QStringLiteral("laravel"), e.folder, {},
                     QStringLiteral(QUARRY_FAKE_PHP), e.root);
    e.c.setMessages(e.id, {}, pending(), &e.messages);
    e.connections.tree()->ensureSession(e.id);
    QTRY_VERIFY_WITH_TIMEOUT(e.connections.tree()->connectedIds().contains(e.id), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(
        e.c.entryOf(e.id, {}, pending()).value(QStringLiteral("found")).toBool(), 10000);
}

void TstMigrationController::useFake(const Env &e, const char *mode, const QString &targetDb)
{
    qputenv("FAKE_PHP_MODE", mode);
    qputenv("FAKE_PHP_LOG", e.log.toUtf8());
    qputenv("FAKE_PHP_TARGET_JSON", targetJson(targetDb.isEmpty() ? e.db : targetDb));
}

QStringList TstMigrationController::logOf(const Env &e)
{
    QFile f(e.log);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

QString TstMigrationController::stateOf(const Env &e, const QString &name)
{
    return e.c.runOf(e.id, {}, name).value(QStringLiteral("state")).toString();
}

QStringList TstMigrationController::kindsOf(const QueryMessagesModel &m)
{
    QStringList out;
    for (int i = 0; i < m.count(); ++i)
        out.append(m.data(m.index(i), QueryMessagesModel::KindRole).toString());
    return out;
}

void TstMigrationController::cleanup()
{
    qunsetenv("FAKE_PHP_MODE");
    qunsetenv("FAKE_PHP_LOG");
    qunsetenv("FAKE_PHP_TARGET_JSON");
    qunsetenv("FAKE_PHP_TICKS");
}

void TstMigrationController::settingsKeepPhpAndProjectRoot()
{
    ConnectionController connections;
    MigrationController c;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    connections.setDatabasePath(m_dir.filePath(QStringLiteral("q-%1.db").arg(++m_files)));
    c.setConnections(&connections);
    c.saveSettings(id, {}, QStringLiteral("laravel"), QDir::tempPath(), {},
                   QStringLiteral("D:/php/php.exe"), QStringLiteral("D:/app"));
    const QVariantMap s = c.settingsOf(id, {});
    QCOMPARE(QDir::fromNativeSeparators(s.value(QStringLiteral("phpPath")).toString()),
             QStringLiteral("D:/php/php.exe"));
    QCOMPARE(QDir::fromNativeSeparators(s.value(QStringLiteral("projectRoot")).toString()),
             QStringLiteral("D:/app"));
    // Both are optional.
    c.saveSettings(id, {}, QStringLiteral("laravel"), QDir::tempPath(), {});
    QVERIFY(c.settingsOf(id, {}).value(QStringLiteral("phpPath")).toString().isEmpty());
    QVERIFY(c.settingsOf(id, {}).value(QStringLiteral("projectRoot")).toString().isEmpty());
}

void TstMigrationController::checkEnvironmentSaysWhatIsEffectiveAndMissing()
{
    MigrationController c;
    QTemporaryDir project;
    QVERIFY(QDir().mkpath(project.filePath(QStringLiteral("database/migrations"))));
    const QString folder = project.filePath(QStringLiteral("database/migrations"));
    const QString laravel = QStringLiteral("laravel");

    // Nothing typed: php from the PATH, the root two levels above the folder, and no artisan there.
    QVariantMap env = c.checkEnvironment(laravel, folder, {}, {});
    QCOMPARE(env.value(QStringLiteral("phpText")).toString(),
             QStringLiteral("Using php from the PATH"));
    QCOMPARE(env.value(QStringLiteral("rootText")).toString(),
             QStringLiteral("Project root: %1").arg(QDir::toNativeSeparators(project.path())));
    QVERIFY(env.value(QStringLiteral("rootError")).toString().contains(QStringLiteral("artisan")));

    // A php that exists and a root with artisan: nothing is missing.
    write(project.filePath(QStringLiteral("artisan")), "<?php\n");
    env = c.checkEnvironment(laravel, folder, QStringLiteral(QUARRY_FAKE_PHP), {});
    QVERIFY(env.value(QStringLiteral("phpText")).toString().startsWith(QStringLiteral("Using ")));
    QVERIFY(env.value(QStringLiteral("phpError")).toString().isEmpty());
    QVERIFY(env.value(QStringLiteral("rootError")).toString().isEmpty());

    // A php that does not exist: said, and the project is still checked on its own.
    env = c.checkEnvironment(laravel, folder, project.filePath(QStringLiteral("no-php.exe")), {});
    QVERIFY(env.value(QStringLiteral("phpError")).toString().contains(QStringLiteral("PHP")));
    QVERIFY(env.value(QStringLiteral("rootError")).toString().isEmpty());
    env = c.checkEnvironment(laravel, folder, project.filePath(QStringLiteral("no-php.exe")),
                             folder); // no artisan in the migrations folder
    QVERIFY(!env.value(QStringLiteral("phpError")).toString().isEmpty());
    QVERIFY(!env.value(QStringLiteral("rootError")).toString().isEmpty());
    QCOMPARE(env.value(QStringLiteral("rootText")).toString(),
             QStringLiteral("Project root: %1").arg(QDir::toNativeSeparators(folder)));

    // No project type: nothing to say.
    env = c.checkEnvironment({}, folder, {}, {});
    QVERIFY(env.value(QStringLiteral("rootError")).toString().isEmpty());
}

void TstMigrationController::canRunSaysWhyNot()
{
    Env e;
    open(e);
    QVERIFY(e.c.canRun(e.id, {}, pending()).value(QStringLiteral("ok")).toBool());
    const auto reason = [&](const QString &id, const QString &name) {
        const QVariantMap r = e.c.canRun(id, {}, name);
        return r.value(QStringLiteral("ok")).toBool()
            ? QString()
            : r.value(QStringLiteral("reason")).toString();
    };
    // Not configured.
    QVERIFY(reason(QUuid::createUuid().toString(QUuid::WithoutBraces), pending())
                .contains(QStringLiteral("no migrations configured")));
    // A name that cannot be a file.
    QVERIFY(!reason(e.id, QStringLiteral("../escape")).isEmpty());
    // An orphan: in the table, no file.
    QVERIFY(sql(e.db,
                "INSERT INTO migrations(migration, batch) VALUES ('2023_12_31_000000_ghost', 1)"));
    e.c.saveSettings(e.id, {}, QStringLiteral("laravel"), e.folder, {},
                     QStringLiteral(QUARRY_FAKE_PHP), e.root);
    QTRY_COMPARE_WITH_TIMEOUT(e.c.entryOf(e.id, {}, QStringLiteral("2023_12_31_000000_ghost"))
                                  .value(QStringLiteral("state"))
                                  .toString(),
                              QStringLiteral("orphan"), 10000);
    QVERIFY(reason(e.id, QStringLiteral("2023_12_31_000000_ghost"))
                .contains(QStringLiteral("no file")));
    // php not found, then a project without artisan.
    e.c.saveSettings(e.id, {}, QStringLiteral("laravel"), e.folder, {},
                     e.root + QStringLiteral("/no-php.exe"), e.root);
    QVERIFY(reason(e.id, pending()).contains(QStringLiteral("PHP was not found")));
    e.c.saveSettings(e.id, {}, QStringLiteral("laravel"), e.folder, {},
                     QStringLiteral(QUARRY_FAKE_PHP), e.folder);
    QVERIFY(reason(e.id, pending()).contains(QStringLiteral("artisan")));
    // Not connected.
    e.c.saveSettings(e.id, {}, QStringLiteral("laravel"), e.folder, {},
                     QStringLiteral(QUARRY_FAKE_PHP), e.root);
    e.connections.tree()->disconnectConnection(e.id);
    QTRY_VERIFY_WITH_TIMEOUT(!e.connections.tree()->connectedIds().contains(e.id), 10000);
    QVERIFY(reason(e.id, pending()).contains(QStringLiteral("Connect to the database")));
    QVERIFY(!e.c.start(e.id, {}, pending(), QStringLiteral("dryRun"))); // nothing starts
    QVERIFY(logOf(e).isEmpty());
}

void TstMigrationController::canRunActionEnforcesMigrationState()
{
    Env e;
    open(e);
    useFake(e, "target");

    // Pending migration: Run, RunAll and DryRun are enabled; Rollback is disabled.
    QVERIFY(e.c.canRunAction(e.id, {}, pending(), QStringLiteral("run"))
                .value(QStringLiteral("ok"))
                .toBool());
    QVERIFY(e.c.canRunAction(e.id, {}, pending(), QStringLiteral("runAll"))
                .value(QStringLiteral("ok"))
                .toBool());
    QVERIFY(e.c.canRunAction(e.id, {}, pending(), QStringLiteral("dryRun"))
                .value(QStringLiteral("ok"))
                .toBool());
    const QVariantMap rollbackOnPending
        = e.c.canRunAction(e.id, {}, pending(), QStringLiteral("rollback"));
    QVERIFY(!rollbackOnPending.value(QStringLiteral("ok")).toBool());
    QVERIFY(rollbackOnPending.value(QStringLiteral("reason"))
                .toString()
                .contains(QStringLiteral("pending")));
    QVERIFY(!e.c.start(e.id, {}, pending(), QStringLiteral("rollback")));

    // Applied migration: Rollback, RunAll (pending > 0) and DryRun are enabled; Run is disabled.
    const QVariantMap runOnApplied = e.c.canRunAction(e.id, {}, applied(), QStringLiteral("run"));
    QVERIFY(!runOnApplied.value(QStringLiteral("ok")).toBool());
    QVERIFY(runOnApplied.value(QStringLiteral("reason"))
                .toString()
                .contains(QStringLiteral("already applied")));
    QVERIFY(!e.c.start(e.id, {}, applied(), QStringLiteral("run")));
    QVERIFY(e.c.canRunAction(e.id, {}, applied(), QStringLiteral("rollback"))
                .value(QStringLiteral("ok"))
                .toBool());
    QVERIFY(e.c.canRunAction(e.id, {}, applied(), QStringLiteral("runAll"))
                .value(QStringLiteral("ok"))
                .toBool());
    QVERIFY(e.c.canRunAction(e.id, {}, applied(), QStringLiteral("dryRun"))
                .value(QStringLiteral("ok"))
                .toBool());

    // When all migrations are applied (pending == 0), RunAll is disabled too.
    QVERIFY(sql(e.db,
                "INSERT INTO migrations(migration, batch) VALUES "
                "('2024_02_01_000000_create_posts_table', 2),"
                "('2024_03_01_000000_create_tags_table', 2);"));
    e.c.saveSettings(e.id, {}, QStringLiteral("laravel"), e.folder, {},
                     QStringLiteral(QUARRY_FAKE_PHP), e.root);
    QTRY_COMPARE_WITH_TIMEOUT(e.c.pendingOf(e.id, {}), 0, 10000);
    const QVariantMap runAllWhenNone
        = e.c.canRunAction(e.id, {}, applied(), QStringLiteral("runAll"));
    QVERIFY(!runAllWhenNone.value(QStringLiteral("ok")).toBool());
    QVERIFY(runAllWhenNone.value(QStringLiteral("reason"))
                .toString()
                .contains(QStringLiteral("no pending")));
    QVERIFY(!e.c.start(e.id, {}, applied(), QStringLiteral("runAll")));
    QVERIFY(logOf(e).isEmpty());
}

void TstMigrationController::dryRunOnTheSameDatabaseRunsWithoutAsking()
{
    Env e;
    open(e);
    useFake(e, "target");
    QSignalSpy changed(&e.c, &MigrationController::runChanged);
    QVERIFY(e.c.start(e.id, {}, pending(), QStringLiteral("dryRun")));
    QCOMPARE(stateOf(e, pending()), QStringLiteral("checking"));
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("done"), 10000);
    const QVariantMap r = e.c.runOf(e.id, {}, pending());
    QCOMPARE(r.value(QStringLiteral("exitCode")).toInt(), 0);
    QCOMPARE(r.value(QStringLiteral("message")).toString(), QStringLiteral("Finished"));
    QVERIFY(r.value(QStringLiteral("warning")).toString().isEmpty());
    QVERIFY(r.value(QStringLiteral("output")).toString().contains(QStringLiteral("Migrated:")));
    QVERIFY(!changed.isEmpty());
    // db:show first, then the migrate with --pretend: nothing else, and no password anywhere.
    const QStringList log = logOf(e);
    QVERIFY(log.contains(QStringLiteral("db:show")));
    QVERIFY(log.contains(QStringLiteral("--pretend")));
    QVERIFY(log.indexOf(QStringLiteral("db:show")) < log.indexOf(QStringLiteral("--pretend")));
    QVERIFY(log.contains(QStringLiteral("--path=database/migrations/") + pending()
                         + QStringLiteral(".php")));
    // The Messages: the command, what it printed (stdout and stderr told apart) and the result.
    const QStringList kinds = kindsOf(e.messages);
    QVERIFY(kinds.contains(QStringLiteral("output")));
    QVERIFY(kinds.contains(QStringLiteral("stderr")));
    QCOMPARE(kinds.last(), QStringLiteral("statement"));
    QCOMPARE(e.messages
                 .data(e.messages.index(kinds.indexOf(QStringLiteral("stderr"))),
                       QueryMessagesModel::TypeRole)
                 .toString(),
             QStringLiteral("stderr"));
    // Nothing changed in the table (a dry run never reads it again).
    QVERIFY(!e.c.isBusy(e.id, {}));
}

void TstMigrationController::dryRunOnAnotherDatabaseWarnsButRuns()
{
    Env e;
    open(e);
    useFake(e, "target", e.root + QStringLiteral("/other-database.db"));
    QVERIFY(e.c.start(e.id, {}, pending(), QStringLiteral("dryRun")));
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("done"), 10000);
    const QString warning
        = e.c.runOf(e.id, {}, pending()).value(QStringLiteral("warning")).toString();
    QVERIFY2(warning.startsWith(QStringLiteral("This dry run would use database ")),
             qPrintable(warning));
    QVERIFY(warning.contains(QStringLiteral("other-database.db")));
    QVERIFY(warning.endsWith(QStringLiteral("not the one of this connection.")));
    QVERIFY(logOf(e).contains(QStringLiteral("--pretend"))); // it did run: it changes nothing
    QVERIFY(kindsOf(e.messages).contains(QStringLiteral("notice")));
    QCOMPARE(e.messages.data(e.messages.index(0), QueryMessagesModel::TypeRole).toString(),
             QStringLiteral("Warning"));
}

void TstMigrationController::dryRunGoesOnWhenTheDatabaseCannotBeVerified()
{
    Env e;
    open(e);
    useFake(e, "nocommand");
    QVERIFY(e.c.start(e.id, {}, pending(), QStringLiteral("dryRun")));
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("done"), 10000);
    QVERIFY(e.c.runOf(e.id, {}, pending())
                .value(QStringLiteral("warning"))
                .toString()
                .contains(QStringLiteral("could not be verified")));
}

void TstMigrationController::runOnAnotherDatabaseIsBlockedAndNeverExecutes()
{
    Env e;
    open(e);
    useFake(e, "target", e.root + QStringLiteral("/other-database.db"));
    QVERIFY(e.c.start(e.id, {}, pending(), QStringLiteral("run")));
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("blocked"), 10000);
    const QVariantMap r = e.c.runOf(e.id, {}, pending());
    QVERIFY(!r.value(QStringLiteral("why")).toString().isEmpty());
    QVERIFY(r.value(QStringLiteral("target")).toMap().value(QStringLiteral("known")).toBool());
    QVERIFY(r.value(QStringLiteral("target"))
                .toMap()
                .value(QStringLiteral("database"))
                .toString()
                .contains(QStringLiteral("other-database.db")));
    QVERIFY(r.value(QStringLiteral("connection"))
                .toMap()
                .value(QStringLiteral("database"))
                .toString()
                .contains(QStringLiteral("app.db")));
    // The two databases and the reason are in the panel and in Messages.
    QVERIFY(
        r.value(QStringLiteral("message")).toString().contains(QStringLiteral("Nothing was run")));
    QVERIFY(kindsOf(e.messages).contains(QStringLiteral("error")));
    // Even asking to confirm anyway does nothing: the state is not "awaitingConfirm".
    e.c.confirm(e.id, {}, pending());
    QTest::qWait(300);
    QCOMPARE(stateOf(e, pending()), QStringLiteral("blocked"));
    QVERIFY(!logOf(e).contains(QStringLiteral("migrate")));
    QVERIFY(!logOf(e).contains(QStringLiteral("migrate:rollback")));
    QVERIFY(!e.c.isBusy(e.id, {})); // and the connection can try again
    // Same for a rollback and runAll.
    QVERIFY(e.c.start(e.id, {}, applied(), QStringLiteral("rollback")));
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, applied()), QStringLiteral("blocked"), 10000);
    QVERIFY(!logOf(e).contains(QStringLiteral("migrate:rollback")));
    QVERIFY(e.c.start(e.id, {}, pending(), QStringLiteral("runAll")));
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("blocked"), 10000);
    QVERIFY(!logOf(e).contains(QStringLiteral("migrate")));
}

void TstMigrationController::aDatabaseThatCannotBeVerifiedBlocksRunAndRollback()
{
    Env e;
    open(e);
    useFake(e, "nocommand"); // Laravel older than 10: no db:show
    const QList<QPair<QString, QString>> cases {
        { pending(), QStringLiteral("run") },
        { pending(), QStringLiteral("runAll") },
        { applied(), QStringLiteral("rollback") },
    };
    for (const auto &[name, action] : cases) {
        QVERIFY(e.c.start(e.id, {}, name, action));
        QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, name), QStringLiteral("blocked"), 10000);
        const QVariantMap r = e.c.runOf(e.id, {}, name);
        QCOMPARE(
            r.value(QStringLiteral("message")).toString(),
            QStringLiteral("The database artisan uses could not be verified, so nothing was run."));
        QVERIFY(!r.value(QStringLiteral("detail")).toString().isEmpty());
        QVERIFY(!r.value(QStringLiteral("target")).toMap().value(QStringLiteral("known")).toBool());
    }
    QVERIFY(!logOf(e).contains(QStringLiteral("migrate")));
    QVERIFY(!logOf(e).contains(QStringLiteral("migrate:rollback")));
    // Output that is not JSON: the same.
    useFake(e, "ok");
    QVERIFY(e.c.start(e.id, {}, pending(), QStringLiteral("run")));
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("blocked"), 10000);
    QVERIFY(!logOf(e).contains(QStringLiteral("--force")));
}

void TstMigrationController::runOnTheSameDatabaseWaitsForConfirmationAndThenRuns()
{
    Env e;
    open(e);
    useFake(e, "target");
    QSignalSpy reports(&e.c, &MigrationController::reportsChanged);
    QVERIFY(e.c.start(e.id, {}, pending(), QStringLiteral("run")));
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("awaitingConfirm"), 10000);
    QCOMPARE(e.c.runOf(e.id, {}, pending()).value(QStringLiteral("message")).toString(),
             QStringLiteral("Waiting for confirmation"));
    // Not executed yet, however long it waits; only db:show ran.
    QTest::qWait(400);
    QCOMPARE(stateOf(e, pending()), QStringLiteral("awaitingConfirm"));
    QVERIFY(!logOf(e).contains(QStringLiteral("migrate")));
    QVERIFY(logOf(e).contains(QStringLiteral("db:show")));
    // The confirmation dialog's data.
    const QVariantMap info = e.c.confirmationOf(e.id, {}, pending());
    QCOMPARE(info.value(QStringLiteral("title")).toString(), QStringLiteral("Run migration"));
    QCOMPARE(info.value(QStringLiteral("confirmText")).toString(), QStringLiteral("Run"));
    QCOMPARE(info.value(QStringLiteral("name")).toString(), pending());
    QCOMPARE(info.value(QStringLiteral("connectionName")).toString(),
             QStringLiteral("Laravel app"));
    QVERIFY(!info.value(QStringLiteral("production")).toBool());
    QCOMPARE(QDir::fromNativeSeparators(info.value(QStringLiteral("database")).toString()),
             QDir::fromNativeSeparators(e.db));
    QCOMPARE(info.value(QStringLiteral("typeToConfirm")).toString(), QStringLiteral("app.db"));
    QVERIFY(info.value(QStringLiteral("phrase"))
                .toString()
                .startsWith(QStringLiteral("This runs php artisan from your project")));
    QVERIFY(info.value(QStringLiteral("rollbackNote")).toString().isEmpty());
    const QString command = info.value(QStringLiteral("command")).toString();
    QVERIFY2(command.contains(QStringLiteral("artisan migrate --path=database/migrations/")
                              + pending() + QStringLiteral(".php --force")),
             qPrintable(command));
    QVERIFY(!command.contains(QStringLiteral("--pretend")));
    QVERIFY(!info.value(QStringLiteral("workingDirectory")).toString().isEmpty());
    // While it waits, nothing else can start on the connection.
    QVERIFY(e.c.isBusy(e.id, {}));
    QVERIFY(!e.c.canRun(e.id, {}, second()).value(QStringLiteral("ok")).toBool());
    QVERIFY(e.c.canRun(e.id, {}, second())
                .value(QStringLiteral("reason"))
                .toString()
                .contains(QStringLiteral("in progress")));
    QVERIFY(!e.c.start(e.id, {}, second(), QStringLiteral("dryRun")));

    // The migration "applies" (the fake does not): then the report is read again on its own.
    QVERIFY(sql(e.db,
                "INSERT INTO migrations(migration, batch) VALUES "
                "('2024_02_01_000000_create_posts_table', 2)"));
    const qsizetype before = reports.size();
    e.c.confirm(e.id, {}, pending());
    QCOMPARE(stateOf(e, pending()), QStringLiteral("running"));
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("done"), 10000);
    const QStringList log = logOf(e);
    const qsizetype at = log.indexOf(QStringLiteral("migrate"));
    QVERIFY(at > 0);
    QVERIFY(log.contains(QStringLiteral("--force")));
    QVERIFY(log.contains(QStringLiteral("--no-interaction")));
    QVERIFY(!log.contains(QStringLiteral("--pretend")));
    QVERIFY(!log.contains(QStringLiteral("migrate:rollback")));
    QTRY_VERIFY_WITH_TIMEOUT(reports.size() > before, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(
        e.c.entryOf(e.id, {}, pending()).value(QStringLiteral("state")).toString(),
        QStringLiteral("applied"), 10000);
    QCOMPARE(e.c.entryOf(e.id, {}, pending()).value(QStringLiteral("batch")).toInt(), 2);
    QVERIFY(!e.c.isBusy(e.id, {}));
    QVERIFY(e.c.canRun(e.id, {}, second()).value(QStringLiteral("ok")).toBool());
}

void TstMigrationController::runAllPendingWaitsForConfirmationAndRunsFolderInOrder()
{
    Env e;
    open(e);
    useFake(e, "target");
    QCOMPARE(e.c.pendingOf(e.id, {}), 2);
    QSignalSpy reports(&e.c, &MigrationController::reportsChanged);
    QVERIFY(e.c.start(e.id, {}, pending(), QStringLiteral("runAll")));
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("awaitingConfirm"), 10000);
    QVERIFY(!logOf(e).contains(QStringLiteral("migrate")));
    QVERIFY(logOf(e).contains(QStringLiteral("db:show")));

    const QVariantMap info = e.c.confirmationOf(e.id, {}, pending());
    QCOMPARE(info.value(QStringLiteral("title")).toString(),
             QStringLiteral("Run all pending migrations"));
    QCOMPARE(info.value(QStringLiteral("confirmText")).toString(),
             QStringLiteral("Run all pending"));
    QCOMPARE(info.value(QStringLiteral("name")).toString(),
             QStringLiteral("All pending migrations (2)"));
    QVERIFY(info.value(QStringLiteral("rollbackNote")).toString().isEmpty());
    const QString command = info.value(QStringLiteral("command")).toString();
    QVERIFY2(command.contains(QStringLiteral("artisan migrate --path=database/migrations --force")),
             qPrintable(command));
    QVERIFY(!command.contains(QStringLiteral(".php")));
    QVERIFY(!command.contains(QStringLiteral("--pretend")));

    // Simulate both pending migrations getting applied, then confirm.
    QVERIFY(sql(e.db,
                "INSERT INTO migrations(migration, batch) VALUES "
                "('2024_02_01_000000_create_posts_table', 2),"
                "('2024_03_01_000000_create_tags_table', 2);"));
    const qsizetype before = reports.size();
    e.c.confirm(e.id, {}, pending());
    QCOMPARE(stateOf(e, pending()), QStringLiteral("running"));
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("done"), 10000);
    const QStringList log = logOf(e);
    QVERIFY(log.contains(QStringLiteral("migrate")));
    QVERIFY(log.contains(QStringLiteral("--path=database/migrations")));
    QVERIFY(log.contains(QStringLiteral("--force")));
    QVERIFY(!log.contains(QStringLiteral("--pretend")));
    QTRY_VERIFY_WITH_TIMEOUT(reports.size() > before, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(e.c.pendingOf(e.id, {}), 0, 10000);
    QVERIFY(!e.c.canRunAction(e.id, {}, pending(), QStringLiteral("runAll"))
                 .value(QStringLiteral("ok"))
                 .toBool());
}

void TstMigrationController::rollbackAsksWithItsOwnWords()
{
    Env e;
    open(e);
    useFake(e, "target");
    QVERIFY(e.c.start(e.id, {}, applied(), QStringLiteral("rollback")));
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, applied()), QStringLiteral("awaitingConfirm"), 10000);
    const QVariantMap info = e.c.confirmationOf(e.id, {}, applied());
    QCOMPARE(info.value(QStringLiteral("title")).toString(), QStringLiteral("Roll back migration"));
    QCOMPARE(info.value(QStringLiteral("confirmText")).toString(), QStringLiteral("Roll back"));
    QCOMPARE(info.value(QStringLiteral("rollbackNote")).toString(),
             QStringLiteral("Laravel only rolls back migrations of the latest batch; if this one "
                            "is not in it, nothing happens."));
    QVERIFY(info.value(QStringLiteral("command"))
                .toString()
                .contains(QStringLiteral("migrate:rollback --path=")));
    e.c.confirm(e.id, {}, applied());
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, applied()), QStringLiteral("done"), 10000);
    QVERIFY(logOf(e).contains(QStringLiteral("migrate:rollback")));
}

void TstMigrationController::decliningRunsNothing()
{
    Env e;
    open(e);
    useFake(e, "target");
    QVERIFY(e.c.start(e.id, {}, pending(), QStringLiteral("run")));
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("awaitingConfirm"), 10000);
    e.c.decline(e.id, {}, pending());
    QCOMPARE(stateOf(e, pending()), QStringLiteral("idle"));
    QVERIFY(!e.c.isBusy(e.id, {}));
    e.c.confirm(e.id, {}, pending()); // late: nothing to confirm any more
    QTest::qWait(300);
    QCOMPARE(stateOf(e, pending()), QStringLiteral("idle"));
    QVERIFY(!logOf(e).contains(QStringLiteral("migrate")));
    // Cancel at the question is the same as declining.
    QVERIFY(e.c.start(e.id, {}, pending(), QStringLiteral("run")));
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("awaitingConfirm"), 10000);
    e.c.cancel(e.id, {}, pending());
    QCOMPARE(stateOf(e, pending()), QStringLiteral("idle"));
    QVERIFY(!logOf(e).contains(QStringLiteral("migrate")));
}

void TstMigrationController::confirmWithoutAQuestionDoesNothing()
{
    Env e;
    open(e);
    useFake(e, "target");
    e.c.confirm(e.id, {}, pending()); // never asked
    QTest::qWait(200);
    QCOMPARE(stateOf(e, pending()), QStringLiteral("idle"));
    // A dry run is never "confirmed" into a run.
    QVERIFY(e.c.start(e.id, {}, pending(), QStringLiteral("dryRun")));
    e.c.confirm(e.id, {}, pending());
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("done"), 10000);
    QVERIFY(logOf(e).contains(QStringLiteral("--pretend"))); // it stayed a dry run
    QVERIFY(!logOf(e).contains(QStringLiteral("migrate:rollback")));
    // An unknown action starts nothing.
    QVERIFY(!e.c.start(e.id, {}, pending(), QStringLiteral("drop")));
}

void TstMigrationController::aProductionConnectionIsMarkedInTheConfirmation()
{
    Env e;
    open(e, true);
    useFake(e, "target");
    QVERIFY(e.c.start(e.id, {}, pending(), QStringLiteral("run")));
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("awaitingConfirm"), 10000);
    const QVariantMap info = e.c.confirmationOf(e.id, {}, pending());
    QVERIFY(info.value(QStringLiteral("production")).toBool());
    QCOMPARE(info.value(QStringLiteral("typeToConfirm")).toString(), QStringLiteral("app.db"));
    e.c.decline(e.id, {}, pending());
}

void TstMigrationController::cancellingWhileItRunsKillsTheProcess()
{
    Env e;
    open(e);
    const QString ticks = e.root + QStringLiteral("/ticks");
    useFake(e, "target");
    QVERIFY(e.c.start(e.id, {}, pending(), QStringLiteral("dryRun")));
    // db:show is on its way with the "target" mode; the migrate itself will be slow.
    qputenv("FAKE_PHP_MODE", "slow");
    qputenv("FAKE_PHP_TICKS", ticks.toUtf8());
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("running"), 10000);
    QVERIFY(e.c.isRunning(e.id, {}, pending()));
    QVERIFY(e.c.anyRunning());
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(ticks).size() >= 1, 10000);
    e.c.cancel(e.id, {}, pending());
    QCOMPARE(stateOf(e, pending()), QStringLiteral("failed"));
    QVERIFY(e.c.runOf(e.id, {}, pending()).value(QStringLiteral("cancelled")).toBool());
    QCOMPARE(e.c.runOf(e.id, {}, pending()).value(QStringLiteral("message")).toString(),
             QStringLiteral("Cancelled"));
    QVERIFY(!e.c.isBusy(e.id, {}));
    QVERIFY(!e.c.anyRunning());
    const qint64 size = QFileInfo(ticks).size();
    QTest::qWait(1500);
    QCOMPARE(QFileInfo(ticks).size(), size); // dead: it does not tick any more
    QVERIFY(kindsOf(e.messages).contains(QStringLiteral("cancelled")));
}

void TstMigrationController::cancellingARunWarnsThatTheDatabaseMayBeHalfMigrated()
{
    Env e;
    open(e);
    useFake(e, "target");
    QVERIFY(e.c.start(e.id, {}, pending(), QStringLiteral("run")));
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("awaitingConfirm"), 10000);
    qputenv("FAKE_PHP_MODE", "slow");
    e.c.confirm(e.id, {}, pending());
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("running"), 10000);
    QSignalSpy reports(&e.c, &MigrationController::reportsChanged);
    e.c.cancel(e.id, {}, pending());
    QCOMPARE(stateOf(e, pending()), QStringLiteral("failed"));
    const QVariantMap r = e.c.runOf(e.id, {}, pending());
    QVERIFY(r.value(QStringLiteral("cancelled")).toBool());
    QVERIFY(r.value(QStringLiteral("message"))
                .toString()
                .contains(QStringLiteral("The database may be left half migrated.")));
    // It may have changed something: the report is read again.
    QTRY_VERIFY_WITH_TIMEOUT(!reports.isEmpty(), 10000);
}

void TstMigrationController::cancellingTheCheckStopsIt()
{
    Env e;
    open(e);
    useFake(e, "slow"); // db:show itself is slow
    QVERIFY(e.c.start(e.id, {}, pending(), QStringLiteral("run")));
    QCOMPARE(stateOf(e, pending()), QStringLiteral("checking"));
    QVERIFY(e.c.isRunning(e.id, {}, pending()));
    e.c.cancel(e.id, {}, pending());
    QCOMPARE(stateOf(e, pending()), QStringLiteral("failed"));
    QCOMPARE(e.c.runOf(e.id, {}, pending()).value(QStringLiteral("message")).toString(),
             QStringLiteral("Cancelled"));
    QVERIFY(!logOf(e).contains(QStringLiteral("migrate")));
    QVERIFY(!e.c.isBusy(e.id, {}));
}

void TstMigrationController::aFailedMigrationShowsTheErrorAndRefreshes()
{
    Env e;
    open(e);
    useFake(e, "target");
    QVERIFY(e.c.start(e.id, {}, pending(), QStringLiteral("run")));
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("awaitingConfirm"), 10000);
    qputenv("FAKE_PHP_MODE", "fail");
    QSignalSpy reports(&e.c, &MigrationController::reportsChanged);
    e.c.confirm(e.id, {}, pending());
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("failed"), 10000);
    const QVariantMap r = e.c.runOf(e.id, {}, pending());
    QCOMPARE(r.value(QStringLiteral("exitCode")).toInt(), 3);
    QCOMPARE(r.value(QStringLiteral("message")).toString(),
             QStringLiteral("Failed with exit code 3"));
    QVERIFY(
        r.value(QStringLiteral("detail")).toString().contains(QStringLiteral("Migration failed")));
    QVERIFY(!r.value(QStringLiteral("cancelled")).toBool());
    const QStringList kinds = kindsOf(e.messages);
    QVERIFY(kinds.contains(QStringLiteral("stderr")));
    QCOMPARE(kinds.last(), QStringLiteral("error"));
    // Whatever it did is read again, with an error as with a success.
    QTRY_VERIFY_WITH_TIMEOUT(!reports.isEmpty(), 10000);
    QVERIFY(!e.c.isBusy(e.id, {}));
}

void TstMigrationController::theOutputKeepsTheFirstFiveThousandLines()
{
    Env e;
    open(e);
    useFake(e, "big"); // db:show prints junk (not verifiable: a dry run goes on), then 16 000 lines
    QVERIFY(e.c.start(e.id, {}, pending(), QStringLiteral("dryRun")));
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("done"), 20000);
    const QVariantMap r = e.c.runOf(e.id, {}, pending());
    QCOMPARE(r.value(QStringLiteral("lines")).toInt(), MigrationController::kMaxOutputLines);
    QVERIFY(r.value(QStringLiteral("truncated")).toBool());
    int outputs = 0;
    int notices = 0;
    for (const QString &kind : kindsOf(e.messages)) {
        outputs += kind == QLatin1String("output") ? 1 : 0;
        notices += kind == QLatin1String("notice") ? 1 : 0;
    }
    QCOMPARE(outputs, MigrationController::kMaxOutputLines);
    QVERIFY(notices >= 2); // the warning, the command and the cut notice
}

void TstMigrationController::leavingATabForgetsItsRunUnlessItRuns()
{
    Env e;
    open(e);
    useFake(e, "target");
    QVERIFY(e.c.start(e.id, {}, pending(), QStringLiteral("dryRun")));
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("done"), 10000);
    e.c.releaseSource(e.id, {}, pending());
    QCOMPARE(stateOf(e, pending()), QStringLiteral("idle"));
    // A run that goes on is kept (the process is still there).
    qputenv("FAKE_PHP_TICKS", (e.root + QStringLiteral("/ticks")).toUtf8());
    QVERIFY(e.c.start(e.id, {}, pending(), QStringLiteral("dryRun")));
    qputenv("FAKE_PHP_MODE", "slow");
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(e, pending()), QStringLiteral("running"), 10000);
    e.c.releaseSource(e.id, {}, pending());
    QCOMPARE(stateOf(e, pending()), QStringLiteral("running"));
    e.c.cancelAll();
    QVERIFY(!e.c.anyRunning());
}

QTEST_GUILESS_MAIN(TstMigrationController)
#include "tst_migration_controller.moc"
