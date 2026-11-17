#include "export_controller.h"

#include "connection_controller.h"
#include "editor_tabs_model.h"
#include "query_controller.h"

#include <quarry/sql_safety.h>

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <array>

namespace quarry::ui {

namespace {

constexpr auto kDirectoryKey = "export/directory";
constexpr auto kDefaultTable = "export_table";

struct FormatInfo
{
    ExportFormat format;
    const char *name;
    const char *extension;
};

// In ExportFormat order: the dialog's list index is the enum value.
constexpr std::array<FormatInfo, 6> kFormats { { { ExportFormat::Csv, "CSV", "csv" },
                                                 { ExportFormat::Tsv, "TSV", "tsv" },
                                                 { ExportFormat::Json, "JSON", "json" },
                                                 { ExportFormat::JsonLines, "JSON Lines", "jsonl" },
                                                 { ExportFormat::SqlInsert, "SQL INSERT", "sql" },
                                                 { ExportFormat::Markdown, "Markdown", "md" } } };

const FormatInfo *formatInfo(int index)
{
    return index >= 0 && index < static_cast<int>(kFormats.size())
        ? &kFormats[static_cast<size_t>(index)]
        : nullptr;
}

// Just enough SQL to find words and parenthesis depth, skipping comments, strings, quoted
// identifiers and dollar-quoted bodies (the same idea as core's SqlLexer, which is private to
// core).
class Scanner
{
public:
    enum class Kind { End, Word, Open, Close, Other };

    struct Token
    {
        Kind kind = Kind::End;
        QString word; // lower case, for Word
    };

    explicit Scanner(QStringView text)
        : m_text(text)
    { }

    Token next()
    {
        skipSpace();
        if (m_pos >= m_text.size())
            return {};
        const QChar c = m_text.at(m_pos);
        if (c == QLatin1Char('(')) {
            ++m_pos;
            return { Kind::Open, {} };
        }
        if (c == QLatin1Char(')')) {
            ++m_pos;
            return { Kind::Close, {} };
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"') || c == QLatin1Char('`')) {
            skipQuoted(c);
            return { Kind::Other, {} };
        }
        if (c == QLatin1Char('[')) {
            const qsizetype end = m_text.indexOf(QLatin1Char(']'), m_pos);
            m_pos = end < 0 ? m_text.size() : end + 1;
            return { Kind::Other, {} };
        }
        if (c == QLatin1Char('$')) {
            const qsizetype close = dollarEnd();
            if (close >= 0) {
                m_pos = close;
                return { Kind::Other, {} };
            }
        }
        if (c.isLetter() || c == QLatin1Char('_')) {
            const qsizetype start = m_pos;
            while (m_pos < m_text.size()
                   && (m_text.at(m_pos).isLetterOrNumber() || m_text.at(m_pos) == QLatin1Char('_')
                       || m_text.at(m_pos) == QLatin1Char('$')))
                ++m_pos;
            return { Kind::Word, m_text.mid(start, m_pos - start).toString().toLower() };
        }
        ++m_pos;
        return { Kind::Other, {} };
    }

private:
    void skipSpace()
    {
        while (m_pos < m_text.size()) {
            const QChar c = m_text.at(m_pos);
            if (c.isSpace()) {
                ++m_pos;
            } else if (c == QLatin1Char('-') && m_text.mid(m_pos, 2) == QLatin1String("--")) {
                const qsizetype end = m_text.indexOf(QLatin1Char('\n'), m_pos);
                m_pos = end < 0 ? m_text.size() : end + 1;
            } else if (c == QLatin1Char('/') && m_text.mid(m_pos, 2) == QLatin1String("/*")) {
                const qsizetype end = m_text.indexOf(QLatin1String("*/"), m_pos + 2);
                m_pos = end < 0 ? m_text.size() : end + 2;
            } else {
                break;
            }
        }
    }

