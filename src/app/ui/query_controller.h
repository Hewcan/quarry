#pragma once

#include "query_state.h"

#include <quarry/connection_profile.h>
#include <quarry/history_entry.h>
#include <quarry/result.h>
#include <quarry/session.h>
#include <quarry/table_query.h>

#include <QDateTime>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace quarry::ui {

class ConnectionController;
class EditorTabsModel;
class HistoryModel;

// Runs what EditorTabsModel::runRequested asks for (ARCHITECTURE sections 5 and 6.1). Each editor
// tab has its own quarry::Session, created on its first run (never before) and closed with the
// tab, when its connection changes or when the connection is deleted; the tree keeps its own.
// The ranges of a plan run in order, each starting on the previous `finished`; the first error
// or a cancellation stops the plan. SqlSafety looks at the whole plan first: one confirmation
// lists every risky statement, and nothing runs until it is answered.
//
// Every statement of a query tab that ends in success or in an error is written to the history
// (HistoryModel, when set): one entry per statement, also for "Run all". A cancelled statement
// and the SELECT behind a data tab are not. The store drops the entries of a connection that
// does not record history.
//
// What the UI shows lives in one QueryState per tab (stateOf(), current).
//
// Declare it before the ConnectionController in a parent: the sessions' code lives in the
// registry's plugins, and children are destroyed in creation order.
class QueryController : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(quarry::ui::EditorTabsModel *tabs READ tabs WRITE setTabs NOTIFY tabsChanged FINAL)
    // Where the statements are recorded (optional).
    Q_PROPERTY(
        quarry::ui::HistoryModel *history READ history WRITE setHistory NOTIFY historyChanged FINAL)
    // The state of the current tab (an idle one when there is no tab).
    Q_PROPERTY(quarry::ui::QueryState *current READ current NOTIFY currentChanged FINAL)
    Q_PROPERTY(int sessionCount READ sessionCount NOTIFY sessionsChanged FINAL)
    // How many rows a data tab reads when its table is opened or refreshed (and per scroll step):
    // a user setting (the UI stores it), kMinDataPageRows..kMaxDataPageRows. The next load of
    // each tab uses it; a tab that is open keeps what it has until it is read again.
    Q_PROPERTY(
        int dataPageRows READ dataPageRows WRITE setDataPageRows NOTIFY dataPageRowsChanged FINAL)

