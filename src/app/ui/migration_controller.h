#pragma once

#include "connection_controller.h"
#include "query_messages_model.h"

#include <quarry/migration_runner.h>
#include <quarry/migration_service.h>

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <map>
#include <memory>
#include <optional>

namespace quarry::ui {

// The migrations integration in the UI (ARCHITECTURE §6.11, §6.12): the per-database settings (the
// connection dialog for a connection's own database, the tree for each database of a server), the
// pending count next to a connection, the Migrations folders of the tree (it pushes the last report
// of each (connection, database) to SchemaTreeModel) and the code of one migration file for its
// tab. A `database` argument is the settings key: "" = the connection's own database (SQLite, or a
// connection with a fixed database), else the database of a server connection without one.
// Everything reads through MigrationService (worker thread + the tree's open metadata session);
// nothing blocks the UI.
//
// Running (H12 phase C, ARCHITECTURE section 6.12): start() runs, rolls back or dry-runs one
// migration with `php artisan` through a MigrationRunner, one action at a time per (connection,
// database). The order is fixed and has no shortcut: canRun -> resolveTarget (read-only `db:show`)
// -> sameDatabase -> (Run and Rollback only) awaitingConfirm -> confirm() -> execute. Run and
// Rollback never execute without confirm(), and never when artisan's database is not the
// connection's (state "blocked"); a dry run only warns. The output goes to the Messages model the
// tab bound with setMessages().
class MigrationController : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(quarry::ui::ConnectionController *connections READ connections WRITE setConnections
                   NOTIFY connectionsChanged FINAL)
    // [{id, name, usesTable, defaultTable}]; the first one is "None" (id "").
    Q_PROPERTY(QVariantList kinds READ kinds CONSTANT FINAL)
    // connectionId -> pending migrations (all its databases added up), for what was inspected in
    // this run.
    Q_PROPERTY(QVariantMap pendingCounts READ pendingCounts NOTIFY countsChanged FINAL)
    // "idle" | "checking" | "ok" | "failed" (the folder typed in the connection dialog)
    Q_PROPERTY(QString folderState READ folderState NOTIFY folderChanged FINAL)
    Q_PROPERTY(QString folderMessage READ folderMessage NOTIFY folderChanged FINAL)

public:
    explicit MigrationController(QObject *parent = nullptr);
    ~MigrationController() override;

    ConnectionController *connections() const { return m_connections; }
    void setConnections(ConnectionController *connections);
    QVariantList kinds() const;
    QVariantMap pendingCounts() const;
    QString folderState() const { return m_folderState; }
    QString folderMessage() const { return m_folderMessage; }

    // Test seam: the service, created on first use from connections.databasePath.
    quarry::MigrationService *service();

    // Loads the saved settings (worker thread). Call after the first frame. Idempotent.
    Q_INVOKABLE void start();
    // {kind, folder, table, phpPath, projectRoot} of a database of a connection; kind "" = none.
    Q_INVOKABLE QVariantMap settingsOf(const QString &connectionId, const QString &database) const;
    // Pending migrations of one database, -1 when it was not inspected (or is not configured).
    Q_INVOKABLE int pendingOf(const QString &connectionId, const QString &database) const;
    // Message for settings that cannot be saved as typed (no disk access), or "".
    Q_INVOKABLE QString validate(const QString &kind, const QString &folder,
                                 const QString &table) const;
    // Checks on the worker thread that the folder exists and can be read -> folderState.
    Q_INVOKABLE void checkFolder(const QString &kind, const QString &folder, const QString &table);
    Q_INVOKABLE void resetFolderCheck();
    // What the runner would use for the typed settings, without starting anything (touches the
    // disk): {phpText, phpError, rootText, rootError}. The texts say what is effective ("Using php
    // from the PATH", "Project root: ..."); an error says what is missing and never blocks saving.
    Q_INVOKABLE QVariantMap checkEnvironment(const QString &kind, const QString &folder,
                                             const QString &phpPath,
                                             const QString &projectRoot) const;
    // Stores the settings of one database (kind "" removes only those) and reads its migrations
    // again. phpPath and projectRoot are optional: empty = php in the PATH, two levels above the
    // folder.
    Q_INVOKABLE void saveSettings(const QString &connectionId, const QString &database,
                                  const QString &kind, const QString &folder, const QString &table,
                                  const QString &phpPath = {}, const QString &projectRoot = {});
    Q_INVOKABLE bool isConfigured(const QString &connectionId, const QString &database) const;
    // Some database of the connection has settings.
    Q_INVOKABLE bool isConfiguredAny(const QString &connectionId) const;