    // A doubled quote character is an escaped one, so it just reopens the string.
    void skipQuoted(QChar quote)
    {
        ++m_pos;
        while (m_pos < m_text.size()) {
            if (m_text.at(m_pos) == quote) {
                ++m_pos;
                if (m_pos >= m_text.size() || m_text.at(m_pos) != quote)
                    return;
            }
            ++m_pos;
        }
    }

    // $tag$ ... $tag$: the index after the closing tag, -1 when this is not a dollar quote.
    qsizetype dollarEnd() const
    {
        qsizetype i = m_pos + 1;
        while (i < m_text.size()
               && (m_text.at(i).isLetterOrNumber() || m_text.at(i) == QLatin1Char('_')))
            ++i;
        if (i >= m_text.size() || m_text.at(i) != QLatin1Char('$'))
            return -1;
        const QStringView tag = m_text.mid(m_pos, i - m_pos + 1);
        const qsizetype close = m_text.indexOf(tag, i + 1);
        return close < 0 ? m_text.size() : close + tag.size();
    }

    QStringView m_text;
    qsizetype m_pos = 0;
};

bool isWriteWord(const QString &w)
{
    return w == QLatin1String("insert") || w == QLatin1String("update")
        || w == QLatin1String("delete") || w == QLatin1String("merge");
}

QString safeFileName(QString name)
{
    for (const QChar bad : QStringLiteral("\\/:*?\"<>|"))
        name.replace(bad, QLatin1Char('_'));
    name = name.trimmed();
    return name.isEmpty() ? QStringLiteral("export") : name;
}

QString lastDirectory()
{
    QString saved = QSettings().value(QString::fromLatin1(kDirectoryKey)).toString();
    if (!saved.isEmpty() && QDir(saved).exists())
        return saved;
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return documents.isEmpty() ? QDir::homePath() : documents;
}

} // namespace

ExportController::ExportController(QObject *parent)
    : QObject(parent)
{ }

ExportController::~ExportController()
{
    // The Exporter (a child) cancels itself and waits for its thread; the session it holds goes
    // with it.
    m_session.reset();
}

// ---- wiring ------------------------------------------------------------------------------------

void ExportController::setQueries(QueryController *queries)
{
    if (queries == m_queries)
        return;
    if (m_queries)
        m_queries->disconnect(this);
    m_queries = queries;
    if (m_queries) {
        connect(m_queries, &QueryController::tabsChanged, this, &ExportController::bindTabs);
        connect(m_queries, &QueryController::currentChanged, this, [this] {
            bindCurrent();
            touch();
        });
        connect(m_queries, &QObject::destroyed, this, [this] {
            m_queries = nullptr;
            m_tabs = nullptr;
        });
    }
    bindTabs();
    emit queriesChanged();
}

void ExportController::bindTabs()
{
    EditorTabsModel *tabs = m_queries ? m_queries->tabs() : nullptr;
    if (tabs == m_tabs)
        return;
    for (const QMetaObject::Connection &link : std::as_const(m_tabLinks))
        disconnect(link);
    m_tabLinks.clear();
    m_tabs = tabs;
    if (m_tabs) {
        m_tabLinks
            = { connect(m_tabs, &EditorTabsModel::currentIndexChanged, this,
                        &ExportController::touch),
                connect(m_tabs, &QAbstractItemModel::dataChanged, this, &ExportController::touch),
                connect(m_tabs, &EditorTabsModel::countChanged, this, &ExportController::touch),
                connect(m_tabs, &QObject::destroyed, this, [this] { m_tabs = nullptr; }) };
    }
    bindCurrent();
    touch();
}

// What the current tab can export depends on its state and on the rows in its grid.
void ExportController::bindCurrent()
{
    for (const QMetaObject::Connection &link : std::as_const(m_stateLinks))
        disconnect(link);
    m_stateLinks.clear();
    QueryState *state = m_queries ? m_queries->current() : nullptr;
    if (state == nullptr)
        return;
    ResultTableModel *results = state->results();
    m_stateLinks
        = { connect(state, &QueryState::changed, this, &ExportController::touch),
            connect(results, &ResultTableModel::columnsChanged, this, &ExportController::touch),
            connect(results, &ResultTableModel::stateChanged, this, &ExportController::touch),
            connect(results, &ResultTableModel::rowsChanged, this, &ExportController::touch) };
}

void ExportController::touch()
{
    emit infoChanged();
}

ConnectionController *ExportController::connections() const
{
    return m_tabs ? m_tabs->connections() : nullptr;
}

const ISqlDialect *ExportController::dialectOf(const QString &connectionId) const
{
    ConnectionController *conns = connections();
    if (conns == nullptr || connectionId.isEmpty())
        return nullptr;
    const auto profile = conns->model()->profile(QUuid::fromString(connectionId));
    if (!profile)
        return nullptr;
    // shortcut: loads the engine's plugin on the UI thread (a few ms, once per engine), as
    // QueryController does.
    IDatabaseProvider *provider = conns->registry().provider(profile->providerId, nullptr);
    return provider ? &provider->dialect() : nullptr;
}

// ---- what can be exported ----------------------------------------------------------------------

bool ExportController::returnsRows(QStringView sql)
{
    Scanner scanner(sql);
    Scanner::Token t = scanner.next();
    while (t.kind == Scanner::Kind::Open) // (SELECT ...) UNION ...
        t = scanner.next();
    if (t.kind != Scanner::Kind::Word)
        return false;
    const QString first = t.word;
    if (first == QLatin1String("show"))
        return true;
    bool select = first == QLatin1String("select") || first == QLatin1String("table");
    const bool explain = first == QLatin1String("explain");
    const bool with = first == QLatin1String("with");
    if (!select && !explain && !with)
        return false;
    int depth = 0;
    bool mainFound = select; // WITH: the command after the CTE list
    for (t = scanner.next(); t.kind != Scanner::Kind::End; t = scanner.next()) {
        if (t.kind == Scanner::Kind::Open) {
            ++depth;
        } else if (t.kind == Scanner::Kind::Close) {
            depth = std::max(0, depth - 1);
        } else if (t.kind == Scanner::Kind::Word) {
            if (t.word == QLatin1String("into")) // SELECT ... INTO creates a table
                return false;
            if (explain) {
                // EXPLAIN ANALYZE runs what it explains.
                if (isWriteWord(t.word) || t.word == QLatin1String("create")
                    || t.word == QLatin1String("drop") || t.word == QLatin1String("alter")
                    || t.word == QLatin1String("truncate"))
                    return false;
            } else if (with) {
                // A data-modifying CTE runs even under a SELECT.
                if (isWriteWord(t.word) && depth > 0)
                    return false;
                if (!mainFound && depth == 0
                    && (isWriteWord(t.word) || t.word == QLatin1String("select")
                        || t.word == QLatin1String("table") || t.word == QLatin1String("values"))) {
                    mainFound = true;
                    select = t.word == QLatin1String("select") || t.word == QLatin1String("table");
                    if (!select)
                        return false;
                }
            }
        }
    }
    return explain || (select && mainFound);
}

int ExportController::rowOf(const QString &tabId) const
{
    if (m_tabs == nullptr || tabId.isEmpty())
        return -1;
    for (int i = 0; i < m_tabs->count(); ++i)
        if (m_tabs->idOf(i) == tabId)
            return i;
    return -1;
}

QString ExportController::statementOf(const QString &tabId) const
{
    const int row = rowOf(tabId);
    if (row < 0 || m_queries == nullptr)
        return {};
    if (const auto object = m_tabs->tableOf(row)) {
        const QString connectionId = m_tabs->connectionIdOf(row);
        const ISqlDialect *dialect = dialectOf(connectionId);
        if (dialect == nullptr)
            return {};
        // The whole table, whatever the grid holds: no page limit, no order.
        return QStringLiteral("SELECT * FROM ") + dialect->qualifiedName(*object);
    }
    return m_queries->resultSqlOf(tabId).trimmed();
}

QVariantMap ExportController::infoFor(const QString &tabId)
{
    QVariantMap out { { QStringLiteral("available"), false },
                      { QStringLiteral("currentAllowed"), false },
                      { QStringLiteral("queryAllowed"), false },
                      { QStringLiteral("loadedRows"), 0 },
                      { QStringLiteral("moreAvailable"), false },
                      { QStringLiteral("pendingChanges"), 0 },
                      { QStringLiteral("tabTitle"), QString() },
                      { QStringLiteral("connectionName"), QString() },
                      { QStringLiteral("defaultTable"), QString::fromLatin1(kDefaultTable) },
                      { QStringLiteral("baseName"), QStringLiteral("export") },
                      { QStringLiteral("currentReason"), QString() },
                      { QStringLiteral("queryReason"), QString() },
                      { QStringLiteral("reason"), QString() } };
    const int row = rowOf(tabId);
    QueryState *state = row >= 0 && m_queries != nullptr ? m_queries->stateOf(tabId) : nullptr;
    if (state == nullptr || m_tabs == nullptr) {
        out[QStringLiteral("reason")] = tr("Open a query tab to export its result.");
        return out;
    }
    const bool isData = m_tabs->isData(row);
    const auto object = m_tabs->tableOf(row);
    const ResultTableModel *results = state->results();
    const QString title = m_tabs->titleOf(row);
    const QString connectionId = m_tabs->connectionIdOf(row);
    out[QStringLiteral("tabTitle")] = title;
    out[QStringLiteral("loadedRows")] = static_cast<qlonglong>(results->exportRows());
    out[QStringLiteral("moreAvailable")] = results->moreAvailable() || results->truncated();
    out[QStringLiteral("pendingChanges")] = isData ? state->edits()->pendingCount() : 0;
    if (object && !object->parts.isEmpty()) {
        out[QStringLiteral("defaultTable")] = object->parts.last();
        out[QStringLiteral("baseName")] = safeFileName(object->parts.last());
    } else {
        out[QStringLiteral("baseName")] = safeFileName(title);
    }
    if (ConnectionController *conns = connections(); conns != nullptr && !connectionId.isEmpty()) {
        if (const auto profile = conns->model()->profile(QUuid::fromString(connectionId)))
            out[QStringLiteral("connectionName")] = profile->name;
    }

    QString currentReason;
    if (state->running())
        currentReason = tr("The query is still running. Export when it ends.");
    else if (results->columns() <= 0)
        currentReason = isData ? tr("The table has not been loaded yet.")
                               : tr("There is no result yet. Run a query that returns rows first.");
    out[QStringLiteral("currentAllowed")] = currentReason.isEmpty();
    out[QStringLiteral("currentReason")] = currentReason;

    QString queryReason;
    if (state->running()) {
        queryReason = tr("The query is still running. Export when it ends.");
    } else if (connectionId.isEmpty()) {
        queryReason = tr("Choose a connection for this tab first.");
    } else if (isData && !object) {
        queryReason = tr("The table of this tab is unknown.");
    } else {
        const QString sql = statementOf(tabId);
        if (sql.isEmpty())
            queryReason = isData ? tr("The database engine could not be loaded.")
                                 : tr("Run a query first: there is nothing to run again.");
        else if (!returnsRows(sql))
            queryReason = tr("Running again is only possible for statements that return rows "
                             "(SELECT, WITH, TABLE, SHOW, EXPLAIN). Export the current result "
                             "instead.");
    }
    out[QStringLiteral("queryAllowed")] = queryReason.isEmpty();
    out[QStringLiteral("queryReason")] = queryReason;

    const bool available = currentReason.isEmpty() || queryReason.isEmpty();
    out[QStringLiteral("available")] = available;
    out[QStringLiteral("reason")] = available ? QString() : currentReason;
    return out;
}

QVariantMap ExportController::info()
{
    if (m_tabs == nullptr)
        return infoFor({});
    return infoFor(m_tabs->idOf(m_tabs->currentIndex()));
}

// ---- files -------------------------------------------------------------------------------------

QVariantList ExportController::formats() const
{
    QVariantList out;
    out.reserve(static_cast<qsizetype>(kFormats.size()));
    for (const FormatInfo &f : kFormats) {
        out.append(QVariantMap { { QStringLiteral("id"), static_cast<int>(f.format) },
                                 { QStringLiteral("name"), QString::fromLatin1(f.name) },
                                 { QStringLiteral("extension"), QString::fromLatin1(f.extension) },
                                 { QStringLiteral("filter"),
                                   QStringLiteral("%1 (*.%2)")
                                       .arg(QLatin1String(f.name), QLatin1String(f.extension)) } });
    }
    return out;
}

QString ExportController::defaultPath(const QString &tabId, int format) const
{
    const FormatInfo *f = formatInfo(format);
    const QString base = const_cast<ExportController *>(this)
                             ->infoFor(tabId)
                             .value(QStringLiteral("baseName"))
                             .toString();
    return QDir(lastDirectory())
        .filePath(safeFileName(base) + QLatin1Char('.')
                  + QLatin1String(f != nullptr ? f->extension : "csv"));
}

QString ExportController::retarget(const QString &path, int format) const
{
    const FormatInfo *f = formatInfo(format);
    if (f == nullptr || path.isEmpty())
        return path;
    const QFileInfo info(path);
    const QString suffix = info.suffix();
    const bool ours = std::ranges::any_of(kFormats, [&](const FormatInfo &k) {
        return suffix.compare(QLatin1String(k.extension), Qt::CaseInsensitive) == 0;
    });
    if (!ours)
        return path;
    return path.left(path.size() - suffix.size()) + QLatin1String(f->extension);
}

QString ExportController::directoryOf(const QString &path) const
{
    return QFileInfo(path).absolutePath();
}

QUrl ExportController::folderUrl(const QString &path) const
{
    return QUrl::fromLocalFile(QFileInfo(path).absolutePath());
}

QUrl ExportController::fileUrl(const QString &path) const
{
    return QUrl::fromLocalFile(path);
}

QString ExportController::toLocalPath(const QUrl &url) const
{
    return QDir::toNativeSeparators(url.toLocalFile());
}

bool ExportController::fileExists(const QString &path) const
{
    return !path.isEmpty() && QFileInfo(path).isFile();
}

void ExportController::showInFolder() const
{
    if (!m_filePath.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_filePath).absolutePath()));
}