public:
    static constexpr int kFetchSize = ResultTableModel::kPageRows;
    static constexpr int kMinDataPageRows = 1;
    static constexpr int kMaxDataPageRows = 10000;

    explicit QueryController(QObject *parent = nullptr);
    // No destructor: the sessions are children, and QObject joins them.

    EditorTabsModel *tabs() const { return m_tabs; }
    void setTabs(EditorTabsModel *tabs);
    HistoryModel *history() const { return m_history; }
    void setHistory(HistoryModel *history);
    QueryState *current() const { return m_current; }
    int sessionCount() const;
    int dataPageRows() const { return m_dataPageRows; }
    // Out-of-range values are brought to the nearest limit.
    void setDataPageRows(int rows);
    // `rows` as dataPageRows would keep it.
    Q_INVOKABLE static int clampDataPageRows(int rows);

    // The state of a tab, created the first time it is asked for. Null for an unknown tab.
    Q_INVOKABLE quarry::ui::QueryState *stateOf(const QString &tabId);
    Q_INVOKABLE bool hasSession(const QString &tabId) const;
    // Stops the plan of the tab: a running statement is interrupted, the rest is dropped. The
    // plan ends with "Query cancelled" (not an error).
    Q_INVOKABLE void cancel(const QString &tabId);
    Q_INVOKABLE void cancelCurrent();
    // Answers to confirmationRequested().
    Q_INVOKABLE void confirm(const QString &tabId);
    Q_INVOKABLE void decline(const QString &tabId);
    // The statement behind the rows of the tab's grid, as it was sent ("" before the first result
    // and after the connection of the tab changed). Export runs it again.
    Q_INVOKABLE QString resultSqlOf(const QString &tabId) const;
    // ---- query plan (H9, ARCHITECTURE section 6.9) ----
    // The tab's engine can show a plan (Capability::Explain); loads the engine the first time.
    Q_INVOKABLE bool supportsExplain(const QString &connectionId);
    // Explain (analyze = false) or Explain analyze of the statement at `position`, by the tab's
    // session. {ok, message, ranges: [{startLine, endLine, ...}]}. Analyze executes the statement,
    // so it only takes SELECT queries and asks first (analyzeConfirmationRequested, answered with
    // confirm() / decline()). A request the engine or the statement cannot satisfy starts nothing:
    // ok is false, `message` says why in plain words, and the tab's plan shows it as its notice.
    Q_INVOKABLE QVariantMap explain(const QString &tabId, bool analyze, int position);
    // ---- data tabs (H2, ARCHITECTURE section 6.4) ----
    // Unsaved changes of one data tab, and of all of them (the app asks before quitting).
    Q_INVOKABLE int pendingChanges(const QString &tabId);
    Q_INVOKABLE int pendingTotal() const;
    // Reads the table again, dropping the pending changes: the UI asks first (pendingChanges).
    Q_INVOKABLE void reload(const QString &tabId);
    // ---- sort and filter of a data tab (H11, ARCHITECTURE section 6.10) ----
    // Reads the table again on the server with this order and these filters: {sortColumn
    // ("" = none), descending, where (SQL condition the user typed, "" = none), filters:
    // [{column, op, value}]}, op being one of filterOperators(). Refused (false, announced) while
    // there are pending changes or a save is in flight; a bad view also gives false and sets the
    // tab's lastError; a `where` that TableQueryBuilder::validateWhere refuses gives false, is
    // announced and emitted by viewFailed, and the view stays as it was. The run can be cancelled
    // like any other (cancel()).
    Q_INVOKABLE bool setDataView(const QString &tabId, const QVariantMap &view);
    // The view of the tab, in the same shape ({} for a tab that is not a data tab).
    Q_INVOKABLE QVariantMap dataViewOf(const QString &tabId) const;
    // The header of the grid (H11 phase 2): sorts by `column`, going ascending -> descending -> no
    // order when it is the column already sorted, ascending when it is another. Keeps the filters
    // and the condition; the answer and the refusals are those of setDataView(), and the new order
    // is announced.
    Q_INVOKABLE bool cycleSort(const QString &tabId, const QString &column);
    // setDataView() for the navigation by foreign keys (H11 phase 4): `origin` is the text the data
    // tab shows under its bar ("Filtered from orders.customer_id = 5", QueryState::filterOrigin).
    // Any later setDataView() that changes the filters clears it.
    Q_INVOKABLE bool setRelatedView(const QString &tabId, const QVariantMap &view,
                                    const QString &origin);
    // Asks that the data tab that opens next for this table of this connection starts with `view`
    // (same shape as setDataView) and `origin`, instead of reading the whole table first. Used
    // once; a tab that is already open is not touched (use setRelatedView). false for a bad view.
    bool presetDataView(const QString &connectionId, const QStringList &parts,
                        const QVariantMap &view, const QString &origin);
    // What the export of a data tab reads (H11 phase 2): the table with the view's filters,
    // condition and order, no hidden key and no page limit. Empty for a tab that is not a data tab.
    std::optional<TableQuery> viewQueryOf(const QString &tabId) const;
    // The tab has a filter, a condition or an order.
    Q_INVOKABLE bool hasDataView(const QString &tabId) const;
    // Counts the rows of the tab's view (same filters and condition, no order) with
    // TableQueryBuilder::count, on the tree's metadata session so the tab's own stream (the rows
    // the grid shows) is not touched. The answer is QueryState::countedTotal; a failure is
    // QueryState::countError and a Messages entry. Does nothing for a tab that is not a data tab
    // or while it is already counting.
    // shortcut: the metadata session is shared with the tree and the migrations; cancelCount()
    // cancels whatever it is running at that moment. Give the count its own session if that bites.
    Q_INVOKABLE void countRows(const QString &tabId);
    Q_INVOKABLE void cancelCount(const QString &tabId);
    // [{op, label, needsValue}] in the order the filter bar shows them.
    Q_INVOKABLE static QVariantList filterOperators();
    // The statements Apply would send, built now with ChangeSqlBuilder: [{number, row, sql,
    // parameters: [{number, value, isNull, text}]}]. `text` is how the parameter is shown
    // ('Alice' or NULL); `value` is exactly what the provider receives. Empty when there is
    // nothing to build or building failed (the tab's lastError says why).
    Q_INVOKABLE QVariantList previewStatements(const QString &tabId);
    // Builds and sends the changes in one transaction. On a production connection nothing is sent
    // until confirmApply() gets the connection name typed exactly; applyConfirmationRequested()
    // asks for it. false when nothing was sent or asked.
    Q_INVOKABLE bool apply(const QString &tabId);
    Q_INVOKABLE bool confirmApply(const QString &tabId, const QString &typedName);
    Q_INVOKABLE void declineApply(const QString &tabId);
    // Answers to passwordRequested().
    Q_INVOKABLE void providePassword(const QString &connectionId, const QString &password);
    Q_INVOKABLE void cancelPassword(const QString &connectionId);

