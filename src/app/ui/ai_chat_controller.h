#pragma once

#include "ai_record_model.h"
#include "ai_script_diff.h"

#include <quarry/ai/ai_prompt.h>
#include <quarry/ai/ai_sanitizer.h>
#include <quarry/ai/claude_cli.h>
#include <quarry/ai/schema_context.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <vector>

namespace quarry::ui {

class AiController;

// The chat of ONE query tab (H13 phase C, ARCHITECTURE section 6.13), born the first time the panel
// of that tab opens and destroyed with the tab. It uses the connection of the tab. The history
// lives in memory only: nothing is written to quarry.db, and closing the tab loses it.
//
// One request at a time. send() runs: availability (the AI is on for the connection and Claude Code
// exists) -> locate the CLI -> the structure of the connection (AiSchemaAdapter, structure only) ->
// ContextBuilder -> mask the literals of the script -> buildPrompt -> ClaudeRunner::ask ->
// parseReply
// -> unmask -> ScriptDiff against the script that was sent. It NEVER runs SQL: the only things it
// reads are the text of the tab and the message of the last failed run; the proposal is text for
// the review view, and writing it into the editor is a decision of the user (Apply).
//
// States: idle | preparing | thinking | done | error | cancelled, and off | needsCli | noConnection
// while nothing can be sent.
class AiChatController : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS

    Q_PROPERTY(QString tabId READ tabId CONSTANT FINAL)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged FINAL)
    // The state in words, for the status card and the screen reader ("Claude is thinking…").
    Q_PROPERTY(QString stateText READ stateText NOTIFY stateChanged FINAL)
    // preparing or thinking.
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged FINAL)
    // "" (it can send) | "off" | "needsCli" | "noConnection", with its reason in plain language.
    Q_PROPERTY(QString unavailableKind READ unavailableKind NOTIFY availabilityChanged FINAL)
    Q_PROPERTY(QString unavailableText READ unavailableText NOTIFY availabilityChanged FINAL)
    Q_PROPERTY(bool canSend READ canSend NOTIFY stateChanged FINAL)
    // The connection of the tab (it can change under the chat).
    Q_PROPERTY(QString connectionId READ connectionId NOTIFY availabilityChanged FINAL)
    // {role: "user" | "assistant", author: "You" | "Claude", text}
    Q_PROPERTY(quarry::ui::AiRecordModel *messages READ messages CONSTANT FINAL)
    // What each request sent, in the order they were made (the "Context sent" panel).
    Q_PROPERTY(quarry::ui::AiRecordModel *contexts READ contexts CONSTANT FINAL)
    // The proposal under review, null when there is none.
    Q_PROPERTY(quarry::ui::AiScriptDiff *diff READ diff NOTIFY diffChanged FINAL)
    // The script of the tab is not the one the proposal was made for: nothing is applied.
    Q_PROPERTY(bool diffStale READ diffStale NOTIFY diffChanged FINAL)
    // The error of the last request ("" kind = none).
    Q_PROPERTY(QString errorKind READ errorKind NOTIFY errorChanged FINAL)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorChanged FINAL)
    // What to do next: the command to sign in, or nothing.
    Q_PROPERTY(QString errorHint READ errorHint NOTIFY errorChanged FINAL)
    // The CLI's own text, already through sanitizeErrorMessage().
    Q_PROPERTY(QString errorDetail READ errorDetail NOTIFY errorChanged FINAL)
    // "settings" when the fix is in the Settings dialog, else "".
    Q_PROPERTY(QString errorAction READ errorAction NOTIFY errorChanged FINAL)
    // What the user typed and did not send (kept when the tab changes).
    Q_PROPERTY(QString draft READ draft WRITE setDraft NOTIFY draftChanged FINAL)