// ---- running -----------------------------------------------------------------------------------

QString ExportController::state() const
{
    switch (m_state) {
    case State::Idle:
        break;
    case State::Preparing:
        return QStringLiteral("preparing");
    case State::Running:
        return QStringLiteral("running");
    case State::Done:
        return QStringLiteral("done");
    case State::Failed:
        return QStringLiteral("failed");
    case State::Cancelled:
        return QStringLiteral("cancelled");
    }
    return QStringLiteral("idle");
}

QString ExportController::progressText() const
{
    return m_rows == 1 ? tr("1 row written")
                       : tr("%1 rows written").arg(QLocale().toString(m_rows));
}

void ExportController::setState(State state)
{
    if (state == m_state)
        return;
    m_state = state;
    emit stateChanged();
}

QString ExportController::start(const QVariantMap &r)
{
    if (busy())
        return tr("An export is already running.");
    const QString tabId = r.value(QStringLiteral("tabId")).toString();
    const bool streaming = r.value(QStringLiteral("scope")).toString() == QLatin1String("query");
    const FormatInfo *format = formatInfo(r.value(QStringLiteral("format")).toInt());
    if (format == nullptr)
        return tr("Choose a format.");
    const QVariantMap info = infoFor(tabId);
    if (streaming && !info.value(QStringLiteral("queryAllowed")).toBool())
        return info.value(QStringLiteral("queryReason")).toString();
    if (!streaming && !info.value(QStringLiteral("currentAllowed")).toBool())
        return info.value(QStringLiteral("currentReason")).toString();

    const QString typed = r.value(QStringLiteral("filePath")).toString().trimmed();
    if (typed.isEmpty())
        return tr("Choose a file to write.");
    const QFileInfo file(typed);
    if (file.isDir())
        return tr("That path is a folder. Type a file name.");
    if (!file.dir().exists())
        return tr("The folder “%1” does not exist.")
            .arg(QDir::toNativeSeparators(file.absolutePath()));

    Request request;
    request.tabId = tabId;
    request.streaming = streaming;
    request.connectionId = m_tabs->connectionIdOf(rowOf(tabId));
    ExportOptions &o = request.options;
    o.format = format->format;
    o.filePath = QDir::toNativeSeparators(file.absoluteFilePath());
    const QString delimiter = r.value(QStringLiteral("delimiter")).toString();
    o.delimiter = delimiter.isEmpty() ? QChar(QLatin1Char(',')) : delimiter.at(0);
    o.header = r.value(QStringLiteral("header"), true).toBool();
    o.bom = r.value(QStringLiteral("bom")).toBool();
    const QString table = r.value(QStringLiteral("tableName")).toString().trimmed();
    o.tableName = table.isEmpty() ? info.value(QStringLiteral("defaultTable")).toString() : table;
    o.rowsPerInsert = std::clamp(r.value(QStringLiteral("rowsPerInsert"), 1).toInt(), 1, 1000);
    o.dialect = dialectOf(request.connectionId);

    if (streaming) {
        const QString sql = statementOf(tabId);
        const bool production = [&] {
            ConnectionController *conns = connections();
            const auto profile = conns != nullptr
                ? conns->model()->profile(QUuid::fromString(request.connectionId))
                : std::nullopt;
            return profile && profile->production;
        }();
        // Never true for what returnsRows() accepts; kept so a change there cannot skip SqlSafety.
        if (SqlSafety::check(sql, production) != SqlRisk::None)
            return tr("This statement needs a confirmation, so it cannot run again for an export.");
        request.statement.sql = sql;
    }

    if (!streaming) {
        resetRun(o.filePath);
        QueryState *state = m_queries->stateOf(tabId);
        m_request = std::move(request);
        m_exporter = new Exporter(m_request->options, this);
        connectExporter();
        setState(State::Running);
        emit announcement(tr("Exporting to %1").arg(QFileInfo(m_filePath).fileName()), false);
        const ResultTableModel *results = state->results();
        m_exporter->startBatches(results->exportColumns(), results->exportBatches());
        return {};
    }
    return beginPreparing(std::move(request));
}

