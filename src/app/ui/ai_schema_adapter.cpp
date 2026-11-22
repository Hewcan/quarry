#include "ai_schema_adapter.h"

#include <QRegularExpression>
#include <QTimer>

#include <algorithm>

namespace quarry::ui {

namespace {

// The folders every provider puts under a table (ARCHITECTURE section 6.3); the same names the
// metadata cache uses for the columns.
const QString kColumnsFolder = QStringLiteral("Columns");
const QString kIndexesFolder = QStringLiteral("Indexes");
const QString kForeignKeysFolder = QStringLiteral("Foreign keys");

QString keyOf(const QStringList &parts)
{
    return parts.join(QChar(0x1F));
}

// "schema" of a relation path: [database, schema, table]. Engines without schemas have
// [database, table] and so no schema.
QString schemaOf(const QStringList &parts)
{
    return parts.size() >= 3 ? parts.at(parts.size() - 2) : QString();
}

// "int4 NOT NULL PK" -> type "int4", not null, primary key. The detail of a column node is what
// the tree shows; the type is whatever is left once the two markers are taken off the end.
quarry::ai::AiColumn columnFrom(const QString &name, const QString &detail)
{
    quarry::ai::AiColumn column;
    column.name = name;
    QString rest = detail.trimmed();
    bool changed = true;
    while (changed) {
        changed = false;
        if (rest == QLatin1String("PK") || rest.endsWith(QLatin1String(" PK"))) {
            column.primaryKey = true;
            rest.chop(2);
            changed = true;
        } else if (rest == QLatin1String("NOT NULL") || rest.endsWith(QLatin1String(" NOT NULL"))) {
            column.nullable = false;
            rest.chop(8);
            changed = true;
        }
        rest = rest.trimmed();
    }
    column.type = rest;
    return column;
}

// "-> schema.table(col, col)" or "-> table(col)". The local columns are not in the tree's text.
quarry::ai::AiForeignKey foreignKeyFrom(const QString &detail)
{
    static const QRegularExpression kShape(QStringLiteral("^->\\s*(.*?)\\s*\\((.*)\\)\\s*$"));
    quarry::ai::AiForeignKey fk;
    QString target;
    const QRegularExpressionMatch m = kShape.match(detail.trimmed());
    if (m.hasMatch()) {
        target = m.captured(1);
        const QStringList columns = m.captured(2).split(QLatin1Char(','), Qt::SkipEmptyParts);
        fk.refColumns.reserve(columns.size());
        for (const QString &c : columns)
            fk.refColumns.append(c.trimmed());
    } else {
        target = detail.trimmed();
        if (target.startsWith(QLatin1String("->")))
            target = target.mid(2).trimmed();
    }
    const QStringList parts = target.split(QLatin1Char('.'));
    fk.refTable = parts.last();
    if (parts.size() >= 2)
        fk.refSchema = parts.at(parts.size() - 2);
    return fk;
}

// "table (col1, col2) unique" (the Indexes folder of a database or schema) -> columns and flag.
quarry::ai::AiIndex indexFrom(const QString &name, const QString &detail)
{
    static const QRegularExpression kShape(QStringLiteral("^.*?\\s*\\((.*)\\)\\s*(unique)?\\s*$"),
                                           QRegularExpression::CaseInsensitiveOption);
    quarry::ai::AiIndex index;
    index.name = name;
    const QRegularExpressionMatch m = kShape.match(detail.trimmed());
    if (m.hasMatch()) {
        const QStringList columns = m.captured(1).split(QLatin1Char(','), Qt::SkipEmptyParts);
        index.columns.reserve(columns.size());
        for (const QString &c : columns)
            index.columns.append(c.trimmed());
        index.unique = !m.captured(2).isEmpty();
    } else {
        index.unique = detail.contains(QLatin1String("unique"), Qt::CaseInsensitive);
    }
    return index;
}

} // namespace

AiSchemaAdapter::AiSchemaAdapter(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<quarry::ai::SchemaContext>();
}

AiSchemaAdapter::~AiSchemaAdapter()
{
    for (auto &entry : m_requests)
        dispose(*entry.second);
}

QString AiSchemaAdapter::dialectOf(const QString &providerId)
{
    if (providerId == QLatin1String("sqlite"))
        return QStringLiteral("sqlite");
    if (providerId == QLatin1String("postgres"))
        return QStringLiteral("postgresql");
    if (providerId == QLatin1String("mysql"))
        return QStringLiteral("mysql");
    if (providerId == QLatin1String("mssql"))
        return QStringLiteral("sqlserver");
    return QStringLiteral("sql");
}

quarry::ai::AiTable AiSchemaAdapter::tableFromDetails(const QString &schema, const QString &name,
                                                      bool isView,
                                                      const quarry::TableDetails &details)
{
    quarry::ai::AiTable table;
    table.schema = schema;
    table.name = name;
    table.isView = isView;
    const QStringList primary = details.primaryKey.value_or(QStringList());
    table.columns.reserve(details.columns.size());
    for (const quarry::ColumnDef &c : details.columns) {
        quarry::ai::AiColumn column;
        column.name = c.name;
        column.type = c.nativeType;
        column.nullable = c.nullable;
        column.primaryKey = primary.contains(c.name);
        column.defaultValue = c.defaultExpression; // sent only with ContextOptions::sendDefaults
        table.columns.push_back(std::move(column));
    }
    table.foreignKeys.reserve(details.foreignKeys.size());
    for (const quarry::ForeignKeyDef &f : details.foreignKeys) {
        quarry::ai::AiForeignKey fk;
        fk.columns = f.columns;
        fk.refSchema = schemaOf(f.referencedTable.parts);
        fk.refTable = f.referencedTable.parts.value(f.referencedTable.parts.size() - 1);
        fk.refColumns = f.referencedColumns;
        table.foreignKeys.push_back(std::move(fk));
    }
    table.indexes.reserve(details.indexes.size());
    for (const quarry::IndexDef &i : details.indexes)
        table.indexes.push_back({ i.name, i.columns, i.unique });
    return table;
}

bool AiSchemaAdapter::excluded(const Req &req, const QString &schema, const QString &name) const
{
    const auto has = [](const QStringList &list, const QString &what) {
        return std::any_of(list.cbegin(), list.cend(), [&](const QString &s) {
            return s.compare(what, Qt::CaseInsensitive) == 0;
        });
    };
    if (!schema.isEmpty() && has(req.options.excludedSchemas, schema))
        return true;
    return has(req.options.excludedTables, name)
        || (!schema.isEmpty() && has(req.options.excludedTables, schema + QLatin1Char('.') + name));
}

quint64 AiSchemaAdapter::contextFor(const Request &request)
{
    const quint64 id = ++m_lastId;
    auto req = std::make_unique<Req>();
    req->id = id;
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDelete): QPointer false positive
    req->session = request.session;
    req->options = request.options;
    req->prioritize = request.prioritize;
    req->dialect = dialectOf(request.providerId);
    req->limit = std::clamp(request.limit, 1, kMaxTables);

