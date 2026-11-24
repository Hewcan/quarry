#include "query_controller.h"

#include "connection_controller.h"
#include "editor_tabs_model.h"
#include "history_model.h"

#include <quarry/cell_formatter.h>
#include <quarry/change_set.h>
#include <quarry/sql_safety.h>

#include <QMetaObject>

#include <algorithm>
#include <memory>

namespace quarry::ui {

namespace {

constexpr int kPreviewChars = 160;
// An EXPLAIN returns a handful of rows (a JSON engine, one): this many are asked for at once.
constexpr int kExplainFetchRows = 100000;
// A plan text up to this many characters is parsed on the UI thread (a few ms); a bigger one on
// the parse thread. ~256 KiB of JSON is a few thousand nodes, where the parser starts to be felt.
constexpr qsizetype kParseOnUiChars = qsizetype { 256 } * 1024;
// Beyond this the text is not kept: the plan could not be shown anyway (kMaxExplainNodes).
constexpr qsizetype kMaxExplainChars = qsizetype { 32 } * 1024 * 1024;

QString toId(const QUuid &id)
{
    return id.toString(QUuid::WithoutBraces);
}

QString riskText(SqlRisk risk)
{
    switch (risk) {
    case SqlRisk::UpdateWithoutWhere:
        return QueryController::tr("UPDATE without WHERE");
    case SqlRisk::DeleteWithoutWhere:
        return QueryController::tr("DELETE without WHERE");
    case SqlRisk::Drop:
        return QueryController::tr("DROP on a production connection");
    case SqlRisk::Truncate:
        return QueryController::tr("TRUNCATE on a production connection");
    case SqlRisk::None:
        break;
    }
    return {};
}

QString kindText(ErrorKind kind)
{
    switch (kind) {
    case ErrorKind::Connection:
        return QueryController::tr("Connection error");
    case ErrorKind::Authentication:
        return QueryController::tr("Authentication error");
    case ErrorKind::Timeout:
        return QueryController::tr("Timeout");
    case ErrorKind::Cancelled:
        return QueryController::tr("Cancelled");
    case ErrorKind::Syntax:
        return QueryController::tr("Syntax error");
    case ErrorKind::Permission:
        return QueryController::tr("Permission denied");
    case ErrorKind::Constraint:
        return QueryController::tr("Constraint violation");
    case ErrorKind::NotFound:
        return QueryController::tr("Not found");
    case ErrorKind::Unsupported:
        return QueryController::tr("Not supported");
    case ErrorKind::Driver:
        return QueryController::tr("Database error");
    case ErrorKind::Internal:
        break;
    }
    return QueryController::tr("Internal error");
}

QString preview(const QString &sql)
{
    QString one = sql.simplified();
    if (one.size() > kPreviewChars)
        one = one.left(kPreviewChars) + QStringLiteral("…");
    return one;
}

bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

// Whether EXPLAIN ANALYZE of this statement changes nothing: it starts with SELECT or WITH and has
// no INSERT, UPDATE, DELETE or MERGE (a data-changing CTE) and no INTO (SELECT INTO makes
// a table) outside comments, strings and quoted names. What the scan is unsure of (an unterminated
// string) is "no". shortcut: SqlSafety has no such check; a function with side effects
// (SELECT nextval(...)) still counts as a read, which the confirmation before ANALYZE covers.
bool isReadOnlyQuery(QStringView sql)
{
    const qsizetype n = sql.size();
    QString first;
    qsizetype i = 0;
    while (i < n) {
        const QChar c = sql.at(i);
        if (c == QLatin1Char('-') && i + 1 < n && sql.at(i + 1) == QLatin1Char('-')) {
            const qsizetype end = sql.indexOf(QLatin1Char('\n'), i);
            i = end < 0 ? n : end + 1;
        } else if (c == QLatin1Char('/') && i + 1 < n && sql.at(i + 1) == QLatin1Char('*')) {
            const qsizetype end = sql.indexOf(QStringView(u"*/"), i + 2);
            if (end < 0)
                return false;
            i = end + 2;
        } else if (c == QLatin1Char('\'') || c == QLatin1Char('"') || c == QLatin1Char('`')) {
            qsizetype j = i + 1;
            while (true) {
                const qsizetype end = sql.indexOf(c, j);
                if (end < 0)
                    return false;
                if (end + 1 < n && sql.at(end + 1) == c) { // doubled quote
                    j = end + 2;
                    continue;
                }
                i = end + 1;
                break;
            }
        } else if (c == QLatin1Char('$')) { // $tag$ ... $tag$
            qsizetype j = i + 1;
            while (j < n && isWordChar(sql.at(j)))
                ++j;
            if (j < n && sql.at(j) == QLatin1Char('$')) {
                const QStringView tag = sql.mid(i, j - i + 1);
                const qsizetype end = sql.indexOf(tag, j + 1);
                if (end < 0)
                    return false;
                i = end + tag.size();
            } else {
                i = i + 1;
            }
        } else if (isWordChar(c)) {
            qsizetype j = i;
            while (j < n && isWordChar(sql.at(j)))
                ++j;
            if (c.isLetter() || c == QLatin1Char('_')) {
                const QString word = sql.mid(i, j - i).toString().toUpper();
                if (first.isEmpty())
                    first = word;
                static const QStringList writes { QStringLiteral("INSERT"),
                                                  QStringLiteral("UPDATE"),
                                                  QStringLiteral("DELETE"), QStringLiteral("MERGE"),
                                                  QStringLiteral("INTO") };
                if (writes.contains(word))
                    return false;
            }
            i = j;
        } else {
            ++i;
        }
    }
    return first == QLatin1String("SELECT") || first == QLatin1String("WITH");
}

Error gone()
{
    return Error {
        ErrorKind::NotFound, QueryController::tr("This connection no longer exists."), {}, {}, {}
    };
}

} // namespace

QueryController::QueryController(QObject *parent)
    : QObject(parent)
    , m_none(new QueryState(this))
    , m_current(m_none)
{
    m_parsePool.setMaxThreadCount(1);
}

void QueryController::setTabs(EditorTabsModel *tabs)
{
    if (tabs == m_tabs)
        return;
    if (m_tabs)
        m_tabs->disconnect(this);
    m_tabs = tabs;
    if (m_tabs) {
        connect(m_tabs, &EditorTabsModel::runRequested, this, &QueryController::onRunRequested);
        connect(m_tabs, &EditorTabsModel::dataRequested, this, &QueryController::onDataRequested);
        connect(m_tabs, &QAbstractItemModel::rowsRemoved, this, &QueryController::sync);
        connect(m_tabs, &QAbstractItemModel::modelReset, this, &QueryController::sync);
        // Connection changes (also when the connection is deleted) arrive as dataChanged.
        connect(m_tabs, &QAbstractItemModel::dataChanged, this, &QueryController::sync);
        connect(m_tabs, &EditorTabsModel::currentIndexChanged, this, &QueryController::sync);
        connect(m_tabs, &EditorTabsModel::countChanged, this, &QueryController::sync);
        connect(m_tabs, &QObject::destroyed, this, [this] { m_tabs = nullptr; });
    }
    sync();
    emit tabsChanged();
}

void QueryController::setHistory(HistoryModel *history)
{
    if (history == m_history)
        return;
    if (m_history)
        m_history->disconnect(this);
    m_history = history;
    if (m_history)
        connect(m_history, &QObject::destroyed, this, [this] { m_history = nullptr; });
    emit historyChanged();
}

void QueryController::recordStatement(const Tab &t, bool success, qint64 durationMs, qint64 rows,
                                      const QString &errorMessage)
{
    // The select behind a data tab is not something the user wrote.
    if (m_history == nullptr || !t.plan || t.plan->data || t.next >= t.plan->ranges.size())
        return;
    const QString sql = t.plan->ranges.at(t.next).sql.trimmed();
    const QUuid connection = QUuid::fromString(t.plan->connectionId);
    if (sql.isEmpty() || connection.isNull())
        return;
    HistoryEntry e;
    e.connectionId = connection;
    e.sql = sql;
    e.executedAt = t.statementStart;
    e.durationMs = durationMs;
    e.rows = rows;
    e.success = success;
    e.errorMessage = errorMessage;
    m_history->record(e);
}

ConnectionController *QueryController::connections() const
{
    return m_tabs ? m_tabs->connections() : nullptr;
}

void QueryController::bindConnections()
{
    ConnectionController *c = connections();
    if (c == m_connections)
        return;
    // Disconnecting by handle is safe even if the old controller is gone.
    for (const QMetaObject::Connection &link : std::as_const(m_connectionLinks))
        disconnect(link);
    m_connectionLinks.clear();
    m_connections = c;
    if (c) {
        m_connectionLinks = { connect(c, &ConnectionController::passwordAvailable, this,
                                      &QueryController::onPasswordAvailable),
                              connect(c, &ConnectionController::passwordUnavailable, this,
                                      &QueryController::onPasswordUnavailable),
                              connect(c->preflight(), &ConnectionPreflight::ready, this,
                                      &QueryController::onPreflightReady),
                              connect(c->preflight(), &ConnectionPreflight::failed, this,
                                      &QueryController::onPreflightFailed),
                              connect(c->preflight(), &ConnectionPreflight::cancelled, this,
                                      &QueryController::onPreflightCancelled) };
    }
}

int QueryController::sessionCount() const
{
    return static_cast<int>(std::ranges::count_if(
        m_tabStates, [](const auto &entry) { return entry.second->session != nullptr; }));
}

// ---- tabs --------------------------------------------------------------------------------------

int QueryController::rowOf(const QString &tabId) const
{
    if (!m_tabs || tabId.isEmpty())
        return -1;
    for (int i = 0; i < m_tabs->count(); ++i)
        if (m_tabs->idOf(i) == tabId)
            return i;
    return -1;
}

QString QueryController::title(const QString &tabId) const
{
    const int row = rowOf(tabId);
    return m_tabs && row >= 0 ? m_tabs->titleOf(row) : QString();
}

QueryController::Tab *QueryController::find(const QString &tabId)
{
    const auto it = m_tabStates.find(tabId);
    return it == m_tabStates.end() ? nullptr : it->second.get();
}

QueryController::Tab &QueryController::tabFor(const QString &tabId)
{
    auto &slot = m_tabStates[tabId];
    if (!slot) {
        slot = std::make_unique<Tab>();
        slot->id = tabId;
        slot->state = new QueryState(this);
        connect(slot->state->results(), &ResultTableModel::fetchRequested, this,
                [this, id = tabId](qsizetype rows) { onFetchRequested(id, rows); });
    }
    return *slot;
}

QueryState *QueryController::stateOf(const QString &tabId)
{
    return rowOf(tabId) < 0 ? nullptr : tabFor(tabId).state;
}

bool QueryController::hasSession(const QString &tabId) const
{
    const auto it = m_tabStates.find(tabId);
    return it != m_tabStates.end() && it->second->session != nullptr;
}

QString QueryController::resultSqlOf(const QString &tabId) const
{
    const auto it = m_tabStates.find(tabId);
    return it == m_tabStates.end() ? QString() : it->second->resultSql;
}

QStringList QueryController::tabIds() const
{
    QStringList ids;
    for (const auto &entry : m_tabStates)
        ids.append(entry.first);
    return ids;
}

void QueryController::updateCurrent()
{
    QueryState *s = m_none;
    if (m_tabs) {
        const QString id = m_tabs->idOf(m_tabs->currentIndex());
        if (!id.isEmpty())
            s = tabFor(id).state;
    }
    if (s == m_current)
        return;
    m_current = s;
    emit currentChanged();
}

// A tab that is gone, or whose connection changed, loses its session, its plan and its results.
void QueryController::sync()
{
    if (!m_tabs)
        return;
    const int before = sessionCount();
    for (const QString &id : tabIds()) {
        Tab *t = find(id);
        if (t == nullptr)
            continue;
        const int row = rowOf(id);
        if (row < 0) {
            dropPlan(*t);
            closeSession(*t);
            if (t->countSession != nullptr) {
                t->countSession->cancel();
                dropCount(*t);
            }
            if (t->state == m_current) {
                m_current = m_none;
                emit currentChanged();
            }
            t->state->deleteLater();
            m_tabStates.erase(id);
        } else if (!t->connectionId.isEmpty() && m_tabs->connectionIdOf(row) != t->connectionId) {
            dropPlan(*t);
            closeSession(*t);
            if (t->countSession != nullptr) {
                t->countSession->cancel();
                dropCount(*t);
            }
            t->connectionId.clear();
            t->resultSql.clear();
            t->state->reset();
        }
    }
    updateCurrent();
    if (sessionCount() != before)
        emit sessionsChanged();
}

void QueryController::dropPreflight(Tab &t)
{
    if (t.preflight == 0)
        return;
    if (ConnectionController *conns = connections())
        conns->preflight()->cancel(t.preflight);
    t.preflight = 0;
}

void QueryController::dropPlan(Tab &t)
{
    dropPreflight(t);
    t.plan.reset();
    t.pending.reset();
    t.stage = Stage::None;
    t.cancelRequested = false;
    t.ops.clear();
}

void QueryController::closeSession(Tab &t)
{
    t.opening = false;
    t.ops.clear();
    t.acceptRows = false;
    t.keyRequest = 0;
    t.product.clear();
    t.explainText.clear();
    if (t.applying && t.state != nullptr)
        t.state->edits()->setSaving(false); // the transaction ends on the worker; nobody waits
    t.applying = false;
    t.awaitingApply = false;
    if (t.session == nullptr)
        return;
    Session *s = t.session;
    t.session = nullptr;
    s->disconnect(this);
    // The session ends on its own thread; it is deleted once it says so.
    connect(s, &Session::closed, s, &QObject::deleteLater);
    s->close();
}

int QueryController::clampDataPageRows(int rows)
{
    return std::clamp(rows, kMinDataPageRows, kMaxDataPageRows);
}

void QueryController::setDataPageRows(int rows)
{
    rows = clampDataPageRows(rows);
    if (rows == m_dataPageRows)
        return;
    m_dataPageRows = rows;
    emit dataPageRowsChanged();
}

// ---- running -----------------------------------------------------------------------------------

void QueryController::onRunRequested(const QVariantMap &request)
{
    bindConnections();
    const QString tabId = request.value(QStringLiteral("tabId")).toString();
    const int row = rowOf(tabId);
    ConnectionController *conns = connections();
    if (row < 0 || conns == nullptr)
        return;
    Tab &t = tabFor(tabId);

    const bool all = request.value(QStringLiteral("kind")).toString() == QLatin1String("all");
    Plan plan;
    plan.connectionId = request.value(QStringLiteral("connectionId")).toString();
    plan.text = m_tabs->textOf(row);
    const QVariantList ranges = request.value(QStringLiteral("ranges")).toList();
    plan.total
        = all ? static_cast<int>(ranges.size()) : request.value(QStringLiteral("count")).toInt();
    const int index = request.value(QStringLiteral("index")).toInt();
    int i = 0;
    for (const QVariant &v : ranges) {
        const QVariantMap r = v.toMap();
        Range range;
        range.begin = r.value(QStringLiteral("begin")).toInt();
        const qsizetype end = r.value(QStringLiteral("end")).toInt();
        range.sql = plan.text.mid(range.begin, end - range.begin);
        range.startLine = r.value(QStringLiteral("startLine")).toInt();
        range.number = all ? i + 1 : index + 1;
        plan.ranges.push_back(std::move(range));
        ++i;
    }

    const auto profile = conns->model()->profile(QUuid::fromString(plan.connectionId));
    if (!profile) {
        t.plan = std::move(plan);
        t.next = 0;
        t.state->begin();
        failPlan(t, gone());
        return;
    }
    plan.production = profile->production;
    plan.connectionName = profile->name;

    // One confirmation for the whole plan, before anything runs.
    QVariantList items;
    for (const Range &r : std::as_const(plan.ranges)) {
        const SqlRisk risk = SqlSafety::check(r.sql, plan.production);
        if (risk == SqlRisk::None)
            continue;
        items.append(QVariantMap {
            { QStringLiteral("label"),
              tr("Statement %1 of %2, line %3").arg(r.number).arg(plan.total).arg(r.startLine) },
            { QStringLiteral("risk"), riskText(risk) },
            { QStringLiteral("sql"), preview(r.sql) } });
    }
    if (items.isEmpty()) {
        t.pending.reset();
        start(t, std::move(plan));
        return;
    }
    const QString name = plan.connectionName;
    const bool production = plan.production;
    t.pending = std::move(plan);
    emit confirmationRequested({ { QStringLiteral("tabId"), tabId },
                                 { QStringLiteral("connectionName"), name },
                                 { QStringLiteral("production"), production },
                                 { QStringLiteral("items"), items } });
}

void QueryController::confirm(const QString &tabId)
{
    Tab *t = find(tabId);
    if (t == nullptr || !t->pending)
        return;
    Plan plan = std::move(*t->pending);
    t->pending.reset();
    start(*t, std::move(plan));
}

void QueryController::decline(const QString &tabId)
{
    Tab *t = find(tabId);
    if (t == nullptr || !t->pending)
        return;
    t->pending.reset();
    announce(tabId, tr("Cancelled. Nothing was run."), false);
}

void QueryController::start(Tab &t, Plan plan)
{
    // A plan that was running is replaced: Session::execute() cancels its statement, and the
    // terminal signal of that statement is skipped (see Tab::ops).
    dropPreflight(t);
    if (t.plan && t.plan->explain && !plan.explain)
        t.state->plan()->setCancelled(); // the plan that was being made is replaced
    t.connectionId = plan.connectionId;
    t.plan = std::move(plan);
    t.next = 0;
    t.planId = ++m_nextPlan;
    t.cancelRequested = false;
    t.stage = Stage::None;
    t.columns.clear();
    t.elapsedBase = 0;
    t.elapsedTotal = 0;
    t.acceptRows = false; // rows of a fetch the new plan interrupted are dropped
    t.explainText.clear();
    t.explainRows = 0;
    t.explainCut = false;
    // A table that is read again keeps the rows on show until the first batch of the new read
    // arrives (the model swaps them in one reset); an Explain leaves the last result where it was.
    const bool rereading = t.plan->data && t.state->results()->columns() > 0;
    t.state->begin(t.plan->explain.has_value() || rereading);
    if (t.plan->data) {
        t.state->setCount(false, -1, {}); // the total of the previous view is stale
        if (rereading)
            t.state->results()->markStale();
    }
    if (const auto mode = t.plan->explain) {
        t.state->plan()->begin(*mode == ExplainMode::Analyze);
        emit planRequested(t.id);
    } else {
        emit runStarted(t.id);
    }
    if (t.session != nullptr)
        runNext(t);
    else
        openSession(t);
}

void QueryController::openSession(Tab &t)
{
    ConnectionController *conns = connections();
    if (conns == nullptr)
        return;
    const auto profile = conns->model()->profile(QUuid::fromString(t.connectionId));
    if (!profile) {
        failPlan(t, gone());
        return;
    }
    switch (profile->passwordMode) {
    case PasswordMode::None:
        proceed(t, {});
        break;
    case PasswordMode::Saved:
        t.stage = Stage::ReadingPassword;
        conns->readPassword(profile->id);
        break;
    case PasswordMode::AskEveryTime:
        if (const auto pw = conns->sessionPassword(profile->id)) {
            proceed(t, *pw);
            break;
        }
        t.stage = Stage::AwaitingPassword;
        emit passwordRequested(toId(profile->id), profile->name,
                               m_authFailed.contains(toId(profile->id)));
        break;
    }
}

void QueryController::proceed(Tab &t, const QString &password)
{
    t.stage = Stage::None;
    ConnectionController *conns = connections();
    if (conns == nullptr)
        return;
    const auto profile = conns->model()->profile(QUuid::fromString(t.connectionId));
    if (!profile) {
        failPlan(t, gone());
        return;
    }
    if (ConnectionPreflight::usesSsh(*profile)) {
        // The tunnel's secret and the server's host key first; onPreflightReady() goes on.
        t.stage = Stage::Preflight;
        t.preflight = conns->preflight()->begin(*profile, password);
        return;
    }
    openWith(t, *profile, Secrets { password });
}

void QueryController::openWith(Tab &t, const ConnectionProfile &profile, const Secrets &secrets)
{
    ConnectionController *conns = connections();
    if (conns == nullptr)
        return;
    // shortcut: loads the provider plugin on the UI thread (a few ms, once per engine), like the
    // schema tree; move it to the session thread when plugins grow heavy.
    auto connection = conns->registry().createConnection(profile, secrets);
    if (!connection) {
        failPlan(t, connection.error());
        return;
    }
    const QString id = t.id;
    auto *s = new Session(std::move(connection).value(), this);
    t.session = s;
    t.opening = true;
    connect(s, &Session::opened, this, [this, id](const ServerInfo &info) { onOpened(id, info); });
    connect(s, &Session::columnsReady, this,
            [this, id](const std::vector<ColumnInfo> &columns) { onColumns(id, columns); });
    connect(s, &Session::finished, this,
            [this, id](const ExecutionStats &stats) { onFinished(id, stats); });
    connect(s, &Session::failed, this, [this, id](const Error &e) { onFailed(id, e); });
    connect(s, &Session::cancelled, this, [this, id] { onCancelled(id); });
    connect(s, &Session::batchReady, this,
            [this, id](const std::shared_ptr<const ResultBatch> &batch) { onBatch(id, batch); });
    connect(s, &Session::childrenReady, this,
            [this, id](quint64 request, const ObjectPath &, const std::vector<SchemaNode> &nodes) {
                onChildren(id, request, nodes);
            });
    connect(s, &Session::childrenFailed, this,
            [this, id](quint64 request, const ObjectPath &, const Error &e) {
                onChildrenFailed(id, request, e);
            });
    connect(s, &Session::changesApplied, this,
            [this, id](int count) { onChangesApplied(id, count); });
    connect(s, &Session::changesFailed, this,
            [this, id](int index, const Error &e) { onChangesFailed(id, index, e); });
    s->open();
    emit sessionsChanged();
    runNext(t);
}

void QueryController::runNext(Tab &t)
{
    if (!t.plan || t.session == nullptr)
        return;
    if (t.next >= t.plan->ranges.size()) {
        finishDone(t);
        return;
    }
    if (t.plan->data && t.plan->ranges.at(t.next).sql.isEmpty()) {
        requestKey(t); // the select is written once the key of the table is known
        return;
    }
    if (t.plan->explain) {
        // The syntax can depend on the server (MySQL or MariaDB): onOpened() goes on from here.
        if (t.opening)
            return;
        if (t.plan->ranges.at(t.next).sql.isEmpty() && !prepareExplain(t))
            return;
    }
    const Range &r = t.plan->ranges.at(t.next);
    t.columns.clear();
    t.elapsedBase = t.elapsedTotal;
    // This statement replaces the stream of the previous one; the grid keeps its rows until a
    // statement with columns replaces them (the last one that returned columns wins).
    t.acceptRows = false;
    t.state->results()->detachStream();
    if (t.plan->explain) {
        announce(t.id,
                 t.plan->explain == ExplainMode::Analyze ? tr("Running the query to analyze it")
                                                         : tr("Getting the query plan"),
                 false);
    } else {
        announce(t.id, tr("Running statement %1 of %2").arg(r.number).arg(t.plan->total), false);
    }
    t.ops.push_back(t.planId);
    Statement statement;
    statement.sql = r.sql;
    statement.parameters = r.parameters;
    statement.fetchSize = kFetchSize;
    if (t.plan->explain)
        statement.fetchSize = kExplainFetchRows;
    else if (t.plan->data)
        statement.fetchSize = m_dataPageRows;
    t.statementStart = QDateTime::currentDateTimeUtc();
    t.statementClock.start();
    t.session->execute(statement);
}

bool QueryController::currentOp(const Tab &t) const
{
    return t.plan && !t.ops.isEmpty() && t.ops.first() == t.planId;
}

// Takes the finished execute off the list; true when it belongs to the plan that is running.
bool QueryController::popOp(Tab &t)
{
    if (t.ops.isEmpty())
        return false;
    const quint64 id = t.ops.takeFirst();
    return t.plan && id == t.planId;
}

void QueryController::onOpened(const QString &tabId, const ServerInfo &info)
{
    Tab *t = find(tabId);
    if (t == nullptr)
        return;
    const bool wasOpening = t->opening;
    t->opening = false;
    t->product = info.product;
    m_authFailed.remove(t->connectionId);
    if (ConnectionController *conns = connections())
        conns->preflight()->openSucceeded(QUuid::fromString(t->connectionId));
    // An Explain waited for the server's product to write its statement.
    if (wasOpening && t->plan && t->plan->explain && t->ops.isEmpty())
        runNext(*t);
}

void QueryController::onColumns(const QString &tabId, const std::vector<ColumnInfo> &columns)
{
    Tab *t = find(tabId);
    if (t == nullptr || !currentOp(*t))
        return;
    t->columns.clear();
    for (const ColumnInfo &c : columns)
        t->columns.append(c.name);
    if (t->plan && t->plan->explain) // the rows are the plan's text, not a grid
        return;
    if (!columns.empty()) {
        if (t->plan && t->next < t->plan->ranges.size())
            t->resultSql = t->plan->ranges.at(t->next).sql;
        t->acceptRows = true;
        t->state->results()->begin(columns);
    }
}

void QueryController::onBatch(const QString &tabId, const std::shared_ptr<const ResultBatch> &batch)
{
    Tab *t = find(tabId);
    if (t != nullptr && t->plan && t->plan->explain) {
        if (currentOp(*t))
            appendExplain(*t, *batch);
        return;
    }
    if (t != nullptr && t->acceptRows)
        t->state->results()->append(batch);
}

// The grid asks for more rows of the last result. Only an idle session can: a plan that is
// running owns it (and its next statement replaces the stream anyway).
void QueryController::onFetchRequested(const QString &tabId, qsizetype rows)
{
    Tab *t = find(tabId);
    if (t == nullptr)
        return;
    if (t->session == nullptr || t->plan || t->applying || !t->ops.isEmpty() || !t->acceptRows) {
        t->state->results()->fetchDeclined();
        return;
    }
    t->ops.push_back(kFetchOp);
    t->session->fetchMore(rows);
}

bool QueryController::popFetchOp(Tab &t)
{
    if (t.ops.isEmpty() || t.ops.first() != kFetchOp)
        return false;
    t.ops.removeFirst();
    return true;
}

// A plan that started meanwhile owns the state: what the interrupted fetch ended with is stale.
void QueryController::onFetchFinished(Tab &t, const ExecutionStats &stats)
{
    if (t.plan)
        return;
    t.elapsedTotal = t.elapsedBase + stats.elapsedMs;
    t.state->setSummary(stats.rowsFetched, stats.affectedRows.value_or(-1), t.elapsedTotal,
                        t.columns, stats.atEnd);
    t.state->results()->finishStream(stats.atEnd);
}

void QueryController::onFetchFailed(Tab &t, const Error &error)
{
    if (t.plan)
        return;
    t.state->results()->streamLost();
    t.state->messages()->append(
        { QStringLiteral("error"), kindText(error.kind), error.message, error.detail, 0, 0 });
    announce(t.id, error.message, true);
    emit planFailed(t.id);
}

void QueryController::onFinished(const QString &tabId, const ExecutionStats &stats)
{
    Tab *t = find(tabId);
    if (t == nullptr)
        return;
    if (popFetchOp(*t)) {
        onFetchFinished(*t, stats);
        return;
    }
    if (!popOp(*t) || !t->plan)
        return;
    if (t->plan->explain) {
        finishExplain(*t, stats);
        return;
    }
    t->elapsedTotal = t->elapsedBase + stats.elapsedMs;
    if (!t->columns.isEmpty())
        t->state->results()->finishStream(stats.atEnd);
    t->state->setSummary(stats.rowsFetched, stats.affectedRows.value_or(-1), t->elapsedTotal,
                         t->columns, stats.atEnd);
    recordStatement(*t, true, stats.elapsedMs,
                    stats.affectedRows.value_or(t->columns.isEmpty() ? -1 : stats.rowsFetched), {});
    const Range &r = t->plan->ranges.at(t->next);
    if (SqlSafety::changesSchema(r.sql))
        emit schemaChanged(t->plan->connectionId);
    t->state->messages()->append({ QStringLiteral("statement"),
                                   tr("Statement"),
                                   tr("Statement %1 of %2: %3")
                                       .arg(r.number)
                                       .arg(t->plan->total)
                                       .arg(t->state->resultText()),
                                   {},
                                   0,
                                   0 });
    for (const QString &n : stats.notices)
        t->state->messages()->append({ QStringLiteral("notice"), tr("Notice"), n, {}, 0, 0 });
    if (t->cancelRequested) { // the statement ended before the cancel reached it
        finishCancelled(*t);
        return;
    }
    ++t->next;
    runNext(*t);
}

void QueryController::onFailed(const QString &tabId, const Error &error)
{
    Tab *t = find(tabId);
    if (t == nullptr)
        return;
    if (t->opening) { // open() failed; the statement queued behind it goes with the session
        ConnectionController *conns = connections();
        Error shown = error;
        bool sshAuth = false;
        if (conns != nullptr) {
            if (const auto profile = conns->model()->profile(QUuid::fromString(t->connectionId))) {
                sshAuth = ConnectionPreflight::isSshAuthError(*profile, error);
                shown = conns->preflight()->openFailed(*profile, error);
            }
        }
        if (error.kind == ErrorKind::Authentication && conns != nullptr && !sshAuth) {
            m_authFailed.insert(t->connectionId);
            // A wrong password must not be tried again silently.
            conns->forgetPassword(QUuid::fromString(t->connectionId));
        }
        closeSession(*t);
        if (t->plan)
            failPlan(*t, shown);
        return;
    }
    if (popFetchOp(*t)) {
        onFetchFailed(*t, error);
        return;
    }
    if (popOp(*t)) {
        if (error.kind != ErrorKind::Cancelled)
            recordStatement(*t, false, t->statementClock.elapsed(), -1, error.message);
        failPlan(*t, error);
    }
}

void QueryController::onCancelled(const QString &tabId)
{
    Tab *t = find(tabId);
    if (t == nullptr)
        return;
    if (t->opening) {
        closeSession(*t);
        if (t->plan)
            finishCancelled(*t);
        return;
    }
    if (popFetchOp(*t)) {
        if (!t->plan)
            t->state->results()->streamLost();
        return;
    }
    if (popOp(*t))
        finishCancelled(*t);
}

void QueryController::cancel(const QString &tabId)
{
    Tab *t = find(tabId);
    if (t == nullptr || !t->plan)
        return;
    t->cancelRequested = true;
    // Waiting for a password, the keychain or the SSH tunnel's checks, or nothing sent yet:
    // nothing to interrupt.
    if (t->stage != Stage::None || t->session == nullptr || t->ops.isEmpty()) {
        dropPreflight(*t);
        t->stage = Stage::None;
        finishCancelled(*t);
        return;
    }
    t->session->cancel();
}

void QueryController::cancelCurrent()
{
    if (m_tabs)
        cancel(m_tabs->idOf(m_tabs->currentIndex()));
}

void QueryController::finishDone(Tab &t)
{
    const QueryState &s = *t.state;
    QString text;
    if (!s.columns().isEmpty())
        text = tr("Done: %1 in %2 ms").arg(s.resultText()).arg(s.elapsedMs());
    else if (s.affectedRows() >= 0)
        text = s.resultText();
    else
        text = tr("Done in %1 ms").arg(s.elapsedMs());
    t.plan.reset();
    t.afterSave = false; // the new rows replaced the pending changes
    t.state->results()->abandonStale();
    t.state->finish(QueryState::State::Done);
    announce(t.id, text, false);
}

void QueryController::finishCancelled(Tab &t)
{
    if (t.plan && t.plan->explain)
        t.state->plan()->setCancelled();
    t.plan.reset();
    t.cancelRequested = false;
    endReload(t); // a cancelled reload leaves the old rows where they were
    t.state->messages()->append(
        { QStringLiteral("cancelled"), tr("Cancelled"), tr("Query cancelled"), {}, 0, 0 });
    t.state->finish(QueryState::State::Cancelled);
    announce(t.id, tr("Query cancelled"), false);
}

void QueryController::failPlan(Tab &t, const Error &error)
{
    int offset = -1;
    int length = 0;
    int line = 0;
    int column = 0;
    const int row = rowOf(t.id);
    const int position = error.position.value_or(-1);
    const bool explaining = t.plan && t.plan->explain;
    // The position only means something for the text that was run (not for an EXPLAIN of it).
    if (t.plan && !explaining && position >= 0 && t.next < t.plan->ranges.size() && row >= 0
        && !t.plan->text.isEmpty() && m_tabs->textOf(row) == t.plan->text) {
        const QString &text = t.plan->text;
        const Range &r = t.plan->ranges.at(t.next);
        const qsizetype size = text.size();
        // At the very end (e.g. "unexpected end of input") the last character is underlined.
        offset = static_cast<int>(
            std::min(r.begin + std::min<qsizetype>(position, r.sql.size()), size - 1));
        qsizetype end = offset;
        while (end < size && isWordChar(text.at(end)))
            ++end;
        length = std::max(1, static_cast<int>(end) - offset);
        line = 1 + static_cast<int>(QStringView(text).left(offset).count(QLatin1Char('\n')));
        const qsizetype lastBreak
            = offset > 0 ? text.lastIndexOf(QLatin1Char('\n'), offset - 1) : -1;
        column = static_cast<int>(offset - lastBreak);
    }
    // A data tab that failed to read its rows with a typed condition: the filter box says it.
    const bool badView = t.plan && t.plan->data && !t.whereExpression.isEmpty();
    t.plan.reset();
    t.cancelRequested = false;
    endReload(t); // a failed reload leaves the old rows where they were
    t.state->messages()->append({ QStringLiteral("error"), kindText(error.kind), error.message,
                                  error.detail, line, column });
    t.state->setError(offset, length, line, column);
    if (t.table)
        t.state->edits()->setLastError(error.message);
    t.state->finish(QueryState::State::Failed);
    QString text = error.message;
    if (line > 0)
        text += QLatin1Char(' ') + tr("Error at line %1, column %2").arg(line).arg(column);
    announce(t.id, text, true);
    if (badView)
        emit viewFailed(t.id, error.message);
    if (explaining) // the Plan tab says it; the results stay in front
        t.state->plan()->setFailure(error.message, error.detail, {});
    else
        emit planFailed(t.id);
}

void QueryController::onPasswordAvailable(QUuid id, const QString &password)
{
    const QString key = toId(id);
    for (const QString &tabId : tabIds()) {
        Tab *t = find(tabId);
        if (t != nullptr && t->plan && t->stage == Stage::ReadingPassword && t->connectionId == key)
            proceed(*t, password);
    }
}

void QueryController::onPasswordUnavailable(QUuid id, const Error &error)
{
    const QString key = toId(id);
    for (const QString &tabId : tabIds()) {
        Tab *t = find(tabId);
        if (t != nullptr && t->plan && t->stage == Stage::ReadingPassword
            && t->connectionId == key) {
            t->stage = Stage::None;
            failPlan(*t, error);
        }
    }
}

QueryController::Tab *QueryController::findByPreflight(int request)
{
    for (const QString &tabId : tabIds()) {
        Tab *t = find(tabId);
        if (t != nullptr && t->plan && t->stage == Stage::Preflight && t->preflight == request)
            return t;
    }
    return nullptr;
}

void QueryController::onPreflightReady(int request, const Secrets &secrets)
{
    Tab *t = findByPreflight(request);
    ConnectionController *conns = connections();
    if (t == nullptr || conns == nullptr)
        return;
    t->preflight = 0;
    t->stage = Stage::None;
    const auto profile = conns->model()->profile(QUuid::fromString(t->connectionId));
    if (!profile) {
        failPlan(*t, gone());
        return;
    }
    openWith(*t, *profile, secrets);
}

void QueryController::onPreflightFailed(int request, const Error &error)
{
    Tab *t = findByPreflight(request);
    if (t == nullptr)
        return;
    t->preflight = 0;
    t->stage = Stage::None;
    failPlan(*t, error);
}

// The user dismissed the SSH secret or the host key prompt.
void QueryController::onPreflightCancelled(int request)
{
    Tab *t = findByPreflight(request);
    if (t == nullptr)
        return;
    t->preflight = 0;
    t->stage = Stage::None;
    finishCancelled(*t);
}

void QueryController::providePassword(const QString &connectionId, const QString &password)
{
    ConnectionController *conns = connections();
    if (conns == nullptr)
        return;
    conns->rememberPassword(QUuid::fromString(connectionId), password);
    for (const QString &tabId : tabIds()) {
        Tab *t = find(tabId);
        if (t != nullptr && t->plan && t->stage == Stage::AwaitingPassword
            && t->connectionId == connectionId)
            proceed(*t, password);
    }
}

void QueryController::cancelPassword(const QString &connectionId)
{
    for (const QString &tabId : tabIds()) {
        Tab *t = find(tabId);
        if (t != nullptr && t->plan && t->stage == Stage::AwaitingPassword
            && t->connectionId == connectionId) {
            t->stage = Stage::None;
            finishCancelled(*t);
        }
    }
}

// ---- query plan --------------------------------------------------------------------------------

bool QueryController::supportsExplain(const QString &connectionId)
{
    ConnectionController *conns = connections();
    if (conns == nullptr || connectionId.isEmpty())
        return false;
    const auto profile = conns->model()->profile(QUuid::fromString(connectionId));
    if (!profile)
        return false;
    // shortcut: loads the engine's plugin on the UI thread (a few ms, once per engine), as
    // EditorTabsModel does.
    IDatabaseProvider *provider = conns->registry().provider(profile->providerId, nullptr);
    return provider != nullptr && provider->info().caps.testFlag(Capability::Explain);
}

void QueryController::refuseExplain(const QString &tabId, const QString &message)
{
    tabFor(tabId).state->plan()->setNotice(message);
    emit planRequested(tabId);
}

QVariantMap QueryController::explain(const QString &tabId, bool analyze, int position)
{
    bindConnections();
    QVariantMap out { { QStringLiteral("ok"), false },
                      { QStringLiteral("message"), QString() },
                      { QStringLiteral("ranges"), QVariantList() } };
    const auto say = [&out](const QString &message) {
        out.insert(QStringLiteral("message"), message);
        return out;
    };
    const int row = rowOf(tabId);
    ConnectionController *conns = connections();
    if (row < 0 || conns == nullptr)
        return say(tr("No query is open."));
    if (m_tabs->isData(row))
        return say(tr("A data tab shows a table; it has no query to explain."));
    if (m_tabs->isMigration(row))
        return say(tr("A migration tab shows code; it has no query to explain."));
    const QString connectionId = m_tabs->connectionIdOf(row);
    if (connectionId.isEmpty())
        return say(tr("Choose a connection first."));
    const auto profile = conns->model()->profile(QUuid::fromString(connectionId));
    if (!profile)
        return say(gone().message);
    IDatabaseProvider *provider = conns->registry().provider(profile->providerId, nullptr);
    if (provider == nullptr)
        return say(tr("The database engine could not be loaded."));
    const ProviderInfo info = provider->info();

    // The statement under the cursor, split from the text as it is now (like Run).
    const QVariantMap found = m_tabs->statementNow(row, position);
    if (found.value(QStringLiteral("index")).toInt() < 0)
        return say(tr("Nothing to run."));
    const QString text = m_tabs->textOf(row);
    const int begin = found.value(QStringLiteral("begin")).toInt();
    const int end = found.value(QStringLiteral("end")).toInt();
    const QString sql = text.mid(begin, end - begin).trimmed();
    if (sql.isEmpty())
        return say(tr("Nothing to run."));

    Tab &t = tabFor(tabId);
    const ExplainMode mode = analyze ? ExplainMode::Analyze : ExplainMode::Plan;
    // Every statement of one tab goes through one conversation: refused ones say why in the Plan
    // tab and in words for the screen reader, and start nothing.
    const auto refuse = [&](const QString &message) {
        refuseExplain(tabId, message);
        return say(message);
    };
    if (!info.caps.testFlag(Capability::Explain))
        return refuse(tr("%1 can't show a query plan.").arg(info.displayName));
    if (provider->dialect().explainStatement(sql, mode, t.product).isEmpty()) {
        return refuse(analyze
                          ? tr("%1 can't run Explain analyze. Use Explain to see the estimated "
                               "plan.")
                                .arg(info.displayName)
                          : tr("%1 can't show a plan for this statement.").arg(info.displayName));
    }
    if (analyze && !isReadOnlyQuery(sql)) {
        return refuse(tr("Explain analyze runs the statement, so it only takes SELECT queries. Use "
                         "Explain to see the plan without running it."));
    }

    Plan plan;
    plan.connectionId = connectionId;
    plan.connectionName = profile->name;
    plan.production = profile->production;
    plan.text = text;
    plan.total = 1;
    plan.explain = mode;
    plan.explainSql = sql;
    Range range;
    range.begin = begin;
    range.startLine = found.value(QStringLiteral("startLine")).toInt();
    plan.ranges.push_back(std::move(range));

    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("ranges"), QVariantList { found });
    out.insert(QStringLiteral("message"),
               analyze ? tr("Explain analyze, line %1").arg(plan.ranges.front().startLine)
                       : tr("Explain, line %1").arg(plan.ranges.front().startLine));
    if (!analyze) {
        t.pending.reset();
        start(t, std::move(plan));
        return out;
    }
    // The whole query runs: nothing starts until the user agrees.
    t.pending = std::move(plan);
    emit analyzeConfirmationRequested({ { QStringLiteral("tabId"), tabId },
                                        { QStringLiteral("connectionName"), profile->name },
                                        { QStringLiteral("production"), profile->production },
                                        { QStringLiteral("sql"), preview(sql) } });
    return out;
}