void ExportController::resetRun(const QString &filePath)
{
    QSettings().setValue(QString::fromLatin1(kDirectoryKey), QFileInfo(filePath).absolutePath());
    m_error = {};
    m_rows = 0;
    m_bytes = 0;
    m_resultText.clear();
    m_cancelRequested = false;
    m_filePath = filePath;
    emit progressChanged();
}

// Streaming: needs a session of its own, which needs the password; the keychain read is the only
// asynchronous step before the session opens.
QString ExportController::beginPreparing(Request request)
{
    ConnectionController *conns = connections();
    const QUuid id = QUuid::fromString(request.connectionId);
    const auto profile = conns != nullptr ? conns->model()->profile(id) : std::nullopt;
    if (!profile)
        return tr("This connection no longer exists.");
    QString password;
    if (profile->passwordMode == PasswordMode::AskEveryTime) {
        const auto remembered = conns->sessionPassword(profile->id);
        if (!remembered)
            return tr("Connect first: run a query on this tab, then export again. This connection "
                      "asks for its password every time and it is not known yet.");
        password = remembered.value_or(QString());
    }
    resetRun(request.options.filePath);
    m_request = std::move(request);
    setState(State::Preparing);
    emit announcement(tr("Preparing the export"), false);
    if (profile->passwordMode == PasswordMode::Saved) {
        connect(conns, &ConnectionController::passwordAvailable, this,
                &ExportController::onPasswordAvailable, Qt::UniqueConnection);
        connect(conns, &ConnectionController::passwordUnavailable, this,
                &ExportController::onPasswordUnavailable, Qt::UniqueConnection);
        m_waitingPassword = profile->id;
        conns->readPassword(profile->id);
    } else {
        openSession(password);
    }
    return {};
}

