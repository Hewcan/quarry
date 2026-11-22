#pragma once

#include <quarry/dump.h>
#include <quarry/result.h>
// Declares the Q_DECLARE_METATYPE for quarry::Error used by the signals.
#include <quarry/session.h>

#include <QObject>
#include <QString>

#include <memory>
#include <unordered_map>

namespace quarry {

// Runs a dump or a restore in the background (H14, ARCHITECTURE §6.14). The object lives on the
// UI thread and never blocks it: PostgreSQL goes through pg_dump / psql / pg_restore with QProcess
// (argument list, no shell, the password only in a temporary PGPASSFILE that is removed at the
// end, the process tree killed on cancel); SQLite is done in-process with the sqlite3 library on
// a thread of its own (same output as the shell's `.dump`, no sqlite3 program needed).
//
// Signals: started(id), progress(id, done, total, step) (total 0 = unknown), output(id, line,
// isError) for the log, then exactly one of finished(id), cancelled(id) or failed(id, Error).
// A cancelled or failed dump leaves no output file; a cancelled or failed restore is rolled back
// when the engine allows it (SQLite script in a transaction, psql --single-transaction,
// pg_restore --single-transaction).
class DumpRunner : public QObject
{
    Q_OBJECT

public:
    explicit DumpRunner(QObject *parent = nullptr);
    // Cancels and waits for whatever is still running.
    ~DumpRunner() override;

    // Returns the request id (> 0), or 0 plus a queued failed(0, Error) when it cannot start
    // (engine not supported, tool missing, SSH tunnel, no output path...).
    quint64 dump(const quarry::DumpTarget &target, const quarry::DumpOptions &options,
                 const quarry::DumpToolPaths &tools = {});
    quint64 restore(const quarry::DumpTarget &target, const quarry::RestoreOptions &options,
                    const quarry::DumpToolPaths &tools = {});

    // cancelled(id) follows. No-op for an unknown id.
    void cancel(quint64 id);
    bool isRunning(quint64 id) const { return m_runs.count(id) != 0; }

    static constexpr qint64 kMaxLogLines = 5000;

signals:
    void started(quint64 id);
    void progress(quint64 id, int done, int total, QString step);
    void output(quint64 id, QString text, bool isError);
    void finished(quint64 id);
    void cancelled(quint64 id);
    void failed(quint64 id, quarry::Error error);

private:
    struct Run;
    quint64 failLater(Error error);
    quint64 startProcess(const DumpTarget &target, const QString &program,
                         const QStringList &args, const QString &partialFile,
                         const QString &finalFile);
    quint64 startSqlite(const DumpTarget &target, bool isDump, const DumpOptions &dump,
                        const RestoreOptions &restore);
    void line(quint64 id, const QString &text, bool isError);
    void drain(Run &run, bool flush);
    void endProcess(quint64 id, int exitCode, bool crashed);
    void finish(quint64 id);

    std::unordered_map<quint64, std::unique_ptr<Run>> m_runs;
    quint64 m_lastId = 0;
};

} // namespace quarry
