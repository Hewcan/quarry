#pragma once

#include <quarry/migration_source.h>
#include <quarry/result.h>
// Declares the Q_DECLARE_METATYPE for quarry::Error used by the signals.
#include <quarry/session.h>

#include <QList>
#include <QObject>
#include <QPointer>

#include <map>
#include <memory>
#include <set>

class QThread;

namespace quarry {

class MigrationServiceWorker;

// UI-thread facade for the migrations integration (ARCHITECTURE §6.11). Owns one worker thread
// for quarry.db (table `migration_settings`) and for the folder scans; the applied migrations
// are read through a Session the caller already has open. Every method returns at once and the
// outcome arrives as a queued signal. Read-only towards the project and its database.
class MigrationService : public QObject
{
    Q_OBJECT

public:
    explicit MigrationService(QString databasePath, QObject *parent = nullptr);
    // Drops the inspections, finishes the queued disk work and joins the thread (up to 5 s).
    ~MigrationService() override;

    // -> settingsLoaded(all enabled settings, each with its database) | failed(Error)
    void loadSettings();
    // Inserts or replaces the row of (settings.connectionId, settings.database); a disabled one
    // (empty kind) deletes only that row. The connection must exist in quarry.db.
    // -> settingsSaved(settings) | failed(Error)
    void saveSettings(const quarry::MigrationSettings &settings);

    // Validates the settings and reads the folder. -> folderChecked(id, files) |
    // folderCheckFailed(id, Error). Returns the request id (> 0).
    quint64 checkFolder(const quarry::MigrationSettings &settings);

    // Scans the folder, then reads the migrations table through `session` (open, used from the
    // UI thread) and crosses both. -> inspected(id, report) | inspectionFailed(id, Error).
    // A missing table is not an error: the report has tableMissing and every file pending.
    // One inspection per session at a time: a new one on the same session drops the old one.
    // With settings.database set, the table is qualified with it when `providerId` is "mysql" or
    // "mssql" (the connection spans databases), quoted with `dialect` (the provider's; it must
    // outlive the session); a "mysql"/"mssql" id without a dialect fails. Any other id (or none)
    // reads the table as is: "postgres", "sqlite" and "odbc" connections are one database.
    quint64 inspect(quarry::Session *session, const quarry::MigrationSettings &settings,
                    const quarry::ISqlDialect *dialect = nullptr, const QString &providerId = {});
    // Reads the code of one migration file (H12 phase B), read-only, on the worker thread: the
    // path is IMigrationSource::filePath (an invalid name is an error, never another path), the
    // file must be a regular file of at most kMaxSourceBytes (1 MiB) and valid UTF-8 (anything
    // else is decoded with replacement characters, and `truncated` is never silently true: a
    // bigger file is an error). -> sourceRead(id, connectionId, database, name, text) |
    // sourceFailed(id, connectionId, database, name, Error). Returns the request id (> 0).
    quint64 readSource(const quarry::MigrationSettings &settings, const QString &name);
    static constexpr qint64 kMaxSourceBytes = qint64(1024) * 1024;
    // Stops the scan and drops the request: no signal follows for `id`. The SELECT already sent
    // is not interrupted. shortcut: Session::cancel() would also end the schema tree's requests
    // on that shared session; a dedicated session would allow a real cancel if this table is
    // ever slow to read.
    void cancel(quint64 id);
    bool isRunning(quint64 id) const { return m_runs.contains(id); }

signals:
    void settingsLoaded(QList<quarry::MigrationSettings> settings);
    void settingsSaved(quarry::MigrationSettings settings);
    void failed(quarry::Error error);
    void folderChecked(quint64 id, int files);
    void folderCheckFailed(quint64 id, quarry::Error error);
    void inspected(quint64 id, quarry::MigrationReport report);
    void inspectionFailed(quint64 id, quarry::Error error);
    void sourceRead(quint64 id, QUuid connectionId, QString database, QString name, QString text);
    void sourceFailed(quint64 id, QUuid connectionId, QString database, QString name,
                      quarry::Error error);

private:
    struct Run;
    template<typename F> void post(F &&task);
    void onScanned(quint64 id, const QStringList &files, const Error &error, bool ok);
    void onSourceDone(quint64 id, const QUuid &connectionId, const QString &database,
                      const QString &name, const QString &text, const Error &error, bool ok);
    void onBatch(Run &run, const ResultBatch &batch);
    void finishRun(quint64 id, const Error *error);

    QThread *m_thread = nullptr;
    MigrationServiceWorker *m_worker = nullptr; // lives on m_thread, deleted there
    std::map<quint64, std::unique_ptr<Run>> m_runs;
    std::set<quint64> m_checks; // folder checks in flight
    std::set<quint64> m_reads; // source reads in flight
    quint64 m_lastId = 0;
};

} // namespace quarry

Q_DECLARE_METATYPE(QList<quarry::MigrationSettings>)
