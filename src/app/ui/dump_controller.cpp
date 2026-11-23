#include "dump_controller.h"

#include "connection_list_model.h"
#include "schema_tree_model.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

namespace quarry::ui {

namespace {

constexpr auto kDirectoryKey = "dump/directory";
constexpr auto kPgDumpKey = "dump/pgDumpPath";
constexpr auto kPgRestoreKey = "dump/pgRestorePath";
constexpr auto kPsqlKey = "dump/psqlPath";

QString lastDirectory()
{
    const QString saved = QSettings().value(QString::fromLatin1(kDirectoryKey)).toString();
    if (!saved.isEmpty() && QDir(saved).exists())
        return saved;
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return documents.isEmpty() ? QDir::homePath() : documents;
}

QString safeFileName(QString name)
{
    for (const QChar bad : QStringLiteral("\\/:*?\"<>|"))
        name.replace(bad, QLatin1Char('_'));
    name = name.trimmed();
    return name.isEmpty() ? QStringLiteral("dump") : name;
}

// The name of the database the user sees: the file of a SQLite database, the database of a server
// (or the connection's name when the profile leaves the database open).
QString shownName(const ConnectionProfile &profile, const DumpTarget &target)
{
    if (profile.providerId == QLatin1String("sqlite"))
        return QFileInfo(target.database).fileName();
    return target.database.isEmpty() ? profile.name : target.database;
}

QString engineName(const QString &providerId)
{
    if (providerId == QLatin1String("sqlite"))
        return QStringLiteral("SQLite");
    if (providerId == QLatin1String("postgres"))
        return QStringLiteral("PostgreSQL");
    return providerId;
}

} // namespace

DumpController::DumpController(QObject *parent)
    : QObject(parent)
    , m_runner(new DumpRunner(this))
    , m_adapter(new AiSchemaAdapter(this))
{
    const QSettings settings;
    m_tools.pgDump = settings.value(QString::fromLatin1(kPgDumpKey)).toString();
    m_tools.pgRestore = settings.value(QString::fromLatin1(kPgRestoreKey)).toString();
    m_tools.psql = settings.value(QString::fromLatin1(kPsqlKey)).toString();
    connectRunner();
    connect(m_adapter, &AiSchemaAdapter::contextReady, this, &DumpController::onContextReady);
    connect(m_adapter, &AiSchemaAdapter::contextFailed, this, &DumpController::onContextFailed);
}

DumpController::~DumpController() = default; // the runner (a child) cancels and waits

void DumpController::setConnections(ConnectionController *connections)
{
    if (connections == m_connections)
        return;
    if (m_connections)
        m_connections->disconnect(this);
    m_connections = connections;
    if (m_connections)
        connect(m_connections, &QObject::destroyed, this, [this] { m_connections = nullptr; });
    emit connectionsChanged();
}

std::optional<ConnectionProfile> DumpController::profileOf(const QString &connectionId) const
{
    if (m_connections == nullptr || connectionId.isEmpty())
        return std::nullopt;
    return m_connections->model()->profile(QUuid::fromString(connectionId));
}

// ---- tools -------------------------------------------------------------------------------------

void DumpController::saveTools()
{
    QSettings settings;
    settings.setValue(QString::fromLatin1(kPgDumpKey), m_tools.pgDump);
    settings.setValue(QString::fromLatin1(kPgRestoreKey), m_tools.pgRestore);
    settings.setValue(QString::fromLatin1(kPsqlKey), m_tools.psql);
    emit toolPathsChanged();
}

void DumpController::setPgDumpPath(const QString &path)
{
    const QString trimmed = path.trimmed();
    if (trimmed == m_tools.pgDump)
        return;
    m_tools.pgDump = trimmed;
    saveTools();
}

void DumpController::setPgRestorePath(const QString &path)
{
    const QString trimmed = path.trimmed();
    if (trimmed == m_tools.pgRestore)
        return;
    m_tools.pgRestore = trimmed;
    saveTools();
}

void DumpController::setPsqlPath(const QString &path)
{
    const QString trimmed = path.trimmed();
    if (trimmed == m_tools.psql)
        return;
    m_tools.psql = trimmed;
    saveTools();
}

QVariantMap DumpController::testTool(const QString &tool) const
{
    DumpTool which = DumpTool::PgDump;
    if (tool == QLatin1String("pg_restore"))
        which = DumpTool::PgRestore;
    else if (tool == QLatin1String("psql"))
        which = DumpTool::Psql;
    else if (tool != QLatin1String("pg_dump"))
        return { { QStringLiteral("ok"), false },
                 { QStringLiteral("message"), tr("Unknown tool.") },
                 { QStringLiteral("detail"), QString() } };
    const Result<QString> found = resolveDumpTool(which, m_tools);
    if (found)
        return { { QStringLiteral("ok"), true },
                 { QStringLiteral("path"), QDir::toNativeSeparators(found.value()) },
                 { QStringLiteral("message"), QDir::toNativeSeparators(found.value()) },
                 { QStringLiteral("detail"), QString() } };
    return { { QStringLiteral("ok"), false },
             { QStringLiteral("path"), QString() },
             { QStringLiteral("message"), found.error().message },
             { QStringLiteral("detail"), found.error().detail } };
}

// ---- what a connection offers ------------------------------------------------------------------

QVariantMap DumpController::infoFor(const QString &connectionId, const QString &database) const
{
    QVariantMap out {
        { QStringLiteral("valid"), false },          { QStringLiteral("name"), QString() },
        { QStringLiteral("providerId"), QString() }, { QStringLiteral("engine"), QString() },
        { QStringLiteral("database"), QString() },   { QStringLiteral("isPostgres"), false },
        { QStringLiteral("production"), false },     { QStringLiteral("connected"), false },
        { QStringLiteral("supported"), false },      { QStringLiteral("reason"), QString() },
        { QStringLiteral("canPickTables"), false }
    };
    const auto profile = profileOf(connectionId);
    if (!profile) {
        out[QStringLiteral("reason")] = tr("This connection no longer exists.");
        return out;
    }
    const DumpTarget target = dumpTargetFromProfile(*profile, {}, database);
    const bool isPostgres = profile->providerId == QLatin1String("postgres");
    out[QStringLiteral("valid")] = true;
    out[QStringLiteral("name")] = profile->name;
    out[QStringLiteral("providerId")] = profile->providerId;
    out[QStringLiteral("engine")] = engineName(profile->providerId);
    out[QStringLiteral("database")] = shownName(*profile, target);
    out[QStringLiteral("isPostgres")] = isPostgres;
    out[QStringLiteral("production")] = profile->production;
    out[QStringLiteral("connected")] = m_connections->tree()->isConnected(connectionId);
    const QString problem = checkTarget(profile, target);
    out[QStringLiteral("supported")] = problem.isEmpty();
    out[QStringLiteral("reason")] = problem;
    // The metadata session is the connection's own database: another one, or a server connection
    // without a fixed database, is dumped whole.
    out[QStringLiteral("canPickTables")]
        = problem.isEmpty() && database.isEmpty() && (!isPostgres || !target.database.isEmpty());
    return out;
}

QString DumpController::checkTarget(const std::optional<ConnectionProfile> &profile,
                                    const DumpTarget &target) const
{
    if (!profile)
        return tr("This connection no longer exists.");
    if (!dumpSupported(profile->providerId))
        return tr("Dump and restore are not available for %1 yet.")
            .arg(engineName(profile->providerId));
    if (target.sshTunnel)
        return tr("This connection goes through an SSH tunnel. The database tools cannot use "
                  "Quarry's tunnel, so dump and restore are not available for it.");
    return {};
}

bool DumpController::needsDatabaseName(const QString &connectionId) const
{
    const auto profile = profileOf(connectionId);
    return profile && profile->production;
}

bool DumpController::restoreConfirmed(const QString &connectionId, const QString &database,
                                      const QString &phrase, const QString &typedName) const
{
    const auto profile = profileOf(connectionId);
    if (!profile || phrase.trimmed() != QLatin1String(kRestorePhrase))
        return false;
    if (!profile->production)
        return true;
    const QString expected = shownName(*profile, dumpTargetFromProfile(*profile, {}, database));
    return !expected.isEmpty() && typedName.trimmed() == expected;
}

// ---- the table selector ------------------------------------------------------------------------

QString DumpController::tablesState() const
{
    return m_tablesState;
}

void DumpController::setTablesState(const QString &state, const QString &error)
{
    m_tablesState = state;
    m_tablesError = error;
    emit tablesChanged();
}

void DumpController::clearTables()
{
    if (m_tablesRequest != 0)
        m_adapter->cancel(m_tablesRequest);
    m_tablesRequest = 0;
    QObject::disconnect(m_treeLink);
    m_tables.clear();
    m_tableKeys.clear();
    m_edges.clear();
    m_tablesTruncated = false;
    m_tablesConnection.clear();
    setTablesState(QStringLiteral("idle"));
}

void DumpController::loadTables(const QString &connectionId, const QString &database)
{
    clearTables();
    m_tablesConnection = connectionId;
    const QVariantMap info = infoFor(connectionId, database);
    if (!info.value(QStringLiteral("canPickTables")).toBool()) {
        setTablesState(QStringLiteral("unavailable"));
        return;
    }
    SchemaTreeModel *tree = m_connections->tree();
    Session *session = tree->metadataSession(connectionId);
    if (session == nullptr) {
        // Opens it like expanding the connection does; never asks for a password.
        const auto access = tree->ensureSession(connectionId);
        if (access != SchemaTreeModel::Access::Opening
            && access != SchemaTreeModel::Access::Connected) {
            setTablesState(QStringLiteral("unavailable"));
            return;
        }
        session = tree->metadataSession(connectionId);
        if (session == nullptr) {
            setTablesState(QStringLiteral("loading"));
            m_treeLink = connect(tree, &SchemaTreeModel::connectedChanged, this,
                                 &DumpController::onConnectedChanged);
            return;
        }
    }
    AiSchemaAdapter::Request request;
    request.providerId = info.value(QStringLiteral("providerId")).toString();
    request.session = session;
    request.limit = kMaxTables;
    setTablesState(QStringLiteral("loading"));
    m_tablesRequest = m_adapter->contextFor(request);
}

void DumpController::onConnectedChanged()
{
    if (m_tablesState != QLatin1String("loading") || m_tablesRequest != 0
        || m_tablesConnection.isEmpty() || m_connections == nullptr)
        return;
    if (m_connections->tree()->metadataSession(m_tablesConnection) == nullptr)
        return;
    const QString id = m_tablesConnection;
    loadTables(id, {}); // the selector only exists for the connection's own database
}

void DumpController::onContextReady(quint64 id, const quarry::ai::SchemaContext &context,
                                    bool truncated)
{
    if (id != m_tablesRequest)
        return;
    m_tablesRequest = 0;
    m_tables.clear();
    m_tableKeys.clear();
    m_edges.clear();
    for (const ai::AiTable &t : context.tables) {
        if (t.isView)
            continue;
        const DumpTable key { t.schema, t.name };
        m_tableKeys.append(key);
        m_tables.append(QVariantMap {
            { QStringLiteral("schema"), t.schema },
            { QStringLiteral("name"), t.name },
            { QStringLiteral("label"), t.schema.isEmpty() ? t.name : t.schema + u'.' + t.name } });
        for (const ai::AiForeignKey &fk : t.foreignKeys)
            m_edges.append(
                { key, { fk.refSchema.isEmpty() ? t.schema : fk.refSchema, fk.refTable } });
    }
    m_tablesTruncated = truncated;
    setTablesState(QStringLiteral("ready"));
}

void DumpController::onContextFailed(quint64 id, const QString &message, const QString &detail)
{
    Q_UNUSED(detail)
    if (id != m_tablesRequest)
        return;
    m_tablesRequest = 0;
    setTablesState(QStringLiteral("failed"), message);
}

QVariantList DumpController::closureOf(const QVariantList &rows) const
{
    QList<DumpTable> selected;
    for (const QVariant &r : rows) {
        const int row = r.toInt();
        if (row >= 0 && row < m_tableKeys.size())
            selected.append(m_tableKeys.at(row));
    }
    QVariantList out;
    for (const DumpTable &t : dependencyClosure(selected, m_edges)) {
        const qsizetype row = m_tableKeys.indexOf(t);
        if (row >= 0)
            out.append(static_cast<int>(row));
    }
    return out;
}

// ---- files -------------------------------------------------------------------------------------

QString DumpController::defaultPath(const QString &connectionId, const QString &database,
                                    bool custom) const
{
    QString base = QStringLiteral("dump");
    if (const auto profile = profileOf(connectionId)) {
        const QString shown = shownName(*profile, dumpTargetFromProfile(*profile, {}, database));
        base = QFileInfo(shown).completeBaseName();
        if (base.isEmpty())
            base = shown;
    }
    return QDir(lastDirectory())
        .filePath(safeFileName(base) + QLatin1Char('.') + QLatin1String(custom ? "dump" : "sql"));
}

QString DumpController::retarget(const QString &path, bool custom) const
{
    if (path.isEmpty())
        return path;
    const QString suffix = QFileInfo(path).suffix();
    if (suffix.compare(QLatin1String("sql"), Qt::CaseInsensitive) != 0
        && suffix.compare(QLatin1String("dump"), Qt::CaseInsensitive) != 0)
        return path;
    return path.left(path.size() - suffix.size()) + QLatin1String(custom ? "dump" : "sql");
}

QUrl DumpController::folderUrl(const QString &path) const
{
    return QUrl::fromLocalFile(QFileInfo(path).absolutePath());
}

QUrl DumpController::fileUrl(const QString &path) const
{
    return QUrl::fromLocalFile(path);
}

QString DumpController::toLocalPath(const QUrl &url) const
{
    return QDir::toNativeSeparators(url.toLocalFile());
}

bool DumpController::fileExists(const QString &path) const
{
    return !path.isEmpty() && QFileInfo(path).isFile();
}

void DumpController::showInFolder() const
{
    if (!m_filePath.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_filePath).absolutePath()));
}

