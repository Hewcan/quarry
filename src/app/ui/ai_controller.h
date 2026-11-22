#pragma once

#include "ai_chat_controller.h"
#include "ai_schema_adapter.h"
#include "completion_controller.h"
#include "connection_controller.h"
#include "editor_tabs_model.h"
#include "query_controller.h"

#include <quarry/ai/claude_runner.h>
#include <quarry/ai/schema_context.h>
#include <quarry/ai_settings.h>

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <deque>
#include <memory>

namespace quarry::ui {

// The AI assistant in the UI (H13, ARCHITECTURE section 6.13): where the user's Claude Code is,
// whether it works, the per-connection settings (off by default) and, from phase C, the chats: one
// AiChatController per query tab, created the first time the tab's panel opens (chatFor) and
// destroyed with the tab.
//
// Everything is created on first use: the object itself does no work (no disk, no process) until a
// dialog asks for something; the ClaudeRunner exists only after testCli(), the settings service and
// the read of `ai_settings` only after the first settingsOf() / isEnabled() / saveSettings(). With
// the AI off nothing here runs at startup.
//
// Global: `cliPath` lives in the UI preferences (QSettings "ui/layout/ai/cliPath", next to
// LayoutStore's keys), never in quarry.db. Per connection: AiSettingsService over quarry.db.
class AiController : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(quarry::ui::ConnectionController *connections READ connections WRITE setConnections
                   NOTIFY connectionsChanged FINAL)
    // Empty = look for it in the usual places.
    Q_PROPERTY(QString cliPath READ cliPath WRITE setCliPath NOTIFY cliPathChanged FINAL)
    // "unknown" | "checking" | "ok" | "notFound" | "incompatible" | "failed"
    Q_PROPERTY(QString cliState READ cliState NOTIFY cliChanged FINAL)
    Q_PROPERTY(QString cliVersion READ cliVersion NOTIFY cliChanged FINAL)
    // The executable that was found (or tried).
    Q_PROPERTY(QString cliLocation READ cliLocation NOTIFY cliChanged FINAL)
    // Plain translated text for the state, written for the user.
    Q_PROPERTY(QString cliMessage READ cliMessage NOTIFY cliChanged FINAL)
    // Raw text of the failure, to copy. Never a secret.
    Q_PROPERTY(QString cliDetail READ cliDetail NOTIFY cliChanged FINAL)
    // What to type in a terminal to sign in ("claude /login").
    Q_PROPERTY(QString loginHint READ loginHint CONSTANT FINAL)
    // The settings of every connection were read from quarry.db.
    Q_PROPERTY(bool settingsReady READ settingsReady NOTIFY settingsReadyChanged FINAL)
    // Message of the last settings operation that failed ("" if it did not).
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged FINAL)
    // What the chats need from the app: the tabs (the script and the connection of each), the
    // queries (the error of the last run) and the autocompletion's caches (a structure already
    // read).
    Q_PROPERTY(quarry::ui::EditorTabsModel *tabs READ tabs WRITE setTabs NOTIFY tabsChanged FINAL)
    Q_PROPERTY(quarry::ui::QueryController *queries READ queries WRITE setQueries NOTIFY
                   queriesChanged FINAL)
    Q_PROPERTY(quarry::ui::CompletionController *completion READ completion WRITE setCompletion
                   NOTIFY completionChanged FINAL)
    // How many chats exist: the editor re-reads existingChat() when it changes.
    Q_PROPERTY(int chatCount READ chatCount NOTIFY chatsChanged FINAL)
    // Keep a local log of what each request sends (off by default; ui/layout/ai/keepLog).
    Q_PROPERTY(bool keepLog READ keepLog WRITE setKeepLog NOTIFY keepLogChanged FINAL)
    // The folder of ai-requests.log (AppLocalData/logs) and the file in it.
    Q_PROPERTY(
        QString logDirectory READ logDirectory WRITE setLogDirectory NOTIFY keepLogChanged FINAL)
    Q_PROPERTY(QString logFile READ logFile NOTIFY keepLogChanged FINAL)

