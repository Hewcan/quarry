#pragma once

#include <quarry/object_path.h>
#include <quarry/result.h>
#include <quarry/session.h>

#include <QHash>
#include <QMetaObject>
#include <QObject>
#include <QString>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace quarry {

struct CachedObject
{
    ObjectPath path;
    QString name;
    QString detail; // as in SchemaNode: column type, "PK"...
};

// Per-connection metadata for autocompletion, loaded on demand through Session::children(). UI
// thread only; the answers of the Session arrive queued. It shares the Session with the schema
// tree: answers to requests it did not make are ignored.
class MetadataCache : public QObject
{
    Q_OBJECT

public:
    enum class State : uint8_t { Empty, Loading, Ready, Failed };

    explicit MetadataCache(QObject *parent = nullptr);
    ~MetadataCache() override;

    // Does not take ownership. Drops all data; nullptr disconnects and ignores late answers.
    void attach(Session *session);
    // Walks from the root through Database/Schema/Folder nodes and stops at Table/View/
    // MaterializedView, without loading their columns. Idempotent while loading or loaded; from
    // Failed it retries from scratch. Needs an attached session.
    void load();
    // Once per table (a failed request may be retried by calling again); changed() when it arrives.
    void ensureColumns(const ObjectPath &table);
    // Drops everything; the next load() starts from zero.
    void invalidate();
    State state() const;

    // Queries, no I/O. resolve() matches the trailing parts of the path without regard to case
    // ({"users"} or {"public", "users"}); with several matches the first in load order wins.
    std::optional<ObjectPath> resolve(const QStringList &parts) const;
    const std::vector<CachedObject> &relations() const; // tables, views, materialized views
    const std::vector<CachedObject> &schemas() const;
    // nullptr: not loaded (yet).
    const std::vector<CachedObject> *columns(const ObjectPath &table) const;

    // Addition to the contract, for CompletionEngine: lowercased names, parallel to relations(),
    // and the indexes of relations() sorted by (lowercased name, index) for prefix search.
    const std::vector<QString> &relationKeys() const;
    const std::vector<qsizetype> &relationOrder() const;

signals:
    void changed();
    void failed(quarry::Error error);

private:
    struct Pending
    {
        quint64 generation = 0;
        bool columns = false; // otherwise a walk step
        ObjectPath path; // columns: the table
    };
    struct Columns
    {
        bool loaded = false;
        std::vector<CachedObject> items;
    };

    void onReady(quint64 requestId, const ObjectPath &parent, const std::vector<SchemaNode> &nodes);
    void onFailed(quint64 requestId, const ObjectPath &parent, const Error &error);
    void requestWalk(const ObjectPath &parent);
    void finishWalk();
    void clearData();
    void detach();

    Session *m_session = nullptr; // not owned; cleared on destroyed()
    QList<QMetaObject::Connection> m_connections;
    State m_state = State::Empty;
    quint64 m_generation = 0; // bumped by attach/invalidate/failure: older answers are stale
    int m_walkPending = 0;
    std::unordered_map<quint64, Pending> m_pending;
    std::vector<CachedObject> m_relations;
    std::vector<CachedObject> m_schemas;
    std::vector<QString> m_keys;
    std::vector<qsizetype> m_order;
    QHash<QString, Columns> m_columns; // key: path parts joined with U+001F
};

} // namespace quarry
