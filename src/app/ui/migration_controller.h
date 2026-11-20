#pragma once

#include "connection_controller.h"
#include "migration_list_model.h"

#include <quarry/migration_service.h>

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

namespace quarry::ui {

// The migrations integration in the UI (ARCHITECTURE §6.11): the per-connection settings of the
// connection dialog, the pending count of the tree and the migrations view of one connection.
// Read-only: it never runs, reverts or edits a migration. Everything goes through
// MigrationService (worker thread + the tree's open metadata session); nothing blocks the UI.
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

    // The view: the connection it shows and its last result.
    Q_PROPERTY(QString connectionId READ connectionId NOTIFY viewChanged FINAL)
    // "idle" | "loading" | "ready" | "failed" | "disconnected" | "unconfigured"
    Q_PROPERTY(QString state READ state NOTIFY viewChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY viewChanged FINAL)
    Q_PROPERTY(QString detail READ detail NOTIFY viewChanged FINAL)
    Q_PROPERTY(int pending READ pending NOTIFY viewChanged FINAL)
    Q_PROPERTY(int applied READ applied NOTIFY viewChanged FINAL)
    Q_PROPERTY(int orphans READ orphans NOTIFY viewChanged FINAL)
    Q_PROPERTY(bool tableMissing READ tableMissing NOTIFY viewChanged FINAL)
    Q_PROPERTY(QString summary READ summary NOTIFY viewChanged FINAL)
    Q_PROPERTY(quarry::ui::MigrationListModel *model READ model CONSTANT FINAL)

public:
    explicit MigrationController(QObject *parent = nullptr);
    ~MigrationController() override;

    ConnectionController *connections() const { return m_connections; }
    void setConnections(ConnectionController *connections);
    QVariantList kinds() const;
    QVariantMap pendingCounts() const { return m_counts; }
    QString folderState() const { return m_folderState; }
    QString folderMessage() const { return m_folderMessage; }
    QString connectionId() const { return m_viewId; }
    QString state() const { return m_state; }
    QString message() const { return m_message; }
    QString detail() const { return m_detail; }
    int pending() const { return m_report.pending; }
    int applied() const { return m_report.applied; }
    int orphans() const { return m_report.orphans; }
    bool tableMissing() const { return m_report.tableMissing; }
    QString summary() const;
    MigrationListModel *model() { return &m_model; }

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
    // Stores the settings (kind "" removes them) and refreshes the count of the connection.
    Q_INVOKABLE void saveSettings(const QString &connectionId, const QString &kind,
                                  const QString &folder, const QString &table);
    // Shows `connectionId` in the view and reads its migrations again.
    Q_INVOKABLE void show(const QString &connectionId);
    Q_INVOKABLE void refresh();
    // Stops the view's reading (the folder scan; see MigrationService::cancel).
    Q_INVOKABLE void cancel();
    Q_INVOKABLE bool isConfigured(const QString &connectionId) const;

signals:
    void connectionsChanged();
    void countsChanged();
    void folderChanged();
    void viewChanged();
    // For the screen reader (polite).
    void announcement(const QString &text);

private:
    void inspect(const QString &connectionId);
    void onConnected();
    void onInspected(quint64 id, const quarry::MigrationReport &report);
    void onFailed(quint64 id, const quarry::Error &error);
    void setView(const QString &state, const QString &message = {}, const QString &detail = {});
    QString idOfRequest(quint64 id) const;

    QPointer<ConnectionController> m_connections;
    std::unique_ptr<quarry::MigrationService> m_service;
    QHash<QString, quarry::MigrationSettings> m_settings; // by connection id
    QHash<QString, quint64> m_requests; // connection id -> inspection in flight
    QStringList m_connected; // last connectedIds of the tree
    QVariantMap m_counts;
    bool m_started = false;

    quint64 m_folderCheck = 0;
    QString m_folderState = QStringLiteral("idle");
    QString m_folderMessage;

    MigrationListModel m_model;
    QString m_viewId;
    QString m_state = QStringLiteral("idle");
    QString m_message;
    QString m_detail;
    quarry::MigrationReport m_report;
};

} // namespace quarry::ui
