#include "saved_queries_model.h"

#include "connection_controller.h"

#include <quarry/connection_store.h>

#include <algorithm>

namespace quarry::ui {

namespace {

constexpr qsizetype kPreviewChars = 120;

QString toId(const QUuid &id)
{
    return id.toString(QUuid::WithoutBraces);
}

std::optional<QUuid> optionalId(const QString &text)
{
    if (text.isEmpty())
        return std::nullopt;
    const QUuid id = QUuid::fromString(text);
    if (id.isNull())
        return std::nullopt;
    return id;
}

template<typename T> bool before(const T &a, const T &b)
{
    if (a.position != b.position)
        return a.position < b.position;
    return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
}

} // namespace

SavedQueriesModel::SavedQueriesModel(QObject *parent)
    : QAbstractListModel(parent)
{ }

void SavedQueriesModel::setDatabasePath(const QString &path)
{
    if (path == m_databasePath)
        return;
    m_databasePath = path;
    emit configChanged();
}

void SavedQueriesModel::setConnections(ConnectionController *connections)
{
    if (connections == m_connections)
        return;
    if (m_connections)
        m_connections->disconnect(this);
    m_connections = connections;
    if (m_connections) {
        // Deleting a connection clears the association of its saved queries in the store.
        connect(m_connections, &ConnectionController::connectionRemoved, this, [this] {
            if (m_started)
                store()->load();
        });
        connect(m_connections, &QObject::destroyed, this, [this] { m_connections = nullptr; });
    }
    emit configChanged();
}

SavedQueryStore *SavedQueriesModel::store()
{
    if (m_store)
        return m_store;
    m_store = new SavedQueryStore(
        m_databasePath.isEmpty() ? ConnectionStore::defaultDatabasePath() : m_databasePath, this);
    connect(m_store, &SavedQueryStore::loaded, this, &SavedQueriesModel::onLoaded);
    connect(m_store, &SavedQueryStore::querySaved, this, &SavedQueriesModel::onQuerySaved);
    connect(m_store, &SavedQueryStore::queryRemoved, this, &SavedQueriesModel::onQueryRemoved);
    connect(m_store, &SavedQueryStore::folderSaved, this, &SavedQueriesModel::onFolderSaved);
    connect(m_store, &SavedQueryStore::folderRemoved, this, &SavedQueriesModel::onFolderRemoved);
    connect(m_store, &SavedQueryStore::failed, this, &SavedQueriesModel::onStoreFailed);
    return m_store;
}

void SavedQueriesModel::start()
{
    if (m_started)
        return;
    m_started = true;
    emit startedChanged();
    emit loadingChanged();
    store()->load();
}

// ---- structure ---------------------------------------------------------------------------------

int SavedQueriesModel::folderIndex(const QUuid &id) const
{
    const auto it = std::ranges::find(m_folders, id, &SavedFolder::id);
    return it == m_folders.end() ? -1 : static_cast<int>(it - m_folders.begin());
}

int SavedQueriesModel::queryIndex(const QUuid &id) const
{
    const auto it = std::ranges::find(m_queries, id, &SavedQuery::id);
    return it == m_queries.end() ? -1 : static_cast<int>(it - m_queries.begin());
}

bool SavedQueriesModel::folderKnown(const std::optional<QUuid> &id) const
{
    return id.has_value() && folderIndex(*id) >= 0;
}

std::vector<int> SavedQueriesModel::queriesOfFolder(const QUuid &id) const
{
    std::vector<int> out;
    for (int i = 0; i < queryCount(); ++i)
        if (m_queries[static_cast<std::size_t>(i)].folderId == id)
            out.push_back(i);
    return out;
}

std::vector<SavedQueriesModel::Row> SavedQueriesModel::childRows(int folderIdx) const
{
    const auto children = queriesOfFolder(m_folders[static_cast<std::size_t>(folderIdx)].id);
    std::vector<Row> rows;
    rows.reserve(children.size());
    int pos = 1;
    for (const int q : children)
        rows.push_back({ false, q, 1, pos++, static_cast<int>(children.size()) });
    return rows;
}

void SavedQueriesModel::sortAll()
{
    std::ranges::stable_sort(m_folders, before<SavedFolder>);
    std::ranges::stable_sort(m_queries, before<SavedQuery>);
}

void SavedQueriesModel::rebuild()
{
    beginResetModel();
    m_rows.clear();
    std::vector<int> top;
    for (int i = 0; i < queryCount(); ++i)
        if (!folderKnown(m_queries[static_cast<std::size_t>(i)].folderId))
            top.push_back(i);
    const int siblings = folderCount() + static_cast<int>(top.size());
    int pos = 1;
    for (int f = 0; f < folderCount(); ++f) {
        m_rows.push_back({ true, f, 0, pos++, siblings });
        if (m_expanded.contains(m_folders[static_cast<std::size_t>(f)].id)) {
            const auto kids = childRows(f);
            m_rows.insert(m_rows.end(), kids.begin(), kids.end());
        }
    }
    for (const int q : top)
        m_rows.push_back({ false, q, 0, pos++, siblings });
    endResetModel();
    emit countChanged();
    emit changed();
}

void SavedQueriesModel::setExpanded(int row, bool expanded)
{
    if (row < 0 || row >= count() || !m_rows[static_cast<std::size_t>(row)].folder)
        return;
    const SavedFolder &f
        = m_folders[static_cast<std::size_t>(m_rows[static_cast<std::size_t>(row)].index)];
    if (m_expanded.contains(f.id) == expanded)
        return;
    if (expanded) {
        m_expanded.insert(f.id);
        const auto kids = childRows(m_rows[static_cast<std::size_t>(row)].index);
        if (!kids.empty()) {
            beginInsertRows({}, row + 1, row + static_cast<int>(kids.size()));
            m_rows.insert(m_rows.begin() + row + 1, kids.begin(), kids.end());
            endInsertRows();
            emit countChanged();
        }
    } else {
        m_expanded.remove(f.id);
        int last = row;
        while (last + 1 < count() && m_rows[static_cast<std::size_t>(last) + 1].depth == 1)
            ++last;
        if (last > row) {
            beginRemoveRows({}, row + 1, last);
            m_rows.erase(m_rows.begin() + row + 1, m_rows.begin() + last + 1);
            endRemoveRows();
            emit countChanged();
        }
    }
    emit dataChanged(index(row, 0), index(row, 0), { ExpandedRole });
}

// ---- operations --------------------------------------------------------------------------------

int SavedQueriesModel::nextPosition(const std::optional<QUuid> &folderId) const
{
    int next = 0;
    for (const SavedQuery &q : m_queries)
        if (q.folderId == folderId)
            next = std::max(next, q.position + 1);
    return next;
}

bool SavedQueriesModel::saveQuery(const QString &name, const QString &sql,
                                  const QString &connectionId, const QString &folderId)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return false;
    start(); // the load is queued before this save, so the list is complete when it answers
    SavedQuery q;
    q.name = trimmed;
    q.sql = sql;
    q.connectionId = optionalId(connectionId);
    q.folderId = optionalId(folderId);
    // shortcut: before the first load the position is 0 (ties sort by name); only a save that
    // beats the panel's first opening can see it.
    q.position = nextPosition(q.folderId);
    store()->saveQuery(q);
    return true;
}