signals:
    void tabsChanged();
    void historyChanged();
    void currentChanged();
    void sessionsChanged();
    void dataPageRowsChanged();
    // For the screen reader, without moving the focus.
    void announcement(const QString &text, bool assertive);
    // {tabId, connectionName, production, items: [{label, risk, sql}]}. Nothing runs until
    // confirm() (or decline()) is called.
    void confirmationRequested(const QVariantMap &request);
    // "Ask every time" and no password remembered for this run. `retry` = the last one failed.
    void passwordRequested(const QString &connectionId, const QString &name, bool retry);
    // The Plan tab of this tab's results should be shown (a plan was asked for, or was refused).
    void planRequested(const QString &tabId);
    // A normal run started: the results come back to the front.
    void runStarted(const QString &tabId);
    // {tabId, connectionName, production, sql}. Explain analyze executes `sql`: nothing runs
    // until confirm() (or decline()) is called.
    void analyzeConfirmationRequested(const QVariantMap &request);
    // The plan of this tab ended in an error (the UI shows the Messages tab).
    void planFailed(const QString &tabId);
    // A statement that changes the schema (CREATE, ALTER, DROP, RENAME) succeeded on this
    // connection: what autocompletion cached about it is stale.
    void schemaChanged(const QString &connectionId);
    // Apply on a production connection: {tabId, connectionName, count}.
    void applyConfirmationRequested(const QVariantMap &request);
    // The statements handed to Session::applyChanges, in the form of previewStatements().
    void changesSent(const QString &tabId, const QVariantList &statements);
    // The changes of the tab were saved; the table is being read again.
    void changesSaved(const QString &tabId, int count);
    // The condition of a data tab's filter was refused (setDataView) or the server rejected it:
    // the message is for the filter box.
    void viewFailed(const QString &tabId, const QString &message);

