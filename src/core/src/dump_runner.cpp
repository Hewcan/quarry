#include "sqlite_dump.h"

#include <quarry/dump_runner.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryFile>
#include <QThread>

#include <utility>

#ifdef Q_OS_WIN
#include <Windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace quarry {

namespace {

Error makeError(ErrorKind kind, QString message, QString detail = {})
{
    return Error { kind, std::move(message), std::move(detail), {}, {} };
}

bool looksLikeError(const QString &line)
{
    return line.contains(QLatin1String("error:"), Qt::CaseInsensitive)
        || line.contains(QLatin1String("fatal:"), Qt::CaseInsensitive);
}

} // namespace

struct DumpRunner::Run
{
    quint64 id = 0;
    QString tool; // "pg_dump"... for messages; empty for SQLite
    QProcess *process = nullptr;
    QString partialFile; // pg_dump writes here; renamed to finalFile on success
    QString finalFile;
    QByteArray out;
    QByteArray err;
    QStringList tail; // the last stderr lines, for the error detail
    qint64 lines = 0;
    QThread *thread = nullptr;
    std::shared_ptr<sqlite_dump::CancelToken> token;
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

DumpRunner::DumpRunner(QObject *parent)
    : QObject(parent)
{ }

DumpRunner::~DumpRunner()
{
    for (auto &entry : m_runs) {
        Run &run = *entry.second;
        if (run.process != nullptr) {
            run.process->disconnect(this);
#ifdef Q_OS_WIN
            if (run.job != nullptr)
                TerminateJobObject(run.job, 1);
#endif
            run.process->kill();
            run.process->waitForFinished(1000);
            if (!run.partialFile.isEmpty())
                QFile::remove(run.partialFile);
        }
        if (run.thread != nullptr) {
            run.token->cancel();
            run.thread->wait();
            delete run.thread;
        }
    }
}

quint64 DumpRunner::failLater(Error error)
{
    QMetaObject::invokeMethod(
        this, [this, error = std::move(error)] { emit failed(0, error); }, Qt::QueuedConnection);
    return 0;
}

quint64 DumpRunner::dump(const DumpTarget &target, const DumpOptions &options,
                         const DumpToolPaths &tools)
{
    if (!dumpSupported(target.providerId))
        return failLater(makeError(ErrorKind::Unsupported,
                                   tr("Dump and restore are not available for this engine yet."),
                                   target.providerId));
    if (options.outputPath.trimmed().isEmpty())
        return failLater(makeError(ErrorKind::NotFound, tr("Choose where to save the dump.")));
    if (target.providerId == QLatin1String("sqlite")) {
        if (options.format != DumpFormat::PlainSql)
            return failLater(
                makeError(ErrorKind::Unsupported, tr("SQLite dumps are always SQL scripts.")));
        return startSqlite(target, true, options, {});
    }
    if (target.sshTunnel)
        return failLater(makeError(ErrorKind::Unsupported,
                                   tr("Dump and restore do not work through an SSH tunnel yet.")));
    const Result<QString> program = resolveDumpTool(DumpTool::PgDump, tools);
    if (!program)
        return failLater(program.error());
    const QString partial = options.outputPath + QStringLiteral(".part");
    QFile::remove(partial);
    return startProcess(target, *program, pgDumpArguments(target, options, partial), partial,
                        options.outputPath);
}

quint64 DumpRunner::restore(const DumpTarget &target, const RestoreOptions &options,
                            const DumpToolPaths &tools)
{
    if (!dumpSupported(target.providerId))
        return failLater(makeError(ErrorKind::Unsupported,
                                   tr("Dump and restore are not available for this engine yet."),
                                   target.providerId));
    if (!QFileInfo(options.inputPath).isFile())
        return failLater(
            makeError(ErrorKind::NotFound, tr("The dump file does not exist."), options.inputPath));
    if (target.providerId == QLatin1String("sqlite"))
        return startSqlite(target, false, {}, options);
    if (target.sshTunnel)
        return failLater(makeError(ErrorKind::Unsupported,
                                   tr("Dump and restore do not work through an SSH tunnel yet.")));
    const bool archive = isPostgresCustomArchive(options.inputPath);
    const Result<QString> program
        = resolveDumpTool(archive ? DumpTool::PgRestore : DumpTool::Psql, tools);
    if (!program)
        return failLater(program.error());
    const QString input = QDir::toNativeSeparators(options.inputPath);
    return startProcess(
        target, *program,
        archive ? pgRestoreArguments(target, input) : psqlRestoreArguments(target, input), {}, {});
}

quint64 DumpRunner::startProcess(const DumpTarget &target, const QString &program,
                                 const QStringList &args, const QString &partialFile,
                                 const QString &finalFile)
{
    const quint64 id = ++m_lastId;
    auto run = std::make_unique<Run>();
    run->id = id;
    run->tool = QFileInfo(program).completeBaseName();
    run->partialFile = partialFile;
    run->finalFile = finalFile;

    auto *process = new QProcess(this);
    run->process = process;

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    // The password never goes on the command line nor in an environment variable that `ps` can
    // show: a pgpass file only this user can read, deleted with the process.
    environment.remove(QStringLiteral("PGPASSWORD"));
    environment.remove(QStringLiteral("PGPASSFILE"));
    environment.insert(QStringLiteral("PGCLIENTENCODING"), QStringLiteral("UTF8"));
    if (!target.password.isEmpty()) {
        auto *passFile = new QTemporaryFile(
            QDir::tempPath() + QStringLiteral("/quarry-pgpass-XXXXXX"), process);
        if (!passFile->open() || passFile->write(pgPassFileLine(target)) < 0
            || !passFile->flush()) {
            delete process;
            return failLater(makeError(ErrorKind::Permission,
                                       tr("A temporary file for the password could not be "
                                          "created."),
                                       passFile->errorString()));
        }
        passFile->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        passFile->close();
        environment.insert(QStringLiteral("PGPASSFILE"),
                           QDir::toNativeSeparators(passFile->fileName()));
    }
    process->setProcessEnvironment(environment);
    process->setProgram(program);
    process->setArguments(args);
    process->setWorkingDirectory(QDir::tempPath());
#ifndef Q_OS_WIN
    // Own process group, so the whole tree can be killed.
    process->setChildProcessModifier([] { ::setsid(); });
#endif

    Run *raw = run.get();
    connect(process, &QProcess::started, this, [this, id, raw] {
#ifdef Q_OS_WIN
        // shortcut: a child started before the job is assigned escapes the kill; the PostgreSQL
        // tools do not spawn children. Mejorar con CREATE_SUSPENDED si se ve.
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
#else
        Q_UNUSED(raw)
#endif
        emit started(id);
    });
    connect(process, &QProcess::readyReadStandardOutput, this, [this, raw] { drain(*raw, false); });
    connect(process, &QProcess::readyReadStandardError, this, [this, raw] { drain(*raw, false); });
    connect(process, &QProcess::finished, this, [this, id](int code, QProcess::ExitStatus status) {
        endProcess(id, code, status == QProcess::CrashExit);
    });
    // Queued: a start failure may be reported inside start(), before the caller knows the id.
    connect(
        process, &QProcess::errorOccurred, this,
        [this, id](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart || !isRunning(id))
                return;
            const QString tool = m_runs.at(id)->tool;
            const QString detail = m_runs.at(id)->process->errorString();
            finish(id);
            emit failed(
                id,
                makeError(ErrorKind::NotFound, tr("%1 could not be started.").arg(tool), detail));
        },
        Qt::QueuedConnection);

