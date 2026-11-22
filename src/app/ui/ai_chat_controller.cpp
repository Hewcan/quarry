#include "ai_chat_controller.h"

#include "ai_controller.h"
#include "ai_schema_adapter.h"
#include "query_messages_model.h"
#include "query_state.h"

#include <quarry/ai/claude_runner.h>
#include <quarry/ai/context_builder.h>
#include <quarry/ai/script_diff.h>

#include <QDateTime>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

#include <algorithm>

namespace quarry::ui {

namespace {

using quarry::ai::AiErrorKind;

// The fixed sentence of the "Context sent" panel: what never leaves the app.
const QString kNotSent = QStringLiteral("Not sent: rows, results or connection details.");

QString kindName(AiErrorKind kind)
{
    switch (kind) {
    case AiErrorKind::CliNotFound:
        return QStringLiteral("cliNotFound");
    case AiErrorKind::NotLoggedIn:
        return QStringLiteral("notLoggedIn");
    case AiErrorKind::IncompatibleVersion:
        return QStringLiteral("incompatibleVersion");
    case AiErrorKind::UsageLimit:
        return QStringLiteral("usageLimit");
    case AiErrorKind::Timeout:
        return QStringLiteral("timeout");
    case AiErrorKind::InvalidOutput:
        return QStringLiteral("invalidOutput");
    case AiErrorKind::Cancelled:
    case AiErrorKind::None:
        return {};
    case AiErrorKind::Failed:
        break;
    }
    return QStringLiteral("failed");
}

// The names a script or a question mentions: the adapter describes these tables first when the
// connection has more than it describes.
QStringList wordsOf(const QString &script, const QString &request)
{
    static const QRegularExpression kWord(QStringLiteral("[A-Za-z_][A-Za-z0-9_]*"));
    QSet<QString> seen;
    QStringList out;
    for (const QString &text : { script, request }) {
        auto it = kWord.globalMatch(text);
        while (it.hasNext() && out.size() < 400) {
            const QString word = it.next().captured();
            if (!seen.contains(word.toLower())) {
                seen.insert(word.toLower());
                out.append(word);
            }
        }
    }
    return out;
}

} // namespace

AiChatController::AiChatController(AiController *owner, const QString &tabId)
    : QObject(owner)
    , m_owner(owner)
    , m_tabId(tabId)
    , m_messages({ QStringLiteral("role"), QStringLiteral("author"), QStringLiteral("text") }, this)
    , m_contexts({ QStringLiteral("time"), QStringLiteral("request"), QStringLiteral("tables"),
                   QStringLiteral("tablesText"), QStringLiteral("ddl"), QStringLiteral("script"),
                   QStringLiteral("errorText"), QStringLiteral("historyCount"),
                   QStringLiteral("note"), QStringLiteral("text") },
                 this)
{
    m_timeout = quarry::ai::ClaudeRunner::kDefaultTimeoutSeconds;
    m_poll.setInterval(300);
    m_poll.setSingleShot(true);
    connect(&m_poll, &QTimer::timeout, this, [this] {
        ++m_polls;
        proceed();
    });
    if (m_owner) {
        // The settings and the path of the CLI decide whether the field is enabled.
        connect(m_owner, &AiController::settingsChanged, this, &AiChatController::refresh);
        connect(m_owner, &AiController::settingsReadyChanged, this, [this] {
            refresh();
            if (m_stage == Stage::Settings)
                proceed();
        });
        connect(m_owner, &AiController::cliChanged, this, &AiChatController::refresh);
        connect(m_owner, &AiController::cliPathChanged, this, &AiChatController::refresh);
        if (m_owner->tabs()) {
            connect(m_owner->tabs(), &QAbstractItemModel::dataChanged, this,
                    [this](const QModelIndex &, const QModelIndex &, const QList<int> &roles) {
                        if (roles.isEmpty() || roles.contains(EditorTabsModel::ConnectionIdRole))
                            refresh();
                    });
        }
    }
    if (m_owner)
        m_owner->ensureSettings(); // the panel is open: reading the settings is what the user asked
                                   // for
    m_unavailable = computeUnavailable();
    applyAvailability();
}

// NOLINTBEGIN(clang-analyzer-cplusplus.NewDelete): QPointer false positive
AiChatController::~AiChatController()
{
    stopWork();
}
// NOLINTEND(clang-analyzer-cplusplus.NewDelete)

// ---- reading the tab
// ------------------------------------------------------------------------------

int AiChatController::row() const
{
    EditorTabsModel *tabs = m_owner ? m_owner->tabs() : nullptr;
    if (tabs == nullptr)
        return -1;
    for (int r = 0; r < tabs->count(); ++r) {
        if (tabs->idOf(r) == m_tabId)
            return r;
    }
    return -1;
}

QString AiChatController::connectionId() const
{
    const int r = row();
    return r < 0 ? QString() : m_owner->tabs()->connectionIdOf(r);
}

QString AiChatController::scriptNow() const
{
    const int r = row();
    return r < 0 ? QString() : m_owner->tabs()->textOf(r);
}

// What the last run of the tab said when it failed, through the sanitizer (a database error can
// quote a value of a row). Nothing else of the run is read: no rows, no columns.
QString AiChatController::lastRunError() const
{
    QueryController *queries = m_owner ? m_owner->queries() : nullptr;
    if (queries == nullptr)
        return {};
    QueryState *state = queries->stateOf(m_tabId);
    if (state == nullptr || state->state() != QLatin1String("failed"))
        return {};
    QueryMessagesModel *messages = state->messages();
    for (int r = messages->rowCount() - 1; r >= 0; --r) {
        const QModelIndex at = messages->index(r);
        if (messages->data(at, QueryMessagesModel::KindRole).toString() != QLatin1String("error"))
            continue;
        QString text = messages->data(at, QueryMessagesModel::TextRole).toString();
        const QString detail = messages->data(at, QueryMessagesModel::DetailRole).toString();
        if (!detail.isEmpty())
            text += QLatin1Char('\n') + detail;
        return quarry::ai::sanitizeErrorMessage(text);
    }
    return {};
}

// ---- availability
// ---------------------------------------------------------------------------------

QString AiChatController::computeUnavailable()
{
    if (!m_owner)
        return QStringLiteral("off");
    const QString id = connectionId();
    if (id.isEmpty())
        return QStringLiteral("noConnection");
    // Until the settings are read the answer is not known: the field stays usable and send() waits.
    if (m_owner->settingsReady() && !m_owner->isEnabled(id))
        return QStringLiteral("off");
    const QString custom = m_owner->cliPath().trimmed();
    if ((!custom.isEmpty() && !QFileInfo(custom).isFile())
        || m_owner->cliState() == QLatin1String("notFound"))
        return QStringLiteral("needsCli");
    return {};
}

QString AiChatController::unavailableText() const
{
    if (m_unavailable == QLatin1String("off"))
        return tr("The AI assistant is off for this connection. Turn it on in the connection's "
                  "settings.");
    if (m_unavailable == QLatin1String("needsCli"))
        return tr("Claude Code was not found. Install it, or enter the path of its executable in "
                  "Settings.");
    if (m_unavailable == QLatin1String("noConnection"))
        return tr("This query has no connection. Choose one in the toolbar to use the assistant.");
    return {};
}

void AiChatController::applyAvailability()
{
    if (busy())
        return;
    const bool waiting = m_state == QLatin1String("idle") || m_state == QLatin1String("off")
        || m_state == QLatin1String("needsCli") || m_state == QLatin1String("noConnection");
    if (!waiting)
        return;
    setState(m_unavailable.isEmpty() ? QStringLiteral("idle") : m_unavailable);
}

void AiChatController::refresh()
{
    const QString now = computeUnavailable();
    const bool changed = now != m_unavailable;
    m_unavailable = now;
    if (changed)
        emit availabilityChanged();
    applyAvailability();
    if (changed)
        emit stateChanged(); // canSend follows the availability
}

// ---- state
// ----------------------------------------------------------------------------------------

QString AiChatController::stateText() const
{
    if (m_state == QLatin1String("preparing"))
        return tr("Preparing the request…");
    if (m_state == QLatin1String("thinking"))
        return tr("Claude is thinking…");
    if (m_state == QLatin1String("done"))
        return m_diff ? tr("Done. Claude proposed changes to the script.") : tr("Done.");
    if (m_state == QLatin1String("error"))
        return m_errorMessage;
    if (m_state == QLatin1String("cancelled"))
        return tr("Cancelled.");
    if (m_state == QLatin1String("idle"))
        return {};
    return unavailableText(); // off, needsCli, noConnection
}

void AiChatController::setState(const QString &state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged();
}

void AiChatController::say(const QString &text, bool assertive)
{
    emit announcement(text, assertive);
}

void AiChatController::setDraft(const QString &text)
{
    if (m_draft == text)
        return;
    m_draft = text;
    emit draftChanged();
}

void AiChatController::clearError()
{
    if (m_errorKind.isEmpty() && m_errorMessage.isEmpty())
        return;
    m_errorKind.clear();
    m_errorMessage.clear();
    m_errorHint.clear();
    m_errorDetail.clear();
    m_errorAction.clear();
    emit errorChanged();
}

void AiChatController::setError(AiErrorKind kind, const QString &detail)
{
    m_errorKind = kindName(kind);
    m_errorHint.clear();
    m_errorAction.clear();
    switch (kind) {
    case AiErrorKind::CliNotFound:
        m_errorMessage = tr("Claude Code could not be started. Install Claude Code, or enter the "
                            "path of its executable in Settings.");
        m_errorAction = QStringLiteral("settings");
        break;
    case AiErrorKind::NotLoggedIn:
        m_errorMessage
            = tr("Claude Code is not signed in. Sign in from a terminal, then ask again.");
        m_errorHint = quarry::ai::loginCommandHint();
        break;
    case AiErrorKind::IncompatibleVersion:
        m_errorMessage
            = tr("This version of Claude Code is not supported. Update Claude Code and try again.");
        m_errorAction = QStringLiteral("settings");
        break;
    case AiErrorKind::UsageLimit:
        m_errorMessage = tr("The usage limit was reached; try again later. Requests count against "
                            "your Claude subscription or API key.");
        break;
    case AiErrorKind::Timeout:
        m_errorMessage
            = tr("Claude did not answer in time. Try again, or ask for something smaller.");
        break;
    case AiErrorKind::InvalidOutput:
        m_errorMessage = tr("Claude Code answered in a form Quarry cannot read.");
        break;
    case AiErrorKind::Cancelled:
    case AiErrorKind::None:
    case AiErrorKind::Failed:
        m_errorMessage = tr("Claude Code could not answer the request.");
        break;
    }
    m_errorDetail = quarry::ai::sanitizeErrorMessage(detail);
    emit errorChanged();
    setState(QStringLiteral("error"));
    say(m_errorMessage, true);
}

void AiChatController::setDiff(AiScriptDiff *diff, bool stale)
{
    AiScriptDiff *old = m_diff;
    m_diff = diff;
    m_stale = stale;
    if (m_diff)
        m_diff->setParent(this);
    emit diffChanged();
    if (old)
        old->deleteLater();
}

// ---- the request
// ----------------------------------------------------------------------------------

bool AiChatController::send(const QString &request)
{
    const QString text = request.trimmed();
    if (busy() || text.isEmpty() || !m_owner || row() < 0)
        return false;
    refresh();
    if (!m_unavailable.isEmpty()) {
        setState(m_unavailable); // said again, in the status card
        say(unavailableText(), true);
        return false;
    }

    // The history is what came before this request; the request itself goes apart.
    m_history.clear();
    m_history.reserve(static_cast<size_t>(m_messages.count()));
    for (const QVariantMap &message : m_messages.rows()) {
        quarry::ai::ChatTurn turn;
        turn.role = message.value(QStringLiteral("role")).toString() == QLatin1String("user")
            ? quarry::ai::ChatTurn::Role::User
            : quarry::ai::ChatTurn::Role::Assistant;
        turn.text = message.value(QStringLiteral("text")).toString();
        m_history.push_back(std::move(turn));
    }
    m_messages.append({ { QStringLiteral("role"), QStringLiteral("user") },
                        { QStringLiteral("author"), tr("You") },
                        { QStringLiteral("text"), text } });
    m_messages.trim(kMaxMessages);

    clearError();
    m_request = text;
    m_script = scriptNow();
    m_scriptName = m_owner->tabs()->titleOf(row());
    m_lastRunError = lastRunError();
    m_note.clear();
    m_polls = 0;
    m_stage = Stage::Settings;
    setState(QStringLiteral("preparing"));
    say(stateText());
    proceed();
    return true;
}

void AiChatController::stopWork()
{
    m_poll.stop();
    m_stage = Stage::None;
    m_locateId = 0;
    if (m_askId != 0) {
        const quint64 id = m_askId;
        m_askId = 0;
        if (m_owner && m_owner->runnerIfCreated())
            m_owner->runnerIfCreated()->cancel(id);
    }
    if (m_adapterId != 0) {
        const quint64 id = m_adapterId;
        m_adapterId = 0;
        if (m_owner && m_owner->adapterIfCreated())
            m_owner->adapterIfCreated()->cancel(id);
    }
}

void AiChatController::cancel()
{
    if (!busy())
        return;
    stopWork();
    setState(QStringLiteral("cancelled"));
    say(tr("Request cancelled"));
}

void AiChatController::proceed()
{
    if (!busy() || !m_owner)
        return;
    quarry::ai::ClaudeRunner *runner = m_owner->chatRunner();
    const QString id = connectionId();

    if (m_stage == Stage::Settings) {
        if (!m_owner->settingsReady()) {
            m_owner->ensureSettings();
            return; // settingsReadyChanged brings us back
        }
        refresh();
        if (!m_unavailable.isEmpty()) {
            stopWork();
            setState(m_unavailable);
            say(unavailableText(), true);
            return;
        }
        m_stage = Stage::Locate;
    }

    if (m_stage == Stage::Locate) {
        if (!m_runnerConnected) {
            m_runnerConnected = true;
            connect(runner, &quarry::ai::ClaudeRunner::located, this, &AiChatController::onLocated);
            connect(runner, &quarry::ai::ClaudeRunner::locateFailed, this,
                    &AiChatController::onLocateFailed);
            connect(runner, &quarry::ai::ClaudeRunner::finished, this,
                    &AiChatController::onFinished);
        }
        m_locateId = runner->locate(m_owner->cliPath().trimmed());
        return; // located() or locateFailed()
    }

    if (m_stage == Stage::Session) {
        ConnectionController *connections = m_owner->connections();
        SchemaTreeModel *tree = connections ? connections->tree() : nullptr;
        if (tree == nullptr) {
            build({}, tr("The connection is not open, so Claude does not know its tables."));
            return;
        }
        if (!m_treeConnected) {
            m_treeConnected = true;
            connect(tree, &SchemaTreeModel::connectedChanged, this, [this] {
                if (m_stage == Stage::Session)
                    proceed();
            });
        }
        const SchemaTreeModel::Access access = tree->ensureSession(id);
        if (access == SchemaTreeModel::Access::Opening
            && m_polls * m_poll.interval() < kSettingsWaitSeconds * 1000) {
            m_poll.start(); // the connection is opening: look again
            return;
        }
        if (access != SchemaTreeModel::Access::Connected) {
            build({},
                  access == SchemaTreeModel::Access::NeedsPassword
                      ? tr("The connection asks for its password, so Claude does not know its "
                           "tables. Open the connection in the tree and ask again.")
                      : tr("The connection is not open, so Claude does not know its tables."));
            return;
        }
        m_stage = Stage::Context;
    }

    if (m_stage == Stage::Context)
        startContext();
}

void AiChatController::onLocated(quint64 id, const QString &path)
{
    if (id == 0 || id != m_locateId || m_stage != Stage::Locate)
        return;
    m_locateId = 0;
    m_cliPath = path;
    m_stage = Stage::Session;
    proceed();
}

void AiChatController::onLocateFailed(quint64 id, const QString &detail)
{
    if (id == 0 || id != m_locateId || m_stage != Stage::Locate)
        return;
    stopWork();
    m_unavailable = QStringLiteral("needsCli");
    emit availabilityChanged();
    setError(AiErrorKind::CliNotFound, detail);
}

void AiChatController::startContext()
{
    ConnectionController *connections = m_owner->connections();
    const QString id = connectionId();
    const auto profile
        = connections ? connections->model()->profile(QUuid::fromString(id)) : std::nullopt;
    const quarry::AiConnectionSettings settings = m_owner->storedSettings(id);

    AiSchemaAdapter::Request request;
    request.prioritize = wordsOf(m_script, m_request);
    request.options = AiController::optionsFrom(settings);
    request.providerId = profile ? profile->providerId : QString();
    request.session = connections->tree()->metadataSession(id);
    request.cache = m_owner->completion() ? m_owner->completion()->cache(id) : nullptr;

    AiSchemaAdapter *adapter = m_owner->adapter();
    if (!m_adapterConnected) {
        m_adapterConnected = true;
        connect(adapter, &AiSchemaAdapter::contextReady, this, &AiChatController::onContextReady);
        connect(adapter, &AiSchemaAdapter::contextFailed, this, &AiChatController::onContextFailed);
    }
    m_adapterId = adapter->contextFor(request);
}

void AiChatController::onContextReady(quint64 id, const quarry::ai::SchemaContext &context,
                                      bool truncated)
{
    if (id == 0 || id != m_adapterId || m_stage != Stage::Context)
        return;
    m_adapterId = 0;
    build(context,
          truncated ? tr("The connection has more tables than were read; only some are known.")
                    : QString());
}

void AiChatController::onContextFailed(quint64 id, const QString &message, const QString &detail)
{
    Q_UNUSED(detail)
    if (id == 0 || id != m_adapterId || m_stage != Stage::Context)
        return;
    m_adapterId = 0;
    // Claude can still help with the script; the panel says that it does not know the tables.
    build({}, tr("The structure of the connection could not be read: %1").arg(message));
}

// Everything that is sent is decided here, in one place: the script with its literals masked, the
// structure of the tables the script and the question need, the error of the last run through the
// sanitizer and the history. Then the CLI is asked. The "Context sent" entry is made from the very
// same values.
void AiChatController::build(const quarry::ai::SchemaContext &schema, const QString &note)
{
    m_stage = Stage::Ask;
    m_note = note;
    const QString id = connectionId();
    const quarry::AiConnectionSettings settings = m_owner->storedSettings(id);
    const quarry::ai::ContextOptions options = AiController::optionsFrom(settings);

    quarry::ai::SchemaContext known = schema;
    if (known.dialect.isEmpty()) {
        ConnectionController *connections = m_owner->connections();
        const auto profile
            = connections ? connections->model()->profile(QUuid::fromString(id)) : std::nullopt;
        known.dialect = AiSchemaAdapter::dialectOf(profile ? profile->providerId : QString());
    }
    quarry::ai::ContextBuilder builder;
    builder.setSchema(known);
    quarry::ai::SelectedContext selected = builder.build(m_script, m_request, options);

    // maskNumbers only means something with the masking on.
    m_masked = settings.maskLiterals ? quarry::ai::maskLiterals(m_script, settings.maskNumbers)
                                     : quarry::ai::MaskedScript { m_script, {} };

    quarry::ai::PromptInput input;
    input.request = m_request;
    input.scriptName = m_scriptName;
    input.script = m_masked.text;
    input.context = selected;
    input.errorMessage = m_lastRunError;
    input.history = m_history;
    const QByteArray prompt = quarry::ai::buildPrompt(input);

    // What the panel shows, and the log keeps, is this and nothing else.
    QString text;
    text += QStringLiteral("Request: ") + m_request + QLatin1Char('\n');
    text += QStringLiteral("Tables (%1): ").arg(selected.tables.size())
        + selected.tables.join(QStringLiteral(", ")) + QLatin1Char('\n');
    if (!m_note.isEmpty())
        text += QStringLiteral("Note: ") + m_note + QLatin1Char('\n');
    text += QStringLiteral("DDL:\n") + selected.ddl + QLatin1Char('\n');
    text += QStringLiteral("Script (literals masked as asked):\n") + m_masked.text
        + QLatin1Char('\n');
    if (!m_lastRunError.isEmpty())
        text += QStringLiteral("Error of the last run (sanitized):\n") + m_lastRunError
            + QLatin1Char('\n');
    text += QStringLiteral("Earlier messages: %1\n").arg(m_history.size());
    text += kNotSent + QLatin1Char('\n');
    const QString stamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    m_contexts.append(
        { { QStringLiteral("time"), stamp },
          { QStringLiteral("request"), m_request },
          { QStringLiteral("tables"), selected.tables },
          { QStringLiteral("tablesText"), selected.tables.join(QStringLiteral(", ")) },
          { QStringLiteral("ddl"), selected.ddl },
          { QStringLiteral("script"), m_masked.text },
          { QStringLiteral("errorText"), m_lastRunError },
          { QStringLiteral("historyCount"), static_cast<int>(m_history.size()) },
          { QStringLiteral("note"), m_note },
          { QStringLiteral("text"), text } });
    m_contexts.trim(kMaxContexts);
    m_owner->logRequest(QStringLiteral("==== ") + stamp + QStringLiteral(" ====\n") + text
                        + QLatin1Char('\n'));

    setState(QStringLiteral("thinking"));
    say(stateText());
    m_askId = 0;
    const quint64 askId = m_owner->chatRunner()->ask(m_cliPath, prompt, m_timeout);
    // A refusal answers from inside the call, with id 0; onFinished handled it already.
    if (m_stage == Stage::Ask && askId != 0)
        m_askId = askId;
}

void AiChatController::onFinished(quint64 id, const quarry::ai::CliOutcome &outcome)
{
    if (m_stage != Stage::Ask || !busy() || (id != m_askId && !(id == 0 && m_askId == 0)))
        return;
    m_askId = 0;
    m_stage = Stage::None;
    if (outcome.error == AiErrorKind::Cancelled) {
        setState(QStringLiteral("cancelled"));
        return;
    }
    if (outcome.error != AiErrorKind::None) {
        setError(outcome.error, outcome.detail);
        return;
    }
    onReply(outcome.text);
}

void AiChatController::onReply(const QString &text)
{
    const quarry::ai::AiReply reply = quarry::ai::parseReply(text);
    QString message = reply.message.trimmed();
    if (message.isEmpty() && reply.proposedScript)
        message = tr("I changed the script.");
    m_messages.append({ { QStringLiteral("role"), QStringLiteral("assistant") },
                        { QStringLiteral("author"), tr("Claude") },
                        { QStringLiteral("text"), message } });
    m_messages.trim(kMaxMessages);

    int changes = 0;
    if (reply.proposedScript) {
        // The literals Claude saw as __V1__ come back; the diff is against the script that was
        // sent.
        m_proposed = quarry::ai::unmaskLiterals(*reply.proposedScript, m_masked.values);
        auto *diff = new AiScriptDiff(quarry::ai::ScriptDiff::compute(m_script, m_proposed));
        if (diff->changeCount() == 0) {
            delete diff;
            setDiff(nullptr, false);
        } else {
            changes = diff->changeCount();
            // The user may have edited the script while Claude was working.
            setDiff(diff, scriptNow() != m_script);
        }
    } else {
        setDiff(nullptr, false);
    }
    setState(QStringLiteral("done"));
    if (changes > 0) {
        // The review view over the editor says "Claude proposed N changes" when it appears.
        emit proposalReady(changes);
    } else {
        say(tr("Claude answered: %1").arg(message.left(300)));
    }
}

// ---- the proposal
// ---------------------------------------------------------------------------------

void AiChatController::discardDiff()
{
    if (m_diff == nullptr)
        return;
    setDiff(nullptr, false);
    if (m_state == QLatin1String("done"))
        emit stateChanged(); // "Done." without the proposal
}

void AiChatController::redoDiff()
{
    if (m_diff == nullptr || m_proposed.isEmpty())
        return;
    const QString current = scriptNow();
    auto *diff = new AiScriptDiff(quarry::ai::ScriptDiff::compute(current, m_proposed));
    if (diff->changeCount() == 0) {
        delete diff;
        setDiff(nullptr, false);
        say(tr("The script already has what Claude proposed."));
        return;
    }
    m_script = current;
    setDiff(diff, false);
    say(diff->changeCount() == 1
            ? tr("Claude's proposal was made again for the current script: 1 change")
            : tr("Claude's proposal was made again for the current script: %1 changes")
                  .arg(diff->changeCount()));
}

void AiChatController::diffApplied()
{
    setDiff(nullptr, false);
    m_proposed.clear();
    emit stateChanged();
}

void AiChatController::clearChat()
{
    cancel();
    m_messages.clear();
    m_contexts.clear();
    m_history.clear();
    m_proposed.clear();
    clearError();
    setDiff(nullptr, false);
    setState(QStringLiteral("idle"));
    applyAvailability();
    emit stateChanged();
}

QString AiChatController::contextText(int rowIndex) const
{
    return m_contexts.get(rowIndex).value(QStringLiteral("text")).toString();
}

} // namespace quarry::ui
