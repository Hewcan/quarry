#include <quarry/ai/claude_runner.h>

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX // std::min and std::clamp below
#endif
#include <Windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace quarry::ai {

using namespace Qt::StringLiterals;

namespace {

constexpr qsizetype kMaxDetailChars = 500;
constexpr int kMinTimeoutSeconds = 10;
constexpr int kCheckTimeoutSeconds = 10;
constexpr int kLookupTimeoutSeconds = 5;
constexpr int kKillWaitMs = 2000;

// What a Run is for (the `kind` of start()).
enum RunKind : int { KindAsk = 0, KindCheck = 1, KindLookup = 2 };

// Test seam: QUARRY_AI_TIMEOUT_MS can only shorten the limit, never extend it.
int limitMs(int seconds)
{
    const int normal = seconds * 1000;
    bool ok = false;
    const int value = qEnvironmentVariableIntValue("QUARRY_AI_TIMEOUT_MS", &ok);
    return ok && value > 0 ? std::min(value, normal) : normal;
}

bool isUsable(const QString &path)
{
    const QFileInfo info(path);
    return info.isAbsolute() && info.isFile() && info.isExecutable();
}

QString cleanPath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString firstLine(const QByteArray &bytes)
{
    const QList<QByteArray> lines = bytes.split('\n');
    for (const QByteArray &raw : lines) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (!line.isEmpty())
            return line;
    }
    return {};
}

QString lastLine(const QByteArray &bytes)
{
    const QList<QByteArray> lines = bytes.split('\n');
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QString line = QString::fromUtf8(*it).trimmed();
        if (!line.isEmpty())
            return line;
    }
    return {};
}

// Qt's QDir::homePath() on Windows asks the system and ignores USERPROFILE, which is what the tests
// (and a user with a redirected profile) change.
QString homeDir()
{
#ifdef Q_OS_WIN
    const QString profile = qEnvironmentVariable("USERPROFILE");
    if (!profile.isEmpty())
        return QDir::fromNativeSeparators(profile);
#endif
    return QDir::homePath();
}

HostOs hostOs()
{
#if defined(Q_OS_WIN)
    return HostOs::Windows;
#elif defined(Q_OS_MACOS)
    return HostOs::MacOs;
#else
    return HostOs::Linux;
#endif
}

} // namespace

