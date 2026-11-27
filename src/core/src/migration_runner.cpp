#include <quarry/migration_runner.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <utility>

#ifdef Q_OS_WIN
#include <Windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace quarry {

namespace {

constexpr qsizetype kMaxDetailChars = 500;

QString tr(const char *text)
{
    return QCoreApplication::translate("quarry::MigrationRunner", text);
}

Error makeError(ErrorKind kind, QString message, QString detail = {})
{
    return { kind, std::move(message), std::move(detail), {}, {} };
}

// The project root of the settings: the explicit one or two levels above the migrations folder.
// Empty when there is neither.
QString projectRootOf(const MigrationSettings &settings)
{
    if (!settings.projectRoot.trimmed().isEmpty())
        return QDir::cleanPath(QFileInfo(settings.projectRoot.trimmed()).absoluteFilePath());
    if (settings.folder.trimmed().isEmpty())
        return {};
    return QDir::cleanPath(QFileInfo(settings.folder.trimmed()).absoluteFilePath()
                           + QStringLiteral("/../.."));
}

// Test seam: QUARRY_RUNNER_TIMEOUT_MS can only shorten the limit, never extend it.
int timeoutMs()
{
    constexpr int kDefault = MigrationRunner::kTimeoutSeconds * 1000;
    bool ok = false;
    const int value = qEnvironmentVariableIntValue("QUARRY_RUNNER_TIMEOUT_MS", &ok);
    return ok && value > 0 ? std::min(value, kDefault) : kDefault;
}

QString lastLine(const QByteArray &bytes)
{
    const QList<QByteArray> lines = bytes.split('\n');
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QString line = QString::fromUtf8(*it).trimmed();
        if (!line.isEmpty())
            return line.left(kMaxDetailChars);
    }
    return {};
}

QString firstString(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        return array.isEmpty() ? QString() : array.at(0).toString();
    }
    return {};
}

int portOf(const QJsonValue &value)
{
    if (value.isDouble())
        return value.toInt();
    if (value.isString())
        return value.toString().trimmed().toInt();
    return 0;
}

Error unverifiable(const QByteArray &out, const QByteArray &err)
{
    QString detail = lastLine(err);
    if (detail.isEmpty())
        detail = lastLine(out);
    return makeError(ErrorKind::Unsupported, tr("The database artisan uses could not be verified."),
                     detail);
}

Result<ArtisanTarget> parseTarget(const QByteArray &out, const QByteArray &err)
{
    const qsizetype open = out.indexOf('{');
    const qsizetype close = out.lastIndexOf('}');
    if (open < 0 || close < open)
        return fail(unverifiable(out, err));
    const QJsonDocument doc = QJsonDocument::fromJson(out.mid(open, close - open + 1));
    if (!doc.isObject())
        return fail(unverifiable(out, err));
    const QJsonObject root = doc.object();
    const QJsonObject platform = root.value(QLatin1String("platform")).toObject();
    const QJsonObject config = platform.value(QLatin1String("config")).toObject();

    ArtisanTarget target;
    target.driver = config.value(QLatin1String("driver")).toString();
    if (target.driver.isEmpty())
        return fail(unverifiable(out, err));
    target.host = firstString(config.value(QLatin1String("host")));
    if (target.host.isEmpty())
        target.host = firstString(
            config.value(QLatin1String("write")).toObject().value(QLatin1String("host")));
    if (target.host.isEmpty())
        target.host = firstString(
            config.value(QLatin1String("read")).toObject().value(QLatin1String("host")));
    target.port = portOf(config.value(QLatin1String("port")));
    target.database = config.value(QLatin1String("database")).toString();
    for (const QJsonValue &name :
         { config.value(QLatin1String("name")), platform.value(QLatin1String("name")),
           root.value(QLatin1String("name")) }) {
        if (name.isString() && !name.toString().isEmpty()) {
            target.connectionName = name.toString();
            break;
        }
    }
    return target;
}

// sameDatabase helpers.
QString providerFor(const QString &driver)
{
    const QString d = driver.trimmed().toLower();
    if (d == QLatin1String("mysql") || d == QLatin1String("mariadb"))
        return QStringLiteral("mysql");
    if (d == QLatin1String("pgsql"))
        return QStringLiteral("postgres");
    if (d == QLatin1String("sqlsrv"))
        return QStringLiteral("mssql");
    if (d == QLatin1String("sqlite"))
        return QStringLiteral("sqlite");
    return {};
}

QString normalizedHost(const QString &host)
{
    QString h = host.trimmed().toLower();
    if (h.isEmpty() || h == QLatin1String("localhost") || h == QLatin1String("127.0.0.1")
        || h == QLatin1String("::1") || h == QLatin1String("[::1]"))
        return QStringLiteral("localhost");
    return h;
}

