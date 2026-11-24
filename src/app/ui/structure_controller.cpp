#include "structure_controller.h"

#include "connection_controller.h"
#include "editor_tabs_model.h"
#include "query_controller.h"
#include "schema_tree_model.h"

#include <quarry/ddl_builder.h>

#include <QUuid>

#include <algorithm>

namespace quarry::ui {

namespace {

constexpr int kPollMs = 300;
constexpr qsizetype kShownValueChars = 40;

QString shortValue(const QString &value)
{
    return value.size() > kShownValueChars ? value.left(kShownValueChars) + QStringLiteral("…")
                                           : value;
}

Error makeError(ErrorKind kind, const QString &message)
{
    return Error { kind, message, {}, {}, {} };
}

} // namespace

StructureController::StructureController(QObject *parent)
    : QObject(parent)
{
    m_poll.setInterval(kPollMs);
    connect(&m_poll, &QTimer::timeout, this, &StructureController::retryWaiting);
}

StructureController::~StructureController() = default;

void StructureController::setConnections(ConnectionController *connections)
{
    if (connections == m_connections)
        return;
    m_connections = connections;
    if (m_connections != nullptr) {
        connect(m_connections, &QObject::destroyed, this, [this] { m_connections = nullptr; });
        // The connection of a tab that was waiting has opened (or failed): look again.
        connect(m_connections->tree(), &SchemaTreeModel::connectedChanged, this,
                &StructureController::retryWaiting);
    }
    emit connectionsChanged();
}

void StructureController::setTabs(EditorTabsModel *tabs)
{
    if (tabs == m_tabs)
        return;
    m_tabs = tabs;
    if (m_tabs != nullptr) {
        connect(m_tabs, &QObject::destroyed, this, [this] { m_tabs = nullptr; });
        connect(m_tabs, &QAbstractItemModel::rowsRemoved, this, [this] { prune(); });
    }
    emit tabsChanged();
}

void StructureController::setQueries(QueryController *queries)
{
    if (queries == m_queries)
        return;
    m_queries = queries;
    if (m_queries != nullptr)
        connect(m_queries, &QObject::destroyed, this, [this] { m_queries = nullptr; });
    emit queriesChanged();
}

void StructureController::prune()
{
    if (m_tabs == nullptr)
        return;
    QSet<QString> live;
    for (int i = 0; i < m_tabs->count(); ++i)
        live.insert(m_tabs->idOf(i));
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (live.contains(it->first))
            ++it;
        else
            it = m_entries.erase(it);
    }
}

// ---- reading -----------------------------------------------------------------------------------

std::optional<ObjectPath> StructureController::objectOf(const QString &tabId,
                                                        QString *connectionId) const
{
    if (m_tabs == nullptr)
        return std::nullopt;
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (m_tabs->idOf(i) != tabId)
            continue;
        if (connectionId != nullptr)
            *connectionId = m_tabs->connectionIdOf(i);
        if (auto table = m_tabs->tableOf(i))
            return table;
        return m_tabs->structureOf(i);
    }
    return std::nullopt;
}

const ISqlDialect *StructureController::dialectOf(const QString &connectionId) const
{
    if (m_connections == nullptr)
        return nullptr;
    const auto profile = m_connections->model()->profile(QUuid::fromString(connectionId));
    if (!profile)
        return nullptr;
    // shortcut: loads the engine's plugin on the UI thread (a few ms, once per engine), as
    // QueryController does.
    IDatabaseProvider *provider = m_connections->registry().provider(profile->providerId, nullptr);
    return provider != nullptr ? &provider->dialect() : nullptr;
}

QString StructureController::ddlOf(const Entry &e) const
{
    const ISqlDialect *dialect = dialectOf(e.connectionId);
    if (dialect == nullptr || !e.details)
        return {};
    return DdlBuilder::build(*dialect, e.object, *e.details, e.object.kind != ObjectKind::Table);
}

void StructureController::watch(Session *session)
{
    if (m_watched.contains(session))
        return;
    m_watched.insert(session);
    connect(session, &Session::described, this,
            [this, session](quint64 id, const ObjectPath &, const TableDetails &details) {
                onDescribed(session, id, details);
            });
    connect(session, &Session::describeFailed, this,
            [this, session](quint64 id, const ObjectPath &, const Error &error) {
                onDescribeFailed(session, id, error);
            });
    connect(session, &QObject::destroyed, this, [this, session] { onSessionGone(session); });
}

void StructureController::fail(const QString &tabId, Entry &e, const Error &error)
{
    e.state = State::Failed;
    e.error = error;
    e.session = nullptr;
    e.request = 0;
    emit changed(tabId);
}

