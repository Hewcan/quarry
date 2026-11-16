#include "history_model.h"

#include "connection_controller.h"

#include <quarry/connection_store.h>

#include <QLocale>
#include <QUuid>

#include <algorithm>

namespace quarry::ui {

namespace {

constexpr qsizetype kPreviewChars = 160;

QString toId(const QUuid &id)
{
    return id.toString(QUuid::WithoutBraces);
}

QString durationText(qint64 ms)
{
    if (ms < 1000)
        return HistoryModel::tr("%1 ms").arg(ms);
    return HistoryModel::tr("%1 s").arg(
        QLocale().toString(static_cast<double>(ms) / 1000.0, 'f', 1));
}

QString rowsText(qint64 rows)
{
    if (rows < 0)
        return {};
    return rows == 1 ? HistoryModel::tr("1 row") : HistoryModel::tr("%1 rows").arg(rows);
}

QString countText(int n)
{
    return n == 1 ? HistoryModel::tr("1 history entry")
                  : HistoryModel::tr("%1 history entries").arg(n);
}

} // namespace

HistoryModel::HistoryModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kDefaultDebounceMs);
    connect(&m_debounce, &QTimer::timeout, this, &HistoryModel::refresh);
}

void HistoryModel::setDatabasePath(const QString &path)
{
    if (path == m_databasePath)
        return;
    m_databasePath = path;
    emit configChanged();
}

void HistoryModel::setConnections(ConnectionController *connections)
{
    if (connections == m_connections)
        return;
    if (m_connections)
        m_connections->disconnect(this);
    m_connections = connections;
    if (m_connections) {
        // The store deletes the entries of a deleted connection; what is listed follows.
        connect(m_connections, &ConnectionController::connectionRemoved, this, [this] {
            if (m_started)
                refresh();
        });
        connect(m_connections, &QObject::destroyed, this, [this] { m_connections = nullptr; });
    }
    emit configChanged();
}

QueryHistoryStore *HistoryModel::store()
{
    if (m_store)
        return m_store;
    m_store = new QueryHistoryStore(
        m_databasePath.isEmpty() ? ConnectionStore::defaultDatabasePath() : m_databasePath, this);
    connect(m_store, &QueryHistoryStore::searched, this, &HistoryModel::onSearched);
    connect(m_store, &QueryHistoryStore::recorded, this, &HistoryModel::onRecorded);
    connect(m_store, &QueryHistoryStore::removed, this, &HistoryModel::onRemoved);
    connect(m_store, &QueryHistoryStore::cleared, this, &HistoryModel::onCleared);
    connect(m_store, &QueryHistoryStore::failed, this, &HistoryModel::onStoreFailed);
    return m_store;
}

// ---- filters -----------------------------------------------------------------------------------

void HistoryModel::setText(const QString &text)
{
    if (text == m_text)
        return;
    m_text = text;
    emit filterChanged();
    if (m_started)
        m_debounce.start();
}

void HistoryModel::setConnectionFilter(const QString &id)
{
    if (id == m_connectionFilter)
        return;
    m_connectionFilter = id;
    emit filterChanged();
    if (m_started)
        refresh();
}

void HistoryModel::setOnlyErrors(bool on)
{
    if (on == m_onlyErrors)
        return;
    m_onlyErrors = on;
    emit filterChanged();
    if (m_started)
        refresh();
}

// ---- searching ---------------------------------------------------------------------------------

void HistoryModel::start()
{
    if (m_started)
        return;
    refresh();
}

void HistoryModel::refresh()
{
    m_debounce.stop();
    if (!m_started) {
        m_started = true;
        emit startedChanged();
    }
    search(false);
}

void HistoryModel::loadMore()
{
    if (m_started && m_hasMore && !m_loading)
        search(true);
}

void HistoryModel::search(bool append)
{
    HistoryQuery q;
    q.requestId = ++m_nextRequest;
    q.text = m_text.trimmed();
    if (!m_connectionFilter.isEmpty())
        q.connectionId = QUuid::fromString(m_connectionFilter);
    q.onlyErrors = m_onlyErrors;
    q.limit = kPageSize;
    q.offset = append ? count() : 0;
    m_latestRequest = q.requestId;
    m_latestAppends = append;
    setLoading(true);
    store()->search(q);
}

void HistoryModel::onSearched(quint64 requestId, const QList<HistoryEntry> &entries, bool hasMore)
{
    if (requestId != m_latestRequest)
        return; // the answer to a filter that was replaced
    setLoading(false);
    if (m_latestAppends) {
        if (!entries.isEmpty()) {
            const int first = count();
            beginInsertRows({}, first, first + static_cast<int>(entries.size()) - 1);
            m_rows.insert(m_rows.end(), entries.begin(), entries.end());
            endInsertRows();
            emit countChanged();
        }
    } else {
        beginResetModel();
        m_rows.assign(entries.begin(), entries.end());
        endResetModel();
        emit countChanged();
        emit announcement(entries.isEmpty() ? tr("No history entries") : countText(count()), false);
    }
    setHasMore(hasMore);
    emit searchFinished();
}