// The EXPLAIN of the user's statement, as this engine and server write it. false: nothing to send.
bool QueryController::prepareExplain(Tab &t)
{
    if (!t.plan)
        return false;
    const ISqlDialect *dialect = dialectOf(t);
    const ExplainMode mode = t.plan->explain.value_or(ExplainMode::Plan);
    const QString sql
        = dialect ? dialect->explainStatement(t.plan->explainSql, mode, t.product) : QString();
    if (sql.isEmpty()) {
        failPlan(t,
                 Error { ErrorKind::Unsupported,
                         tr("This engine can't show a plan for this statement."),
                         {},
                         {},
                         {} });
        return false;
    }
    t.plan->ranges[t.next].sql = sql;
    return true;
}

// The cells of a row joined by a tab and the rows by a newline: what parseExplain() reads. No
// QVariant per cell: the text comes straight from the column buffers.
void QueryController::appendExplain(Tab &t, const ResultBatch &batch)
{
    for (qsizetype r = 0; r < batch.rowCount && !t.explainCut; ++r) {
        if (t.explainRows++ > 0)
            t.explainText += QLatin1Char('\n');
        bool firstCell = true;
        for (const ColumnBuffer &column : batch.columns) {
            if (!firstCell)
                t.explainText += QLatin1Char('\t');
            firstCell = false;
            if (!column.isNull(r))
                t.explainText += CellFormatter::copy(column, r);
        }
        t.explainCut = t.explainText.size() > kMaxExplainChars;
    }
}