void ExportController::onPasswordAvailable(QUuid id, const QString &password)
{
    if (m_state != State::Preparing || m_waitingPassword.isNull() || id != m_waitingPassword)
        return;
    m_waitingPassword = QUuid();
    openSession(password);
}

void ExportController::onPasswordUnavailable(QUuid id, const Error &error)
{
    if (m_state != State::Preparing || m_waitingPassword.isNull() || id != m_waitingPassword)
        return;
    m_waitingPassword = QUuid();
    m_error = error;
    finishWith(State::Failed, error.message, true);
}

void ExportController::openSession(const QString &password)
{
    ConnectionController *conns = connections();
    const auto profile = conns != nullptr && m_request
        ? conns->model()->profile(QUuid::fromString(m_request->connectionId))
        : std::nullopt;
    if (!profile) {
        m_error
            = Error { ErrorKind::NotFound, tr("This connection no longer exists."), {}, {}, {} };
        finishWith(State::Failed, m_error.message, true);
        return;
    }
    if (ConnectionPreflight::usesSsh(*profile)) {
        // Never prompts here (the export has no dialog): a tunnel secret or host key that is not
        // known yet ends in an error that says where to confirm it.
        connect(conns->preflight(), &ConnectionPreflight::ready, this,
                &ExportController::onPreflightReady, Qt::UniqueConnection);
        connect(conns->preflight(), &ConnectionPreflight::failed, this,
                &ExportController::onPreflightFailed, Qt::UniqueConnection);
        m_preflight = conns->preflight()->begin(*profile, password, false);
        return;
    }
    openWith(*profile, Secrets { password });
}

