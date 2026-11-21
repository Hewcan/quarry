#pragma once

#include "connection_controller.h"

#include <quarry/migration_service.h>

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

namespace quarry::ui {

// The migrations integration in the UI (ARCHITECTURE §6.11, §6.12): the per-connection settings of
// the connection dialog, the pending count next to a connection, the Migrations folder of the tree
// (it pushes the last report of each connection to SchemaTreeModel) and the code of one migration
// file for its tab. Read-only: it never runs, reverts or edits a migration. Everything goes
// through MigrationService (worker thread + the tree's open metadata session); nothing blocks the
// UI.
class MigrationController : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(quarry::ui::ConnectionController *connections READ connections WRITE setConnections
                   NOTIFY connectionsChanged FINAL)
    // [{id, name, usesTable, defaultTable}]; the first one is "None" (id "").
    Q_PROPERTY(QVariantList kinds READ kinds CONSTANT FINAL)
    // connectionId -> pending migrations, for the connections inspected in this run.
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
    QVariantMap pendingCounts() const { return m_counts; }
    QString folderState() const { return m_folderState; }
    QString folderMessage() const { return m_folderMessage; }

    // Test seam: the service, created on first use from connections.databasePath.
    quarry::MigrationService *service();

    // Loads the saved settings (worker thread). Call after the first frame. Idempotent.
    Q_INVOKABLE void start();
    // {kind, folder, table} of a connection; kind "" = none.
    Q_INVOKABLE QVariantMap settingsOf(const QString &connectionId) const;
    // Message for settings that cannot be saved as typed (no disk access), or "".
    Q_INVOKABLE QString validate(const QString &kind, const QString &folder,
                                 const QString &table) const;
    // Checks on the worker thread that the folder exists and can be read -> folderState.
    Q_INVOKABLE void checkFolder(const QString &kind, const QString &folder, const QString &table);
    Q_INVOKABLE void resetFolderCheck();
    // Stores the settings (kind "" removes them) and reads the migrations of the connection again.
    Q_INVOKABLE void saveSettings(const QString &connectionId, const QString &kind,
                                  const QString &folder, const QString &table);
    Q_INVOKABLE bool isConfigured(const QString &connectionId) const;

    // ---- the code of a migration (H12 phase B) ----
    // Asks for the file of migration `name`; the answer is sourceChanged(connectionId, name) and
    // sourceOf(). Asking again for one that failed or is loading is a new request.
    Q_INVOKABLE void readSource(const QString &connectionId, const QString &name);
    // {state: "loading" | "ready" | "failed" | "none", text, message, detail, file (the file
    // name), definition (KSyntaxHighlighting)}.
    Q_INVOKABLE QVariantMap sourceOf(const QString &connectionId, const QString &name) const;
    // Forgets the text (the tab was closed).
    Q_INVOKABLE void releaseSource(const QString &connectionId, const QString &name);
    // {found, state: "pending" | "applied" | "orphan", batch (-1 = none)} in the last report.
    Q_INVOKABLE QVariantMap entryOf(const QString &connectionId, const QString &name) const;

signals:
    void connectionsChanged();
    void countsChanged();
    void folderChanged();
    // The project of some connection was set, changed or removed (isConfigured() may answer
    // differently).
    void settingsChanged();
    void sourceChanged(const QString &connectionId, const QString &name);
    // The last report of some connection changed (entryOf() may answer differently).
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

    void inspect(const QString &connectionId);
    void onConnected();
    void onInspected(quint64 id, const quarry::MigrationReport &report);
    void onFailed(quint64 id, const quarry::Error &error);
    void onSourceRead(quint64 id, const QUuid &connectionId, const QString &name,
                      const QString &text);
    void onSourceFailed(quint64 id, const QUuid &connectionId, const QString &name,
                        const quarry::Error &error);
    // Tells the tree what it shows of `connectionId`.
    void pushFeed(const QString &connectionId);
    QString idOfRequest(quint64 id) const;
    static QString sourceKey(const QString &connectionId, const QString &name);

    QPointer<ConnectionController> m_connections;
    std::unique_ptr<quarry::MigrationService> m_service;
    QHash<QString, quarry::MigrationSettings> m_settings; // by connection id
    QHash<QString, quint64> m_requests; // connection id -> inspection in flight
    QHash<QString, quarry::MigrationReport> m_reports; // connection id -> last report
    QHash<QString, QString> m_failures; // connection id -> why the last inspection failed
    QHash<QString, Source> m_sources; // sourceKey -> code of a migration
    QStringList m_connected; // last connectedIds of the tree
    QVariantMap m_counts;
    bool m_started = false;

    quint64 m_folderCheck = 0;
    QString m_folderState = QStringLiteral("idle");
    QString m_folderMessage;
};

} // namespace quarry::ui
