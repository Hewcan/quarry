#pragma once

#include <quarry/saved_folder.h>
#include <quarry/saved_query.h>
#include <quarry/saved_query_store.h>

#include <QAbstractListModel>
#include <QSet>
#include <QString>
#include <QUuid>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <vector>

namespace quarry::ui {

class ConnectionController;

// The saved queries and their folders (ARCHITECTURE section 11.3) as a two-level tree flattened
// into one list, so a ListView can show it and the keyboard can walk it: folders first (by
// position), each followed by its queries while it is expanded, then the queries of the top
// level. Owns the SavedQueryStore, created by start() (the first time the panel opens).
//
// Changes go to the store and the list follows the store's answer: nothing is shown that was not
// stored. Expanding and collapsing only insert or remove the rows of that folder (no reset).
class SavedQueriesModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(
        QString databasePath READ databasePath WRITE setDatabasePath NOTIFY configChanged FINAL)
    // Gives the connection names.
    Q_PROPERTY(quarry::ui::ConnectionController *connections READ connections WRITE setConnections
                   NOTIFY configChanged FINAL)
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
    Q_PROPERTY(int queryCount READ queryCount NOTIFY countChanged FINAL)
    Q_PROPERTY(int folderCount READ folderCount NOTIFY countChanged FINAL)
    Q_PROPERTY(bool started READ started NOTIFY startedChanged FINAL)
    // Started and the first answer has not come yet.
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged FINAL)

public:
    enum Role {
        KindRole = Qt::UserRole + 1, // "folder" | "query"
        IdRole,
        NameRole,
        SqlRole, // queries only
        PreviewRole,
        DepthRole, // 0 = top level, 1 = inside a folder
        ExpandedRole, // folders only
        ChildCountRole, // folders only
        ConnectionIdRole, // "" = any connection
        ConnectionNameRole,
        FolderIdRole, // queries: "" = top level
        PositionRole, // 1-based among its siblings
        SiblingCountRole,
    };

    explicit SavedQueriesModel(QObject *parent = nullptr);

    QString databasePath() const { return m_databasePath; }
    void setDatabasePath(const QString &path);
    ConnectionController *connections() const { return m_connections; }
    void setConnections(ConnectionController *connections);
    int count() const { return static_cast<int>(m_rows.size()); }
    int queryCount() const { return static_cast<int>(m_queries.size()); }
    int folderCount() const { return static_cast<int>(m_folders.size()); }
    bool started() const { return m_started; }
    bool loading() const { return m_started && !m_loaded; }

    // Creates the store and loads. Idempotent.
    Q_INVOKABLE void start();

    Q_INVOKABLE void setExpanded(int row, bool expanded);
    // New query (the folder id "" = top level, the connection id "" = any connection).
    // false when the name is empty.
    Q_INVOKABLE bool saveQuery(const QString &name, const QString &sql, const QString &connectionId,
                               const QString &folderId);
    Q_INVOKABLE bool renameQuery(const QString &id, const QString &name);
    // "" = top level. The folder opens so the query is seen.
    Q_INVOKABLE void moveQuery(const QString &id, const QString &folderId);
    Q_INVOKABLE void removeQuery(const QString &id);
    Q_INVOKABLE bool newFolder(const QString &name);
    Q_INVOKABLE bool renameFolder(const QString &id, const QString &name);
    // Its queries go to the top level.
    Q_INVOKABLE void removeFolder(const QString &id);

    // The folders, for the "Move to" menu: [{id, name}].
    Q_INVOKABLE QVariantList folders() const;
    Q_INVOKABLE int queriesIn(const QString &folderId) const;
    Q_INVOKABLE int indexOfId(const QString &id) const;
    Q_INVOKABLE QVariantMap get(int row) const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // The store, created on first use. Tests wait on its signals.
    quarry::SavedQueryStore *store();

signals:
    void configChanged();
    void countChanged();
    void startedChanged();
    void loadingChanged();
    // For the screen reader, without moving the focus.
    void announcement(const QString &text, bool assertive);
    // The list was rebuilt after an answer of the store, or loaded.
    void changed();
    // A query was stored (a new one or a changed one).
    void querySaved(const QString &id, const QString &name);
    void folderSaved(const QString &id, const QString &name);
    void failed(const QString &message, const QString &detail);

private:
    struct Row
    {
        bool folder = false;
        int index = 0; // into m_folders or m_queries
        int depth = 0;
        int position = 1;
        int siblings = 1;
    };

    void onLoaded(const QList<quarry::SavedFolder> &folders,
                  const QList<quarry::SavedQuery> &queries);
    void onQuerySaved(const quarry::SavedQuery &query);
    void onQueryRemoved(const QUuid &id);
    void onFolderSaved(const quarry::SavedFolder &folder);
    void onFolderRemoved(const QUuid &id, const QList<quarry::SavedQuery> &moved);
    void onStoreFailed(const quarry::Error &error);
    void rebuild();
    std::vector<Row> childRows(int folderIndex) const;
    std::vector<int> queriesOfFolder(const QUuid &id) const;
    bool folderKnown(const std::optional<QUuid> &id) const;
    int folderIndex(const QUuid &id) const;
    int queryIndex(const QUuid &id) const;
    QString folderName(const std::optional<QUuid> &id) const;
    QString connectionName(const std::optional<QUuid> &id) const;
    int nextPosition(const std::optional<QUuid> &folderId) const;
    void sortAll();

    QString m_databasePath;
    ConnectionController *m_connections = nullptr; // cleared by its destroyed() signal
    quarry::SavedQueryStore *m_store = nullptr;
    std::vector<quarry::SavedFolder> m_folders;
    std::vector<quarry::SavedQuery> m_queries;
    QSet<QUuid> m_expanded;
    std::vector<Row> m_rows;
    bool m_started = false;
    bool m_loaded = false;
};

} // namespace quarry::ui