    m_runs.emplace(id, std::move(run));
    process->start(QIODevice::ReadOnly);
    return id;
}

quint64 DumpRunner::startSqlite(const DumpTarget &target, bool isDump, const DumpOptions &dump,
                                const RestoreOptions &restore)
{
    const quint64 id = ++m_lastId;
    auto run = std::make_unique<Run>();
    run->id = id;
    run->token = std::make_shared<sqlite_dump::CancelToken>();

    const std::shared_ptr<sqlite_dump::CancelToken> token = run->token;
    const QString file = target.database;
    QThread *thread = QThread::create([this, id, token, file, isDump, dump, restore] {
        sqlite_dump::Callbacks callbacks;
        callbacks.progress = [this, id](int done, int total, const QString &step) {
            QMetaObject::invokeMethod(
                this,
                [this, id, done, total, step] {
                    if (isRunning(id))
                        emit progress(id, done, total, step);
                },
                Qt::QueuedConnection);
        };
        callbacks.log = [this, id](const QString &text) {
            QMetaObject::invokeMethod(
                this, [this, id, text] { line(id, text, false); }, Qt::QueuedConnection);
        };
        const Result<void> result = isDump ? sqlite_dump::dump(file, dump, *token, callbacks)
                                           : sqlite_dump::restore(file, restore, *token, callbacks);
        QMetaObject::invokeMethod(
            this,
            [this, id, result] {
                if (!isRunning(id))
                    return;
                finish(id);
                if (result)
                    emit finished(id);
                else if (result.error().kind == ErrorKind::Cancelled)
                    emit cancelled(id);
                else
                    emit failed(id, result.error());
            },
            Qt::QueuedConnection);
    });
    run->thread = thread;
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(
        thread, &QThread::started, this,
        [this, id] {
            if (isRunning(id))
                emit started(id);
        },
        Qt::QueuedConnection);
    m_runs.emplace(id, std::move(run));
    thread->start();
    return id;
}

