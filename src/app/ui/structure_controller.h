#pragma once

#include <quarry/object_path.h>
#include <quarry/provider_api.h>
#include <quarry/result.h>
#include <quarry/session.h>

#include <QHash>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <map>
#include <optional>

namespace quarry::ui {

class ConnectionController;
class EditorTabsModel;
class QueryController;

// The structure of the table behind a data tab or a structure tab (H11 phase 4, ARCHITECTURE
// section 6.10): columns, indexes, foreign keys and the reconstructed DDL, read with
// Session::describe on the tree's metadata session (never on the session of the tab, so the rows
// of the grid are not touched). Asynchronous, cached per tab: a data tab asks once and uses it to
// know its foreign keys; a structure tab asks again with reload() ("Try again"). A failure is kept
// as the tab's state and nobody else is bothered: a data tab simply has no foreign keys then.
//
// "Go to related row" lives here too: it needs the foreign keys of the tab and opens (or shows) the
// data tab of the referenced table, filtered by the key, through QueryController.
//
// Declare it after the ConnectionController, like MigrationController: it listens to the tree.
class StructureController : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(quarry::ui::ConnectionController *connections READ connections WRITE setConnections
                   NOTIFY connectionsChanged FINAL)
    Q_PROPERTY(quarry::ui::EditorTabsModel *tabs READ tabs WRITE setTabs NOTIFY tabsChanged FINAL)
    Q_PROPERTY(quarry::ui::QueryController *queries READ queries WRITE setQueries NOTIFY
                   queriesChanged FINAL)

public:
    // How long a tab waits for the connection of the tree to open before giving up.
    static constexpr int kWaitMs = 30000;

    explicit StructureController(QObject *parent = nullptr);
    ~StructureController() override;

    ConnectionController *connections() const { return m_connections; }
    void setConnections(ConnectionController *connections);
    EditorTabsModel *tabs() const { return m_tabs; }
    void setTabs(EditorTabsModel *tabs);
    QueryController *queries() const { return m_queries; }
    void setQueries(QueryController *queries);

    // Reads the structure of the tab's object unless it is read or being read. A data tab and a
    // structure tab are the same to it.
    Q_INVOKABLE void load(const QString &tabId);
    // Reads it again (a structure tab's "Try again").
    Q_INVOKABLE void reload(const QString &tabId);
    // {state ("idle" | "loading" | "ready" | "failed"), message, detail, isView, empty,
    //  name (the object), columns: [{name, type, nullable, nullableText ("NOT NULL" / "nullable"),
    //  defaultText ("" = none), primaryKey, marks (text: "Primary key", "Foreign key to t.c"),
    //  foreignKeys: [texts], position (1-based), length ("" = none), autoIncrement,
    //  autoIncrementText ("yes" / "no"), description}], indexes: [{name, columns, unique,
    //  uniqueText}], foreignKeys: [{name, columns, table, parts, kind, refColumns, text, openable}],
    //  keys: [{name, kind ("Primary key" / "Unique"), columns}], ddl,
    //  tableType ("TABLE" | "VIEW" | "MATERIALIZED VIEW"), description, strictSupported, strict}.
    // The properties tab of a data tab (TablePropertiesView.qml) reads all of it.
    Q_INVOKABLE QVariantMap infoOf(const QString &tabId) const;
    // The foreign keys of the tab by column: {columnName: "Foreign key to customers.id"}.
    Q_INVOKABLE QVariantMap linkColumns(const QString &tabId) const;
    // The text to announce for a column of the tab that is part of a foreign key ("" = it is not).
    Q_INVOKABLE QString linkText(const QString &tabId, const QString &column) const;
    // "Go to related row" for the cell (row, column name) of a data tab. {started, message}: with
    // started = false nothing was opened and `message` says why (no foreign key there, a NULL key,
    // the structure not read yet, the filter refused). Otherwise the referenced table is checked
    // (describe) and opened or shown filtered by the key; the end is relatedOpened() or
    // relatedFailed(). A tab already open with the same filter is only shown.
    Q_INVOKABLE QVariantMap goToRelated(const QString &tabId, int row, const QString &column);
    // Drops what is cached for tabs that are gone.
    void prune();

signals:
    void connectionsChanged();
    void tabsChanged();
    void queriesChanged();
    // The structure of this tab changed state (loading, ready, failed).
    void changed(const QString &tabId);
    // For the screen reader, without moving the focus.
    void announcement(const QString &text, bool assertive);
    // The data tab `targetTabId` of the referenced table is now the current one.
    void relatedOpened(const QString &tabId, const QString &targetTabId);
    // The referenced table could not be opened; `message` is in plain language.
    void relatedFailed(const QString &tabId, const QString &message);

private:
    enum class State { Idle, Loading, Ready, Failed };
    struct Entry
    {
        std::optional<TableDetails> details;
        ObjectPath object;
        Error error;
        QString connectionId;
        QString ddl;
        // Only compared, never dereferenced: a destroyed session is forgotten by onSessionGone().
        const Session *session = nullptr;
        quint64 request = 0;
        qint64 waitedMs = 0;
        State state = State::Idle;
    };
    // The describe() of a referenced table that goToRelated() sent before opening its tab.
    struct Related
    {
        const Session *session = nullptr;
        quint64 request = 0;
        QString tabId; // the tab the user was in
        QString connectionId;
        QStringList parts;
        QString kind;
        QString origin;
        QVariantMap view;
    };

    void start(const QString &tabId, Entry &e);
    void fail(const QString &tabId, Entry &e, const Error &error);
    void retryWaiting();
    // Asks the tree's session; false when the connection is still opening (the poll tries again).
    bool describeNow(const QString &tabId, Entry &e);
    void onDescribed(const Session *session, quint64 requestId, const TableDetails &details);
    void onDescribeFailed(const Session *session, quint64 requestId, const Error &error);
    void watch(Session *session);
    void onSessionGone(const Session *session);
    void finishRelated(const Related &related, const std::optional<TableDetails> &details,
                       const Error *error);
    QString ddlOf(const Entry &e) const;
    const ISqlDialect *dialectOf(const QString &connectionId) const;
    QString providerIdOf(const QString &connectionId) const;
    std::optional<ObjectPath> objectOf(const QString &tabId, QString *connectionId) const;
    const ForeignKeyDef *foreignKeyOf(const Entry &e, const QString &column) const;
    static QString displayName(const ObjectPath &path);
    static QString kindName(ObjectKind kind);

    // Not QPointers: clang-analyzer (Qt 6.12) reports a false use-after-free in ~QWeakPointer;
    // cleared on destroyed() instead.
    ConnectionController *m_connections = nullptr;
    EditorTabsModel *m_tabs = nullptr;
    QueryController *m_queries = nullptr;
    QList<QMetaObject::Connection> m_links;
    std::map<QString, Entry> m_entries;
    QList<Related> m_related;
    QSet<const Session *> m_watched;
    QTimer m_poll; // while a tab waits for the connection to open
};

} // namespace quarry::ui