void QueryController::explainFailed(Tab &t, const Error &error, const QString &rawText)
{
    t.plan.reset();
    t.cancelRequested = false;
    t.state->messages()->append(
        { QStringLiteral("error"), kindText(error.kind), error.message, error.detail, 0, 0 });
    t.state->plan()->setFailure(error.message, error.detail, rawText);
    t.state->finish(QueryState::State::Failed);
    announce(t.id, error.message, true);
}

// The EXPLAIN ran: its rows are in explainText. A small text is parsed here; a big one on the
// parse thread, so the UI never waits for it, and its result comes back as a queued call.
void QueryController::finishExplain(Tab &t, const ExecutionStats &stats)
{
    if (!t.plan)
        return;
    t.elapsedTotal = t.elapsedBase + stats.elapsedMs;
    recordStatement(t, true, stats.elapsedMs, stats.rowsFetched, {});
    if (t.cancelRequested) { // the statement ended before the cancel reached it
        t.explainText.clear();
        finishCancelled(t);
        return;
    }
    QString text = std::move(t.explainText);
    t.explainText.clear();
    const ISqlDialect *dialect = dialectOf(t);
    if (t.explainCut) {
        explainFailed(
            t, Error { ErrorKind::Internal, tr("The plan is too large to show."), {}, {}, {} }, {});
        return;
    }
    if (dialect == nullptr) {
        explainFailed(
            t,
            Error {
                ErrorKind::Internal, tr("The database engine could not be loaded."), {}, {}, {} },
            text);
        return;
    }
    const ExplainMode mode = t.plan->explain.value_or(ExplainMode::Plan);
    if (text.size() <= kParseOnUiChars) {
        onExplainParsed(t.id, t.planId, dialect->parseExplain(text, mode, t.product), text);
        return;
    }
    const auto input = std::make_shared<const QString>(std::move(text));
    m_parsePool.start([this, dialect, input, mode, product = t.product, id = t.id,
                       planId = t.planId] {
        auto parsed
            = std::make_shared<Result<ExplainPlan>>(dialect->parseExplain(*input, mode, product));
        QMetaObject::invokeMethod(
            this,
            [this, id, planId, parsed, input] {
                onExplainParsed(id, planId, std::move(*parsed), *input);
            },
            Qt::QueuedConnection);
    });
}