void DumpController::copyLog() const
{
    copyText(m_log.join(QLatin1Char('\n')));
}

void DumpController::copyText(const QString &text) const
{
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(text);
}

QString DumpController::checkFile(const QString &path, bool mustExist) const
{
    if (path.isEmpty())
        return mustExist ? tr("Choose the file to restore from.") : tr("Choose a file to write.");
    const QFileInfo file(path);
    if (file.isDir())
        return tr("That path is a folder. Type a file name.");
    if (mustExist && !file.isFile())
        return tr("The file “%1” does not exist.").arg(QDir::toNativeSeparators(path));
    if (!mustExist && !file.dir().exists())
        return tr("The folder “%1” does not exist.")
            .arg(QDir::toNativeSeparators(file.absolutePath()));
    return {};
}

// ---- running -----------------------------------------------------------------------------------

QString DumpController::state() const
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

void DumpController::setState(State state)
{
    if (state == m_state)
        return;
    m_state = state;
    emit stateChanged();
}

QString DumpController::startDump(const QVariantMap &r)
{
    if (busy())
        return tr("Another dump or restore is already running.");
    const QString connectionId = r.value(QStringLiteral("connectionId")).toString();
    const auto profile = profileOf(connectionId);
    Job job;
    job.isDump = true;
    job.connectionId = connectionId;
    if (profile)
        job.target
            = dumpTargetFromProfile(*profile, {}, r.value(QStringLiteral("database")).toString());
    if (const QString problem = checkTarget(profile, job.target); !problem.isEmpty())
        return problem;

    const QString typed = r.value(QStringLiteral("filePath")).toString().trimmed();
    if (const QString problem = checkFile(typed, false); !problem.isEmpty())
        return problem;
    DumpOptions &o = job.dump;
    o.outputPath = QDir::toNativeSeparators(QFileInfo(typed).absoluteFilePath());
    const QString content = r.value(QStringLiteral("content")).toString();
    o.content = content == QLatin1String("schema") ? DumpContent::SchemaOnly
        : content == QLatin1String("data")         ? DumpContent::DataOnly
                                                   : DumpContent::SchemaAndData;
    const bool isPostgres = profile->providerId == QLatin1String("postgres");
    o.format = isPostgres && r.value(QStringLiteral("format")).toString() == QLatin1String("custom")
        ? DumpFormat::PostgresCustom
        : DumpFormat::PlainSql;
    o.includeOwnership = isPostgres && r.value(QStringLiteral("includeOwnership")).toBool();
    if (isPostgres)
        o.schemas = r.value(QStringLiteral("schemas")).toStringList();

    const QVariantList rows = r.value(QStringLiteral("tables")).toList();
    if (!rows.isEmpty()) {
        if (m_tablesState != QLatin1String("ready") || m_tablesConnection != connectionId)
            return tr("The list of tables is not loaded. Dump the whole database or try again.");
        QList<DumpTable> selected;
        for (const QVariant &row : rows) {
            const int i = row.toInt();
            if (i < 0 || i >= m_tableKeys.size())
                return tr("A selected table is no longer in the list.");
            selected.append(m_tableKeys.at(i));
        }
        o.tables = r.value(QStringLiteral("includeReferenced")).toBool()
            ? dependencyClosure(selected, m_edges)
            : selected;
    }
    return begin(std::move(job));
}