public:
    explicit AiController(QObject *parent = nullptr);
    ~AiController() override;

    ConnectionController *connections() const { return m_connections; }
    void setConnections(ConnectionController *connections);
    QString cliPath() const;
    void setCliPath(const QString &path);
    QString cliState() const { return m_state; }
    QString cliVersion() const { return m_version; }
    QString cliLocation() const { return m_location; }
    QString cliMessage() const { return m_message; }
    QString cliDetail() const { return m_detail; }
    QString loginHint() const;
    bool settingsReady() const { return m_ready; }
    QString lastError() const { return m_lastError; }
    EditorTabsModel *tabs() const { return m_tabs; }
    void setTabs(EditorTabsModel *tabs);
    QueryController *queries() const { return m_queries; }
    void setQueries(QueryController *queries);
    CompletionController *completion() const { return m_completion; }
    void setCompletion(CompletionController *completion);
    int chatCount() const { return static_cast<int>(m_chats.size()); }
    bool keepLog() const;
    void setKeepLog(bool on);
    QString logDirectory() const;
    void setLogDirectory(const QString &directory);
    QString logFile() const;

    // `locate(cliPath, true)` and then `check(path)`: reads `claude --version`, which spends
    // nothing. Asynchronous; the answer is cliState. A typed path that is not a file is notFound
    // without starting anything (the runner would fall back to the usual places).
    Q_INVOKABLE QString toLocalPath(const QUrl &url) const;
    Q_INVOKABLE void testCli();
    // Forgets a test in flight (the dialog closed): its answer is ignored and the process ends.
    Q_INVOKABLE void cancelCli();

    // ---- per connection ----
    // Reads every row from quarry.db the first time (asynchronous); until then the defaults.
    Q_INVOKABLE void ensureSettings();
    // {enabled, excludedSchemas, excludedTables, sendComments, sendDefaults, sendChecks,
    //  sendEnumValues, sendDefinitions, maskLiterals, maskNumbers, maxTables}; the defaults for a
    // connection without a row (the AI off).
    Q_INVOKABLE QVariantMap settingsOf(const QString &connectionId);
    Q_INVOKABLE bool isEnabled(const QString &connectionId);
    // Stores the settings of a connection (normalized in core: lists trimmed and de-duplicated,
    // maxTables 1..200). Turning the AI ON for a connection marked as production needs
    // `confirmedProduction: true` in the map (the UI asks the user to type the name first); without
    // it nothing is stored, `lastError` says why and false is returned. Same for an unknown
    // connection. The answer is settingsChanged(connectionId) or saveFailed(...).
    Q_INVOKABLE bool saveSettings(const QString &connectionId, const QVariantMap &settings);
    // The settings of a connection as a quarry::ai::ContextOptions (the maskers use the rest).
    static quarry::ai::ContextOptions optionsFrom(const quarry::AiConnectionSettings &settings);
    quarry::AiConnectionSettings storedSettings(const QString &connectionId);
    static QVariantMap toMap(const quarry::AiConnectionSettings &settings);
    static quarry::AiConnectionSettings fromMap(const QUuid &id, const QVariantMap &map);

    // ---- the chats (phase C) ----
    // The chat of a query tab; created on the first call (nothing exists before the panel opens).
    // Null for an id that is not a tab of the editor.
    Q_INVOKABLE quarry::ui::AiChatController *chatFor(const QString &tabId);
    Q_INVOKABLE quarry::ui::AiChatController *existingChat(const QString &tabId) const;
    // Cancels and deletes the chat of a tab (the tab closed).
    Q_INVOKABLE void forgetChat(const QString &tabId);
    // Opens the folder of the request log in the file manager (nothing if it does not exist yet).
    Q_INVOKABLE void openLogFolder();
    // What the chats use. The runner is the one of testCli(): one object for the app.
    quarry::ai::ClaudeRunner *chatRunner() { return runner(); }
    AiSchemaAdapter *adapter();
    AiSchemaAdapter *adapterIfCreated() const { return m_adapter; }
    // Appends one request to the local log when keepLog is on; the disk work runs in a worker.
    void logRequest(const QString &text);
    // Waits for the pending log writes (tests).
    static void flushLog();
    // Appends `text` to <directory>/ai-requests.log; when it would pass `maxBytes`, the file is
    // rotated first (ai-requests.log.1, then .2: two copies are kept).
    static void appendLog(const QString &directory, const QString &text,
                          qint64 maxBytes = kMaxLogBytes);
    static constexpr qint64 kMaxLogBytes = qint64(1024) * 1024;

    // Test seams: null until something needs them (QML tests ask whether they exist).
    Q_INVOKABLE bool hasRunner() const { return m_runner != nullptr; }
    Q_INVOKABLE bool hasAdapter() const { return m_adapter != nullptr; }
    quarry::ai::ClaudeRunner *runnerIfCreated() const { return m_runner; }
    quarry::AiSettingsService *serviceIfCreated() const { return m_service.get(); }
    quarry::AiSettingsService *service();

signals:
    void connectionsChanged();
    void cliPathChanged();
    void cliChanged();
    void settingsReadyChanged();
    void lastErrorChanged();
    void tabsChanged();
    void queriesChanged();
    void completionChanged();
    void chatsChanged();
    void keepLogChanged();
    // `settingsOf(connectionId)` may answer differently. An empty id: everything was (re)loaded.
    void settingsChanged(const QString &connectionId);
    void saveFailed(const QString &connectionId, const QString &message, const QString &detail);

private:
    struct Op
    {
        QUuid id; // null: a load
        bool load = false;
    };

    quarry::ai::ClaudeRunner *runner();
    void setCli(const QString &state, const QString &version, const QString &location,
                const QString &message, const QString &detail);
    void onLocated(quint64 id, const QString &path);
    void onLocateFailed(quint64 id, const QString &detail);
    void onChecked(quint64 id, const quarry::ai::CliInfo &info);
    void setLastError(const QString &message);
    void onLoaded(const QList<quarry::AiConnectionSettings> &list);
    void onSaved(const quarry::AiConnectionSettings &settings);
    void onFailed(const quarry::Error &error);
    QString databasePath() const;

    QPointer<ConnectionController> m_connections;
    QPointer<EditorTabsModel> m_tabs;
    QPointer<QueryController> m_queries;
    QPointer<CompletionController> m_completion;
    QHash<QString, QPointer<AiChatController>> m_chats; // by tab id
    AiSchemaAdapter *m_adapter = nullptr; // child; created by the first chat that needs it
    mutable bool m_keepLog = false;
    mutable bool m_keepLogRead = false;
    QString m_logDirectory;
    quarry::ai::ClaudeRunner *m_runner = nullptr; // child; created by the first testCli()
    quint64 m_locateId = 0;
    quint64 m_checkId = 0;
    QString m_state = QStringLiteral("unknown");
    QString m_version;
    QString m_location;
    QString m_message;
    QString m_detail;

    std::unique_ptr<quarry::AiSettingsService> m_service;
    QHash<QString, quarry::AiConnectionSettings> m_settings; // by connection id
    std::deque<Op> m_ops; // the worker answers in order
    bool m_loadStarted = false;
    bool m_ready = false;
    QString m_lastError;

    mutable QString m_cliPath;
    mutable bool m_cliPathRead = false;
};

} // namespace quarry::ui