void ExportController::onPreflightReady(int request, const Secrets &secrets)
{
    ConnectionController *conns = connections();
    if (m_preflight == 0 || request != m_preflight || m_state != State::Preparing
        || conns == nullptr || !m_request)
        return;
    m_preflight = 0;
    const auto profile = conns->model()->profile(QUuid::fromString(m_request->connectionId));
    if (!profile) {
        m_error
            = Error { ErrorKind::NotFound, tr("This connection no longer exists."), {}, {}, {} };
        finishWith(State::Failed, m_error.message, true);
        return;
    }
    openWith(*profile, secrets);
}

void ExportController::onPreflightFailed(int request, const Error &error)
{
    if (m_preflight == 0 || request != m_preflight || m_state != State::Preparing)
        return;
    m_preflight = 0;
    m_error = error;
    finishWith(State::Failed, error.message, true);
}

void ExportController::openWith(const ConnectionProfile &profile, const Secrets &secrets)
{
    ConnectionController *conns = connections();
    if (conns == nullptr)
        return;
    // shortcut: loads the provider plugin on the UI thread (a few ms, once per engine), like
    // QueryController::proceed().
    auto connection = conns->registry().createConnection(profile, secrets);
    if (!connection) {
        m_error = connection.error();
        finishWith(State::Failed, m_error.message, true);
        return;
    }
    // No parent: the Exporter takes the session over and closes it.
    m_session = std::make_unique<Session>(std::move(connection).value());
    connect(m_session.get(), &Session::opened, this, &ExportController::onSessionOpened);
    connect(m_session.get(), &Session::failed, this, &ExportController::onSessionFailed);
    m_session->open();
}