void QueryController::onExplainParsed(const QString &tabId, quint64 planId,
                                      Result<ExplainPlan> &&parsed, const QString &text)
{
    Tab *t = find(tabId);
    // A plan that was cancelled or replaced meanwhile left nothing to receive this.
    if (t == nullptr || !t->plan || !t->plan->explain || t->planId != planId)
        return;
    if (!parsed) {
        explainFailed(*t, parsed.error(), text);
        return;
    }
    ExplainPlan plan = std::move(parsed).value();
    if (plan.rawText.isEmpty())
        plan.rawText = text;
    PlanModel *model = t->state->plan();
    model->setPlan(plan);
    const int steps = model->nodeCount();
    t->state->setSummary(0, -1, t->elapsedTotal, {}, true);
    t->plan.reset();
    t->state->finish(QueryState::State::Done);
    QString said
        = steps == 1 ? tr("Query plan ready, 1 step") : tr("Query plan ready, %1 steps").arg(steps);
    if (model->truncated())
        said += QLatin1String(". ") + tr("Showing the first %1 steps").arg(steps);
    t->state->messages()->append({ QStringLiteral("statement"), tr("Statement"), said, {}, 0, 0 });
    announce(tabId, said, false);
}

// ---- data tabs ---------------------------------------------------------------------------------