bool SavedQueriesModel::renameQuery(const QString &id, const QString &name)
{
    const QString trimmed = name.trimmed();
    const int i = queryIndex(QUuid::fromString(id));
    if (trimmed.isEmpty() || i < 0)
        return false;
    SavedQuery q = m_queries[static_cast<std::size_t>(i)];
    q.name = trimmed;
    store()->saveQuery(q);
    return true;
}

void SavedQueriesModel::moveQuery(const QString &id, const QString &folderId)
{
    const int i = queryIndex(QUuid::fromString(id));
    if (i < 0)
        return;
    SavedQuery q = m_queries[static_cast<std::size_t>(i)];
    const auto target = optionalId(folderId);
    if (q.folderId == target)
        return;
    q.folderId = target;
    q.position = nextPosition(target);
    store()->saveQuery(q);
}

void SavedQueriesModel::removeQuery(const QString &id)
{
    const QUuid uuid = QUuid::fromString(id);
    if (!uuid.isNull())
        store()->removeQuery(uuid);
}

bool SavedQueriesModel::newFolder(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return false;
    start();
    SavedFolder f;
    f.name = trimmed;
    f.position = folderCount();
    store()->saveFolder(f);
    return true;
}

bool SavedQueriesModel::renameFolder(const QString &id, const QString &name)
{
    const QString trimmed = name.trimmed();
    const int i = folderIndex(QUuid::fromString(id));
    if (trimmed.isEmpty() || i < 0)
        return false;
    SavedFolder f = m_folders[static_cast<std::size_t>(i)];
    f.name = trimmed;
    store()->saveFolder(f);
    return true;
}

