#pragma once

#include "ai_schema_adapter.h"
#include "connection_controller.h"

#include <quarry/ai/schema_context.h>
#include <quarry/dump.h>
#include <quarry/dump_runner.h>
#include <quarry/result.h>

#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUuid>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <optional>

namespace quarry::ui {

// The back end of the "Dump..." and "Restore..." dialog (H14 phase B, ARCHITECTURE section 6.14,
// UX.md "Volcado y restauracion"). One job at a time; the work itself is the core's DumpRunner
// (native tools or in-process SQLite), so nothing here waits on the UI thread except a few stat
// calls (the tools' paths).
//
//   idle -> preparing (reading the password) -> running -> done | failed | cancelled.
//
// The password comes from the keychain like ExportController does: a "Saved" one is read
// asynchronously through ConnectionController::readPassword(); an "Ask every time" connection must
// already have it in memory (a query ran on it). It only travels inside DumpRunner's temporary
// password file.
//
// The list of tables of the selector comes from the connection's metadata session (the one of the
// schema tree) through AiSchemaAdapter, structure only. Without that session, or for a database
// other than the connection's own, the dialog dumps the whole database.
//
// Declare it before the ConnectionController in a parent: the runner's children are destroyed in
// creation order and the sessions' code lives in the registry's plugins. Destroying it cancels a
// running job.
class DumpController : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(quarry::ui::ConnectionController *connections READ connections WRITE setConnections
                   NOTIFY connectionsChanged FINAL)
    // "idle" | "preparing" | "running" | "done" | "failed" | "cancelled"
    Q_PROPERTY(QString state READ state NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged FINAL)
    // cancel() was called and the end has not arrived yet.
    Q_PROPERTY(bool cancelling READ cancelling NOTIFY stateChanged FINAL)
    // "dump" | "restore": what the last (or current) job is.
    Q_PROPERTY(QString mode READ mode NOTIFY stateChanged FINAL)
    // Steps done and total of the running job; total 0 = unknown (an indeterminate bar).
    Q_PROPERTY(int progressDone READ progressDone NOTIFY progressChanged FINAL)
    Q_PROPERTY(int progressTotal READ progressTotal NOTIFY progressChanged FINAL)
    // What it is doing now (table name or "Starting...").
    Q_PROPERTY(QString step READ step NOTIFY progressChanged FINAL)
    // The tool's output, oldest first, at most kMaxLogLines (older lines are dropped).
    Q_PROPERTY(QStringList log READ log NOTIFY logChanged FINAL)
    Q_PROPERTY(QString filePath READ filePath NOTIFY stateChanged FINAL)
    // "Dump written to <path> (X MB)" or "Restore finished", once done.
    Q_PROPERTY(QString resultText READ resultText NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString errorDetail READ errorDetail NOTIFY stateChanged FINAL)
    // Where pg_dump, pg_restore and psql are ("" = search the PATH and the usual folders); kept in
    // QSettings.
    Q_PROPERTY(QString pgDumpPath READ pgDumpPath WRITE setPgDumpPath NOTIFY toolPathsChanged FINAL)
    Q_PROPERTY(QString pgRestorePath READ pgRestorePath WRITE setPgRestorePath NOTIFY
                   toolPathsChanged FINAL)
    Q_PROPERTY(QString psqlPath READ psqlPath WRITE setPsqlPath NOTIFY toolPathsChanged FINAL)
    // The selector's tables: [{schema, name, label}] (tables only, no views), see loadTables().
    Q_PROPERTY(QVariantList tables READ tables NOTIFY tablesChanged FINAL)
    // "idle" | "loading" | "ready" | "unavailable" | "failed"
    Q_PROPERTY(QString tablesState READ tablesState NOTIFY tablesChanged FINAL)
    // The connection has more tables than the selector shows (kMaxTables).
    Q_PROPERTY(bool tablesTruncated READ tablesTruncated NOTIFY tablesChanged FINAL)
    // The text of the failure while loading the list ("" otherwise).
    Q_PROPERTY(QString tablesError READ tablesError NOTIFY tablesChanged FINAL)

public:
    enum class State { Idle, Preparing, Running, Done, Failed, Cancelled };