void QueryController::onDataRequested(const QString &tabId)
{
    bindConnections();
    const int row = rowOf(tabId);
    ConnectionController *conns = connections();
    if (row < 0 || conns == nullptr)
        return;
    const auto table = m_tabs->tableOf(row);
    if (!table)
        return;
    Tab &t = tabFor(tabId);
    t.table = table;
    if (m_initialView && m_initialView->connectionId == m_tabs->connectionIdOf(row)
        && m_initialView->parts == table->parts) {
        t.filters = std::move(m_initialView->filters);
        t.sort.reset();
        t.whereExpression.clear();
        t.state->setFilterOrigin(m_initialView->origin);
        m_initialView.reset();
    }
    // A count of the previous view is stale; its session is told to stop.
    if (t.countSession != nullptr) {
        t.countSession->cancel();
        dropCount(t);
    }
    t.state->results()->setPageRows(m_dataPageRows);
    Plan plan;
    plan.data = true;
    plan.total = 1;
    plan.connectionId = m_tabs->connectionIdOf(row);
    plan.ranges.push_back(Range {}); // its sql is filled in by requestKey()
    t.state->edits()->setLastError({});
    const auto profile = conns->model()->profile(QUuid::fromString(plan.connectionId));
    if (!profile) {
        t.plan = std::move(plan);
        t.next = 0;
        t.state->begin();
        failPlan(t, gone());
        return;
    }
    plan.production = profile->production;
    plan.connectionName = profile->name;
    start(t, std::move(plan));
}