private:
    struct Range
    {
        qsizetype begin = 0;
        QString sql;
        QVariantList parameters; // data tabs: the filter values
        int startLine = 1;
        int number = 1; // "Statement <number> of <total>"
    };
    struct Plan
    {
        QString connectionId;
        QString connectionName;
        bool production = false;
        int total = 0;
        QString text; // the tab's text when it was requested: error positions refer to it
        QList<Range> ranges;
        // Data tab: the select is written once the key of the table is known.
        bool data = false;
        // Explain / Explain analyze: `explainSql` is the user's statement; ranges[0].sql is the
        // engine's EXPLAIN, written once the server (ServerInfo::product) is known.
        std::optional<ExplainMode> explain;
        QString explainSql;
    };
    enum class Stage { None, ReadingPassword, AwaitingPassword, Preflight };
    // Fields from the widest to the narrowest, so the struct has little padding.
    struct Tab
    {
        QueryState *state = nullptr;
        Session *session = nullptr;
        qsizetype explainRows = 0; // rows of explainText
        quint64 planId = 0;
        QDateTime statementStart; // UTC, when the current statement was sent
        qint64 elapsedBase = 0; // time of the statements before the current one
        qint64 elapsedTotal = 0;
        // Data tab (a table opened from the tree).
        quint64 keyRequest = 0; // Session::children() of the columns, until answered
        // "Count rows": the metadata session the COUNT(*) runs on, until it ends.
        Session *countSession = nullptr; // cleared when it is destroyed (see countRows)
        QElapsedTimer statementClock;
        std::optional<qint64> countValue;
        QString id;
        QString connectionId; // the connection of the session and of the last run
        QString product; // ServerInfo::product of the session, once it opened
        QString explainText; // the rows of the running EXPLAIN: cells by tab, rows by newline
        // One entry per execute() sent, in order, holding the plan it belongs to. Every
        // execute ends with exactly one terminal signal, so the first entry says whose it is;
        // those of a replaced plan are skipped. A fetchMore() sent for the grid is an entry
        // with the id kFetchOp: it ends with one terminal signal as well.
        QList<quint64> ops;
        QStringList columns;
        QString resultSql; // the statement whose columns the grid shows
        std::vector<ColumnFilter> filters; // H11: the view the select is built with
        QString whereExpression; // the condition the user typed, already validated
        QList<QMetaObject::Connection> countLinks;
        std::vector<int> sentRows; // statement index -> row of the edit model
        std::optional<ObjectPath> table;
        std::optional<SortKey> sort;
        std::optional<Plan> plan; // running
        std::optional<Plan> pending; // waiting for the confirmation
        Stage stage = Stage::None;
        int preflight = 0; // ConnectionPreflight request while stage is Preflight
        int next = 0; // range being run
        bool explainCut = false; // explainText hit kMaxExplainChars
        bool opening = false; // open() asked, neither opened nor failed yet
        bool cancelRequested = false;
        bool acceptRows = false; // batches go to the grid: set by columnsReady of the plan
        bool countStarted = false; // the session started the count (its own signals are not ours)
        bool applying = false; // applyChanges() sent, no answer yet
        bool awaitingApply = false; // production: waiting for the typed name
    };
    static constexpr quint64 kFetchOp = 0; // plan ids start at 1

    Tab &tabFor(const QString &tabId);
    Tab *find(const QString &tabId);
    int rowOf(const QString &tabId) const;
    QString title(const QString &tabId) const;
    QStringList tabIds() const;
    void bindConnections();
    ConnectionController *connections() const;

    void sync();
    void updateCurrent();
    void dropPlan(Tab &t);
    void closeSession(Tab &t);
    // Forgets the running count: no signal of its session is heard any more. The session itself is
    // not cancelled here.
    void dropCount(Tab &t);
    // The count ended: a total, or `error` (null with no total = cancelled).
    void finishCount(Tab &t, const Error *error);

    void onRunRequested(const QVariantMap &request);
    void onDataRequested(const QString &tabId);
    void requestKey(Tab &t);
    // Fills the current range of a data plan with the select of `hidden` and the tab's view.
    bool writeSelect(Tab &t, const ISqlDialect &dialect, const std::optional<QString> &hidden);
    void onChildren(const QString &tabId, quint64 requestId, const std::vector<SchemaNode> &nodes);
    void onChildrenFailed(const QString &tabId, quint64 requestId, const Error &error);
    const ISqlDialect *dialectOf(const Tab &t) const;
    // The filters of a view map; false (and the tab's lastError set) when one is not valid.
    bool parseFilters(Tab &t, const QVariantList &items, std::vector<ColumnFilter> &out) const;
    // Builds what Apply would send; false (and lastError set) when it cannot.
    bool build(Tab &t, std::vector<PlannedStatement> &statements, std::vector<int> &rows);
    static QVariantList describe(const std::vector<PlannedStatement> &statements,
                                 const std::vector<int> &rows, const TableEditModel &edits);
    void send(Tab &t);
    void onChangesApplied(const QString &tabId, int count);
    void onChangesFailed(const QString &tabId, int index, const Error &error);
    void start(Tab &t, Plan plan);
    void openSession(Tab &t);
    void proceed(Tab &t, const QString &password);
    void openWith(Tab &t, const quarry::ConnectionProfile &profile, const quarry::Secrets &secrets);
    void dropPreflight(Tab &t);
    Tab *findByPreflight(int request);
    void onPreflightReady(int request, const quarry::Secrets &secrets);
    void onPreflightFailed(int request, const quarry::Error &error);
    void onPreflightCancelled(int request);
    void runNext(Tab &t);
    bool prepareExplain(Tab &t);
    void appendExplain(Tab &t, const ResultBatch &batch);
    void finishExplain(Tab &t, const ExecutionStats &stats);
    void onExplainParsed(const QString &tabId, quint64 planId, Result<ExplainPlan> &&parsed,
                         const QString &text);
    void explainFailed(Tab &t, const Error &error, const QString &rawText);
    void refuseExplain(const QString &tabId, const QString &message);
    bool popOp(Tab &t);
    bool currentOp(const Tab &t) const;
    void onOpened(const QString &tabId, const quarry::ServerInfo &info);
    void onColumns(const QString &tabId, const std::vector<ColumnInfo> &columns);
    void onBatch(const QString &tabId, const std::shared_ptr<const ResultBatch> &batch);
    void onFetchRequested(const QString &tabId, qsizetype rows);
    bool popFetchOp(Tab &t);
    void onFetchFinished(Tab &t, const ExecutionStats &stats);
    void onFetchFailed(Tab &t, const Error &error);
    void onFinished(const QString &tabId, const ExecutionStats &stats);
    void onFailed(const QString &tabId, const Error &error);
    void onCancelled(const QString &tabId);
    void onPasswordAvailable(QUuid id, const QString &password);
    void onPasswordUnavailable(QUuid id, const Error &error);
    void finishDone(Tab &t);
    void finishCancelled(Tab &t);
    void failPlan(Tab &t, const Error &error);
    void recordStatement(const Tab &t, bool success, qint64 durationMs, qint64 rows,
                         const QString &errorMessage);
    void announce(const QString &tabId, const QString &text, bool assertive);

    // Not a QPointer: clang-analyzer (Qt 6.12) reports a false use-after-free in ~QWeakPointer.
    // Cleared on destroyed() instead.
    EditorTabsModel *m_tabs = nullptr;
    HistoryModel *m_history = nullptr; // cleared on destroyed(), like m_tabs
    // Only compared, never dereferenced; the links are dropped by handle.
    ConnectionController *m_connections = nullptr;
    QList<QMetaObject::Connection> m_connectionLinks;
    std::map<QString, std::unique_ptr<Tab>> m_tabStates;
    QueryState *m_none = nullptr; // for "no tab"
    QueryState *m_current = nullptr;
    QSet<QString> m_authFailed; // connections whose last password was refused
    quint64 m_nextPlan = 0;
    // presetDataView(): consumed by the next onDataRequested() of that table.
    struct InitialView
    {
        QString connectionId;
        QStringList parts;
        QString origin;
        std::vector<ColumnFilter> filters;
    };
    std::optional<InitialView> m_initialView;
    int m_dataPageRows = ResultTableModel::kDataPageRows;
    // Parses plans too big for the UI thread (kParseOnUiChars). One thread: plans come one at a
    // time per tab and parsing is CPU-bound. Last member: it joins before the rest goes.
    QThreadPool m_parsePool;
};

} // namespace quarry::ui