void SavedQueriesModel::removeFolder(const QString &id)
{
    const QUuid uuid = QUuid::fromString(id);
    if (!uuid.isNull())
        store()->removeFolder(uuid);
}

// ---- answers of the store ----------------------------------------------------------------------

QString SavedQueriesModel::folderName(const std::optional<QUuid> &id) const
{
    const int i = id ? folderIndex(*id) : -1;
    return i < 0 ? QString() : m_folders[static_cast<std::size_t>(i)].name;
}

void SavedQueriesModel::onLoaded(const QList<SavedFolder> &folders,
                                 const QList<SavedQuery> &queries)
{
    m_folders.assign(folders.begin(), folders.end());
    m_queries.assign(queries.begin(), queries.end());
    // Folders that vanished must not stay "expanded".
    for (auto it = m_expanded.begin(); it != m_expanded.end();) {
        it = folderIndex(*it) >= 0 ? std::next(it) : m_expanded.erase(it);
    }
    sortAll();
    const bool wasLoading = loading();
    m_loaded = true;
    rebuild();
    if (wasLoading)
        emit loadingChanged();
}

void SavedQueriesModel::onQuerySaved(const SavedQuery &query)
{
    const int i = queryIndex(query.id);
    QString text;
    if (i < 0) {
        text = tr("Query “%1” saved").arg(query.name);
        m_queries.push_back(query);
    } else {
        const SavedQuery &old = m_queries[static_cast<std::size_t>(i)];
        if (old.folderId != query.folderId)
            text = query.folderId
                ? tr("“%1” moved to folder %2").arg(query.name, folderName(query.folderId))
                : tr("“%1” moved to the top level").arg(query.name);
        else if (old.name != query.name)
            text = tr("Renamed to “%1”").arg(query.name);
        else
            text = tr("Query “%1” saved").arg(query.name);
        m_queries[static_cast<std::size_t>(i)] = query;
    }
    if (query.folderId && folderKnown(query.folderId))
        m_expanded.insert(*query.folderId); // so the query shows where it went
    sortAll();
    rebuild();
    emit querySaved(toId(query.id), query.name);
    emit announcement(text, false);
}

void SavedQueriesModel::onQueryRemoved(const QUuid &id)
{
    const int i = queryIndex(id);
    if (i < 0)
        return;
    m_queries.erase(m_queries.begin() + i);
    rebuild();
    emit announcement(tr("Saved query deleted"), false);
}

void SavedQueriesModel::onFolderSaved(const SavedFolder &folder)
{
    const int i = folderIndex(folder.id);
    QString text;
    if (i < 0) {
        text = tr("Folder “%1” created").arg(folder.name);
        m_folders.push_back(folder);
    } else {
        text = tr("Folder renamed to “%1”").arg(folder.name);
        m_folders[static_cast<std::size_t>(i)] = folder;
    }
    sortAll();
    rebuild();
    emit folderSaved(toId(folder.id), folder.name);
    emit announcement(text, false);
}

void SavedQueriesModel::onFolderRemoved(const QUuid &id, const QList<SavedQuery> &moved)
{
    const int i = folderIndex(id);
    if (i < 0)
        return;
    m_folders.erase(m_folders.begin() + i);
    m_expanded.remove(id);
    for (const SavedQuery &q : moved) {
        if (const int k = queryIndex(q.id); k >= 0)
            m_queries[static_cast<std::size_t>(k)] = q;
    }
    for (SavedQuery &q : m_queries) // defensive: nothing may point to a folder that is gone
        if (q.folderId == id)
            q.folderId.reset();
    sortAll();
    rebuild();
    const qsizetype n = moved.size();
    emit announcement(n == 0       ? tr("Folder deleted")
                          : n == 1 ? tr("Folder deleted. 1 query moved to the top level")
                                   : tr("Folder deleted. %1 queries moved to the top level").arg(n),
                      false);
}