    if (request.session.isNull()) {
        m_requests.emplace(id, std::move(req));
        QTimer::singleShot(0, this, [this, id] {
            fail(id, tr("The connection is not open, so its structure cannot be read."), {});
        });
        return id;
    }

    // The relations: the connection's cache when it is ready, else a temporary one that walks the
    // tree through the same session.
    if (request.cache != nullptr && request.cache->state() == quarry::MetadataCache::State::Ready) {
        const std::vector<quarry::CachedObject> relations = request.cache->relations();
        describeTables(*req, relations);
        m_requests.emplace(id, std::move(req));
        // The requests were sent; with nothing to ask the answer is still never inside this call.
        QTimer::singleShot(0, this, [this, id] { finishIfDone(id); });
        return id;
    }
    auto *cache = new quarry::MetadataCache(this);
    req->ownCache = cache;
    cache->attach(request.session);
    connect(cache, &quarry::MetadataCache::changed, this, [this, id] { onWalked(id); });
    connect(cache, &quarry::MetadataCache::failed, this,
            [this, id](const quarry::Error &error) { fail(id, error.message, error.detail); });
    m_requests.emplace(id, std::move(req));
    cache->load();
    return id;
}

void AiSchemaAdapter::onWalked(quint64 id)
{
    const auto it = m_requests.find(id);
    if (it == m_requests.end())
        return;
    Req &req = *it->second;
    if (req.described || req.ownCache.isNull()
        || req.ownCache->state() != quarry::MetadataCache::State::Ready)
        return;
    describeTables(req, req.ownCache->relations());
    finishIfDone(id);
}