    // ---- the code of a migration (H12 phase B) ----
    // Asks for the file of migration `name`; the answer is sourceChanged(connectionId, database,
    // name) and sourceOf(). Asking again for one that failed or is loading is a new request.
    Q_INVOKABLE void readSource(const QString &connectionId, const QString &database,
                                const QString &name);
    // {state: "loading" | "ready" | "failed" | "none", text, message, detail, file (the file
    // name), definition (KSyntaxHighlighting)}.
    Q_INVOKABLE QVariantMap sourceOf(const QString &connectionId, const QString &database,
                                     const QString &name) const;
    // Forgets the text (the tab was closed).
    Q_INVOKABLE void releaseSource(const QString &connectionId, const QString &database,
                                   const QString &name);
    // {found, state: "pending" | "applied" | "orphan", batch (-1 = none)} in the last report.
    Q_INVOKABLE QVariantMap entryOf(const QString &connectionId, const QString &database,
                                    const QString &name) const;

    // ---- running migrations (H12 phase C) ----
    // action: "run" | "runAll" | "rollback" | "dryRun". {ok, reason}: whether the buttons of a
    // migration tab can start something now, and if not why, in plain translated language.
    Q_INVOKABLE QVariantMap canRun(const QString &connectionId, const QString &database,
                                   const QString &name) const;
    // Like canRun(), plus per-action state checks (Run is off on applied migrations, Rollback is
    // off on pending migrations, RunAll is off when there are no pending migrations).
    Q_INVOKABLE QVariantMap canRunAction(const QString &connectionId, const QString &database,
                                         const QString &name, const QString &action) const;
    // Starts the flow for an action; false (and nothing happens) when canRunAction says no.
    Q_INVOKABLE bool start(const QString &connectionId, const QString &database,
                           const QString &name, const QString &action);
    // The answer to the confirmation of Run and Rollback. Only confirm() executes them.
    Q_INVOKABLE void confirm(const QString &connectionId, const QString &database,
                             const QString &name);
    Q_INVOKABLE void decline(const QString &connectionId, const QString &database,
                             const QString &name);
    // Stops what the tab is doing: the check, the question or the process (the database may be left
    // half migrated, and the output says so).
    Q_INVOKABLE void cancel(const QString &connectionId, const QString &database,
                            const QString &name);
    // {state: "idle" | "checking" | "awaitingConfirm" | "running" | "done" | "failed" | "blocked",
    //  action, message, detail, exitCode (-1 = none), cancelled, output, lines, truncated,
    //  warning (dry run on another database), command, target{...}, connection{...}, why}
    Q_INVOKABLE QVariantMap runOf(const QString &connectionId, const QString &database,
                                  const QString &name) const;
    // What the confirmation dialog shows: {title, confirmText, name, action, connectionName,
    // production, server, database, host, phrase, rollbackNote, command, workingDirectory,
    // typeToConfirm (what to type on a production connection)}.
    Q_INVOKABLE QVariantMap confirmationOf(const QString &connectionId, const QString &database,
                                           const QString &name) const;
    // Where the tab's Messages go (the tab's QueryState). Unbound: the output is only in runOf().
    Q_INVOKABLE void setMessages(const QString &connectionId, const QString &database,
                                 const QString &name, quarry::ui::QueryMessagesModel *messages);
    // A migration of the database is checking, waiting or running (closing asks first).
    Q_INVOKABLE bool isBusy(const QString &connectionId, const QString &database) const;
    Q_INVOKABLE bool isRunning(const QString &connectionId, const QString &database,
                               const QString &name) const;
    Q_INVOKABLE bool anyRunning() const;
    // Cancels every running action (the app is closing, or the user agreed to).
    Q_INVOKABLE void cancelAll();

    static constexpr int kMaxOutputLines = 5000;

    // Test seam: the runner (a MigrationRunner on the UI thread).
    quarry::MigrationRunner *runner() { return &m_runner; }

signals:
    void connectionsChanged();
    // The state of one migration's run changed (runOf() may answer differently).
    void runChanged(const QString &connectionId, const QString &database, const QString &name);
    // What canRun() depends on may have changed (a connection opened or closed).
    void availabilityChanged();
    void countsChanged();
    void folderChanged();
    // The project of some database was set, changed or removed (isConfigured() may answer
    // differently).
    void settingsChanged();
    void sourceChanged(const QString &connectionId, const QString &database, const QString &name);
    // The last report of some database changed (entryOf() may answer differently).
    void reportsChanged();

private:
    struct Source
    {
        QString state = QStringLiteral("none");
        QString text;
        QString message;
        QString detail;
        quint64 request = 0;
    };

