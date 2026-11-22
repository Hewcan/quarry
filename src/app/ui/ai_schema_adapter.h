#pragma once

#include <quarry/ai/schema_context.h>
#include <quarry/metadata_cache.h>
#include <quarry/object_path.h>
#include <quarry/provider_api.h>
#include <quarry/result.h>
#include <quarry/session.h>

#include <QHash>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <memory>
#include <unordered_map>
#include <vector>

namespace quarry::ui {

// Turns what the app already knows about the STRUCTURE of a connection into a
// quarry::ai::SchemaContext for the AI assistant (H13 phase B, ARCHITECTURE section 6.13). Only
// structure: the tables and views, their columns, keys and indexes. It never reads a row, never
// runs a statement and never includes the headers of results or of execution; it asks for the same
// introspection as the schema tree (Session::describe), asynchronously, so the UI thread never
// waits. The description of each table is the provider's own (Session::describe): columns with
// their types, nullability and defaults, the primary key, the foreign keys with the local AND the
// referenced columns, and the indexes. Comments, CHECK constraints, enum values and definitions are
// not part of TableDetails, so they stay empty until a provider gives them; quarry::ai decides with
// ContextOptions what reaches the prompt.
//
// The list of tables comes from a MetadataCache (the connection's own when given and ready, else a
// temporary one that walks the tree); up to `limit` tables are then described through the same
// session.
class AiSchemaAdapter : public QObject
{
    Q_OBJECT

public:
    // At most this many tables are described: a schema with thousands of tables would be thousands
    // of requests for a prompt that holds 40 (ContextOptions::maxTables is 1..200).
    static constexpr int kMaxTables = 200;

    explicit AiSchemaAdapter(QObject *parent = nullptr);
    ~AiSchemaAdapter() override;

    // "sqlite" | "postgresql" | "mysql" | "sqlserver" for the provider id; "sql" for an unknown
    // one.
    static QString dialectOf(const QString &providerId);

    // The conversion of a provider's own description of one table (IConnection::describe). Used
    // where a description exists; the live path below rebuilds the same thing from the tree's
    // nodes.
    static quarry::ai::AiTable tableFromDetails(const QString &schema, const QString &name,
                                                bool isView, const quarry::TableDetails &details);

    struct Request
    {
        // Tables are described in this order of preference: first these names (the ones the script
        // or the question mention), then the load order. Case-insensitive, "table" or
        // "schema.table".
        QStringList prioritize;
        // The relations (and only for them) to skip, as in ContextOptions: they are never even
        // asked.
        quarry::ai::ContextOptions options;
        QString providerId;
        QPointer<quarry::Session> session; // the connection's metadata session
        const quarry::MetadataCache *cache = nullptr; // optional, not owned
        int limit = kMaxTables; // how many tables to describe at most, 1..kMaxTables
    };

    // Starts building the context of a connection; returns the request id (> 0). The answer is
    // contextReady or contextFailed with that id, never inside this call.
    quint64 contextFor(const Request &request);
    // Stops a request: its answers are ignored and contextCancelled(id) follows.
    void cancel(quint64 id);
    bool isRunning(quint64 id) const { return m_requests.count(id) > 0; }

signals:
    // `truncated`: the connection has more tables than were described (kMaxTables or `limit`).
    void contextReady(quint64 id, quarry::ai::SchemaContext context, bool truncated);
    void contextFailed(quint64 id, QString message, QString detail);
    void contextCancelled(quint64 id);

private:
    struct Rel
    {
        quarry::ObjectPath path;
        QString schema;
        QString name;
        bool isView = false;
        bool failed = false;
        quarry::ai::AiTable table;
    };
    struct Req
    {
        quint64 id = 0;
        QPointer<quarry::Session> session;
        QPointer<quarry::MetadataCache> ownCache;
        quarry::ai::ContextOptions options;
        QStringList prioritize;
        QString dialect;
        int limit = kMaxTables;
        bool truncated = false;
        bool described = false; // the table list is known and the requests were sent
        std::vector<Rel> rels;
        std::unordered_map<quint64, qsizetype> steps; // the session's request id -> index in rels
        QList<QMetaObject::Connection> links;
        QString firstMessage;
        QString firstDetail;
    };

    void beginWalk(quint64 id);
    void onWalked(quint64 id);
    void describeTables(Req &req, const std::vector<quarry::CachedObject> &relations);
    void onDescribed(quint64 id, quint64 sessionRequest, const quarry::TableDetails &details);
    void onDescribeFailed(quint64 id, quint64 sessionRequest, const quarry::Error &error);
    void finishIfDone(quint64 id);
    void fail(quint64 id, const QString &message, const QString &detail);
    void dispose(Req &req);
    bool excluded(const Req &req, const QString &schema, const QString &name) const;

    std::unordered_map<quint64, std::unique_ptr<Req>> m_requests;
    quint64 m_lastId = 0;
};

} // namespace quarry::ui

Q_DECLARE_METATYPE(quarry::ai::SchemaContext)