bool StructureController::describeNow(const QString &tabId, Entry &e)
{
    if (m_connections == nullptr) {
        fail(tabId, e, makeError(ErrorKind::Connection, tr("The connection is not available.")));
        return true;
    }
    SchemaTreeModel *tree = m_connections->tree();
    switch (tree->ensureSession(e.connectionId)) {
    case SchemaTreeModel::Access::Connected: {
        Session *session = tree->metadataSession(e.connectionId);
        if (session == nullptr) {
            fail(tabId, e,
                 makeError(ErrorKind::Connection,
                           tr("The connection is not open, so the structure cannot be read.")));
            return true;
        }
        watch(session);
        e.session = session;
        e.request = session->describe(e.object);
        return true;
    }
    case SchemaTreeModel::Access::Opening:
        return false;
    case SchemaTreeModel::Access::NeedsPassword:
        fail(tabId, e,
             makeError(ErrorKind::Connection,
                       tr("The connection asks for its password. Open it in the tree first, then "
                          "try again.")));
        return true;
    case SchemaTreeModel::Access::Unavailable:
        break;
    }
    fail(tabId, e,
         makeError(ErrorKind::Connection,
                   tr("The connection is not open, so the structure cannot be read.")));
    return true;
}

void StructureController::start(const QString &tabId, Entry &e)
{
    QString connection;
    const auto object = objectOf(tabId, &connection);
    e = Entry {};
    if (!object || connection.isEmpty()) {
        fail(tabId, e, makeError(ErrorKind::NotFound, tr("This tab has no table to read.")));
        return;
    }
    e.object = *object;
    e.connectionId = connection;
    e.state = State::Loading;
    emit changed(tabId);
    if (!describeNow(tabId, e) && !m_poll.isActive())
        m_poll.start();
}

void StructureController::load(const QString &tabId)
{
    const auto it = m_entries.find(tabId);
    if (it != m_entries.end()
        && (it->second.state == State::Loading || it->second.state == State::Ready))
        return;
    start(tabId, m_entries[tabId]);
}

void StructureController::reload(const QString &tabId)
{
    Entry &e = m_entries[tabId];
    if (e.state == State::Loading)
        return;
    start(tabId, e);
}

// The tabs that waited for the connection of the tree: ask again (the connection opened, failed
// or is still opening), and give up after kWaitMs.
void StructureController::retryWaiting()
{
    const bool tick = sender() == &m_poll;
    QStringList waiting;
    for (const auto &entry : m_entries)
        if (entry.second.state == State::Loading && entry.second.session == nullptr)
            waiting.append(entry.first);
    bool stillWaiting = false;
    for (const QString &tabId : std::as_const(waiting)) {
        const auto it = m_entries.find(tabId);
        if (it == m_entries.end())
            continue;
        Entry &e = it->second;
        if (tick)
            e.waitedMs += kPollMs;
        if (describeNow(tabId, e))
            continue;
        if (e.waitedMs >= kWaitMs)
            fail(tabId, e,
                 makeError(ErrorKind::Connection,
                           tr("The connection took too long to open, so the structure cannot be "
                              "read.")));
        else
            stillWaiting = true;
    }
    if (!stillWaiting)
        m_poll.stop();
}

void StructureController::onDescribed(const Session *session, quint64 requestId,
                                      const TableDetails &details)
{
    for (qsizetype i = 0; i < m_related.size(); ++i) {
        if (m_related.at(i).session == session && m_related.at(i).request == requestId) {
            const Related related = m_related.takeAt(i);
            finishRelated(related, details, nullptr);
            return;
        }
    }
    for (auto &entry : m_entries) {
        Entry &e = entry.second;
        if (e.state != State::Loading || e.session != session || e.request != requestId)
            continue;
        e.details = details;
        e.ddl = ddlOf(e);
        e.state = State::Ready;
        e.session = nullptr;
        e.request = 0;
        emit changed(entry.first);
        return;
    }
}

void StructureController::onDescribeFailed(const Session *session, quint64 requestId,
                                           const Error &error)
{
    for (qsizetype i = 0; i < m_related.size(); ++i) {
        if (m_related.at(i).session == session && m_related.at(i).request == requestId) {
            const Related related = m_related.takeAt(i);
            finishRelated(related, std::nullopt, &error);
            return;
        }
    }
    for (auto &entry : m_entries) {
        Entry &e = entry.second;
        if (e.state == State::Loading && e.session == session && e.request == requestId) {
            fail(entry.first, e, error);
            return;
        }
    }
}

