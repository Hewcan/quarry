#pragma once

#include "query_controller.h"

#include <quarry/export_options.h>
#include <quarry/exporter.h>
#include <quarry/result.h>
#include <quarry/session.h>

#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QStringView>
#include <QUrl>
#include <QUuid>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <optional>

namespace quarry::ui {

class ConnectionController;
class EditorTabsModel;

// The "Export..." dialog's back end (ARCHITECTURE section 14, UX.md "Exportacion (H5)"). One
// export at a time: it owns the Exporter (formatting and file I/O run on its thread, never on the
// UI one) and, for "Run the query again", the extra Session the streaming needs.
//
//   idle -> preparing (reading the password, opening the session) -> running -> done | failed |
//   cancelled. "Current result" goes from idle to running at once.
//
// Scope "query" runs the statement again on a session of its own, so the grid and the tab are not
// touched. Only statements that return rows are accepted (returnsRows()), and SqlSafety looks at
// them like at any other run. The password is never asked for here: an "Ask every time"
// connection needs it in memory already (the tab ran a query).
//
// Declare it before the ConnectionController in a parent, like QueryController: the sessions' code
// lives in the registry's plugins, and children are destroyed in creation order. Destroying it
// cancels a running export.
class ExportController : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(quarry::ui::QueryController *queries READ queries WRITE setQueries NOTIFY
                   queriesChanged FINAL)
    // What can be exported from the current tab: see infoFor(). Re-read when the tab, its state or
    // its rows change.
    Q_PROPERTY(QVariantMap info READ info NOTIFY infoChanged FINAL)
    // "idle" | "preparing" | "running" | "done" | "failed" | "cancelled"
    Q_PROPERTY(QString state READ state NOTIFY stateChanged FINAL)
    // preparing or running: no other export starts.
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged FINAL)
    // cancel() was called and the end has not arrived yet.
    Q_PROPERTY(bool cancelling READ cancelling NOTIFY stateChanged FINAL)
    Q_PROPERTY(qint64 rows READ rows NOTIFY progressChanged FINAL)
    Q_PROPERTY(qint64 bytes READ bytes NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString filePath READ filePath NOTIFY stateChanged FINAL)
    // "N rows written", with the locale's thousands separator.
    Q_PROPERTY(QString progressText READ progressText NOTIFY progressChanged FINAL)
    // "Exported N rows (X MB) to <path>" once done.
    Q_PROPERTY(QString resultText READ resultText NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString errorDetail READ errorDetail NOTIFY stateChanged FINAL)

public:
    enum class State { Idle, Preparing, Running, Done, Failed, Cancelled };

    explicit ExportController(QObject *parent = nullptr);
    ~ExportController() override;

    QueryController *queries() const { return m_queries; }
    void setQueries(QueryController *queries);
    QVariantMap info();
    QString state() const;
    bool busy() const { return m_state == State::Preparing || m_state == State::Running; }
    bool cancelling() const { return busy() && m_cancelRequested; }
    qint64 rows() const { return m_rows; }
    qint64 bytes() const { return m_bytes; }
    QString filePath() const { return m_filePath; }
    QString progressText() const;
    QString resultText() const { return m_resultText; }
    QString errorMessage() const { return m_error.message; }
    QString errorDetail() const { return m_error.detail; }

    // True when `sql` is a statement that only reads: SELECT, WITH (whose main command is a
    // SELECT), TABLE, SHOW or EXPLAIN (not of a statement that changes data). SELECT ... INTO is
    // not. Comments, strings and quoted identifiers are skipped. shortcut: a function with side
    // effects called from a SELECT cannot be told apart; the rows-only rule is the one of H5.
    static bool returnsRows(QStringView sql);

    // What the dialog needs about a tab: {tabTitle, connectionName, currentAllowed (bool),
    // currentReason, loadedRows, moreAvailable, queryAllowed, queryReason, defaultTable,
    // baseName, pendingChanges, available (either scope), reason (why not, when !available)}.
    // The reasons are texts for the user, already translated.
    Q_INVOKABLE QVariantMap infoFor(const QString &tabId);

    // [{id, name, extension, filter}] in ExportFormat order.
    Q_INVOKABLE QVariantList formats() const;
    // The folder of the last export (or Documents), a suggested file in it for the tab, and the
    // same path with the extension of another format when it still carries one of ours.
    Q_INVOKABLE QString defaultPath(const QString &tabId, int format) const;
    Q_INVOKABLE QString retarget(const QString &path, int format) const;
    Q_INVOKABLE QString directoryOf(const QString &path) const;
    Q_INVOKABLE QUrl folderUrl(const QString &path) const;
    Q_INVOKABLE QUrl fileUrl(const QString &path) const;
    Q_INVOKABLE QString toLocalPath(const QUrl &url) const;
    Q_INVOKABLE bool fileExists(const QString &path) const;

    // request: {tabId, scope ("current" | "query"), format (ExportFormat as int), filePath,
    // delimiter (a character, Csv), header, bom, tableName, rowsPerInsert}. Returns "" when the
    // export started (state() is "running" or "preparing"), else the reason it did not, in plain
    // language; nothing changes then. The caller asks before replacing an existing file.
    Q_INVOKABLE QString start(const QVariantMap &request);
    // Ends a running export with state "cancelled" (the end arrives asynchronously).
    Q_INVOKABLE void cancel();
    // Back to idle after done, failed or cancelled. Does nothing while busy.
    Q_INVOKABLE void reset();
    // Opens the folder of the exported file in the system file manager.
    Q_INVOKABLE void showInFolder() const;

signals:
    void queriesChanged();
    void infoChanged();
    void stateChanged();
    void progressChanged();
    // For the screen reader, without moving the focus: start, cancel, end and error only.
    void announcement(const QString &text, bool assertive);

private:
    struct Request
    {
        QString tabId;
        bool streaming = false;
        ExportOptions options;
        Statement statement;
        QString connectionId;
    };

    ConnectionController *connections() const;
    void bindTabs();
    void bindCurrent();
    void touch();
    const ISqlDialect *dialectOf(const QString &connectionId) const;
    QString statementOf(const QString &tabId) const;
    int rowOf(const QString &tabId) const;
    void resetRun(const QString &filePath);
    QString beginPreparing(Request request);
    void openSession(const QString &password);
    void onSessionOpened();
    void onSessionFailed(const Error &error);
    void onPasswordAvailable(QUuid id, const QString &password);
    void onPasswordUnavailable(QUuid id, const Error &error);
    void connectExporter();
    void dropSession();
    void setState(State state);
    void finishWith(State state, const QString &text, bool assertive);
    void releaseExporter();

    // Not a QPointer: clang-analyzer (Qt 6.12) reports a false use-after-free in ~QWeakPointer.
    // Cleared on destroyed() instead.
    QueryController *m_queries = nullptr;
    EditorTabsModel *m_tabs = nullptr;
    QList<QMetaObject::Connection> m_tabLinks; // to the tabs model
    QList<QMetaObject::Connection> m_stateLinks; // to the current QueryState
    Exporter *m_exporter = nullptr; // a child; deleted once it has ended
    std::unique_ptr<Session> m_session; // preparing: not handed to the Exporter yet
    std::optional<Request> m_request; // the export being prepared or run
    QUuid m_waitingPassword; // keychain read in flight
    State m_state = State::Idle;
    bool m_cancelRequested = false;
    qint64 m_rows = 0;
    qint64 m_bytes = 0;
    QString m_filePath;
    QString m_resultText;
    Error m_error;
};

} // namespace quarry::ui