void QueryController::reload(const QString &tabId)
{
    Tab *t = find(tabId);
    if (t == nullptr || !t->table)
        return;
    if (t->applying) {
        announce(tabId, tr("Saving changes, please wait."), false);
        return;
    }
    t->awaitingApply = false;
    t->afterSave = false;
    t->state->edits()->discardAll();
    onDataRequested(tabId);
}

// A read of the table ended without replacing the rows on show (it failed or was cancelled): they
// stay, and the changes that were saved are no longer pending.
void QueryController::endReload(Tab &t)
{
    t.state->results()->abandonStale();
    if (t.afterSave) {
        t.afterSave = false;
        t.state->edits()->discardAll();
    }
}

const ISqlDialect *QueryController::dialectOf(const Tab &t) const
{
    ConnectionController *conns = connections();
    if (conns == nullptr)
        return nullptr;
    const auto profile = conns->model()->profile(QUuid::fromString(t.connectionId));
    if (!profile)
        return nullptr;
    // shortcut: loads the engine's plugin on the UI thread (a few ms, once per engine), as
    // EditorTabsModel does.
    IDatabaseProvider *provider = conns->registry().provider(profile->providerId, nullptr);
    return provider ? &provider->dialect() : nullptr;
}

// The key of a table comes from the columns folder of the tree (Session::children): the column
// nodes say "PK" in their detail. shortcut: Session has no describe(); ask core for one if that
// detail text ever stops being a stable contract.
void QueryController::requestKey(Tab &t)
{
    const ISqlDialect *dialect = dialectOf(t);
    if (dialect == nullptr || !t.table || t.session == nullptr) {
        failPlan(
            t,
            Error {
                ErrorKind::Internal, tr("The database engine could not be loaded."), {}, {}, {} });
        return;
    }
    if (t.table->kind != ObjectKind::Table) {
        TableEditModel::Config config;
        config.table = *t.table;
        config.readOnlyReason = t.table->kind == ObjectKind::View
            ? tr("This is a view, so it is read-only.")
            : tr("This is a materialized view, so it is read-only.");
        if (!writeSelect(t, *dialect, std::nullopt))
            return;
        t.state->edits()->configure(config);
        runNext(t);
        return;
    }
    t.keyRequest = t.session->children(ObjectPath {
        t.table->parts + QStringList { QStringLiteral("Columns") }, ObjectKind::Folder });
}

void QueryController::onChildren(const QString &tabId, quint64 requestId,
                                 const std::vector<SchemaNode> &nodes)
{
    Tab *t = find(tabId);
    if (t == nullptr || !t->plan || !t->plan->data || t->keyRequest == 0
        || requestId != t->keyRequest)
        return;
    t->keyRequest = 0;
    const ISqlDialect *dialect = dialectOf(*t);
    if (dialect == nullptr || !t->table) {
        failPlan(
            *t,
            Error {
                ErrorKind::Internal, tr("The database engine could not be loaded."), {}, {}, {} });
        return;
    }
    QStringList primary;
    QSet<QString> names;
    for (const SchemaNode &n : nodes) {
        names.insert(n.name.toLower());
        if (n.detail.split(QLatin1Char(' '), Qt::SkipEmptyParts).contains(QLatin1String("PK")))
            primary.append(n.name);
    }
    TableEditModel::Config config;
    config.table = *t->table;
    std::optional<QString> hidden;
    if (!primary.isEmpty()) {
        config.keyKind = RowKey::Kind::PrimaryKey;
        config.keyColumns = primary;
    } else {
        QString identifier;
        if (const auto id = dialect->rowIdentifier()) {
            identifier = *id;
            if (id->compare(QLatin1String("rowid"), Qt::CaseInsensitive) == 0
                && names.contains(id->toLower())) {
                // SQLite: a column called rowid hides the alias; its other names still reach the
                // row.
                identifier.clear();
                for (const QString &alias : { QStringLiteral("_rowid_"), QStringLiteral("oid") })
                    if (identifier.isEmpty() && !names.contains(alias))
                        identifier = alias;
            } else if (names.contains(id->toLower())) {
                identifier.clear();
            }
        }
        if (identifier.isEmpty()) {
            config.readOnlyReason
                = tr("This table has no primary key and no usable row identifier, so it is "
                     "read-only.");
        } else {
            config.keyKind = RowKey::Kind::RowIdentifier;
            config.keyColumns = { identifier };
            config.warning = tr("This table has no primary key. Rows are identified by %1, which "
                                "can change; refresh after saving.")
                                 .arg(identifier);
            hidden = identifier;
        }
    }
    if (!writeSelect(*t, *dialect, hidden))
        return;
    t->state->edits()->configure(config);
    runNext(*t);
}

bool QueryController::writeSelect(Tab &t, const ISqlDialect &dialect,
                                  const std::optional<QString> &hidden)
{
    if (!t.table || !t.plan)
        return false;
    TableQuery query;
    query.table = *t.table;
    query.hiddenColumn = hidden;
    query.filters = t.filters;
    query.whereExpression = t.whereExpression;
    query.sort = t.sort;
    auto statement = TableQueryBuilder::select(dialect, query);
    if (!statement) {
        failPlan(t, statement.error());
        return false;
    }
    Range &r = t.plan->ranges[t.next];
    r.sql = statement->sql;
    r.parameters = statement->parameters;
    return true;
}

// ---- sort and filter of a data tab (H11) -------------------------------------------------------

namespace {

struct FilterOperator
{
    const char *name;
    const char *label;
    ColumnFilter::Op op;
    bool needsValue;
};

// The labels are translated by filterOperators().
// clang-format off
constexpr FilterOperator kFilterOperators[] = {
    { "equals", QT_TRANSLATE_NOOP("quarry::ui::QueryController", "equals"),
      ColumnFilter::Op::Equals, true },
    { "notEquals", QT_TRANSLATE_NOOP("quarry::ui::QueryController", "does not equal"),
      ColumnFilter::Op::NotEquals, true },
    { "contains", QT_TRANSLATE_NOOP("quarry::ui::QueryController", "contains"),
      ColumnFilter::Op::Contains, true },
    { "startsWith", QT_TRANSLATE_NOOP("quarry::ui::QueryController", "starts with"),
      ColumnFilter::Op::StartsWith, true },
    { "less", QT_TRANSLATE_NOOP("quarry::ui::QueryController", "is less than"),
      ColumnFilter::Op::Less, true },
    { "lessOrEqual", QT_TRANSLATE_NOOP("quarry::ui::QueryController", "is at most"),
      ColumnFilter::Op::LessOrEqual, true },
    { "greater", QT_TRANSLATE_NOOP("quarry::ui::QueryController", "is greater than"),
      ColumnFilter::Op::Greater, true },
    { "greaterOrEqual", QT_TRANSLATE_NOOP("quarry::ui::QueryController", "is at least"),
      ColumnFilter::Op::GreaterOrEqual, true },
    { "isNull", QT_TRANSLATE_NOOP("quarry::ui::QueryController", "is NULL"),
      ColumnFilter::Op::IsNull, false },
    { "isNotNull", QT_TRANSLATE_NOOP("quarry::ui::QueryController", "is not NULL"),
      ColumnFilter::Op::IsNotNull, false },
};
// clang-format on

std::optional<ColumnFilter::Op> filterOp(const QString &name)
{
    for (const FilterOperator &o : kFilterOperators)
        if (name == QLatin1String(o.name))
            return o.op;
    return std::nullopt;
}

QString filterOpName(ColumnFilter::Op op)
{
    for (const FilterOperator &o : kFilterOperators)
        if (o.op == op)
            return QLatin1String(o.name);
    return {};
}

} // namespace

QVariantList QueryController::filterOperators()
{
    QVariantList out;
    for (const FilterOperator &o : kFilterOperators) {
        QVariantMap item;
        item.insert(QStringLiteral("op"), QLatin1String(o.name));
        item.insert(QStringLiteral("label"), tr(o.label));
        item.insert(QStringLiteral("needsValue"), o.needsValue);
        out << item;
    }
    return out;
}