void StructureController::onSessionGone(const Session *session)
{
    m_watched.remove(session);
    const Error gone = makeError(ErrorKind::Connection,
                                 tr("The connection was closed before the structure was read."));
    QStringList failed;
    for (const auto &entry : m_entries)
        if (entry.second.state == State::Loading && entry.second.session == session)
            failed.append(entry.first);
    for (const QString &tabId : std::as_const(failed))
        fail(tabId, m_entries[tabId], gone);
    for (qsizetype i = m_related.size() - 1; i >= 0; --i) {
        if (m_related.at(i).session == session) {
            const Related related = m_related.takeAt(i);
            finishRelated(related, std::nullopt, &gone);
        }
    }
}

// ---- what the views show -----------------------------------------------------------------------

QString StructureController::displayName(const ObjectPath &path)
{
    const qsizetype n = path.parts.size();
    if (n >= 3)
        return path.parts.at(n - 2) + QLatin1Char('.') + path.parts.at(n - 1);
    return path.parts.value(n - 1);
}

QString StructureController::kindName(ObjectKind kind)
{
    switch (kind) {
    case ObjectKind::View:
        return QStringLiteral("view");
    case ObjectKind::MaterializedView:
        return QStringLiteral("materializedView");
    default:
        return QStringLiteral("table");
    }
}

const ForeignKeyDef *StructureController::foreignKeyOf(const Entry &e, const QString &column) const
{
    if (e.state != State::Ready || !e.details)
        return nullptr;
    for (const ForeignKeyDef &fk : e.details->foreignKeys)
        if (fk.columns.contains(column, Qt::CaseSensitive) && !fk.referencedColumns.isEmpty())
            return &fk;
    return nullptr;
}

QVariantMap StructureController::infoOf(const QString &tabId) const
{
    QVariantMap out { { QStringLiteral("state"), QStringLiteral("idle") },
                      { QStringLiteral("message"), QString() },
                      { QStringLiteral("detail"), QString() },
                      { QStringLiteral("isView"), false },
                      { QStringLiteral("empty"), false },
                      { QStringLiteral("name"), QString() },
                      { QStringLiteral("columns"), QVariantList() },
                      { QStringLiteral("indexes"), QVariantList() },
                      { QStringLiteral("foreignKeys"), QVariantList() },
                      { QStringLiteral("ddl"), QString() } };
    const auto it = m_entries.find(tabId);
    if (it == m_entries.end())
        return out;
    const Entry &e = it->second;
    out[QStringLiteral("name")] = displayName(e.object);
    out[QStringLiteral("isView")] = e.object.kind != ObjectKind::Table;
    switch (e.state) {
    case State::Idle:
        return out;
    case State::Loading:
        out[QStringLiteral("state")] = QStringLiteral("loading");
        return out;
    case State::Failed:
        out[QStringLiteral("state")] = QStringLiteral("failed");
        out[QStringLiteral("message")] = e.error.message;
        out[QStringLiteral("detail")] = e.error.detail;
        return out;
    case State::Ready:
        break;
    }
    out[QStringLiteral("state")] = QStringLiteral("ready");
    if (!e.details)
        return out;
    const TableDetails &d = *e.details;
    const QStringList primary = d.primaryKey.value_or(QStringList());
    out[QStringLiteral("empty")] = d.columns.empty();

    QVariantList columns;
    columns.reserve(static_cast<qsizetype>(d.columns.size()));
    for (const ColumnDef &c : d.columns) {
        QStringList marks;
        QStringList links;
        const bool isPrimary = primary.contains(c.name);
        if (isPrimary)
            marks.append(tr("Primary key"));
        for (const ForeignKeyDef &fk : d.foreignKeys) {
            const qsizetype at = fk.columns.indexOf(c.name);
            if (at < 0)
                continue;
            const QString target = fk.referencedColumns.size() > at
                ? tr("Foreign key to %1.%2")
                      .arg(displayName(fk.referencedTable), fk.referencedColumns.at(at))
                : tr("Foreign key to %1").arg(displayName(fk.referencedTable));
            marks.append(target);
            links.append(target);
        }
        columns.append(QVariantMap {
            { QStringLiteral("name"), c.name },
            { QStringLiteral("type"), c.nativeType.isEmpty() ? tr("unknown") : c.nativeType },
            { QStringLiteral("nullable"), c.nullable },
            { QStringLiteral("nullableText"), c.nullable ? tr("nullable") : tr("NOT NULL") },
            { QStringLiteral("defaultText"), c.defaultExpression },
            { QStringLiteral("primaryKey"), isPrimary },
            { QStringLiteral("marks"), marks.join(QStringLiteral(", ")) },
            { QStringLiteral("foreignKeys"), links } });
    }
    out[QStringLiteral("columns")] = columns;

    QVariantList indexes;
    indexes.reserve(static_cast<qsizetype>(d.indexes.size()));
    for (const IndexDef &i : d.indexes)
        indexes.append(QVariantMap {
            { QStringLiteral("name"), i.name },
            { QStringLiteral("columns"), i.columns.join(QStringLiteral(", ")) },
            { QStringLiteral("unique"), i.unique },
            { QStringLiteral("uniqueText"), i.unique ? tr("unique") : tr("not unique") } });
    out[QStringLiteral("indexes")] = indexes;

    QVariantList keys;
    keys.reserve(static_cast<qsizetype>(d.foreignKeys.size()));
    for (const ForeignKeyDef &fk : d.foreignKeys) {
        const QString from = fk.columns.join(QStringLiteral(", "));
        const QString to = fk.referencedColumns.join(QStringLiteral(", "));
        keys.append(QVariantMap {
            { QStringLiteral("name"), fk.name },
            { QStringLiteral("columns"), from },
            { QStringLiteral("table"), displayName(fk.referencedTable) },
            { QStringLiteral("parts"), fk.referencedTable.parts },
            { QStringLiteral("kind"), kindName(fk.referencedTable.kind) },
            { QStringLiteral("refColumns"), to },
            { QStringLiteral("text"),
              tr("%1 references %2 (%3)").arg(from, displayName(fk.referencedTable), to) },
            { QStringLiteral("openable"), !fk.referencedTable.parts.isEmpty() } });
    }
    out[QStringLiteral("foreignKeys")] = keys;
    out[QStringLiteral("ddl")] = e.ddl;
    return out;
}

