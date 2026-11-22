#pragma once

#include <quarry/migration_source.h>
#include <quarry/result.h>
// Declares the Q_DECLARE_METATYPE for quarry::Error used by the signals.
#include <quarry/session.h>

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace quarry {

enum class MigrationAction : uint8_t {
    Run, // migrate --path=<file>: applies that migration. Changes the database.
    Rollback, // migrate:rollback --path=<file>: reverts it if it is in the last batch. Changes it.
    DryRun, // migrate --path=<file> --pretend: prints the SQL, changes nothing.
};

// The database `php artisan` would use, read from `php artisan db:show --json` (read-only).
struct ArtisanTarget
{
    QString driver; // Laravel's name: mysql, mariadb, pgsql, sqlsrv, sqlite
    QString host;
    QString database; // a file path for sqlite
    QString connectionName; // Laravel's connection name ("mysql", "default"...), for messages
    int port = 0; // 0 = not given
    friend bool operator==(const ArtisanTarget &, const ArtisanTarget &) = default;
};

// The database Quarry's connection points at.
struct ConnectionTarget
{
    QString providerId; // "sqlite", "postgres", "mysql", "mssql", "odbc"
    QString host;
    QString database; // the file path for sqlite
    int port = 0; // 0 = the engine's default
};

// Whether artisan would touch the same database as the Quarry connection (ARCHITECTURE §6.12).
// Driver and engine must correspond (mysql/mariadb = "mysql", pgsql = "postgres", sqlsrv =
// "mssql", sqlite = "sqlite"; an "odbc" connection never matches: it cannot be told). Host:
// equal ignoring case, with localhost, 127.0.0.1 and ::1 as one; a missing port is the engine's
// default (3306, 5432, 1433). Database: equal (case-sensitive; for sqlite, the same canonical
// path). When it does not match, `why` (if given) says which part differs, in plain translated
// language with both values.
bool sameDatabase(const ArtisanTarget &artisan, const ConnectionTarget &connection,
                  QString *why = nullptr);

// The programs a migration action needs, resolved from the settings (no process started).
struct RunnerEnvironment
{
    QString php; // absolute path of the php executable
    QString projectRoot; // absolute path of the project (it holds `artisan`)
};

// Runs `php artisan` for the migrations integration (H12 phase C, ARCHITECTURE §6.12). It runs
// project code the user wrote, so it is the only place that does, and it keeps to these rules:
// QProcess with an argument list and no shell; working directory = the project root; the
// environment is the app's own plus NO_COLOR=1 and TERM=dumb, never a password or anything of the
// connection; stdout and stderr are streamed (at most kMaxOutputBytes, then it says it was cut);
// a time limit of kTimeoutSeconds after which the process tree is killed; nothing blocks the UI
// thread (QProcess is asynchronous: the object lives on the UI thread). A migration file given
// by name goes through IMigrationSource::filePath, so a name can never reach another path.
class MigrationRunner : public QObject
{
    Q_OBJECT

public:
    explicit MigrationRunner(QObject *parent = nullptr);
    // Kills any process still running.
    ~MigrationRunner() override;

    // Finds php (MigrationSettings::phpPath, or "php" in the PATH when empty) and the project
    // root (MigrationSettings::projectRoot, or two levels above the folder when empty: the
    // folder of a Laravel project is <root>/database/migrations) and checks that `artisan` is a
    // file there. Errors say what is missing, in plain translated language. Touches the disk,
    // starts nothing. Cheap enough for the UI thread.
    static Result<RunnerEnvironment> resolve(const MigrationSettings &settings);

    // The arguments after the program for an action, e.g. {"artisan", "migrate",
    // "--path=database/migrations/2024_01_01_000000_x.php", "--force", "--no-interaction",
    // "--no-ansi"}; Rollback uses "migrate:rollback", DryRun adds "--pretend". The path is
    // relative to the project root when the file is under it and, if not, an absolute one with
    // "--realpath". Empty when the name is not a valid migration name. For tests and for
    // showing the command to the user.
    static QStringList arguments(const MigrationSettings &settings, const QString &name,
                                 MigrationAction action);

    // `php artisan db:show --json --no-interaction --no-ansi` (read-only). The JSON is read from
    // `platform.config` (driver, host, port, database) and `name` (the connection name); a
    // missing command (Laravel older than 10) or output that cannot be read is an Error that says
    // the target could not be verified. -> targetResolved(id, target) | failed(id, Error).
    // Returns the request id (> 0), or 0 and a failed(0, Error) if resolve() fails.
    quint64 resolveTarget(const quarry::MigrationSettings &settings);

    // Runs one action on one migration. Run and Rollback must only be called after
    // sameDatabase() accepted resolveTarget()'s result: the runner does not check it itself.
    // -> started(id), output(id, text, isError) as it arrives, then finished(id, exitCode) (any
    // exit code, 0 = ok) | failed(id, Error) (could not start, timed out, name not valid) |
    // cancelled(id). Returns the request id (> 0), or 0 plus failed(0, Error).
    quint64 execute(const quarry::MigrationSettings &settings, const QString &name,
                    quarry::MigrationAction action);

    // Kills the process tree of `id`. cancelled(id) follows; the database may be left half
    // migrated (the caller says so). No-op for an unknown id.
    void cancel(quint64 id);
    bool isRunning(quint64 id) const { return m_runs.count(id) != 0; }

    static constexpr int kTimeoutSeconds = 300;
    static constexpr qint64 kMaxOutputBytes = qint64(1024) * 1024;

signals:
    void targetResolved(quint64 id, quarry::ArtisanTarget target);
    void started(quint64 id);
    void output(quint64 id, QString text, bool isError);
    void finished(quint64 id, int exitCode);
    void cancelled(quint64 id);
    void failed(quint64 id, quarry::Error error);

private:
    struct Run;
    quint64 start(const MigrationSettings &settings, const QStringList &args, bool isTarget);
    void onFinished(quint64 id, int exitCode, QProcess::ExitStatus status);
    void onError(quint64 id, QProcess::ProcessError error);
    void drain(Run &run);
    void killTree(Run &run);
    void finish(quint64 id);

    // std::unordered_map, not QHash: the values are move-only.
    std::unordered_map<quint64, std::unique_ptr<Run>> m_runs;
    quint64 m_lastId = 0;
};

} // namespace quarry

Q_DECLARE_METATYPE(quarry::ArtisanTarget)
Q_DECLARE_METATYPE(quarry::MigrationAction)