    // The tables the selector can hold: AiSchemaAdapter::kMaxTables.
    static constexpr int kMaxTables = AiSchemaAdapter::kMaxTables;
    static constexpr qsizetype kMaxLogLines = 2000;
    // What the user types to confirm a restore (not translated: the same text for everybody).
    static constexpr auto kRestorePhrase = "RESTORE";

    explicit DumpController(QObject *parent = nullptr);
    ~DumpController() override;

    ConnectionController *connections() const { return m_connections; }
    void setConnections(ConnectionController *connections);
    QString state() const;
    bool busy() const { return m_state == State::Preparing || m_state == State::Running; }
    bool cancelling() const { return busy() && m_cancelRequested; }
    QString mode() const { return m_isDump ? QStringLiteral("dump") : QStringLiteral("restore"); }
    int progressDone() const { return m_done; }
    int progressTotal() const { return m_total; }
    QString step() const { return m_step; }
    QStringList log() const { return m_log; }
    QString filePath() const { return m_filePath; }
    QString resultText() const { return m_resultText; }
    QString errorMessage() const { return m_error.message; }
    QString errorDetail() const { return m_error.detail; }
    QString pgDumpPath() const { return m_tools.pgDump; }
    void setPgDumpPath(const QString &path);
    QString pgRestorePath() const { return m_tools.pgRestore; }
    void setPgRestorePath(const QString &path);
    QString psqlPath() const { return m_tools.psql; }
    void setPsqlPath(const QString &path);
    QVariantList tables() const { return m_tables; }
    QString tablesState() const;
    bool tablesTruncated() const { return m_tablesTruncated; }
    QString tablesError() const { return m_tablesError; }

    // What the dialog needs about a connection (and `database`, "" = the connection's own):
    // {valid, name, providerId, engine, database (the name shown), isPostgres, production,
    // connected, supported (a dump of this engine is possible at all), reason (why not, already
    // translated, when !supported), canPickTables}.
    Q_INVOKABLE QVariantMap infoFor(const QString &connectionId,
                                    const QString &database = {}) const;

    // Fills tables / tablesState for the selector. Never blocks: an open metadata session is
    // asked through AiSchemaAdapter (structure only, no rows); a connection that is not open yet
    // is opened the way the tree does (never asking for a password) and the list follows.
    Q_INVOKABLE void loadTables(const QString &connectionId, const QString &database = {});
    // Forgets the list (the dialog closed).
    Q_INVOKABLE void clearTables();
    // `rows` (indexes into tables) plus every table they point at through foreign keys, as
    // indexes into tables, in the order of dependencyClosure(). Tables the list does not hold (it
    // was cut at kMaxTables) are left out here but still dumped by startDump().
    Q_INVOKABLE QVariantList closureOf(const QVariantList &rows) const;

    // Whether the typed text confirms a restore into this connection: the fixed phrase
    // (kRestorePhrase) and, when the connection is flagged production, the name of the database.
    Q_INVOKABLE bool restoreConfirmed(const QString &connectionId, const QString &database,
                                      const QString &phrase, const QString &typedName) const;
    Q_INVOKABLE bool needsDatabaseName(const QString &connectionId) const;
    Q_INVOKABLE QString restorePhrase() const { return QString::fromLatin1(kRestorePhrase); }

    // request (dump): {connectionId, database, content ("both" | "schema" | "data"), format
    // ("plain" | "custom"), schemas (list), tables (rows of tables; empty = all),
    // includeReferenced (bool), includeOwnership (bool), filePath}.
    // request (restore): {connectionId, database, filePath, phrase, typedName}.
    // Returns "" when the job started (state() is "preparing" or "running"), else the reason it did
    // not, in plain language; nothing changes then. The caller asks before replacing a file.
    Q_INVOKABLE QString startDump(const QVariantMap &request);
    Q_INVOKABLE QString startRestore(const QVariantMap &request);
    // Ends a running job with state "cancelled" (the end arrives asynchronously).
    Q_INVOKABLE void cancel();
    // Back to idle after done, failed or cancelled. Does nothing while busy.
    Q_INVOKABLE void reset();