void DumpRunner::cancel(quint64 id)
{
    const auto it = m_runs.find(id);
    if (it == m_runs.end())
        return;
    Run &run = *it->second;
    if (run.thread != nullptr) {
        // The thread rolls back and then reports cancelled(id).
        run.token->cancel();
        return;
    }
#ifdef Q_OS_WIN
    if (run.job != nullptr)
        TerminateJobObject(run.job, 1);
#else
    if (const qint64 pid = run.process->processId(); pid > 0)
        ::kill(static_cast<pid_t>(-pid), SIGKILL);
#endif
    run.process->kill();
    finish(id);
    emit cancelled(id);
}

void DumpRunner::line(quint64 id, const QString &text, bool isError)
{
    const auto it = m_runs.find(id);
    if (it == m_runs.end())
        return;
    Run &run = *it->second;
    if (isError) {
        run.tail.append(text);
        if (run.tail.size() > 20)
            run.tail.removeFirst();
    }
    ++run.lines;
    if (run.lines < kMaxLogLines)
        emit output(id, text, isError);
    else if (run.lines == kMaxLogLines)
        emit output(id, tr("[log truncated]"), true);
}

void DumpRunner::drain(Run &run, bool flush)
{
    const quint64 id = run.id;
    QList<std::pair<QString, bool>> lines;
    const auto take = [&](QByteArray &buffer, bool fromStderr) {
        qsizetype newline = 0;
        while ((newline = buffer.indexOf('\n')) >= 0) {
            QByteArray text = buffer.left(newline);
            buffer.remove(0, newline + 1);
            if (text.endsWith('\r'))
                text.chop(1);
            const QString decoded = QString::fromUtf8(text);
            lines.append({ decoded, fromStderr && looksLikeError(decoded) });
        }
        if (flush && !buffer.isEmpty()) {
            const QString decoded = QString::fromUtf8(buffer);
            lines.append({ decoded, fromStderr && looksLikeError(decoded) });
            buffer.clear();
        }
    };
    run.out += run.process->readAllStandardOutput();
    run.err += run.process->readAllStandardError();
    take(run.out, false);
    take(run.err, true);
    for (const auto &[text, isError] : std::as_const(lines)) {
        if (!isRunning(id))
            return;
        line(id, text, isError);
        if (isRunning(id))
            emit progress(id, static_cast<int>(m_runs.at(id)->lines), 0, text);
    }
}

void DumpRunner::endProcess(quint64 id, int exitCode, bool crashed)
{
    const auto it = m_runs.find(id);
    if (it == m_runs.end())
        return;
    drain(*it->second, true);
    if (!isRunning(id))
        return;
    Run &run = *m_runs.at(id);
    const QString tool = run.tool;
    const QString detail = run.tail.join(QLatin1Char('\n'));
    const QString partial = run.partialFile;
    const QString target = run.finalFile;
    finish(id);

    if (!crashed && exitCode == 0) {
        if (!partial.isEmpty()) {
            QFile::remove(target);
            if (!QFile::rename(partial, target)) {
                QFile::remove(partial);
                emit failed(id,
                            makeError(ErrorKind::Permission,
                                      tr("The dump file could not be written."), target));
                return;
            }
        }
        emit finished(id);
        return;
    }
    if (!partial.isEmpty())
        QFile::remove(partial);
    ErrorKind kind = ErrorKind::Driver;
    QString message = crashed ? tr("%1 stopped unexpectedly.").arg(tool)
                              : tr("%1 failed with exit code %2.").arg(tool).arg(exitCode);
    if (detail.contains(QLatin1String("server version mismatch"))
        || detail.contains(QLatin1String("unsupported version"))) {
        kind = ErrorKind::Unsupported;
        message = tr("%1 is older than the server or the file. Install a newer PostgreSQL client "
                     "or set its folder in the dump settings.")
                      .arg(tool);
    } else if (detail.contains(QLatin1String("password authentication failed"))
               || detail.contains(QLatin1String("no password supplied"))) {
        kind = ErrorKind::Authentication;
        message = tr("The server rejected the user or the password.");
    } else if (detail.contains(QLatin1String("could not connect"))
               || detail.contains(QLatin1String("connection to server"))) {
        kind = ErrorKind::Connection;
        message = tr("%1 could not connect to the server.").arg(tool);
    }
    emit failed(id, makeError(kind, message, detail));
}

void DumpRunner::finish(quint64 id)
{
    const auto it = m_runs.find(id);
    if (it == m_runs.end())
        return;
    std::unique_ptr<Run> run = std::move(it->second);
    m_runs.erase(it);
    if (run->thread != nullptr) {
        // Deleted by its own finished() (connected in startSqlite).
        return;
    }
    QProcess *process = run->process;
    process->disconnect(this);
    const QString partial = run->partialFile;
    // Not deleted here: this may run inside one of the process's own signals. A killed process
    // has not reported its death yet: wait for it so QProcess does not warn.
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
    } else {
        connect(process, &QProcess::finished, process, [process, partial] {
            if (!partial.isEmpty())
                QFile::remove(partial);
            process->deleteLater();
        });
    }
}

} // namespace quarry