struct ClaudeRunner::Run
{
    quint64 id = 0;
    int kind = KindAsk;
    QString path;
    QByteArray stdinText;
    QByteArray out;
    QByteArray err;
    qint64 total = 0; // bytes taken from both channels
    QProcess *process = nullptr; // null when the run failed before starting
    QTimer *timer = nullptr;
    std::unique_ptr<QTemporaryDir> workDir; // removed with the Run, after the process is gone
    bool overflow = false;
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

ClaudeRunner::ClaudeRunner(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<quarry::ai::CliInfo>();
    qRegisterMetaType<quarry::ai::CliOutcome>();
}

ClaudeRunner::~ClaudeRunner()
{
    for (auto &entry : m_runs) {
        if (entry.second->process == nullptr)
            continue;
        entry.second->process->disconnect(this);
        killTree(*entry.second);
        entry.second->process->waitForFinished(1000);
    }
}

quint64 ClaudeRunner::locate(const QString &customPath, bool forget)
{
    if (forget) {
        m_haveLocated = false;
        m_located.clear();
    }

    QString found;
    const QString custom = customPath.trimmed();
    if (!custom.isEmpty() && isUsable(custom)) {
        found = cleanPath(custom);
    } else if (m_haveLocated) {
        found = m_located; // empty: the last search found nothing
    } else {
        const QStringList candidates
            = candidatePaths(hostOs(), homeDir(), qEnvironmentVariable("LOCALAPPDATA"),
                             qEnvironmentVariable("APPDATA"));
        for (const QString &candidate : candidates) {
            if (isUsable(candidate)) {
                found = cleanPath(candidate);
                break;
            }
        }
        if (found.isEmpty()) {
#ifndef Q_OS_WIN
            // One run of the login shell, only when nothing else exists: PATH changes made in the
            // user's shell profile are not in the environment of a desktop-launched app.
            const QString shell = qEnvironmentVariable("SHELL");
            if (QFileInfo(shell).isAbsolute() && QFileInfo(shell).isExecutable()) {
                return start(shell, loginShellLookupArguments(), {}, kLookupTimeoutSeconds,
                             KindLookup);
            }
#endif
            m_haveLocated = true;
            m_located.clear();
        } else {
            m_haveLocated = true;
            m_located = found;
        }
    }

    const quint64 id = ++m_lastId;
    // Never inside the call: the caller connects its slots after getting the id back.
    QTimer::singleShot(0, this, [this, id, found] {
        if (found.isEmpty())
            emit locateFailed(id, u"claude was not found in the usual install locations."_s);
        else
            emit located(id, found);
    });
    return id;
}

quint64 ClaudeRunner::check(const QString &path)
{
    return start(path, { u"--version"_s }, {}, kCheckTimeoutSeconds, KindCheck);
}

quint64 ClaudeRunner::ask(const QString &path, const QByteArray &stdinText, int timeoutSeconds)
{
    if (path.trimmed().isEmpty()) {
        emit finished(0, { AiErrorKind::Failed, {}, u"No path to the claude executable."_s });
        return 0;
    }
    if (stdinText.size() > kMaxStdinBytes) {
        emit finished(0,
                      { AiErrorKind::Failed,
                        {},
                        u"The request is larger than %1 bytes."_s.arg(kMaxStdinBytes) });
        return 0;
    }
    const int seconds = std::clamp(timeoutSeconds, kMinTimeoutSeconds, kMaxTimeoutSeconds);
    return start(path, claudeArguments(), stdinText, seconds, KindAsk);
}

quint64 ClaudeRunner::start(const QString &path, const QStringList &args,
                            const QByteArray &stdinText, int timeoutSeconds, int kind)
{
    const quint64 id = ++m_lastId;
    auto run = std::make_unique<Run>();
    run->id = id;
    run->kind = kind;
    run->path = path;
    run->stdinText = stdinText;
    Run *raw = run.get();

    if (kind == KindAsk) {
        run->workDir = std::make_unique<QTemporaryDir>(QDir::tempPath() + u"/quarry-ai-XXXXXX"_s);
        if (!run->workDir->isValid()) {
            const CliOutcome outcome { AiErrorKind::Failed,
                                       {},
                                       u"Could not create a temporary folder: "_s
                                           + run->workDir->errorString() };
            m_runs.emplace(id, std::move(run));
            QTimer::singleShot(0, this, [this, id, outcome] { finish(id, outcome); });
            return id;
        }
    }

    auto *process = new QProcess(this);
    run->process = process;
    run->timer = new QTimer(process);
    run->timer->setSingleShot(true);
    run->timer->setInterval(limitMs(timeoutSeconds));

    // Own copy of the environment: ANTHROPIC_API_KEY and the rest stay as the user has them.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(u"NO_COLOR"_s, u"1"_s);
    environment.insert(u"TERM"_s, u"dumb"_s);
    process->setProcessEnvironment(environment);
    process->setProgram(path);
    process->setArguments(args);
    if (run->workDir)
        process->setWorkingDirectory(run->workDir->path());
#ifndef Q_OS_WIN
    // Own process group, so the whole tree can be killed.
    process->setChildProcessModifier([] { ::setsid(); });
#endif

    connect(process, &QProcess::started, this, [this, id, raw] {
#ifdef Q_OS_WIN
        // shortcut: a child started before the job is assigned escapes the kill; claude does not
        // spawn within microseconds of starting. Use CREATE_SUSPENDED if it is ever seen.
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
        // The text goes in by stdin only, then stdin is closed so the CLI sees the end of it.
        if (!raw->stdinText.isEmpty())
            raw->process->write(raw->stdinText);
        raw->process->closeWriteChannel();
        if (raw->kind == KindAsk) {
            // Queued: on some platforms this slot runs inside start(), before the id is returned.
            QTimer::singleShot(0, this, [this, id] {
                if (isRunning(id))
                    emit started(id);
            });
        }
    });
    const auto onData = [this, raw] {
        drain(*raw);
        if (raw->overflow)
            killTree(*raw); // onFinished reports it
    };
    connect(process, &QProcess::readyReadStandardOutput, this, onData);
    connect(process, &QProcess::readyReadStandardError, this, onData);
    connect(process, &QProcess::finished, this,
            [this, id](int code, QProcess::ExitStatus status) { onFinished(id, code, status); });
    // Queued: a start failure may be reported inside start(), before the caller knows the id.
    connect(
        process, &QProcess::errorOccurred, this,
        [this, id](QProcess::ProcessError error) { onError(id, error); }, Qt::QueuedConnection);
    connect(run->timer, &QTimer::timeout, this, [this, id, timeoutSeconds] {
        const auto it = m_runs.find(id);
        if (it == m_runs.end())
            return;
        killTree(*it->second);
        finish(id,
               { AiErrorKind::Timeout,
                 {},
                 u"Time limit: %1 s"_s.arg(limitMs(timeoutSeconds) / 1000.0) });
    });

    m_runs.emplace(id, std::move(run));
    process->start(QIODevice::ReadWrite);
    raw->timer->start();
    return id;
}

void ClaudeRunner::drain(Run &run)
{
    const QByteArray out = run.process->readAllStandardOutput();
    const QByteArray err = run.process->readAllStandardError();
    if (run.overflow)
        return; // the process is being killed: what it still prints is dropped
    run.total += out.size() + err.size();
    if (run.total > kMaxOutputBytes) {
        run.overflow = true;
        run.out.clear();
        run.err.clear();
        return;
    }
    run.out += out;
    run.err += err;
}

void ClaudeRunner::killTree(Run &run)
{
    if (run.process == nullptr)
        return;
        // shortcut: tree kill duplicated from core/migration_runner.cpp because quarry_ai cannot
        // link core; unify when a third process runner appears.
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

void ClaudeRunner::finish(quint64 id, const CliOutcome &outcome)
{
    const auto it = m_runs.find(id);
    if (it == m_runs.end())
        return;
    std::unique_ptr<Run> run = std::move(it->second);
    m_runs.erase(it);
    const int kind = run->kind;
    const QString path = run->path;

    if (run->process != nullptr) {
        run->timer->stop();
        run->process->disconnect(this);
        // A killed process has not reported its death yet: wait for it, so that the temporary
        // folder (its working directory) can be removed and QProcess does not warn.
        if (run->process->state() != QProcess::NotRunning
            && !run->process->waitForFinished(kKillWaitMs)) {
            connect(run->process, &QProcess::finished, run->process, &QObject::deleteLater);
        } else {
            // Not deleted at once: this may run inside one of the process's own signals.
            run->process->deleteLater();
        }
    }
    run.reset(); // the temporary folder is gone before the signal

    switch (kind) {
    case KindAsk:
        emit finished(id, outcome);
        break;
    case KindCheck: {
        CliInfo info;
        info.path = path;
        if (outcome.error == AiErrorKind::None) {
            info.version = outcome.text;
            info.compatible = isCompatibleVersion(outcome.text);
            if (!info.compatible) {
                info.error = AiErrorKind::IncompatibleVersion;
                info.detail = outcome.text;
            }
        } else {
            // CliInfo only knows these three; a timeout or a cancel is a failure to read it.
            info.error = outcome.error == AiErrorKind::CliNotFound ? AiErrorKind::CliNotFound
                                                                   : AiErrorKind::Failed;
            info.detail = outcome.detail.left(kMaxDetailChars);
        }
        emit checked(id, info);
        break;
    }
    default: { // KindLookup
        const QString found = outcome.error == AiErrorKind::None && isUsable(outcome.text)
            ? cleanPath(outcome.text)
            : QString();
        m_haveLocated = outcome.error != AiErrorKind::Cancelled;
        m_located = found;
        if (found.isEmpty())
            emit locateFailed(id, u"claude was not found in the usual install locations."_s);
        else
            emit located(id, found);
        break;
    }
    }
}

void ClaudeRunner::onFinished(quint64 id, int exitCode, QProcess::ExitStatus status)
{
    const auto it = m_runs.find(id);
    if (it == m_runs.end())
        return;
    Run &run = *it->second;
    drain(run);
    const int code = status == QProcess::CrashExit && exitCode == 0 ? 1 : exitCode;

    CliOutcome outcome;
    if (run.overflow) {
        // Nothing of the output is kept or shown: it is not what the CLI should print.
        outcome = { AiErrorKind::InvalidOutput,
                    {},
                    u"The output is larger than %1 bytes."_s.arg(kMaxOutputBytes) };
    } else if (run.kind == KindAsk) {
        outcome = parseClaudeOutput(run.out, run.err, code);
    } else if (run.kind == KindLookup) {
        const QString line = code == 0 ? lastLine(run.out) : QString();
        outcome = line.isEmpty() ? CliOutcome { AiErrorKind::Failed, {}, lastLine(run.err) }
                                 : CliOutcome { AiErrorKind::None, line, {} };
    } else { // KindCheck
        static const QRegularExpression kVersion(uR"(\d+\.\d+\.\d+)"_s);
        const QString line = firstLine(run.out);
        if (code == 0 && kVersion.match(line).hasMatch()) {
            outcome = { AiErrorKind::None, line, {} };
        } else {
            QString detail = lastLine(run.err);
            if (detail.isEmpty())
                detail = line;
            if (detail.isEmpty())
                detail = u"claude --version exited with code %1."_s.arg(code);
            outcome = { AiErrorKind::Failed, {}, detail.left(kMaxDetailChars) };
        }
    }
    finish(id, outcome);
}

void ClaudeRunner::onError(quint64 id, QProcess::ProcessError error)
{
    const auto it = m_runs.find(id);
    if (error != QProcess::FailedToStart || it == m_runs.end())
        return;
    const QString detail = it->second->process->errorString();
    finish(id, { AiErrorKind::CliNotFound, {}, detail.left(kMaxDetailChars) });
}

void ClaudeRunner::cancel(quint64 id)
{
    const auto it = m_runs.find(id);
    if (it == m_runs.end())
        return;
    killTree(*it->second);
    finish(id, { AiErrorKind::Cancelled, {}, {} });
}

} // namespace quarry::ai