QString DumpController::startRestore(const QVariantMap &r)
{
    if (busy())
        return tr("Another dump or restore is already running.");
    const QString connectionId = r.value(QStringLiteral("connectionId")).toString();
    const QString database = r.value(QStringLiteral("database")).toString();
    const auto profile = profileOf(connectionId);
    Job job;
    job.isDump = false;
    job.connectionId = connectionId;
    if (profile)
        job.target = dumpTargetFromProfile(*profile, {}, database);
    if (const QString problem = checkTarget(profile, job.target); !problem.isEmpty())
        return problem;
    // The dialog checks it too; this is the one that counts.
    if (!restoreConfirmed(connectionId, database, r.value(QStringLiteral("phrase")).toString(),
                          r.value(QStringLiteral("typedName")).toString()))
        return tr("The confirmation does not match. Nothing was restored.");
    const QString typed = r.value(QStringLiteral("filePath")).toString().trimmed();
    if (const QString problem = checkFile(typed, true); !problem.isEmpty())
        return problem;
    job.restore.inputPath = QDir::toNativeSeparators(QFileInfo(typed).absoluteFilePath());
    return begin(std::move(job));
}

void DumpController::resetRun(const QString &filePath)
{
    if (!filePath.isEmpty())
        QSettings().setValue(QString::fromLatin1(kDirectoryKey),
                             QFileInfo(filePath).absolutePath());
    m_error = {};
    m_done = 0;
    m_total = 0;
    m_step.clear();
    m_log.clear();
    m_resultText.clear();
    m_cancelRequested = false;
    m_filePath = filePath;
    m_runId = 0;
    emit progressChanged();
    emit logChanged();
}