int effectivePort(int port, const QString &providerId)
{
    if (port > 0)
        return port;
    if (providerId == QLatin1String("mysql"))
        return 3306;
    if (providerId == QLatin1String("postgres"))
        return 5432;
    if (providerId == QLatin1String("mssql"))
        return 1433;
    return 0;
}

QString comparablePath(const QString &path)
{
    const QFileInfo info(path);
    QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty())
        canonical = QDir::cleanPath(info.absoluteFilePath());
#ifdef Q_OS_WIN
    canonical = canonical.toLower();
#endif
    return canonical;
}

} // namespace

struct MigrationRunner::Run
{
    quint64 id = 0;
    QProcess *process = nullptr;
    QTimer *timer = nullptr;
    bool isTarget = false;
    qint64 total = 0; // bytes taken from both channels
    bool truncated = false;
    QByteArray out; // execute: partial line; target: the whole output
    QByteArray err;
#ifdef Q_OS_WIN
    HANDLE job = nullptr; // kills the whole tree; closing it kills what is left
    Run() = default;
    Run(const Run &) = delete;
    Run &operator=(const Run &) = delete;
    ~Run()
    {
        if (job != nullptr)
            CloseHandle(job);
    }
#endif
};

MigrationRunner::MigrationRunner(QObject *parent)
    : QObject(parent)
{ }

MigrationRunner::~MigrationRunner()
{
    for (auto &entry : m_runs) {
        entry.second->process->disconnect(this);
        killTree(*entry.second);
        entry.second->process->waitForFinished(1000);
    }
}

Result<RunnerEnvironment> MigrationRunner::resolve(const MigrationSettings &settings)
{
    RunnerEnvironment env;
    const QString configured = settings.phpPath.trimmed();
    if (!configured.isEmpty() && QFileInfo(configured).isAbsolute()) {
        const QFileInfo info(configured);
        if (info.isFile() && info.isExecutable())
            env.php = QDir::cleanPath(info.absoluteFilePath());
    } else {
        env.php = QStandardPaths::findExecutable(configured.isEmpty() ? QStringLiteral("php")
                                                                      : configured);
    }
    if (env.php.isEmpty()) {
        return fail(makeError(ErrorKind::NotFound,
                              tr("PHP was not found. Set its path in the connection's migrations "
                                 "settings."),
                              configured.isEmpty() ? QStringLiteral("php") : configured));
    }

    env.projectRoot = projectRootOf(settings);
    if (env.projectRoot.isEmpty()) {
        return fail(
            makeError(ErrorKind::Syntax, tr("Choose the migrations folder of the project first.")));
    }
    if (!QFileInfo(env.projectRoot + QStringLiteral("/artisan")).isFile()) {
        return fail(makeError(ErrorKind::NotFound,
                              tr("The project folder has no artisan file. Set the project root in "
                                 "the connection's migrations settings."),
                              env.projectRoot));
    }
    return env;
}

QStringList MigrationRunner::arguments(const MigrationSettings &settings, const QString &name,
                                       MigrationAction action)
{
    const IMigrationSource *source = findMigrationSource(settings.kind);
    if (source == nullptr)
        return {};
    const QString root = projectRootOf(settings);
    const QString targetPath = action == MigrationAction::RunAll ? settings.folder.trimmed()
                                                                 : source->filePath(settings, name);
    if (targetPath.isEmpty() || root.isEmpty())
        return {};

    const QString absolute = QDir::cleanPath(QFileInfo(targetPath).absoluteFilePath());
    const QString relative = QDir(root).relativeFilePath(absolute);
    const bool inside = !QDir::isAbsolutePath(relative) && relative != QLatin1String("..")
        && !relative.startsWith(QLatin1String("../"));

    QStringList args { QStringLiteral("artisan"),
                       action == MigrationAction::Rollback ? QStringLiteral("migrate:rollback")
                                                           : QStringLiteral("migrate"),
                       QStringLiteral("--path=") + (inside ? relative : absolute),
                       QStringLiteral("--force"),
                       QStringLiteral("--no-interaction"),
                       QStringLiteral("--no-ansi") };
    if (action == MigrationAction::DryRun)
        args << QStringLiteral("--pretend");
    if (!inside)
        args << QStringLiteral("--realpath");
    return args;
}

quint64 MigrationRunner::resolveTarget(const MigrationSettings &settings)
{
    return start(settings,
                 { QStringLiteral("artisan"), QStringLiteral("db:show"), QStringLiteral("--json"),
                   QStringLiteral("--no-interaction"), QStringLiteral("--no-ansi") },
                 true);
}