bool QueryController::parseFilters(Tab &t, const QVariantList &items,
                                   std::vector<ColumnFilter> &out) const
{
    out.reserve(static_cast<size_t>(items.size()));
    for (const QVariant &item : items) {
        const QVariantMap f = item.toMap();
        const auto op = filterOp(f.value(QStringLiteral("op")).toString());
        const QString column = f.value(QStringLiteral("column")).toString();
        if (!op || column.isEmpty()) {
            t.state->edits()->setLastError(tr("The filter is not valid."));
            return false;
        }
        out.push_back(ColumnFilter { column, *op, f.value(QStringLiteral("value")).toString() });
    }
    return true;
}

namespace {

bool sameFilters(const std::vector<ColumnFilter> &a, const std::vector<ColumnFilter> &b)
{
    return std::ranges::equal(a, b, [](const ColumnFilter &x, const ColumnFilter &y) {
        return x.column == y.column && x.op == y.op && x.value == y.value;
    });
}

} // namespace

bool QueryController::setDataView(const QString &tabId, const QVariantMap &view)
{
    Tab *t = find(tabId);
    if (t == nullptr || !t->table)
        return false;
    if (t->applying) {
        announce(tabId, tr("Saving changes, please wait."), false);
        return false;
    }
    if (t->state->edits()->pendingCount() > 0) {
        const QString said = tr("Save or discard the pending changes before sorting or filtering.");
        announce(tabId, said, true);
        return false;
    }
    std::vector<ColumnFilter> filters;
    if (!parseFilters(*t, view.value(QStringLiteral("filters")).toList(), filters))
        return false;
    std::optional<SortKey> sort;
    const QString sortColumn = view.value(QStringLiteral("sortColumn")).toString();
    if (!sortColumn.isEmpty())
        sort = SortKey { sortColumn, view.value(QStringLiteral("descending")).toBool() };
    // The condition is checked before anything changes: a refused one leaves the view as it was.
    auto where = TableQueryBuilder::validateWhere(view.value(QStringLiteral("where")).toString());
    if (!where) {
        announce(tabId, where.error().message, true);
        emit viewFailed(tabId, where.error().message);
        return false;
    }
    // The text "Filtered from ..." describes the filters: other filters, other text.
    if (!sameFilters(filters, t->filters))
        t->state->setFilterOrigin({});
    t->filters = std::move(filters);
    t->sort = std::move(sort);
    t->whereExpression = std::move(*where);
    t->awaitingApply = false;
    onDataRequested(tabId); // a new plan replaces (and cancels) the running one
    return true;
}

bool QueryController::cycleSort(const QString &tabId, const QString &column)
{
    Tab *t = find(tabId);
    if (t == nullptr || !t->table || column.isEmpty())
        return false;
    QVariantMap view = dataViewOf(tabId);
    QString said;
    if (!t->sort || t->sort->column != column) {
        view.insert(QStringLiteral("sortColumn"), column);
        view.insert(QStringLiteral("descending"), false);
        said = tr("Sorted by %1, ascending").arg(column);
    } else if (!t->sort->descending) {
        view.insert(QStringLiteral("descending"), true);
        said = tr("Sorted by %1, descending").arg(column);
    } else {
        view.insert(QStringLiteral("sortColumn"), QString());
        view.insert(QStringLiteral("descending"), false);
        said = tr("Sort removed");
    }
    if (!setDataView(tabId, view))
        return false;
    announce(tabId, said, false);
    return true;
}

bool QueryController::setRelatedView(const QString &tabId, const QVariantMap &view,
                                     const QString &origin)
{
    if (!setDataView(tabId, view))
        return false;
    if (Tab *t = find(tabId); t != nullptr)
        t->state->setFilterOrigin(origin);
    return true;
}

bool QueryController::presetDataView(const QString &connectionId, const QStringList &parts,
                                     const QVariantMap &view, const QString &origin)
{
    InitialView initial;
    initial.connectionId = connectionId;
    initial.parts = parts;
    initial.origin = origin;
    for (const QVariant &item : view.value(QStringLiteral("filters")).toList()) {
        const QVariantMap f = item.toMap();
        const auto op = filterOp(f.value(QStringLiteral("op")).toString());
        const QString column = f.value(QStringLiteral("column")).toString();
        if (!op || column.isEmpty())
            return false;
        initial.filters.push_back(
            ColumnFilter { column, *op, f.value(QStringLiteral("value")).toString() });
    }
    m_initialView = std::move(initial);
    return true;
}

std::optional<TableQuery> QueryController::viewQueryOf(const QString &tabId) const
{
    const auto it = m_tabStates.find(tabId);
    if (it == m_tabStates.end())
        return std::nullopt;
    const Tab &t = *it->second;
    if (!t.table)
        return std::nullopt;
    TableQuery query;
    query.table = *t.table;
    query.filters = t.filters;
    query.whereExpression = t.whereExpression;
    query.sort = t.sort;
    return query;
}

bool QueryController::hasDataView(const QString &tabId) const
{
    const auto it = m_tabStates.find(tabId);
    if (it == m_tabStates.end() || !it->second->table)
        return false;
    const Tab &t = *it->second;
    return !t.filters.empty() || !t.whereExpression.isEmpty() || t.sort.has_value();
}

QVariantMap QueryController::dataViewOf(const QString &tabId) const
{
    const auto it = m_tabStates.find(tabId);
    if (it == m_tabStates.end() || !it->second->table)
        return {};
    const Tab &t = *it->second;
    QVariantList filters;
    for (const ColumnFilter &f : t.filters) {
        QVariantMap item;
        item.insert(QStringLiteral("column"), f.column);
        item.insert(QStringLiteral("op"), filterOpName(f.op));
        item.insert(QStringLiteral("value"), f.value);
        filters << item;
    }
    QVariantMap out;
    out.insert(QStringLiteral("sortColumn"), t.sort ? t.sort->column : QString());
    out.insert(QStringLiteral("descending"), t.sort && t.sort->descending);
    out.insert(QStringLiteral("filters"), filters);
    out.insert(QStringLiteral("where"), t.whereExpression);
    return out;
}

// ---- row count of a data tab -------------------------------------------------------------------

void QueryController::dropCount(Tab &t)
{
    for (const QMetaObject::Connection &c : std::as_const(t.countLinks))
        disconnect(c);
    t.countLinks.clear();
    t.countSession = nullptr;
    t.countStarted = false;
    t.countValue.reset();
}

void QueryController::finishCount(Tab &t, const Error *error)
{
    const std::optional<qint64> total = t.countValue;
    dropCount(t);
    if (error != nullptr && error->kind != ErrorKind::Cancelled) {
        t.state->setCount(false, -1, error->message);
        t.state->messages()->append({ QStringLiteral("error"), kindText(error->kind),
                                      error->message, error->detail, 0, 0 });
        announce(t.id, error->message, true);
    } else if (error == nullptr && total) {
        t.state->setCount(false, *total, {});
    } else {
        t.state->setCount(false, -1, {});
        announce(t.id, tr("Counting cancelled"), false);
    }
}

void QueryController::cancelCount(const QString &tabId)
{
    Tab *t = find(tabId);
    if (t == nullptr || !t->state->counting())
        return;
    Session *session = t->countSession;
    t->countValue.reset();
    // The session answers with cancelled(); the count is closed here as well so the button is
    // back at once and a late answer cannot be taken for a total.
    finishCount(*t, nullptr);
    if (session != nullptr)
        session->cancel();
}

void QueryController::countRows(const QString &tabId)
{
    Tab *t = find(tabId);
    if (t == nullptr || !t->table || t->state->counting())
        return;
    ConnectionController *conns = connections();
    const ISqlDialect *dialect = conns != nullptr ? dialectOf(*t) : nullptr;
    Session *session = nullptr;
    if (conns != nullptr) {
        SchemaTreeModel *tree = conns->tree();
        if (tree->ensureSession(t->connectionId) == SchemaTreeModel::Access::Connected)
            session = tree->metadataSession(t->connectionId);
    }
    if (dialect == nullptr || session == nullptr) {
        const Error notReady { ErrorKind::Connection,
                               tr("The rows cannot be counted now: the connection is not ready "
                                  "in the tree. Try again in a moment."),
                               {},
                               {},
                               {} };
        finishCount(*t, &notReady);
        return;
    }
    TableQuery query;
    query.table = *t->table;
    query.filters = t->filters;
    query.whereExpression = t->whereExpression;
    auto statement = TableQueryBuilder::count(*dialect, query);
    if (!statement) {
        finishCount(*t, &statement.error());
        return;
    }
    statement->fetchSize = 10;
    t->state->setCount(true, -1, {});
    announce(tabId, tr("Counting rows…"), false);
    t->countSession = session;
    t->countStarted = false;
    t->countValue.reset();
    // The session is shared: only what it says after it started this statement is ours.
    t->countLinks.push_back(connect(session, &Session::started, this, [this, id = tabId] {
        if (Tab *tab = find(id); tab != nullptr && tab->countSession != nullptr)
            tab->countStarted = true;
    }));
    t->countLinks.push_back(connect(
        session, &Session::batchReady, this,
        [this, id = tabId](const std::shared_ptr<const ResultBatch> &b) {
            Tab *tab = find(id);
            if (tab == nullptr || !tab->countStarted || !b || b->columns.empty() || b->rowCount < 1)
                return;
            const ColumnBuffer &cell = b->columns[0];
            switch (cell.typeAt(0)) {
            case ValueType::Int64:
            case ValueType::Bool:
                tab->countValue = cell.int64At(0);
                break;
            case ValueType::Double:
                tab->countValue = static_cast<qint64>(cell.doubleAt(0));
                break;
            case ValueType::Null:
                break;
            default: { // some drivers answer a decimal as text
                bool ok = false;
                const qint64 v = QString::fromUtf8(cell.bytesAt(0)).toLongLong(&ok);
                if (ok)
                    tab->countValue = v;
                break;
            }
            }
        }));
    t->countLinks.push_back(connect(session, &Session::finished, this, [this, id = tabId] {
        Tab *tab = find(id);
        if (tab == nullptr || !tab->countStarted)
            return;
        if (tab->countValue) {
            finishCount(*tab, nullptr);
            return;
        }
        const Error none {
            ErrorKind::Internal, tr("The server did not answer the count."), {}, {}, {}
        };
        finishCount(*tab, &none);
    }));
    t->countLinks.push_back(
        connect(session, &Session::failed, this, [this, id = tabId](const Error &e) {
            if (Tab *tab = find(id); tab != nullptr && tab->countStarted)
                finishCount(*tab, &e);
        }));
    t->countLinks.push_back(connect(session, &Session::cancelled, this, [this, id = tabId] {
        if (Tab *tab = find(id); tab != nullptr && tab->countStarted)
            finishCount(*tab, nullptr);
    }));
    t->countLinks.push_back(connect(session, &QObject::destroyed, this, [this, id = tabId] {
        Tab *tab = find(id);
        if (tab == nullptr || tab->countSession == nullptr)
            return;
        tab->countSession = nullptr; // finishCount() must not touch it
        const Error gone { ErrorKind::Connection,
                           tr("The connection was closed before the rows were counted."),
                           {},
                           {},
                           {} };
        finishCount(*tab, &gone);
    }));
    session->execute(*statement);
}

