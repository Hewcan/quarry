#pragma once

#include "query_state.h"

#include <quarry/result.h>
#include <quarry/session.h>

#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
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

// Runs what EditorTabsModel::runRequested asks for (ARCHITECTURE sections 5 and 6.1). Each editor
// tab has its own quarry::Session, created on its first run (never before) and closed with the
// tab, when its connection changes or when the connection is deleted; the tree keeps its own.
// The ranges of a plan run in order, each starting on the previous `finished`; the first error
// or a cancellation stops the plan. SqlSafety looks at the whole plan first: one confirmation
// lists every risky statement, and nothing runs until it is answered.
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
    // The state of the current tab (an idle one when there is no tab).
    Q_PROPERTY(quarry::ui::QueryState *current READ current NOTIFY currentChanged FINAL)
    Q_PROPERTY(int sessionCount READ sessionCount NOTIFY sessionsChanged FINAL)

public:
    static constexpr int kFetchSize = ResultTableModel::kPageRows;

    explicit QueryController(QObject *parent = nullptr);
    // No destructor: the sessions are children, and QObject joins them.

    EditorTabsModel *tabs() const { return m_tabs; }
    void setTabs(EditorTabsModel *tabs);
    QueryState *current() const { return m_current; }
    int sessionCount() const;

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
    // ---- data tabs (H2, ARCHITECTURE section 6.4) ----
    // Unsaved changes of one data tab, and of all of them (the app asks before quitting).
    Q_INVOKABLE int pendingChanges(const QString &tabId);
    Q_INVOKABLE int pendingTotal() const;
    // Reads the table again, dropping the pending changes: the UI asks first (pendingChanges).
    Q_INVOKABLE void reload(const QString &tabId);
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
    void currentChanged();
    void sessionsChanged();
    // For the screen reader, without moving the focus.
    void announcement(const QString &text, bool assertive);
    // {tabId, connectionName, production, items: [{label, risk, sql}]}. Nothing runs until
    // confirm() (or decline()) is called.
    void confirmationRequested(const QVariantMap &request);
    // "Ask every time" and no password remembered for this run. `retry` = the last one failed.
    void passwordRequested(const QString &connectionId, const QString &name, bool retry);
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

private:
    struct Range
    {
        qsizetype begin = 0;
        QString sql;
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
    };
    enum class Stage { None, ReadingPassword, AwaitingPassword };
    struct Tab
    {
        QString id;
        QueryState *state = nullptr;
        QString connectionId; // the connection of the session and of the last run
        Session *session = nullptr;
        bool opening = false; // open() asked, neither opened nor failed yet
        Stage stage = Stage::None;
        std::optional<Plan> plan; // running
        std::optional<Plan> pending; // waiting for the confirmation
        int next = 0; // range being run
        quint64 planId = 0;
        // One entry per execute() sent, in order, holding the plan it belongs to. Every
        // execute ends with exactly one terminal signal, so the first entry says whose it is;
        // those of a replaced plan are skipped. A fetchMore() sent for the grid is an entry
        // with the id kFetchOp: it ends with one terminal signal as well.
        QList<quint64> ops;
        bool cancelRequested = false;
        QStringList columns;
        qint64 elapsedBase = 0; // time of the statements before the current one
        qint64 elapsedTotal = 0;
        bool acceptRows = false; // batches go to the grid: set by columnsReady of the plan
        // Data tab (a table opened from the tree).
        std::optional<ObjectPath> table;
        quint64 keyRequest = 0; // Session::children() of the columns, until answered
        bool applying = false; // applyChanges() sent, no answer yet
        bool awaitingApply = false; // production: waiting for the typed name
        std::vector<int> sentRows; // statement index -> row of the edit model
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

    void onRunRequested(const QVariantMap &request);
    void onDataRequested(const QString &tabId);
    void requestKey(Tab &t);
    void onChildren(const QString &tabId, quint64 requestId, const std::vector<SchemaNode> &nodes);
    void onChildrenFailed(const QString &tabId, quint64 requestId, const Error &error);
    const ISqlDialect *dialectOf(const Tab &t) const;
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
    void runNext(Tab &t);
    bool popOp(Tab &t);
    bool currentOp(const Tab &t) const;
    void onOpened(const QString &tabId);
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
    void announce(const QString &tabId, const QString &text, bool assertive);

    // Not a QPointer: clang-analyzer (Qt 6.12) reports a false use-after-free in ~QWeakPointer.
    // Cleared on destroyed() instead.
    EditorTabsModel *m_tabs = nullptr;
    // Only compared, never dereferenced; the links are dropped by handle.
    ConnectionController *m_connections = nullptr;
    QList<QMetaObject::Connection> m_connectionLinks;
    std::map<QString, std::unique_ptr<Tab>> m_tabStates;
    QueryState *m_none = nullptr; // for "no tab"
    QueryState *m_current = nullptr;
    QSet<QString> m_authFailed; // connections whose last password was refused
    quint64 m_nextPlan = 0;
};

} // namespace quarry::ui