QString DumpController::begin(Job job)
{
    const auto profile = profileOf(job.connectionId);
    if (!profile)
        return tr("This connection no longer exists.");
    QString password;
    const bool needsPassword = profile->providerId != QLatin1String("sqlite");
    if (needsPassword && profile->passwordMode == PasswordMode::AskEveryTime) {
        // shortcut: the dialog never asks for the password; the connection must have been used
        // (opened) once so it is in memory. Ask here when a prompt is wanted.
        const auto remembered = m_connections->sessionPassword(profile->id);
        if (!remembered)
            return tr("Connect first: open the connection in the tree, then try again. This "
                      "connection asks for its password every time and it is not known yet.");
        password = *remembered;
    }
    m_isDump = job.isDump;
    resetRun(job.isDump ? job.dump.outputPath : job.restore.inputPath);
    m_job = std::move(job);
    setState(State::Preparing);
    emit announcement(m_isDump ? tr("Preparing the dump") : tr("Preparing the restore"), false);
    if (needsPassword && profile->passwordMode == PasswordMode::Saved) {
        connect(m_connections, &ConnectionController::passwordAvailable, this,
                &DumpController::onPasswordAvailable, Qt::UniqueConnection);
        connect(m_connections, &ConnectionController::passwordUnavailable, this,
                &DumpController::onPasswordUnavailable, Qt::UniqueConnection);
        m_waitingPassword = profile->id;
        m_connections->readPassword(profile->id);
    } else {
        launch(password);
    }
    return {};
}