    // The tools. tool: "pg_dump" | "pg_restore" | "psql". {ok, path (resolved) or message, detail}.
    // shortcut: looks at the disk on the UI thread (a few stat calls), once per click on Test.
    Q_INVOKABLE QVariantMap testTool(const QString &tool) const;

    // Files. The folder of the last job (or Documents) and a suggested name; the same path with the
    // extension of the other format.
    Q_INVOKABLE QString defaultPath(const QString &connectionId, const QString &database,
                                    bool custom) const;
    Q_INVOKABLE QString retarget(const QString &path, bool custom) const;
    Q_INVOKABLE QUrl folderUrl(const QString &path) const;
    Q_INVOKABLE QUrl fileUrl(const QString &path) const;
    Q_INVOKABLE QString toLocalPath(const QUrl &url) const;
    Q_INVOKABLE bool fileExists(const QString &path) const;
    // Opens the folder of the file in the system file manager.
    Q_INVOKABLE void showInFolder() const;
    Q_INVOKABLE void copyLog() const;
    Q_INVOKABLE void copyText(const QString &text) const;

signals:
    void connectionsChanged();
    void stateChanged();
    void progressChanged();
    void logChanged();
    void toolPathsChanged();
    void tablesChanged();
    // For the screen reader, without moving the focus: start, cancel, end and error only.
    void announcement(const QString &text, bool assertive);

private:
    struct Job
    {
        bool isDump = true;
        QString connectionId;
        quarry::DumpTarget target; // no password yet
        quarry::DumpOptions dump;
        quarry::RestoreOptions restore;
    };

    void setTablesState(const QString &state, const QString &error = {});
    void onContextReady(quint64 id, const quarry::ai::SchemaContext &context, bool truncated);
    void onContextFailed(quint64 id, const QString &message, const QString &detail);
    void onConnectedChanged();
    QString begin(Job job);
    void launch(const QString &password);
    void onPasswordAvailable(QUuid id, const QString &password);
    void onPasswordUnavailable(QUuid id, const Error &error);
    void setState(State state);
    void finishWith(State state, const QString &text, bool assertive);
    void resetRun(const QString &filePath);
    void appendLog(const QString &line);
    void connectRunner();
    void saveTools();
    QString checkTarget(const std::optional<ConnectionProfile> &profile,
                        const DumpTarget &target) const;
    QString checkFile(const QString &path, bool mustExist) const;
    std::optional<ConnectionProfile> profileOf(const QString &connectionId) const;

    ConnectionController *m_connections = nullptr; // cleared on destroyed()
    DumpRunner *m_runner = nullptr; // a child
    AiSchemaAdapter *m_adapter = nullptr; // a child
    DumpToolPaths m_tools;
    State m_state = State::Idle;
    bool m_cancelRequested = false;
    bool m_isDump = true;
    std::optional<Job> m_job; // being prepared or run
    QUuid m_waitingPassword; // keychain read in flight
    quint64 m_runId = 0;
    int m_done = 0;
    int m_total = 0;
    QString m_step;
    QStringList m_log;
    bool m_logPending = false; // one logChanged per event loop turn
    QString m_filePath;
    QString m_resultText;
    Error m_error;

    // The selector.
    QVariantList m_tables;
    QList<DumpTable> m_tableKeys; // same order as m_tables
    QList<DumpTableEdge> m_edges;
    QString m_tablesState = QStringLiteral("idle");
    bool m_tablesTruncated = false;
    QString m_tablesError;
    quint64 m_tablesRequest = 0;
    QString m_tablesConnection; // being loaded or loaded
    QMetaObject::Connection m_treeLink; // waiting for the connection to open
};

} // namespace quarry::ui