    void inspect(const QString &connectionId, const QString &database);
    void onConnected();
    void onInspected(quint64 id, const quarry::MigrationReport &report);
    void onFailed(quint64 id, const quarry::Error &error);
    void onSourceRead(quint64 id, const QUuid &connectionId, const QString &database,
                      const QString &name, const QString &text);
    void onSourceFailed(quint64 id, const QUuid &connectionId, const QString &database,
                        const QString &name, const quarry::Error &error);
    // Tells the tree what it shows of one database of a connection.
    void pushFeed(const QString &connectionId, const QString &database);
    QString scopeOfRequest(quint64 id) const;
    // The key of everything that is per (connection, database): a connection id and a database name
    // cannot contain the separator.
    static QString scopeKey(const QString &connectionId, const QString &database);
    static QString connectionOfScope(const QString &scope);
    static QString databaseOfScope(const QString &scope);
    static QString sourceKey(const QString &connectionId, const QString &database,
                             const QString &name);
    // The databases of `connectionId` that have settings.
    QStringList databasesOf(const QString &connectionId) const;

    // One migration's run: the state the tab shows and what the flow needs between its steps.
    struct Run
    {
        QString connectionId;
        QString database;
        QString name;
        QString state = QStringLiteral("idle");
        QString action;
        QString message;
        QString detail;
        QString warning;
        QString why; // blocked: which part of the databases differs
        int exitCode = -1;
        bool cancelled = false;
        bool truncated = false;
        QStringList lines;
        quint64 request = 0; // the runner's id of the step in flight
        bool executing = false; // false while the target is being read
        bool confirmed = false; // Run and Rollback execute only after confirm()
        QStringList errorTail; // the last lines of stderr, for the error
        MigrationSettings settings; // what it started with
        quarry::ArtisanTarget target;
        bool targetKnown = false;
        QPointer<QueryMessagesModel> messages;
    };

    Run &run(const QString &connectionId, const QString &database, const QString &name);
    const Run *findRun(const QString &connectionId, const QString &database,
                       const QString &name) const;
    Run *findRun(const QString &connectionId, const QString &database, const QString &name);
    // `database` (a server's database the settings are about) replaces the profile's blank one.
    std::optional<quarry::ConnectionTarget> connectionTargetOf(const QString &connectionId,
                                                               const QString &database) const;
    QVariantMap targetMap(const Run &r) const;
    void changed(const QString &key);
    void say(Run &r, const QString &kind, const QString &type, const QString &text,
             const QString &detail = {});
    void addLine(Run &r, const QString &text, bool isError);
    void beginRun(Run &r, quarry::MigrationAction action);
    void finishRun(const QString &key, const QString &state, const QString &message,
                   const QString &detail = {});
    void execute(const QString &key, Run &r);
    void onTargetResolved(quint64 id, const quarry::ArtisanTarget &target);
    void onRunnerFailed(quint64 id, const quarry::Error &error);
    void onRunnerOutput(quint64 id, const QString &text, bool isError);
    void onRunnerFinished(quint64 id, int exitCode);
    void onRunnerCancelled(quint64 id);
    static QString commandText(const QString &program, const QStringList &args);

    QPointer<ConnectionController> m_connections;
    quarry::MigrationRunner m_runner;
    std::map<QString, Run> m_runs; // sourceKey -> run (node based: references stay valid)
    QHash<quint64, QString> m_steps; // runner id -> sourceKey
    QHash<QString, QString> m_active; // scope -> sourceKey of its action in progress
    std::optional<quarry::Error> m_immediate; // failed(0, ...) inside a runner call
    std::unique_ptr<quarry::MigrationService> m_service;
    QHash<QString, quarry::MigrationSettings> m_settings; // by scope
    QHash<QString, quint64> m_requests; // scope -> inspection in flight
    QHash<QString, quarry::MigrationReport> m_reports; // scope -> last report
    QHash<QString, QString> m_failures; // scope -> why the last inspection failed
    QHash<QString, Source> m_sources; // sourceKey -> code of a migration
    QStringList m_connected; // last connectedIds of the tree
    QHash<QString, int> m_counts; // scope -> pending
    bool m_started = false;

    quint64 m_folderCheck = 0;
    QString m_folderState = QStringLiteral("idle");
    QString m_folderMessage;
};

} // namespace quarry::ui