void DumpController::onPasswordAvailable(QUuid id, const QString &password)
{
    if (m_state != State::Preparing || m_waitingPassword.isNull() || id != m_waitingPassword)
        return;
    m_waitingPassword = QUuid();
    launch(password);
}

void DumpController::onPasswordUnavailable(QUuid id, const Error &error)
{
    if (m_state != State::Preparing || m_waitingPassword.isNull() || id != m_waitingPassword)
        return;
    m_waitingPassword = QUuid();
    m_error = error;
    finishWith(State::Failed, error.message, true);
}

void DumpController::launch(const QString &password)
{
    if (!m_job)
        return;
    DumpTarget target = m_job->target;
    target.password = password;
    setState(State::Running);
    emit announcement(m_isDump ? tr("Dump started") : tr("Restore started"), false);
    // 0: it could not start; failed(0, ...) follows, queued.
    m_runId = m_isDump ? m_runner->dump(target, m_job->dump, m_tools)
                       : m_runner->restore(target, m_job->restore, m_tools);
}

void DumpController::connectRunner()
{
    connect(m_runner, &DumpRunner::progress, this,
            [this](quint64 id, int done, int total, const QString &step) {
                if (!busy() || id != m_runId)
                    return;
                m_done = done;
                m_total = total;
                m_step = step;
                emit progressChanged();
            });
    connect(m_runner, &DumpRunner::output, this, [this](quint64 id, const QString &text, bool) {
        if (busy() && id == m_runId)
            appendLog(text);
    });
    connect(m_runner, &DumpRunner::finished, this, [this](quint64 id) {
        if (!busy() || id != m_runId)
            return;
        if (m_isDump) {
            const qint64 size = QFileInfo(m_filePath).size();
            m_resultText
                = tr("Dump written to %1 (%2)")
                      .arg(QDir::toNativeSeparators(m_filePath), QLocale().formattedDataSize(size));
        } else {
            m_resultText = tr("Restore finished. The database now holds the contents of %1.")
                               .arg(QFileInfo(m_filePath).fileName());
        }
        finishWith(State::Done, m_isDump ? tr("Dump finished") : tr("Restore finished"), false);
    });
    connect(m_runner, &DumpRunner::failed, this, [this](quint64 id, const Error &error) {
        if (!busy() || id != m_runId)
            return;
        m_error = error;
        finishWith(State::Failed, error.message, true);
    });
    connect(m_runner, &DumpRunner::cancelled, this, [this](quint64 id) {
        if (!busy() || id != m_runId)
            return;
        finishWith(State::Cancelled, m_isDump ? tr("Dump cancelled") : tr("Restore cancelled"),
                   false);
    });
}

