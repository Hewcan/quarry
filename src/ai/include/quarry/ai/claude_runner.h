#pragma once

#include <quarry/ai/claude_cli.h>

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <memory>
#include <unordered_map>

namespace quarry::ai {

// What `claude --version` said about one executable.
struct CliInfo
{
    QString path; // absolute path that was run
    QString version; // the first line of the output ("2.0.14 (Claude Code)"), empty on failure
    bool compatible = false; // isCompatibleVersion(version)
    AiErrorKind error = AiErrorKind::None; // CliNotFound, IncompatibleVersion or Failed
    QString detail; // raw text for the "Details" of an error, never a secret
};

// Runs the user's own Claude Code CLI for the H13 chat (ARCHITECTURE §6.13). The only place in
// the app that starts it. Rules, all tested with a fake `claude`: QProcess with an argument list
// and no shell; the arguments are exactly claudeArguments() and never carry text from the user, the
// script or the schema (all of that goes through stdin, then stdin is closed); the working
// directory is a fresh empty temporary folder, so no CLAUDE.md, .claude/ or project settings of
// whatever folder the app was started from can be read; the environment is the app's own plus
// NO_COLOR=1 and TERM=dumb (an ANTHROPIC_API_KEY the user already has stays: it is the
// documented alternative to the subscription); stdout and stderr are read up to kMaxOutputBytes
// (more is InvalidOutput); a time limit after which the whole process tree is killed (Job Object
// on Windows, a process group on Unix) and Timeout follows; cancel() does the same and gives
// Cancelled. Nothing blocks the thread of the object: it is asynchronous (the object lives where
// it was created) and has no thread of its own. quarry_ai links only Qt6::Core, so the tree
// kill is written again here and not shared with the migration runner (`// shortcut:` in the
// code: unify when a third process runner appears).
class ClaudeRunner : public QObject
{
    Q_OBJECT

public:
    explicit ClaudeRunner(QObject *parent = nullptr);
    // Kills any process still running and removes the temporary folders.
    ~ClaudeRunner() override;

    // Looks for the executable: `customPath` first (a file that exists and is executable), then
    // candidatePaths() for this OS, then, on Unix and only if none exists, one run of
    // `$SHELL -l -c "command -v claude"` (loginShellLookupArguments(), 5 s limit). The answer is
    // cached for the life of the object; pass `forget = true` to look again.
    // -> located(id, absolutePath) | locateFailed(id, detail) (the CLI is not installed: the UI
    // says so and shows the install hint). Returns the request id (> 0).
    quint64 locate(const QString &customPath, bool forget = false);

    // Runs `<path> --version` (10 s limit): reading the version spends nothing. -> checked(id,
    // CliInfo). A file that cannot be started is error CliNotFound; a version below
    // kMinimumVersion is IncompatibleVersion with `version` filled.
    quint64 check(const QString &path);

    // One run of `claude` with claudeArguments() and `stdinText` (UTF-8) on stdin. `timeoutSeconds`
    // is clamped to 10..kMaxTimeoutSeconds. -> started(id), then exactly one finished(id,
    // outcome); the outcome comes from parseClaudeOutput() (so NotLoggedIn, UsageLimit,
    // InvalidOutput and Failed are in `outcome.error`), or is Timeout, Cancelled or CliNotFound
    // (the file cannot be started). A non-zero exit code is not an exception: parseClaudeOutput
    // decides. Returns the request id (> 0); 0 with a finished(0, ...) when the arguments are
    // refused (empty path, stdin over kMaxStdinBytes).
    quint64 ask(const QString &path, const QByteArray &stdinText,
                int timeoutSeconds = kDefaultTimeoutSeconds);

    // Kills the process tree of `id` and gives finished(id, Cancelled). No-op for an unknown id.
    void cancel(quint64 id);
    bool isRunning(quint64 id) const { return m_runs.count(id) > 0; }

    static constexpr int kDefaultTimeoutSeconds = 120;
    static constexpr int kMaxTimeoutSeconds = 600;
    static constexpr qint64 kMaxOutputBytes = qint64(8) * 1024 * 1024;
    static constexpr qint64 kMaxStdinBytes = qint64(2) * 1024 * 1024;

signals:
    void located(quint64 id, QString path);
    void locateFailed(quint64 id, QString detail);
    void checked(quint64 id, quarry::ai::CliInfo info);
    void started(quint64 id);
    void finished(quint64 id, quarry::ai::CliOutcome outcome);

private:
    struct Run;
    quint64 start(const QString &path, const QStringList &args, const QByteArray &stdinText,
                  int timeoutSeconds, int kind);
    void onFinished(quint64 id, int exitCode, QProcess::ExitStatus status);
    void onError(quint64 id, QProcess::ProcessError error);
    void drain(Run &run);
    void killTree(Run &run);
    void finish(quint64 id, const CliOutcome &outcome);

    std::unordered_map<quint64, std::unique_ptr<Run>> m_runs;
    QString m_located; // cache of locate()
    bool m_haveLocated = false;
    quint64 m_lastId = 0;
};

} // namespace quarry::ai

Q_DECLARE_METATYPE(quarry::ai::CliInfo)
Q_DECLARE_METATYPE(quarry::ai::CliOutcome)