void AiSchemaAdapter::describeTables(Req &req, const std::vector<quarry::CachedObject> &relations)
{
    req.described = true;
    req.rels.reserve(relations.size());
    for (const quarry::CachedObject &object : relations) {
        Rel rel;
        rel.path = object.path;
        rel.name = object.name;
        rel.schema = schemaOf(object.path.parts);
        rel.isView = object.path.kind != quarry::ObjectKind::Table;
        if (excluded(req, rel.schema, rel.name))
            continue;
        req.rels.push_back(std::move(rel));
    }
    // The names the question mentions go first, so a schema over the limit still has them.
    if (!req.prioritize.isEmpty()) {
        const auto wanted = [&](const Rel &rel) {
            return std::any_of(req.prioritize.cbegin(), req.prioritize.cend(),
                               [&](const QString &p) {
                                   return p.compare(rel.name, Qt::CaseInsensitive) == 0
                                       || (!rel.schema.isEmpty()
                                           && p.compare(rel.schema + QLatin1Char('.') + rel.name,
                                                        Qt::CaseInsensitive)
                                               == 0);
                               });
        };
        std::stable_partition(req.rels.begin(), req.rels.end(), wanted);
    }
    if (static_cast<qsizetype>(req.rels.size()) > req.limit) {
        req.rels.resize(static_cast<size_t>(req.limit));
        req.truncated = true;
    }

    quarry::Session *session = req.session;
    const quint64 id = req.id;
    req.links.push_back(connect(
        session, &quarry::Session::childrenReady, this,
        [this, id](quint64 rid, const quarry::ObjectPath &,
                   const std::vector<quarry::SchemaNode> &nodes) { onChildren(id, rid, nodes); }));
    req.links.push_back(
        connect(session, &quarry::Session::childrenFailed, this,
                [this, id](quint64 rid, const quarry::ObjectPath &, const quarry::Error &error) {
                    onChildrenFailed(id, rid, error);
                }));

    QHash<QString, qsizetype> parents; // parent path -> already asked
    for (qsizetype i = 0; i < static_cast<qsizetype>(req.rels.size()); ++i) {
        Rel &rel = req.rels[static_cast<size_t>(i)];
        rel.table.schema = rel.schema;
        rel.table.name = rel.name;
        rel.table.isView = rel.isView;
        req.byTable.insert(keyOf(rel.path.parts), i);
        const QStringList base = rel.path.parts;
        quint64 rid = session->children(
            { base + QStringList { kColumnsFolder }, quarry::ObjectKind::Folder });
        req.steps.emplace(rid, Step { i, StepKind::Columns });
        if (rel.isView)
            continue;
        rid = session->children(
            { base + QStringList { kForeignKeysFolder }, quarry::ObjectKind::Folder });
        req.steps.emplace(rid, Step { i, StepKind::ForeignKeys });
        // One list of indexes per database or schema, with their columns, not one per table.
        const QStringList parentParts = base.mid(0, base.size() - 1);
        const QString parentKey = keyOf(parentParts);
        if (!parents.contains(parentKey)) {
            parents.insert(parentKey, i);
            rid = session->children(
                { parentParts + QStringList { kIndexesFolder }, quarry::ObjectKind::Folder });
            req.steps.emplace(rid, Step { i, StepKind::Indexes });
        }
    }
}