void SavedQueriesModel::onStoreFailed(const Error &error)
{
    if (loading()) {
        m_loaded = true;
        emit loadingChanged();
    }
    emit failed(error.message, error.detail);
    emit announcement(error.message, true);
}

// ---- model -------------------------------------------------------------------------------------

QString SavedQueriesModel::connectionName(const std::optional<QUuid> &id) const
{
    if (m_connections == nullptr || !id)
        return {};
    ConnectionListModel *list = m_connections->model();
    const int row = list->indexOf(toId(*id));
    return row < 0 ? QString() : list->index(row, 0).data(ConnectionListModel::NameRole).toString();
}

QVariantList SavedQueriesModel::folders() const
{
    QVariantList out;
    out.reserve(folderCount());
    for (const SavedFolder &f : m_folders)
        out.push_back(QVariantMap { { QStringLiteral("id"), toId(f.id) },
                                    { QStringLiteral("name"), f.name } });
    return out;
}

int SavedQueriesModel::queriesIn(const QString &folderId) const
{
    const QUuid id = QUuid::fromString(folderId);
    return static_cast<int>(queriesOfFolder(id).size());
}

int SavedQueriesModel::indexOfId(const QString &id) const
{
    const QUuid uuid = QUuid::fromString(id);
    if (uuid.isNull())
        return -1;
    for (int r = 0; r < count(); ++r) {
        const Row &row = m_rows[static_cast<std::size_t>(r)];
        const QUuid &own = row.folder ? m_folders[static_cast<std::size_t>(row.index)].id
                                      : m_queries[static_cast<std::size_t>(row.index)].id;
        if (own == uuid)
            return r;
    }
    return -1;
}

int SavedQueriesModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant SavedQueriesModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid))
        return {};
    const Row &row = m_rows[static_cast<std::size_t>(index.row())];
    const SavedFolder *folder
        = row.folder ? &m_folders[static_cast<std::size_t>(row.index)] : nullptr;
    const SavedQuery *query
        = row.folder ? nullptr : &m_queries[static_cast<std::size_t>(row.index)];
    switch (role) {
    case KindRole:
        return row.folder ? QStringLiteral("folder") : QStringLiteral("query");
    case IdRole:
        return toId(row.folder ? folder->id : query->id);
    case NameRole:
        return row.folder ? folder->name : query->name;
    case SqlRole:
        return row.folder ? QString() : query->sql;
    case PreviewRole: {
        if (row.folder)
            return QString();
        QString one = query->sql.simplified();
        if (one.size() > kPreviewChars)
            one = one.left(kPreviewChars) + QStringLiteral("…");
        return one;
    }
    case DepthRole:
        return row.depth;
    case ExpandedRole:
        return row.folder && m_expanded.contains(folder->id);
    case ChildCountRole:
        return row.folder ? static_cast<int>(queriesOfFolder(folder->id).size()) : 0;
    case ConnectionIdRole:
        return !row.folder && query->connectionId ? toId(*query->connectionId) : QString();
    case ConnectionNameRole:
        return row.folder ? QString() : connectionName(query->connectionId);
    case FolderIdRole:
        return !row.folder && query->folderId && folderKnown(query->folderId)
            ? toId(*query->folderId)
            : QString();
    case PositionRole:
        return row.position;
    case SiblingCountRole:
        return row.siblings;
    default:
        return {};
    }
}

QHash<int, QByteArray> SavedQueriesModel::roleNames() const
{
    return { { KindRole, "kind" },
             { IdRole, "itemId" },
             { NameRole, "name" },
             { SqlRole, "sql" },
             { PreviewRole, "preview" },
             { DepthRole, "depth" },
             { ExpandedRole, "expanded" },
             { ChildCountRole, "childCount" },
             { ConnectionIdRole, "connectionId" },
             { ConnectionNameRole, "connectionName" },
             { FolderIdRole, "folderId" },
             { PositionRole, "position" },
             { SiblingCountRole, "siblings" } };
}

QVariantMap SavedQueriesModel::get(int row) const
{
    QVariantMap map;
    if (row < 0 || row >= count())
        return map;
    const QHash<int, QByteArray> names = roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it)
        map.insert(QString::fromUtf8(it.value()), index(row, 0).data(it.key()));
    return map;
}

} // namespace quarry::ui