// Lines come in bursts: one notification per event loop turn is enough for the view.
void DumpController::appendLog(const QString &line)
{
    m_log.append(line);
    if (m_log.size() > kMaxLogLines)
        m_log.remove(0, m_log.size() - kMaxLogLines);
    if (m_logPending)
        return;
    m_logPending = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_logPending = false;
            emit logChanged();
        },
        Qt::QueuedConnection);
}

void DumpController::finishWith(State state, const QString &text, bool assertive)
{
    m_job.reset();
    m_cancelRequested = false;
    setState(state);
    emit announcement(text, assertive);
}

void DumpController::cancel()
{
    if (!busy() || m_cancelRequested)
        return;
    m_cancelRequested = true;
    if (m_state == State::Preparing) {
        m_waitingPassword = QUuid();
        finishWith(State::Cancelled, m_isDump ? tr("Dump cancelled") : tr("Restore cancelled"),
                   false);
        return;
    }
    emit stateChanged(); // cancelling
    if (m_runId != 0)
        m_runner->cancel(m_runId);
    // else: it never started; failed(0, ...) arrives and ends it.
}

void DumpController::reset()
{
    if (busy())
        return;
    m_error = {};
    m_done = 0;
    m_total = 0;
    m_step.clear();
    m_log.clear();
    m_resultText.clear();
    m_filePath.clear();
    setState(State::Idle);
    emit progressChanged();
    emit logChanged();
}

} // namespace quarry::ui