// ---- writing -----------------------------------------------------------------------------------

void HistoryModel::record(const HistoryEntry &entry)
{
    store()->record(entry);
}

bool HistoryModel::matches(const HistoryEntry &entry) const
{
    // shortcut: not checked against the text filter; the next search shows the entry.
    if (!m_text.trimmed().isEmpty())
        return false;
    if (!m_connectionFilter.isEmpty()
        && (!entry.connectionId || toId(*entry.connectionId) != m_connectionFilter))
        return false;
    return !m_onlyErrors || !entry.success;
}

void HistoryModel::onRecorded(const HistoryEntry &entry)
{
    if (!m_started || !matches(entry))
        return;
    beginInsertRows({}, 0, 0);
    m_rows.insert(m_rows.begin(), entry);
    endInsertRows();
    emit countChanged();
}

void HistoryModel::remove(int row)
{
    if (row < 0 || row >= count())
        return;
    store()->remove(m_rows[static_cast<std::size_t>(row)].id);
}

void HistoryModel::onRemoved(qint64 id)
{
    const auto it = std::ranges::find(m_rows, id, &HistoryEntry::id);
    if (it == m_rows.end())
        return;
    const int row = static_cast<int>(it - m_rows.begin());
    beginRemoveRows({}, row, row);
    m_rows.erase(it);
    endRemoveRows();
    emit countChanged();
    emit announcement(tr("History entry deleted"), false);
}

void HistoryModel::clear()
{
    std::optional<QUuid> id;
    if (!m_connectionFilter.isEmpty())
        id = QUuid::fromString(m_connectionFilter);
    store()->clear(id);
}

void HistoryModel::clearConnection(const QString &connectionId)
{
    const QUuid id = QUuid::fromString(connectionId);
    if (!id.isNull())
        store()->clear(id);
}

void HistoryModel::onCleared(const std::optional<QUuid> &connectionId, qint64 deleted)
{
    emit cleared(connectionId ? toId(*connectionId) : QString(), deleted);
    if (m_started)
        refresh();
    emit announcement(tr("History cleared"), false);
}

void HistoryModel::onStoreFailed(const Error &error)
{
    setLoading(false);
    emit failed(error.message, error.detail);
}

void HistoryModel::setLoading(bool loading)
{
    if (loading == m_loading)
        return;
    m_loading = loading;
    emit loadingChanged();
}

void HistoryModel::setHasMore(bool hasMore)
{
    if (hasMore == m_hasMore)
        return;
    m_hasMore = hasMore;
    emit hasMoreChanged();
}

// ---- model -------------------------------------------------------------------------------------

QString HistoryModel::connectionName(const QUuid &id) const
{
    if (m_connections == nullptr || id.isNull())
        return {};
    ConnectionListModel *list = m_connections->model();
    const int row = list->indexOf(toId(id));
    return row < 0 ? QString() : list->index(row, 0).data(ConnectionListModel::NameRole).toString();
}

QString HistoryModel::sqlOf(int row) const
{
    return row >= 0 && row < count() ? m_rows[static_cast<std::size_t>(row)].sql : QString();
}

int HistoryModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant HistoryModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid))
        return {};
    const HistoryEntry &e = m_rows[static_cast<std::size_t>(index.row())];
    switch (role) {
    case IdRole:
        return e.id;
    case SqlRole:
        return e.sql;
    case PreviewRole: {
        QString one = e.sql.simplified();
        if (one.size() > kPreviewChars)
            one = one.left(kPreviewChars) + QStringLiteral("…");
        return one;
    }
    case ConnectionIdRole:
        return e.connectionId ? toId(*e.connectionId) : QString();
    case ConnectionNameRole:
        return e.connectionId ? connectionName(*e.connectionId) : tr("Deleted connection");
    case WhenRole:
        return QLocale().toString(e.executedAt.toLocalTime(), QLocale::ShortFormat);
    case DetailRole: {
        QStringList parts;
        if (!e.success)
            parts << tr("Failed");
        parts << durationText(e.durationMs);
        if (const QString r = rowsText(e.rows); !r.isEmpty())
            parts << r;
        return parts.join(QStringLiteral(" · "));
    }
    case SuccessRole:
        return e.success;
    case ErrorMessageRole:
        return e.errorMessage;
    default:
        return {};
    }
}

QHash<int, QByteArray> HistoryModel::roleNames() const
{
    return { { IdRole, "entryId" },
             { SqlRole, "sql" },
             { PreviewRole, "preview" },
             { ConnectionIdRole, "connectionId" },
             { ConnectionNameRole, "connectionName" },
             { WhenRole, "when" },
             { DetailRole, "detail" },
             { SuccessRole, "success" },
             { ErrorMessageRole, "errorMessage" } };
}

QVariantMap HistoryModel::get(int row) const
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