void AiSchemaAdapter::onChildren(quint64 id, quint64 sessionRequest,
                                 const std::vector<quarry::SchemaNode> &nodes)
{
    const auto it = m_requests.find(id);
    if (it == m_requests.end())
        return;
    Req &req = *it->second;
    const auto step = req.steps.find(sessionRequest);
    if (step == req.steps.end())
        return; // the tree's own request
    const Step what = step->second;
    req.steps.erase(step);

    switch (what.kind) {
    case StepKind::Columns: {
        Rel &rel = req.rels[static_cast<size_t>(what.relation)];
        for (const quarry::SchemaNode &node : nodes) {
            if (node.path.kind == quarry::ObjectKind::Column)
                rel.table.columns.push_back(columnFrom(node.name, node.detail));
        }
        break;
    }
    case StepKind::ForeignKeys: {
        Rel &rel = req.rels[static_cast<size_t>(what.relation)];
        for (const quarry::SchemaNode &node : nodes) {
            if (node.path.kind == quarry::ObjectKind::ForeignKey)
                rel.table.foreignKeys.push_back(foreignKeyFrom(node.detail));
        }
        break;
    }
    case StepKind::Indexes:
        for (const quarry::SchemaNode &node : nodes) {
            if (node.path.kind != quarry::ObjectKind::Index || node.path.parts.size() < 2)
                continue;
            const auto owner
                = req.byTable.constFind(keyOf(node.path.parts.mid(0, node.path.parts.size() - 1)));
            if (owner != req.byTable.constEnd())
                req.rels[static_cast<size_t>(owner.value())].table.indexes.push_back(
                    indexFrom(node.name, node.detail));
        }
        break;
    }
    finishIfDone(id);
}

void AiSchemaAdapter::onChildrenFailed(quint64 id, quint64 sessionRequest,
                                       const quarry::Error &error)
{
    const auto it = m_requests.find(id);
    if (it == m_requests.end())
        return;
    Req &req = *it->second;
    const auto step = req.steps.find(sessionRequest);
    if (step == req.steps.end())
        return;
    const Step what = step->second;
    req.steps.erase(step);
    if (req.firstMessage.isEmpty()) {
        req.firstMessage = error.message;
        req.firstDetail = error.detail;
    }
    // Without its columns a table would be described wrongly, so it is left out; a missing list of
    // foreign keys or indexes only makes the description shorter.
    if (what.kind == StepKind::Columns)
        req.rels[static_cast<size_t>(what.relation)].failed = true;
    finishIfDone(id);
}

void AiSchemaAdapter::finishIfDone(quint64 id)
{
    const auto it = m_requests.find(id);
    if (it == m_requests.end())
        return;
    Req &req = *it->second;
    if (!req.described || !req.steps.empty())
        return;

    quarry::ai::SchemaContext context;
    context.dialect = req.dialect;
    context.tables.reserve(req.rels.size());
    qsizetype failed = 0;
    for (Rel &rel : req.rels) {
        if (rel.failed)
            ++failed;
        else
            context.tables.push_back(std::move(rel.table));
    }
    const bool truncated = req.truncated;
    const QString message = req.firstMessage;
    const QString detail = req.firstDetail;
    const bool nothing = context.tables.empty() && failed > 0;
    dispose(req);
    m_requests.erase(id);
    if (nothing)
        emit contextFailed(id, message, detail);
    else
        emit contextReady(id, std::move(context), truncated);
}

void AiSchemaAdapter::fail(quint64 id, const QString &message, const QString &detail)
{
    const auto it = m_requests.find(id);
    if (it == m_requests.end())
        return;
    dispose(*it->second);
    m_requests.erase(it);
    emit contextFailed(id, message, detail);
}

void AiSchemaAdapter::cancel(quint64 id)
{
    const auto it = m_requests.find(id);
    if (it == m_requests.end())
        return;
    dispose(*it->second);
    m_requests.erase(it);
    emit contextCancelled(id);
}

void AiSchemaAdapter::dispose(Req &req)
{
    for (const QMetaObject::Connection &link : std::as_const(req.links))
        disconnect(link);
    req.links.clear();
    if (!req.ownCache.isNull()) {
        req.ownCache->disconnect(this);
        req.ownCache->attach(nullptr);
        req.ownCache->deleteLater();
    }
    req.steps.clear();
}

} // namespace quarry::ui