quint64 MigrationRunner::execute(const MigrationSettings &settings, const QString &name,
                                 MigrationAction action)
{
    const QStringList args = arguments(settings, name, action);
    if (args.isEmpty()) {
        emit failed(0,
                    makeError(ErrorKind::Syntax, tr("That is not a valid migration name."), name));
        return 0;
    }
    return start(settings, args, false);
}

quint64 MigrationRunner::start(const MigrationSettings &settings, const QStringList &args,
                               bool isTarget)
{
    const Result<RunnerEnvironment> env = resolve(settings);
    if (!env) {
        emit failed(0, env.error());
        return 0;
    }

    const quint64 id = ++m_lastId;
    auto run = std::make_unique<Run>();
    run->id = id;
    run->isTarget = isTarget;
    auto *process = new QProcess(this);
    run->process = process;
    run->timer = new QTimer(process);
    run->timer->setSingleShot(true);
    run->timer->setInterval(timeoutMs());

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    environment.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    process->setProcessEnvironment(environment);
    process->setProgram(env->php);
    process->setArguments(args);
    process->setWorkingDirectory(env->projectRoot);
#ifndef Q_OS_WIN
    // Own process group, so the whole tree can be killed.
    process->setChildProcessModifier([] { ::setsid(); });
#endif

    Run *raw = run.get();
    connect(process, &QProcess::started, this, [this, id, raw] {
#ifdef Q_OS_WIN
        // shortcut: a child started before the job is assigned escapes the kill; php does not
        // spawn within microseconds of starting. Mejorar con CREATE_SUSPENDED si se ve.
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job != nullptr) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info {};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
            HANDLE handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                                        static_cast<DWORD>(raw->process->processId()));
            if (handle != nullptr) {
                if (AssignProcessToJobObject(job, handle))
                    raw->job = job;
                CloseHandle(handle);
            }
            if (raw->job == nullptr)
                CloseHandle(job);
        }
#endif
        if (!raw->isTarget)
            emit started(id);
    });
    connect(process, &QProcess::readyReadStandardOutput, this, [this, raw] { drain(*raw); });
    connect(process, &QProcess::readyReadStandardError, this, [this, raw] { drain(*raw); });
    connect(process, &QProcess::finished, this,
            [this, id](int code, QProcess::ExitStatus status) { onFinished(id, code, status); });
    // Queued: a start failure may be reported inside start(), before the caller knows the id.
    connect(
        process, &QProcess::errorOccurred, this,
        [this, id](QProcess::ProcessError error) { onError(id, error); }, Qt::QueuedConnection);
    connect(run->timer, &QTimer::timeout, this, [this, id] {
        const auto it = m_runs.find(id);
        if (it == m_runs.end())
            return;
        killTree(*it->second);
        finish(id);
        emit failed(id,
                    makeError(ErrorKind::Timeout,
                              tr("The migration took too long and was stopped."),
                              tr("Time limit: %1 s").arg(timeoutMs() / 1000.0)));
    });

    m_runs.emplace(id, std::move(run));
    process->start(QIODevice::ReadOnly);
    raw->timer->start();
    return id;
}

void MigrationRunner::drain(Run &run)
{
    const quint64 id = run.id;
    QList<std::pair<QString, bool>> lines;
    const auto take = [&](QByteArray data, QByteArray &buffer, bool isError) {
        if (run.truncated)
            return;
        bool cut = false;
        const qint64 room = std::max<qint64>(0, kMaxOutputBytes - run.total);
        if (data.size() > room) {
            data.truncate(room);
            cut = true;
        }
        run.total += data.size();
        buffer += data;
        if (run.isTarget) {
            run.truncated = cut;
            return;
        }
        qsizetype newline = 0;
        while ((newline = buffer.indexOf('\n')) >= 0) {
            QByteArray line = buffer.left(newline);
            buffer.remove(0, newline + 1);
            if (line.endsWith('\r'))
                line.chop(1);
            lines.append({ QString::fromUtf8(line), isError });
        }
        if (cut) {
            if (!buffer.isEmpty())
                lines.append({ QString::fromUtf8(buffer), isError });
            buffer.clear();
            lines.append({ tr("[output truncated]"), true });
            run.truncated = true;
        }
    };
    take(run.process->readAllStandardOutput(), run.out, false);
    take(run.process->readAllStandardError(), run.err, true);
    // `run` may be gone after a slot cancels it: only `id` from here on.
    for (const auto &[text, isError] : std::as_const(lines)) {
        if (!isRunning(id))
            return;
        emit output(id, text, isError);
    }
}