QVariantMap StructureController::linkColumns(const QString &tabId) const
{
    QVariantMap out;
    const auto it = m_entries.find(tabId);
    if (it == m_entries.end() || it->second.state != State::Ready)
        return out;
    const std::optional<TableDetails> &details = it->second.details;
    if (!details)
        return out;
    for (const ForeignKeyDef &fk : details->foreignKeys)
        for (qsizetype i = 0; i < fk.columns.size(); ++i) {
            if (i >= fk.referencedColumns.size() || out.contains(fk.columns.at(i)))
                continue;
            out.insert(fk.columns.at(i),
                       tr("Foreign key to %1.%2")
                           .arg(displayName(fk.referencedTable), fk.referencedColumns.at(i)));
        }
    return out;
}

QString StructureController::linkText(const QString &tabId, const QString &column) const
{
    return linkColumns(tabId).value(column).toString();
}

// ---- go to related row -------------------------------------------------------------------------

QVariantMap StructureController::goToRelated(const QString &tabId, int row, const QString &column)
{
    const auto refuse = [this](const QString &message, bool assertive) {
        emit announcement(message, assertive);
        return QVariantMap { { QStringLiteral("started"), false },
                             { QStringLiteral("message"), message } };
    };
    const auto it = m_entries.find(tabId);
    if (m_tabs == nullptr || m_queries == nullptr)
        return refuse(tr("There is no related row to open."), true);
    if (it == m_entries.end() || it->second.state == State::Loading
        || it->second.state == State::Idle)
        return refuse(tr("The structure of this table is still loading. Try again in a moment."),
                      false);
    const Entry &e = it->second;
    const ForeignKeyDef *fk = foreignKeyOf(e, column);
    if (fk == nullptr)
        return refuse(tr("%1 is not a foreign key.").arg(column), false);
    QueryState *state = m_queries->stateOf(tabId);
    if (state == nullptr)
        return refuse(tr("There is no related row to open."), true);
    TableEditModel *edits = state->edits();
    ResultTableModel *results = state->results();
    if (row < 0 || row >= edits->rows())
        return refuse(tr("There is no current row."), false);

    // Every column of the key, in the grid and with a value.
    QStringList values;
    QVariantList filters;
    for (qsizetype i = 0; i < fk->columns.size(); ++i) {
        int at = -1;
        for (int c = 0; c < results->columns() && at < 0; ++c)
            if (results->columnName(c) == fk->columns.at(i))
                at = c;
        if (at < 0 || i >= fk->referencedColumns.size())
            return refuse(tr("The related row cannot be found: the column %1 is not in the table.")
                              .arg(fk->columns.at(i)),
                          true);
        if (edits->isNullAt(row, at))
            return refuse(tr("This row has no related row"), true);
        const QString value = edits->editText(row, at);
        values.append(value);
        filters.append(QVariantMap { { QStringLiteral("column"), fk->referencedColumns.at(i) },
                                     { QStringLiteral("op"), QStringLiteral("equals") },
                                     { QStringLiteral("value"), value } });
    }
    const ObjectPath target = fk->referencedTable;
    const QString sourceName = e.object.parts.value(e.object.parts.size() - 1);
    QStringList shown;
    for (const QString &v : std::as_const(values))
        shown.append(shortValue(v));
    const QString origin = fk->columns.size() == 1
        ? tr("Filtered from %1.%2 = %3").arg(sourceName, fk->columns.at(0), shown.at(0))
        : tr("Filtered from %1.(%2) = (%3)")
              .arg(sourceName, fk->columns.join(QStringLiteral(", ")),
                   shown.join(QStringLiteral(", ")));
    QVariantMap view { { QStringLiteral("sortColumn"), QString() },
                       { QStringLiteral("descending"), false },
                       { QStringLiteral("where"), QString() },
                       { QStringLiteral("filters"), filters } };
    const QString connectionId = e.connectionId;
    const QString kind = kindName(target.kind);

    // Already open: shown, with its filter replaced when it is another one.
    if (const int open = m_tabs->dataRowOf(connectionId, target.parts, kind); open >= 0) {
        const QString openId = m_tabs->idOf(open);
        const QVariantMap now = m_queries->dataViewOf(openId);
        const bool same = now.value(QStringLiteral("filters")).toList() == filters
            && now.value(QStringLiteral("where")).toString().isEmpty()
            && now.value(QStringLiteral("sortColumn")).toString().isEmpty();
        if (!same && !m_queries->setRelatedView(openId, view, origin)) {
            return QVariantMap {
                { QStringLiteral("started"), false },
                { QStringLiteral("message"),
                  tr("Save or discard the pending changes of %1 first.").arg(displayName(target)) }
            };
        }
        m_tabs->setCurrentIndex(open);
        emit announcement(origin, false);
        emit relatedOpened(tabId, openId);
        return QVariantMap { { QStringLiteral("started"), true },
                             { QStringLiteral("message"), origin } };
    }

    // Not open: the table is checked first (a view that no longer exists, another schema), so a
    // broken tab is never opened.
    Session *session
        = m_connections != nullptr ? m_connections->tree()->metadataSession(connectionId) : nullptr;
    if (session == nullptr)
        return refuse(tr("The connection is not open, so the related table cannot be opened."),
                      true);
    watch(session);
    Related related;
    related.session = session;
    related.tabId = tabId;
    related.connectionId = connectionId;
    related.parts = target.parts;
    related.kind = kind;
    related.origin = origin;
    related.view = view;
    related.request = session->describe(target);
    m_related.append(related);
    return QVariantMap { { QStringLiteral("started"), true },
                         { QStringLiteral("message"), origin } };
}