void ExportController::onSessionOpened()
{
    if (m_state != State::Preparing || !m_session || !m_request)
        return;
    if (ConnectionController *conns = connections())
        conns->preflight()->openSucceeded(QUuid::fromString(m_request->connectionId));
    m_session->disconnect(this);
    m_exporter = new Exporter(m_request->options, this);
    connectExporter();
    setState(State::Running);
    emit announcement(tr("Exporting to %1").arg(QFileInfo(m_filePath).fileName()), false);
    m_exporter->start(std::move(m_session), m_request->statement);
}

void ExportController::onSessionFailed(const Error &error)
{
    if (m_state != State::Preparing)
        return;
    Error shown = error;
    if (ConnectionController *conns = connections(); conns != nullptr && m_request) {
        const auto profile = conns->model()->profile(QUuid::fromString(m_request->connectionId));
        const bool sshAuth = profile && ConnectionPreflight::isSshAuthError(*profile, error);
        if (profile)
            shown = conns->preflight()->openFailed(*profile, error);
        // A wrong password must not be tried again silently.
        if (error.kind == ErrorKind::Authentication && !sshAuth)
            conns->forgetPassword(QUuid::fromString(m_request->connectionId));
    }
    dropSession();
    m_error = shown;
    finishWith(State::Failed, shown.message, true);
}