void MigrationRunner::killTree(Run &run)
{
#ifdef Q_OS_WIN
    if (run.job != nullptr)
        TerminateJobObject(run.job, 1);
#else
    const qint64 pid = run.process->processId();
    if (pid > 0)
        ::kill(static_cast<pid_t>(-pid), SIGKILL);
#endif
    run.process->kill();
}

void MigrationRunner::finish(quint64 id)
{
    const auto it = m_runs.find(id);
    if (it == m_runs.end())
        return;
    std::unique_ptr<Run> run = std::move(it->second);
    m_runs.erase(it);
    run->timer->stop();
    run->process->disconnect(this);
    // Not deleted here: this may run inside one of the process's own signals. A process that was
    // just killed has not reported its death yet: wait for it so QProcess does not warn.
    if (run->process->state() == QProcess::NotRunning)
        run->process->deleteLater();
    else
        connect(run->process, &QProcess::finished, run->process, &QObject::deleteLater);
}

void MigrationRunner::onFinished(quint64 id, int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status)
    const auto it = m_runs.find(id);
    if (it == m_runs.end())
        return;
    Run &run = *it->second;
    drain(run);
    if (!isRunning(id))
        return;

    if (run.isTarget) {
        const QByteArray out = run.out;
        const QByteArray err = run.err;
        finish(id);
        if (exitCode != 0) {
            emit failed(id, unverifiable(out, err));
            return;
        }
        const Result<ArtisanTarget> target = parseTarget(out, err);
        if (target)
            emit targetResolved(id, *target);
        else
            emit failed(id, target.error());
        return;
    }

    // The end of the output may not end in a newline.
    QList<std::pair<QString, bool>> rest;
    if (!run.out.isEmpty())
        rest.append({ QString::fromUtf8(run.out), false });
    if (!run.err.isEmpty())
        rest.append({ QString::fromUtf8(run.err), true });
    for (const auto &[text, isError] : std::as_const(rest)) {
        emit output(id, text, isError);
        if (!isRunning(id))
            return;
    }
    finish(id);
    emit finished(id, exitCode);
}

void MigrationRunner::onError(quint64 id, QProcess::ProcessError error)
{
    const auto it = m_runs.find(id);
    if (error != QProcess::FailedToStart || it == m_runs.end())
        return;
    const QString detail = it->second->process->errorString();
    finish(id);
    emit failed(id, makeError(ErrorKind::NotFound, tr("PHP could not be started."), detail));
}

void MigrationRunner::cancel(quint64 id)
{
    const auto it = m_runs.find(id);
    if (it == m_runs.end())
        return;
    killTree(*it->second);
    finish(id);
    emit cancelled(id);
}

bool sameDatabase(const ArtisanTarget &artisan, const ConnectionTarget &connection, QString *why)
{
    const auto reject = [&](QString text) {
        if (why != nullptr)
            *why = std::move(text);
        return false;
    };
    if (connection.providerId == QLatin1String("odbc")) {
        return reject(tr("An ODBC connection cannot be matched with the project's database."));
    }
    const QString engine = providerFor(artisan.driver);
    if (engine.isEmpty()) {
        return reject(
            tr("The project's database driver '%1' is not supported.").arg(artisan.driver));
    }
    if (engine != connection.providerId) {
        return reject(tr("artisan uses the '%1' driver but this connection is of type '%2'.")
                          .arg(artisan.driver, connection.providerId));
    }

    if (engine == QLatin1String("sqlite")) {
        if (comparablePath(artisan.database) == comparablePath(connection.database))
            return true;
        return reject(tr("artisan uses the SQLite file '%1' but this connection uses '%2'.")
                          .arg(artisan.database, connection.database));
    }

    if (normalizedHost(artisan.host) != normalizedHost(connection.host)) {
        return reject(
            tr("artisan uses database '%1' on host '%2' but this connection uses '%3' "
               "on '%4'.")
                .arg(artisan.database, artisan.host, connection.database, connection.host));
    }
    const int artisanPort = effectivePort(artisan.port, engine);
    const int connectionPort = effectivePort(connection.port, engine);
    if (artisanPort != connectionPort) {
        return reject(tr("artisan uses port %1 on host '%2' but this connection uses port %3.")
                          .arg(artisanPort)
                          .arg(artisan.host)
                          .arg(connectionPort));
    }
    if (artisan.database != connection.database) {
        return reject(
            tr("artisan uses database '%1' on host '%2' but this connection uses '%3' "
               "on '%4'.")
                .arg(artisan.database, artisan.host, connection.database, connection.host));
    }
    return true;
}

} // namespace quarry