public:
    // The history is kept to this many messages in memory (the prompt takes fewer: maxChars).
    static constexpr int kMaxMessages = 200;
    // The requests listed in "Context sent".
    static constexpr int kMaxContexts = 20;
    static constexpr int kSettingsWaitSeconds = 30;

    AiChatController(AiController *owner, const QString &tabId);
    // Cancels whatever is running: the process of Claude does not outlive the tab.
    ~AiChatController() override;

    QString tabId() const { return m_tabId; }
    QString state() const { return m_state; }
    QString stateText() const;
    bool busy() const
    {
        return m_state == QLatin1String("preparing") || m_state == QLatin1String("thinking");
    }
    QString unavailableKind() const { return m_unavailable; }
    QString unavailableText() const;
    bool canSend() const { return !busy() && m_unavailable.isEmpty(); }
    QString connectionId() const;
    AiRecordModel *messages() { return &m_messages; }
    AiRecordModel *contexts() { return &m_contexts; }
    AiScriptDiff *diff() const { return m_diff; }
    bool diffStale() const { return m_diff != nullptr && m_stale; }
    QString errorKind() const { return m_errorKind; }
    QString errorMessage() const { return m_errorMessage; }
    QString errorHint() const { return m_errorHint; }
    QString errorDetail() const { return m_errorDetail; }
    QString errorAction() const { return m_errorAction; }
    QString draft() const { return m_draft; }
    void setDraft(const QString &text);

    // Starts a request; false when nothing was started (empty text, one is running, the AI is off
    // for the connection, Claude Code is missing: the state says which).
    Q_INVOKABLE bool send(const QString &request);
    // Cancels the context or the process; the state is "cancelled" and no error is shown.
    Q_INVOKABLE void cancel();
    // Forgets the messages, the requests listed, the error and the proposal.
    Q_INVOKABLE void clearChat();
    // Drops the proposal without touching the script.
    Q_INVOKABLE void discardDiff();
    // Makes the proposal again against the script of the tab as it is now.
    Q_INVOKABLE void redoDiff();
    // The view wrote the result into the editor: the proposal is over.
    Q_INVOKABLE void diffApplied();
    // Reads the settings and the tab again (the panel opened, the connection changed).
    Q_INVOKABLE void refresh();
    // The text of a "Context sent" row, as copied and as logged.
    Q_INVOKABLE QString contextText(int row) const;

    // Test seam: the time limit of the CLI run (the runner clamps it to 10..600 s).
    void setTimeoutSeconds(int seconds) { m_timeout = seconds; }

signals:
    void stateChanged();
    void availabilityChanged();
    void diffChanged();
    void errorChanged();
    void draftChanged();
    // For the screen reader (the panel says it with Accessible.announce).
    void announcement(const QString &text, bool assertive);
    // A proposal is ready for review in the editor of the tab.
    void proposalReady(int changes);

private:
    enum class Stage : uint8_t { None, Settings, Locate, Session, Context, Ask };

    int row() const;
    QString scriptNow() const;
    QString lastRunError() const;
    QString computeUnavailable();
    void applyAvailability();
    void setState(const QString &state);
    void setError(quarry::ai::AiErrorKind kind, const QString &detail);
    void clearError();
    void say(const QString &text, bool assertive = false);
    void proceed();
    void startContext();
    void build(const quarry::ai::SchemaContext &schema, const QString &note);
    void onLocated(quint64 id, const QString &path);
    void onLocateFailed(quint64 id, const QString &detail);
    void onContextReady(quint64 id, const quarry::ai::SchemaContext &context, bool truncated);
    void onContextFailed(quint64 id, const QString &message, const QString &detail);
    void onFinished(quint64 id, const quarry::ai::CliOutcome &outcome);
    void onReply(const QString &text);
    void setDiff(AiScriptDiff *diff, bool stale);
    void stopWork();

    QPointer<AiController> m_owner;
    QString m_tabId;
    QString m_state = QStringLiteral("idle");
    QString m_unavailable;
    AiRecordModel m_messages;
    AiRecordModel m_contexts;
    AiScriptDiff *m_diff = nullptr;
    bool m_stale = false;
    QString m_errorKind;
    QString m_errorMessage;
    QString m_errorHint;
    QString m_errorDetail;
    QString m_errorAction;
    QString m_draft;
    int m_timeout = 120;

    // The request in flight.
    Stage m_stage = Stage::None;
    QString m_request;
    QString m_script; // the script as it was when the request was made (what the diff is against)
    QString m_scriptName;
    QString m_cliPath;
    QString m_lastRunError; // sanitized, taken when the request was made
    QString m_note; // why the structure is missing, when it is
    std::vector<quarry::ai::ChatTurn> m_history;
    quarry::ai::MaskedScript m_masked;
    QString m_proposed; // the proposal with its literals back (for redoDiff)
    quint64 m_locateId = 0;
    quint64 m_adapterId = 0;
    quint64 m_askId = 0;
    QTimer m_poll;
    int m_polls = 0;
    bool m_treeConnected = false;
    bool m_runnerConnected = false;
    bool m_adapterConnected = false;
};

} // namespace quarry::ui