void StructureController::finishRelated(const Related &related,
                                        const std::optional<TableDetails> &details,
                                        const Error *error)
{
    const QString name = related.parts.value(related.parts.size() - 1);
    if (!details || m_tabs == nullptr || m_queries == nullptr) {
        const QString why = error != nullptr && !error->message.isEmpty()
            ? error->message
            : tr("It was not found in this connection.");
        const QString message = tr("The related table %1 cannot be opened: %2").arg(name, why);
        emit announcement(message, true);
        emit relatedFailed(related.tabId, message);
        return;
    }
    if (!m_queries->presetDataView(related.connectionId, related.parts, related.view,
                                   related.origin)) {
        const QString message = tr("The related table %1 cannot be opened: %2")
                                    .arg(name, tr("the filter is not valid."));
        emit announcement(message, true);
        emit relatedFailed(related.tabId, message);
        return;
    }
    const int row = m_tabs->openData(related.connectionId, related.parts, related.kind);
    if (row < 0) {
        const QString message
            = tr("The related table %1 cannot be opened: %2").arg(name, tr("it is not available."));
        emit announcement(message, true);
        emit relatedFailed(related.tabId, message);
        return;
    }
    const QString targetId = m_tabs->idOf(row);
    // The structure of the new tab is what was just read: it does not ask again.
    Entry &e = m_entries[targetId];
    e = Entry {};
    e.object = ObjectPath { related.parts,
                            related.kind == QLatin1String("view") ? ObjectKind::View
                                : related.kind == QLatin1String("materializedView")
                                ? ObjectKind::MaterializedView
                                : ObjectKind::Table };
    e.connectionId = related.connectionId;
    e.details = *details;
    e.ddl = ddlOf(e);
    e.state = State::Ready;
    emit changed(targetId);
    emit announcement(related.origin, false);
    emit relatedOpened(related.tabId, targetId);
}

} // namespace quarry::ui
