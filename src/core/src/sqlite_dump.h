#pragma once

#include <quarry/dump.h>
#include <quarry/result.h>

#include <QMutex>
#include <QString>

#include <atomic>
#include <functional>

struct sqlite3;

namespace quarry::sqlite_dump {

// Shared between the worker thread and DumpRunner::cancel: the flag is checked between rows and
// statements, and interrupt() stops a statement that is running.
class CancelToken
{
public:
    void cancel();
    bool isCancelled() const { return m_cancelled.load(); }
    void setDatabase(sqlite3 *db);

private:
    std::atomic_bool m_cancelled { false };
    QMutex m_mutex;
    sqlite3 *m_db = nullptr;
};

struct Callbacks
{
    std::function<void(int done, int total, const QString &step)> progress;
    std::function<void(const QString &text)> log;
};

// The same script as the sqlite3 shell's `.dump` (foreign keys off, one transaction, tables and
// their rows, then indexes, triggers and views, and sqlite_sequence). Written to a QSaveFile:
// the output only appears when everything worked. Runs on the caller's thread.
Result<void> dump(const QString &databaseFile, const DumpOptions &options, CancelToken &cancel,
                  const Callbacks &callbacks);

// Runs a SQL script statement by statement, like the shell's `.read`. If it fails or is
// cancelled with a transaction open, the transaction is rolled back.
Result<void> restore(const QString &databaseFile, const RestoreOptions &options,
                     CancelToken &cancel, const Callbacks &callbacks);

} // namespace quarry::sqlite_dump