void QueryController::onChildrenFailed(const QString &tabId, quint64 requestId, const Error &error)
{
    Tab *t = find(tabId);
    if (t == nullptr || !t->plan || t->keyRequest == 0 || requestId != t->keyRequest)
        return;
    t->keyRequest = 0;
    if (error.kind == ErrorKind::Cancelled)
        finishCancelled(*t);
    else
        failPlan(*t, error);
}

int QueryController::pendingChanges(const QString &tabId)
{
    Tab *t = find(tabId);
    return t != nullptr && t->table ? t->state->edits()->pendingCount() : 0;
}

int QueryController::pendingTotal() const
{
    int total = 0;
    for (const auto &entry : m_tabStates)
        if (entry.second->table)
            total += entry.second->state->edits()->pendingCount();
    return total;
}

bool QueryController::build(Tab &t, std::vector<PlannedStatement> &statements,
                            std::vector<int> &rows)
{
    TableEditModel *edits = t.state->edits();
    statements.clear();
    rows.clear();
    const ISqlDialect *dialect = dialectOf(t);
    if (dialect == nullptr) {
        edits->setLastError(tr("The database engine could not be loaded."));
        return false;
    }
    auto built = ChangeSqlBuilder::build(*dialect, edits->target(), edits->buildEdits(&rows));
    if (!built) {
        edits->setLastError(built.error().message);
        return false;
    }
    statements = std::move(built).value();
    return true;
}

QVariantList QueryController::describe(const std::vector<PlannedStatement> &statements,
                                       const std::vector<int> &rows, const TableEditModel &edits)
{
    QVariantList out;
    int number = 0;
    for (const PlannedStatement &p : statements) {
        QVariantList parameters;
        int k = 0;
        for (const QVariant &v : p.statement.parameters) {
            const bool isNull = v.isNull();
            QString quoted = v.toString();
            quoted.replace(QLatin1Char('\''), QStringLiteral("''"));
            const QString text
                = isNull ? QStringLiteral("NULL") : QLatin1Char('\'') + quoted + QLatin1Char('\'');
            parameters.append(QVariantMap { { QStringLiteral("number"), ++k },
                                            { QStringLiteral("value"), v },
                                            { QStringLiteral("isNull"), isNull },
                                            { QStringLiteral("text"), text } });
        }
        const auto edit = static_cast<std::size_t>(p.editIndex);
        out.append(QVariantMap {
            { QStringLiteral("number"), ++number },
            { QStringLiteral("row"), edit < rows.size() ? edits.rowLabel(rows[edit]) : QString() },
            { QStringLiteral("sql"), p.statement.sql },
            { QStringLiteral("parameters"), parameters } });
    }
    return out;
}

QVariantList QueryController::previewStatements(const QString &tabId)
{
    Tab *t = find(tabId);
    if (t == nullptr || !t->table || !t->state->edits()->editable()
        || t->state->edits()->pendingCount() == 0)
        return {};
    std::vector<PlannedStatement> statements;
    std::vector<int> rows;
    if (!build(*t, statements, rows))
        return {};
    return describe(statements, rows, *t->state->edits());
}

bool QueryController::apply(const QString &tabId)
{
    Tab *t = find(tabId);
    if (t == nullptr || !t->table || t->awaitingApply || t->state->edits()->reloading())
        return false;
    if (t->applying) {
        announce(tabId, tr("Saving changes, please wait."), false);
        return false;
    }
    TableEditModel *edits = t->state->edits();
    if (!edits->editable() || edits->pendingCount() == 0) {
        announce(tabId, tr("There are no changes to save."), false);
        return false;
    }
    if (t->session == nullptr || t->plan) {
        edits->setLastError(tr("The table is still loading. Save when it has loaded."));
        announce(tabId, edits->lastError(), true);
        return false;
    }
    std::vector<PlannedStatement> statements;
    std::vector<int> rows;
    if (!build(*t, statements, rows)) {
        announce(tabId, edits->lastError(), true);
        return false;
    }
    ConnectionController *conns = connections();
    const auto profile = conns != nullptr
        ? conns->model()->profile(QUuid::fromString(t->connectionId))
        : std::nullopt;
    if (profile && profile->production) {
        t->awaitingApply = true;
        emit applyConfirmationRequested(
            { { QStringLiteral("tabId"), tabId },
              { QStringLiteral("connectionName"), profile->name },
              { QStringLiteral("count"), static_cast<int>(statements.size()) } });
        return true;
    }
    send(*t);
    return true;
}

bool QueryController::confirmApply(const QString &tabId, const QString &typedName)
{
    Tab *t = find(tabId);
    if (t == nullptr || !t->awaitingApply)
        return false;
    ConnectionController *conns = connections();
    const auto profile = conns != nullptr
        ? conns->model()->profile(QUuid::fromString(t->connectionId))
        : std::nullopt;
    if (!profile || typedName != profile->name)
        return false;
    send(*t);
    return true;
}

void QueryController::declineApply(const QString &tabId)
{
    Tab *t = find(tabId);
    if (t == nullptr || !t->awaitingApply)
        return;
    t->awaitingApply = false;
    announce(tabId, tr("Cancelled. Nothing was saved."), false);
}

// What reaches the session is built here, again, from the pending changes: the preview and the
// statements come from the same builder over the same deltas.
void QueryController::send(Tab &t)
{
    t.awaitingApply = false;
    TableEditModel *edits = t.state->edits();
    std::vector<PlannedStatement> statements;
    std::vector<int> rows;
    if (t.session == nullptr || !build(t, statements, rows)) {
        announce(t.id, edits->lastError(), true);
        return;
    }
    t.sentRows.clear();
    for (const PlannedStatement &p : statements) {
        const auto edit = static_cast<std::size_t>(p.editIndex);
        t.sentRows.push_back(edit < rows.size() ? rows[edit] : -1);
    }
    t.applying = true;
    edits->setSaving(true); // no edit, undo, discard or reload until the answer
    edits->setLastError({});
    const auto count = static_cast<int>(statements.size());
    announce(t.id, count == 1 ? tr("Saving 1 change") : tr("Saving %1 changes").arg(count), false);
    emit changesSent(t.id, describe(statements, rows, *edits));
    t.session->applyChanges(std::move(statements));
}

void QueryController::onChangesApplied(const QString &tabId, int count)
{
    Tab *t = find(tabId);
    if (t == nullptr || !t->applying)
        return;
    t->applying = false;
    t->state->edits()->setSaving(false);
    const QString text = count == 1 ? tr("Saved 1 change") : tr("Saved %1 changes").arg(count);
    // The table is read again. The saved values stay on show until the new rows replace them (the
    // reset drops the pending changes); a read that fails or is cancelled drops them in
    // endReload().
    t->awaitingApply = false;
    onDataRequested(tabId);
    t->afterSave = t->plan.has_value();
    t->state->messages()->append({ QStringLiteral("statement"), tr("Saved"), text, {}, 0, 0 });
    announce(tabId, text, false);
    emit changesSaved(tabId, count);
}

void QueryController::onChangesFailed(const QString &tabId, int index, const Error &error)
{
    Tab *t = find(tabId);
    if (t == nullptr || !t->applying)
        return;
    t->applying = false;
    TableEditModel *edits = t->state->edits();
    edits->setSaving(false);
    QString text = error.message;
    int row = -1;
    if (index >= 0 && static_cast<std::size_t>(index) < t->sentRows.size()) {
        row = t->sentRows[static_cast<std::size_t>(index)];
        text = tr("%1: %2").arg(edits->rowLabel(row), error.message);
    }
    edits->setLastError(text);
    t->state->messages()->append(
        { QStringLiteral("error"), kindText(error.kind), text, error.detail, 0, 0 });
    announce(tabId, text, true);
    if (row >= 0)
        emit edits->focusRowRequested(row);
    emit planFailed(tabId);
}

void QueryController::announce(const QString &tabId, const QString &text, bool assertive)
{
    const bool isCurrent = m_tabs && m_tabs->idOf(m_tabs->currentIndex()) == tabId;
    emit announcement(isCurrent ? text : tr("%1: %2").arg(title(tabId), text), assertive);
}

} // namespace quarry::ui
