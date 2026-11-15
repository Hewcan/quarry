#include "quarry/metadata_cache.h"

#include <algorithm>

namespace quarry {
namespace {

const QString kColumnsFolder = QStringLiteral("Columns");

QString pathKey(const QStringList &parts)
{
    return parts.join(QChar(0x1f));
}

bool endsWithParts(const QStringList &path, const QStringList &parts)
{
    if (path.size() < parts.size()) {
        return false;
    }
    const qsizetype offset = path.size() - parts.size();
    for (qsizetype i = 0; i < parts.size(); ++i) {
        if (path.at(offset + i).compare(parts.at(i), Qt::CaseInsensitive) != 0) {
            return false;
        }
    }
    return true;
}

} // namespace

MetadataCache::MetadataCache(QObject *parent)
    : QObject(parent)
{ }

MetadataCache::~MetadataCache() = default;

void MetadataCache::detach()
{
    for (const QMetaObject::Connection &c : std::as_const(m_connections)) {
        disconnect(c);
    }
    m_connections.clear();
    m_session = nullptr;
}

void MetadataCache::attach(Session *session)
{
    detach();
    m_pending.clear();
    clearData();
    m_session = session;
    if (session == nullptr) {
        return;
    }
    m_connections.push_back(
        connect(session, &Session::childrenReady, this,
                [this](quint64 id, const ObjectPath &parent, const std::vector<SchemaNode> &nodes) {
                    onReady(id, parent, nodes);
                }));
    m_connections.push_back(connect(session, &Session::childrenFailed, this,
                                    [this](quint64 id, const ObjectPath &parent,
                                           const Error &error) { onFailed(id, parent, error); }));
    m_connections.push_back(connect(session, &QObject::destroyed, this, [this] {
        m_connections.clear(); // the sender is gone: nothing to disconnect
        m_session = nullptr;
        m_pending.clear();
        clearData();
    }));
}

// Bumps the generation, so every answer still on its way is ignored.
void MetadataCache::clearData()
{
    ++m_generation;
    const bool hadData = m_state != State::Empty || !m_relations.empty() || !m_columns.isEmpty();
    m_state = State::Empty;
    m_walkPending = 0;
    m_relations.clear();
    m_schemas.clear();
    m_keys.clear();
    m_order.clear();
    m_columns.clear();
    if (hadData) {
        emit changed();
    }
}

void MetadataCache::invalidate()
{
    clearData();
}

void MetadataCache::load()
{
    if (m_session == nullptr || m_state == State::Loading || m_state == State::Ready) {
        return;
    }
    if (m_state == State::Failed) {
        clearData();
    }
    m_state = State::Loading;
    requestWalk(ObjectPath { {}, ObjectKind::Database });
}

void MetadataCache::requestWalk(const ObjectPath &parent)
{
    const quint64 id = m_session->children(parent);
    m_pending.insert({ id, Pending { m_generation, false, parent } });
    ++m_walkPending;
}

void MetadataCache::ensureColumns(const ObjectPath &table)
{
    if (m_session == nullptr) {
        return;
    }
    const QString key = pathKey(table.parts);
    if (m_columns.contains(key)) {
        return;
    }
    m_columns.insert(key, Columns {});
    const quint64 id = m_session->children(
        ObjectPath { table.parts + QStringList { kColumnsFolder }, ObjectKind::Folder });
    m_pending.insert({ id, Pending { m_generation, true, table } });
}

void MetadataCache::onReady(quint64 requestId, const ObjectPath &parent,
                            const std::vector<SchemaNode> &nodes)
{
    const auto it = m_pending.find(requestId);
    if (it == m_pending.end()) {
        return; // someone else's request (the schema tree shares the session)
    }
    const Pending pending = it->second;
    m_pending.erase(it);
    if (pending.generation != m_generation) {
        return; // invalidated or re-attached since the request
    }

    if (pending.columns) {
        Columns &entry = m_columns[pathKey(pending.path.parts)];
        entry.loaded = true;
        for (const SchemaNode &node : nodes) {
            if (node.path.kind == ObjectKind::Column) {
                entry.items.push_back({ node.path, node.name, node.detail });
            }
        }
        emit changed();
        return;
    }

    --m_walkPending;
    bool hasSchemas = false;
    for (const SchemaNode &node : nodes) {
        switch (node.path.kind) {
        case ObjectKind::Schema:
            hasSchemas = true;
            m_schemas.push_back({ node.path, node.name, node.detail });
            requestWalk(node.path);
            break;
        case ObjectKind::Database:
        case ObjectKind::Folder:
            requestWalk(node.path);
            break;
        case ObjectKind::Table:
        case ObjectKind::View:
        case ObjectKind::MaterializedView:
            m_relations.push_back({ node.path, node.name, node.detail });
            break;
        default:
            break;
        }
    }
    // shortcut: engines without schemas (SQLite) put the tables straight under the database, so the
    // database acts as the schema ("main."); upgrade if an engine has both levels with tables at
    // each.
    if (parent.kind == ObjectKind::Database && !parent.parts.isEmpty() && !hasSchemas) {
        m_schemas.push_back({ parent, parent.parts.last(), QString() });
    }
    if (m_walkPending == 0) {
        finishWalk();
    }
}

void MetadataCache::finishWalk()
{
    const size_t n = m_relations.size();
    m_keys.clear();
    m_keys.reserve(n);
    for (const CachedObject &object : m_relations) {
        m_keys.push_back(object.name.toLower());
    }
    m_order.resize(n);
    for (size_t i = 0; i < n; ++i) {
        m_order[i] = static_cast<qsizetype>(i);
    }
    std::sort(m_order.begin(), m_order.end(), [this](qsizetype a, qsizetype b) {
        const QString &ka = m_keys[static_cast<size_t>(a)];
        const QString &kb = m_keys[static_cast<size_t>(b)];
        return ka == kb ? a < b : ka < kb;
    });
    m_state = State::Ready;
    emit changed();
}

void MetadataCache::onFailed(quint64 requestId, const ObjectPath &, const Error &error)
{
    const auto it = m_pending.find(requestId);
    if (it == m_pending.end()) {
        return;
    }
    const Pending pending = it->second;
    m_pending.erase(it);
    if (pending.generation != m_generation) {
        return;
    }
    if (pending.columns) {
        // The tables stay usable; calling ensureColumns() again retries.
        m_columns.remove(pathKey(pending.path.parts));
        emit failed(error);
        return;
    }
    clearData(); // drops the partial walk and ignores its remaining answers
    m_state = State::Failed;
    emit failed(error);
}

MetadataCache::State MetadataCache::state() const
{
    return m_state;
}

std::optional<ObjectPath> MetadataCache::resolve(const QStringList &parts) const
{
    if (parts.isEmpty()) {
        return std::nullopt;
    }
    const QString key = parts.last().toLower();
    auto it = std::lower_bound(
        m_order.begin(), m_order.end(), key,
        [this](qsizetype i, const QString &k) { return m_keys[static_cast<size_t>(i)] < k; });
    // Equal keys are ordered by index, so the first match is the first in load order.
    for (; it != m_order.end() && m_keys[static_cast<size_t>(*it)] == key; ++it) {
        const ObjectPath &path = m_relations[static_cast<size_t>(*it)].path;
        if (endsWithParts(path.parts, parts)) {
            return path;
        }
    }
    return std::nullopt;
}

const std::vector<CachedObject> &MetadataCache::relations() const
{
    return m_relations;
}

const std::vector<CachedObject> &MetadataCache::schemas() const
{
    return m_schemas;
}

const std::vector<CachedObject> *MetadataCache::columns(const ObjectPath &table) const
{
    const auto it = m_columns.constFind(pathKey(table.parts));
    return it != m_columns.constEnd() && it->loaded ? &it->items : nullptr;
}

const std::vector<QString> &MetadataCache::relationKeys() const
{
    return m_keys;
}

const std::vector<qsizetype> &MetadataCache::relationOrder() const
{
    return m_order;
}

} // namespace quarry