void ExportController::dropSession()
{
    if (!m_session)
        return;
    Session *s = m_session.release();
    s->disconnect(this);
    // The session ends on its own thread; it is deleted once it says so.
    connect(s, &Session::closed, s, &QObject::deleteLater);
    s->close();
}

void ExportController::connectExporter()
{
    connect(m_exporter, &Exporter::progress, this, [this](qint64 rows) {
        m_rows = rows;
        emit progressChanged();
    });
    connect(m_exporter, &Exporter::finished, this, [this](const ExportStats &stats) {
        m_rows = stats.rows;
        m_bytes = stats.bytes;
        m_filePath = stats.filePath.isEmpty() ? m_filePath : stats.filePath;
        const QString size = QLocale().formattedDataSize(stats.bytes);
        const QString rows = QLocale().toString(stats.rows);
        m_resultText = stats.rows == 1
            ? tr("Exported 1 row (%1) to %2").arg(size, QDir::toNativeSeparators(m_filePath))
            : tr("Exported %1 rows (%2) to %3")
                  .arg(rows, size, QDir::toNativeSeparators(m_filePath));
        emit progressChanged();
        finishWith(State::Done, m_resultText, false);
    });
    connect(m_exporter, &Exporter::failed, this, [this](const Error &error) {
        m_error = error;
        finishWith(State::Failed, error.message, true);
    });
    connect(m_exporter, &Exporter::cancelled, this,
            [this] { finishWith(State::Cancelled, tr("Export cancelled"), false); });
}

void ExportController::releaseExporter()
{
    if (m_exporter == nullptr)
        return;
    m_exporter->disconnect(this);
    // It is inside one of its own signals: it goes at the end of this event.
    m_exporter->deleteLater();
    m_exporter = nullptr;
}

void ExportController::finishWith(State state, const QString &text, bool assertive)
{
    releaseExporter();
    m_request.reset();
    m_cancelRequested = false;
    setState(state);
    emit announcement(text, assertive);
}

void ExportController::cancel()
{
    if (!busy() || m_cancelRequested)
        return;
    m_cancelRequested = true;
    if (m_state == State::Preparing) {
        m_waitingPassword = QUuid();
        if (m_preflight != 0) {
            if (ConnectionController *conns = connections())
                conns->preflight()->cancel(m_preflight);
            m_preflight = 0;
        }
        dropSession();
        finishWith(State::Cancelled, tr("Export cancelled"), false);
        return;
    }
    emit stateChanged(); // cancelling
    if (m_exporter != nullptr)
        m_exporter->cancel();
}

void ExportController::reset()
{
    if (busy())
        return;
    m_error = {};
    m_rows = 0;
    m_bytes = 0;
    m_resultText.clear();
    m_filePath.clear();
    setState(State::Idle);
    emit progressChanged();
}

} // namespace quarry::ui
