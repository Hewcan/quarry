#include "migration_controller.h"

#include <quarry/connection_store.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace quarry::ui {

namespace {

constexpr QChar kScopeSeparator = QLatin1Char('\x1f');

MigrationSettings makeSettings(const QString &connectionId, const QString &database,
                               const QString &kind, const QString &folder, const QString &table,
                               const QString &phpPath = {}, const QString &projectRoot = {})
{
    MigrationSettings s;
    s.connectionId = QUuid::fromString(connectionId);
    s.database = database.trimmed();
    s.kind = kind;
    s.folder = QDir::fromNativeSeparators(folder.trimmed());
    s.table = table.trimmed();
    s.phpPath = QDir::fromNativeSeparators(phpPath.trimmed());
    s.projectRoot = QDir::fromNativeSeparators(projectRoot.trimmed());
    return s;
}

// The messages of core's MigrationRunner are translated there with the context
// "quarry::MigrationRunner" through a helper that lupdate cannot follow, so they are listed here
// for it to put them in quarry_es.ts. Keep in step with src/core/src/migration_runner.cpp.
// NOLINTBEGIN(bugprone-suspicious-missing-comma): long messages continue on the next line
[[maybe_unused]] const char *const kRunnerMessages[] = {
    QT_TRANSLATE_NOOP("quarry::MigrationRunner",
                      "PHP was not found. Set its path in the connection's migrations settings."),
    QT_TRANSLATE_NOOP("quarry::MigrationRunner",
                      "Choose the migrations folder of the project first."),
    QT_TRANSLATE_NOOP("quarry::MigrationRunner",
                      "The project folder has no artisan file. Set the project root in the "
                      "connection's migrations settings."),
    QT_TRANSLATE_NOOP("quarry::MigrationRunner", "That is not a valid migration name."),
    QT_TRANSLATE_NOOP("quarry::MigrationRunner", "The migration took too long and was stopped."),
    QT_TRANSLATE_NOOP("quarry::MigrationRunner", "Time limit: %1 s"),
    QT_TRANSLATE_NOOP("quarry::MigrationRunner", "[output truncated]"),
    QT_TRANSLATE_NOOP("quarry::MigrationRunner", "PHP could not be started."),
    QT_TRANSLATE_NOOP("quarry::MigrationRunner",
                      "The database artisan uses could not be verified."),
    QT_TRANSLATE_NOOP("quarry::MigrationRunner",
                      "An ODBC connection cannot be matched with the project's database."),
    QT_TRANSLATE_NOOP("quarry::MigrationRunner",
                      "The project's database driver '%1' is not supported."),
    QT_TRANSLATE_NOOP("quarry::MigrationRunner",
                      "artisan uses the '%1' driver but this connection is of type '%2'."),
    QT_TRANSLATE_NOOP("quarry::MigrationRunner",
                      "artisan uses the SQLite file '%1' but this connection uses '%2'."),
    QT_TRANSLATE_NOOP(
        "quarry::MigrationRunner",
        "artisan uses database '%1' on host '%2' but this connection uses '%3' on '%4'."),
    QT_TRANSLATE_NOOP("quarry::MigrationRunner",
                      "artisan uses port %1 on host '%2' but this connection uses port %3."),
};
// NOLINTEND(bugprone-suspicious-missing-comma)

// A hard cap for what a Messages entry or the error detail carries of a process's text.
constexpr int kMaxDetailChars = 2000;

std::optional<MigrationAction> parseAction(const QString &name)
{
    if (name == QLatin1String("run"))
        return MigrationAction::Run;
    if (name == QLatin1String("rollback"))
        return MigrationAction::Rollback;
    if (name == QLatin1String("dryRun"))
        return MigrationAction::DryRun;
    return std::nullopt;
}

QString actionText(MigrationAction action)
{
    switch (action) {
    case MigrationAction::Run:
        return QStringLiteral("run");
    case MigrationAction::Rollback:
        return QStringLiteral("rollback");
    case MigrationAction::DryRun:
        break;
    }
    return QStringLiteral("dryRun");
}

} // namespace

MigrationController::MigrationController(QObject *parent)
    : QObject(parent)
{
    connect(&m_runner, &MigrationRunner::targetResolved, this,
            &MigrationController::onTargetResolved);
    connect(&m_runner, &MigrationRunner::output, this, &MigrationController::onRunnerOutput);
    connect(&m_runner, &MigrationRunner::finished, this, &MigrationController::onRunnerFinished);
    connect(&m_runner, &MigrationRunner::cancelled, this, &MigrationController::onRunnerCancelled);
    connect(&m_runner, &MigrationRunner::failed, this, &MigrationController::onRunnerFailed);
}

// NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDelete): QPointer false positive
MigrationController::~MigrationController() = default;

void MigrationController::setConnections(ConnectionController *connections)
{
    if (m_connections == connections)
        return;
    if (m_connections)
        m_connections->tree()->disconnect(this);
    m_connections = connections;
    if (m_connections)
        connect(m_connections->tree(), &SchemaTreeModel::connectedChanged, this,
                &MigrationController::onConnected);
    emit connectionsChanged();
}

MigrationService *MigrationController::service()
{
    if (!m_service) {
        const QString path = m_connections ? m_connections->databasePath() : QString();
        m_service = std::make_unique<MigrationService>(
            path.isEmpty() ? quarry::ConnectionStore::defaultDatabasePath() : path);
        connect(m_service.get(), &MigrationService::settingsLoaded, this,
                [this](const QList<MigrationSettings> &list) {
                    const QStringList before = m_settings.keys();
                    m_settings.clear();
                    for (const MigrationSettings &s : list)
                        m_settings.insert(
                            scopeKey(s.connectionId.toString(QUuid::WithoutBraces), s.database), s);
                    for (const QString &scope : m_settings.keys() + before) // the tree grows (or
                        pushFeed(connectionOfScope(scope),
                                 databaseOfScope(scope)); // drops) folders
                    emit settingsChanged();
                    m_connected.clear(); // inspect whatever is already connected
                    onConnected();
                });
        connect(m_service.get(), &MigrationService::folderChecked, this,
                [this](quint64 id, int files) {
                    if (id != m_folderCheck)
                        return;
                    m_folderState = QStringLiteral("ok");
                    m_folderMessage = files == 0
                        ? tr("The folder can be read, but it has no migration files yet.")
                        : tr("Found %n migration file(s).", nullptr, files);
                    emit folderChanged();
                });
        connect(m_service.get(), &MigrationService::folderCheckFailed, this,
                [this](quint64 id, const Error &e) {
                    if (id != m_folderCheck)
                        return;
                    m_folderState = QStringLiteral("failed");
                    m_folderMessage = e.message;
                    emit folderChanged();
                });
        connect(m_service.get(), &MigrationService::inspected, this,
                &MigrationController::onInspected);
        connect(m_service.get(), &MigrationService::inspectionFailed, this,
                &MigrationController::onFailed);
        connect(m_service.get(), &MigrationService::sourceRead, this,
                &MigrationController::onSourceRead);
        connect(m_service.get(), &MigrationService::sourceFailed, this,
                &MigrationController::onSourceFailed);
    }
    return m_service.get();
}

void MigrationController::start()
{
    if (m_started)
        return;
    m_started = true;
    service()->loadSettings();
}

QVariantList MigrationController::kinds() const
{
    QVariantList list { QVariantMap { { QStringLiteral("id"), QString() },
                                      { QStringLiteral("name"), tr("None") },
                                      { QStringLiteral("usesTable"), false },
                                      { QStringLiteral("defaultTable"), QString() } } };
    for (const IMigrationSource *s : migrationSources())
        list.append(QVariantMap { { QStringLiteral("id"), s->id() },
                                  { QStringLiteral("name"), s->displayName() },
                                  { QStringLiteral("usesTable"), s->usesTable() },
                                  { QStringLiteral("defaultTable"), s->defaultTable() } });
    return list;
}

QVariantMap MigrationController::settingsOf(const QString &connectionId,
                                            const QString &database) const
{
    const MigrationSettings s = m_settings.value(scopeKey(connectionId, database));
    return { { QStringLiteral("kind"), s.kind },
             { QStringLiteral("folder"), QDir::toNativeSeparators(s.folder) },
             { QStringLiteral("table"), s.table },
             { QStringLiteral("phpPath"), QDir::toNativeSeparators(s.phpPath) },
             { QStringLiteral("projectRoot"), QDir::toNativeSeparators(s.projectRoot) } };
}

QVariantMap MigrationController::checkEnvironment(const QString &kind, const QString &folder,
                                                  const QString &phpPath,
                                                  const QString &projectRoot) const
{
    const MigrationSettings s = makeSettings({}, {}, kind, folder, {}, phpPath, projectRoot);
    QVariantMap out { { QStringLiteral("phpText"), QString() },
                      { QStringLiteral("phpError"), QString() },
                      { QStringLiteral("rootText"), QString() },
                      { QStringLiteral("rootError"), QString() } };
    out[QStringLiteral("phpText")] = s.phpPath.isEmpty()
        ? tr("Using php from the PATH")
        : tr("Using %1").arg(QDir::toNativeSeparators(s.phpPath));
    QString root;
    if (!s.projectRoot.isEmpty())
        root = QDir::cleanPath(QFileInfo(s.projectRoot).absoluteFilePath());
    else if (QDir::isAbsolutePath(s.folder))
        root = QDir::cleanPath(s.folder + QStringLiteral("/../.."));
    out[QStringLiteral("rootText")] = root.isEmpty()
        ? tr("Project root: two levels above the migrations folder")
        : tr("Project root: %1").arg(QDir::toNativeSeparators(root));
    if (kind.isEmpty() || root.isEmpty())
        return out;
    // Not blocking: the buttons of a migration say what is missing when it matters.
    if (const auto env = MigrationRunner::resolve(s); !env) {
        // resolve() checks php first. Its error names the php it looked for in `detail`; anything
        // else is about the project. The project is checked again with a php that exists (this
        // very program, never started: resolve() only looks at the disk).
        if (env.error().detail == (s.phpPath.isEmpty() ? QStringLiteral("php") : s.phpPath))
            out[QStringLiteral("phpError")] = env.error().message;
        MigrationSettings withPhp = s;
        withPhp.phpPath = QDir::fromNativeSeparators(QCoreApplication::applicationFilePath());
        if (const auto project = MigrationRunner::resolve(withPhp); !project)
            out[QStringLiteral("rootError")] = project.error().message;
    }
    return out;
}

bool MigrationController::isConfigured(const QString &connectionId, const QString &database) const
{
    return m_settings.contains(scopeKey(connectionId, database));
}

bool MigrationController::isConfiguredAny(const QString &connectionId) const
{
    return !databasesOf(connectionId).isEmpty();
}

int MigrationController::pendingOf(const QString &connectionId, const QString &database) const
{
    return m_counts.value(scopeKey(connectionId, database), -1);
}

QVariantMap MigrationController::pendingCounts() const
{
    QVariantMap out;
    for (auto it = m_counts.cbegin(); it != m_counts.cend(); ++it) {
        const QString id = connectionOfScope(it.key());
        out.insert(id, out.value(id).toInt() + it.value());
    }
    return out;
}

QString MigrationController::scopeKey(const QString &connectionId, const QString &database)
{
    return connectionId + kScopeSeparator + database.trimmed();
}

QString MigrationController::connectionOfScope(const QString &scope)
{
    return scope.left(scope.indexOf(kScopeSeparator));
}

QString MigrationController::databaseOfScope(const QString &scope)
{
    return scope.mid(scope.indexOf(kScopeSeparator) + 1);
}

QStringList MigrationController::databasesOf(const QString &connectionId) const
{
    QStringList out;
    for (auto it = m_settings.cbegin(); it != m_settings.cend(); ++it) {
        if (connectionOfScope(it.key()) == connectionId)
            out.append(it->database);
    }
    return out;
}

QString MigrationController::validate(const QString &kind, const QString &folder,
                                      const QString &table) const
{
    if (kind.isEmpty())
        return {};
    const IMigrationSource *source = findMigrationSource(kind);
    if (source == nullptr)
        return tr("Choose the type of project.");
    const auto error = source->validate(makeSettings({}, {}, kind, folder, table));
    return error ? error->message : QString();
}

void MigrationController::checkFolder(const QString &kind, const QString &folder,
                                      const QString &table)
{
    if (kind.isEmpty()) {
        resetFolderCheck();
        return;
    }
    m_folderCheck = service()->checkFolder(makeSettings({}, {}, kind, folder, table));
    m_folderState = QStringLiteral("checking");
    m_folderMessage = tr("Checking the folder…");
    emit folderChanged();
}

void MigrationController::resetFolderCheck()
{
    m_folderCheck = 0;
    m_folderState = QStringLiteral("idle");
    m_folderMessage.clear();
    emit folderChanged();
}

void MigrationController::saveSettings(const QString &connectionId, const QString &database,
                                       const QString &kind, const QString &folder,
                                       const QString &table, const QString &phpPath,
                                       const QString &projectRoot)
{
    const MigrationSettings s
        = makeSettings(connectionId, database, kind, folder, table, phpPath, projectRoot);
    if (s.connectionId.isNull())
        return;
    const QString scope = scopeKey(connectionId, s.database);
    if (s.isEnabled() && validate(kind, folder, table).isEmpty())
        m_settings.insert(scope, s);
    else
        m_settings.remove(scope);
    service()->saveSettings(
        m_settings.value(scope, makeSettings(connectionId, s.database, {}, {}, {})));
    if (const auto it = m_requests.find(scope); it != m_requests.end()) {
        service()->cancel(*it);
        m_requests.erase(it);
    }
    m_reports.remove(scope);
    m_failures.remove(scope);
    emit reportsChanged();
    if (m_counts.remove(scope) > 0)
        emit countsChanged();
    emit settingsChanged();
    pushFeed(connectionId, s.database);
    if (m_connected.contains(connectionId))
        inspect(connectionId, s.database);
}

void MigrationController::inspect(const QString &connectionId, const QString &database)
{
    const QString scope = scopeKey(connectionId, database);
    const auto settings = m_settings.constFind(scope);
    if (settings == m_settings.cend()) {
        pushFeed(connectionId, database);
        return;
    }
    if (const auto it = m_requests.find(scope); it != m_requests.end())
        service()->cancel(*it);
    Session *session = nullptr;
    if (m_connections) {
        SchemaTreeModel *tree = m_connections->tree();
        if (tree->ensureSession(connectionId) == SchemaTreeModel::Access::Connected)
            session = tree->metadataSession(connectionId);
    }
    const auto profile = m_connections
        ? m_connections->model()->profile(QUuid::fromString(connectionId))
        : std::nullopt;
    if (session == nullptr || !profile) {
        // ensureSession() may be opening it: onConnected() reads the migrations once it is.
        m_requests.remove(scope);
        pushFeed(connectionId, database);
        return;
    }
    // shortcut: the first call loads the engine's plugin on the UI thread (it is open already, so
    // it is loaded), as the completion and the editor tabs do for their dialect.
    IDatabaseProvider *provider = m_connections->registry().provider(profile->providerId);
    m_requests.insert(scope,
                      service()->inspect(session, *settings,
                                         provider != nullptr ? &provider->dialect() : nullptr,
                                         profile->providerId));
    pushFeed(connectionId, database);
}

void MigrationController::onConnected()
{
    if (!m_connections)
        return;
    const QStringList now = m_connections->tree()->connectedIds();
    for (const QString &id : now) {
        if (m_connected.contains(id))
            continue;
        for (const QString &database : databasesOf(id))
            inspect(id, database);
    }
    for (const QString &id : std::as_const(m_connected)) {
        if (now.contains(id))
            continue;
        // Disconnected: what was read is no longer what the tree shows.
        for (const QString &database : databasesOf(id)) {
            const QString scope = scopeKey(id, database);
            if (const auto it = m_requests.find(scope); it != m_requests.end()) {
                service()->cancel(*it);
                m_requests.erase(it);
            }
            m_reports.remove(scope);
            m_failures.remove(scope);
            if (m_counts.remove(scope) > 0)
                emit countsChanged();
            pushFeed(id, database);
        }
        emit reportsChanged();
    }
    m_connected = now;
    emit availabilityChanged();
}

QString MigrationController::scopeOfRequest(quint64 id) const
{
    for (auto it = m_requests.cbegin(); it != m_requests.cend(); ++it) {
        if (it.value() == id)
            return it.key();
    }
    return {};
}

void MigrationController::onInspected(quint64 id, const MigrationReport &report)
{
    const QString scope = scopeOfRequest(id);
    if (scope.isEmpty())
        return;
    m_requests.remove(scope);
    m_failures.remove(scope);
    m_reports.insert(scope, report);
    m_counts.insert(scope, report.pending);
    emit countsChanged();
    pushFeed(connectionOfScope(scope), databaseOfScope(scope));
    emit reportsChanged();
}

void MigrationController::onFailed(quint64 id, const Error &error)
{
    const QString scope = scopeOfRequest(id);
    if (scope.isEmpty())
        return;
    m_requests.remove(scope);
    m_reports.remove(scope);
    m_failures.insert(scope, error.message);
    if (m_counts.remove(scope) > 0)
        emit countsChanged();
    pushFeed(connectionOfScope(scope), databaseOfScope(scope));
    emit reportsChanged();
}

void MigrationController::pushFeed(const QString &connectionId, const QString &database)
{
    if (!m_connections)
        return;
    const QString scope = scopeKey(connectionId, database);
    MigrationFeed feed;
    if (m_settings.contains(scope)) {
        if (const auto report = m_reports.constFind(scope); report != m_reports.cend()) {
            feed.state = MigrationFeed::State::Ready;
            feed.report = *report;
        } else if (m_requests.contains(scope)) {
            feed.state = MigrationFeed::State::Reading;
        } else if (const auto failure = m_failures.constFind(scope); failure != m_failures.cend()) {
            feed.state = MigrationFeed::State::Failed;
            feed.message = *failure;
        } else {
            feed.state = MigrationFeed::State::NotRead;
        }
    }
    m_connections->tree()->setMigrations(connectionId, database, feed);
}

QVariantMap MigrationController::entryOf(const QString &connectionId, const QString &database,
                                         const QString &name) const
{
    QVariantMap out { { QStringLiteral("found"), false } };
    const auto report = m_reports.constFind(scopeKey(connectionId, database));
    if (report == m_reports.cend())
        return out;
    for (const MigrationEntry &e : report->entries) {
        if (e.name != name)
            continue;
        out.insert(QStringLiteral("found"), true);
        out.insert(QStringLiteral("state"),
                   e.state == MigrationState::Applied      ? QStringLiteral("applied")
                       : e.state == MigrationState::Orphan ? QStringLiteral("orphan")
                                                           : QStringLiteral("pending"));
        out.insert(QStringLiteral("batch"), e.batch.value_or(-1));
        break;
    }
    return out;
}

// ---- the code of a migration ------------------------------------------------------------------

QString MigrationController::sourceKey(const QString &connectionId, const QString &database,
                                       const QString &name)
{
    return scopeKey(connectionId, database) + QLatin1Char('/') + name;
}

void MigrationController::readSource(const QString &connectionId, const QString &database,
                                     const QString &name)
{
    const QString key = sourceKey(connectionId, database, name);
    const auto settings = m_settings.constFind(scopeKey(connectionId, database));
    if (settings == m_settings.cend()) {
        m_sources[key] = Source {
            QStringLiteral("failed"), {}, tr("This database has no migrations configured."), {}, 0
        };
        emit sourceChanged(connectionId, database, name);
        return;
    }
    Source &source = m_sources[key];
    if (source.state == QLatin1String("loading"))
        return;
    source
        = Source { QStringLiteral("loading"), {}, {}, {}, service()->readSource(*settings, name) };
    emit sourceChanged(connectionId, database, name);
}

QVariantMap MigrationController::sourceOf(const QString &connectionId, const QString &database,
                                          const QString &name) const
{
    QVariantMap out { { QStringLiteral("state"), QStringLiteral("none") },
                      { QStringLiteral("text"), QString() },
                      { QStringLiteral("message"), QString() },
                      { QStringLiteral("detail"), QString() },
                      { QStringLiteral("file"), QString() },
                      { QStringLiteral("definition"), QStringLiteral("PHP") } };
    const auto settings = m_settings.constFind(scopeKey(connectionId, database));
    if (settings != m_settings.cend()) {
        if (const IMigrationSource *source = findMigrationSource(settings->kind)) {
            out.insert(QStringLiteral("file"),
                       QFileInfo(source->filePath(*settings, name)).fileName());
            out.insert(QStringLiteral("definition"), source->highlightingDefinition());
        }
    }
    const auto it = m_sources.constFind(sourceKey(connectionId, database, name));
    if (it != m_sources.cend()) {
        out.insert(QStringLiteral("state"), it->state);
        out.insert(QStringLiteral("text"), it->text);
        out.insert(QStringLiteral("message"), it->message);
        out.insert(QStringLiteral("detail"), it->detail);
    }
    return out;
}

void MigrationController::releaseSource(const QString &connectionId, const QString &database,
                                        const QString &name)
{
    m_sources.remove(sourceKey(connectionId, database, name));
    // The tab is gone: what its run showed goes with it, unless the process is still going.
    const auto it = m_runs.find(sourceKey(connectionId, database, name));
    if (it != m_runs.end()) {
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDelete): QPointer false positive
        it->second.messages = nullptr;
        if (!isRunning(connectionId, database, name)
            && it->second.state != QLatin1String("awaitingConfirm"))
            m_runs.erase(it);
    }
}

void MigrationController::onSourceRead(quint64 id, const QUuid &connectionId,
                                       const QString &database, const QString &name,
                                       const QString &text)
{
    const QString cid = connectionId.toString(QUuid::WithoutBraces);
    const auto it = m_sources.find(sourceKey(cid, database, name));
    if (it == m_sources.end() || it->request != id)
        return; // closed meanwhile, or asked again
    it->state = QStringLiteral("ready");
    it->text = text;
    emit sourceChanged(cid, database, name);
}

void MigrationController::onSourceFailed(quint64 id, const QUuid &connectionId,
                                         const QString &database, const QString &name,
                                         const Error &error)
{
    const QString cid = connectionId.toString(QUuid::WithoutBraces);
    const auto it = m_sources.find(sourceKey(cid, database, name));
    if (it == m_sources.end() || it->request != id)
        return;
    it->state = QStringLiteral("failed");
    it->message = error.message;
    it->detail = error.detail;
    emit sourceChanged(cid, database, name);
}

// ---- running migrations (H12 phase C) --------------------------------------------------------

MigrationController::Run &MigrationController::run(const QString &connectionId,
                                                   const QString &database, const QString &name)
{
    Run &r = m_runs[sourceKey(connectionId, database, name)];
    if (r.connectionId.isEmpty()) {
        r.connectionId = connectionId;
        r.database = database.trimmed();
        r.name = name;
    }
    return r;
}

const MigrationController::Run *MigrationController::findRun(const QString &connectionId,
                                                             const QString &database,
                                                             const QString &name) const
{
    const auto it = m_runs.find(sourceKey(connectionId, database, name));
    return it == m_runs.end() ? nullptr : &it->second;
}

MigrationController::Run *MigrationController::findRun(const QString &connectionId,
                                                       const QString &database, const QString &name)
{
    const auto it = m_runs.find(sourceKey(connectionId, database, name));
    return it == m_runs.end() ? nullptr : &it->second;
}

std::optional<ConnectionTarget>
MigrationController::connectionTargetOf(const QString &connectionId, const QString &database) const
{
    if (!m_connections)
        return std::nullopt;
    const auto profile = m_connections->model()->profile(QUuid::fromString(connectionId));
    if (!profile)
        return std::nullopt;
    ConnectionTarget t;
    t.providerId = profile->providerId;
    if (t.providerId == QLatin1String("sqlite")) {
        t.database = profile->values.value(QStringLiteral("file")).toString();
    } else {
        t.host = profile->values.value(QStringLiteral("host")).toString();
        t.port = profile->values.value(QStringLiteral("port")).toInt();
        t.database = profile->values.value(QStringLiteral("database")).toString();
        if (!database.isEmpty()) // the project's database: what artisan must agree with
            t.database = database;
    }
    return t;
}

namespace {

QString hostText(const QString &host, int port)
{
    QString out = host.isEmpty()
        ? QCoreApplication::translate("quarry::ui::MigrationController", "this computer")
        : host;
    if (port > 0 && !host.isEmpty())
        out += QLatin1Char(':') + QString::number(port);
    return out;
}

QString quoted(const QString &arg)
{
    if (!arg.isEmpty() && !arg.contains(QLatin1Char(' ')) && !arg.contains(QLatin1Char('"')))
        return arg;
    QString escaped = arg;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

} // namespace

QString MigrationController::commandText(const QString &program, const QStringList &args)
{
    QStringList parts { quoted(QDir::toNativeSeparators(program)) };
    for (const QString &a : args)
        parts.append(quoted(a));
    return parts.join(QLatin1Char(' '));
}

QVariantMap MigrationController::canRun(const QString &connectionId, const QString &database,
                                        const QString &name) const
{
    const auto no = [](const QString &reason) {
        return QVariantMap { { QStringLiteral("ok"), false },
                             { QStringLiteral("reason"), reason } };
    };
    const QString scope = scopeKey(connectionId, database);
    const auto settings = m_settings.constFind(scope);
    if (settings == m_settings.cend())
        return no(tr("This database has no migrations configured."));
    if (entryOf(connectionId, database, name).value(QStringLiteral("state")).toString()
        == QLatin1String("orphan"))
        return no(tr("This migration has no file in the folder, so there is nothing to run."));
    if (!m_connections || !m_connections->tree()->connectedIds().contains(connectionId))
        return no(tr("Connect to the database first: the migration is checked against it."));
    if (m_active.contains(scope))
        return no(tr("Another migration action is in progress on this database."));
    if (MigrationRunner::arguments(*settings, name, MigrationAction::DryRun).isEmpty())
        return no(tr("That is not a valid migration name."));
    if (const auto env = MigrationRunner::resolve(*settings); !env)
        return no(env.error().message);
    return { { QStringLiteral("ok"), true }, { QStringLiteral("reason"), QString() } };
}

void MigrationController::changed(const QString &key)
{
    const auto it = m_runs.find(key);
    if (it != m_runs.end())
        emit runChanged(it->second.connectionId, it->second.database, it->second.name);
}

void MigrationController::say(Run &r, const QString &kind, const QString &type, const QString &text,
                              const QString &detail)
{
    if (r.messages)
        r.messages->append({ kind, type, text, detail, 0, 0 });
}

void MigrationController::addLine(Run &r, const QString &text, bool isError)
{
    if (r.lines.size() >= kMaxOutputLines) {
        if (!r.truncated) {
            r.truncated = true;
            say(r, QStringLiteral("notice"), tr("Notice"),
                tr("Only the first %1 lines of the output are shown.").arg(kMaxOutputLines));
        }
        return;
    }
    r.lines.append(text);
    if (isError) {
        r.errorTail.append(text);
        if (r.errorTail.size() > 5)
            r.errorTail.removeFirst();
    }
    say(r, isError ? QStringLiteral("stderr") : QStringLiteral("output"),
        isError ? QStringLiteral("stderr") : tr("Output"), text);
}

void MigrationController::beginRun(Run &r, MigrationAction action)
{
    r.state = QStringLiteral("checking");
    r.action = actionText(action);
    r.message = tr("Checking the database…");
    r.detail.clear();
    r.warning.clear();
    r.why.clear();
    r.exitCode = -1;
    r.cancelled = false;
    r.truncated = false;
    r.lines.clear();
    r.errorTail.clear();
    r.request = 0;
    r.executing = false;
    r.confirmed = false;
    r.targetKnown = false;
    r.target = {};
    r.settings = m_settings.value(scopeKey(r.connectionId, r.database));
    if (r.messages)
        r.messages->clear();
}

void MigrationController::finishRun(const QString &key, const QString &state,
                                    const QString &message, const QString &detail)
{
    const auto it = m_runs.find(key);
    if (it == m_runs.end())
        return;
    Run &r = it->second;
    r.state = state;
    r.message = message;
    r.detail = detail;
    r.request = 0;
    const QString scope = scopeKey(r.connectionId, r.database);
    if (m_active.value(scope) == key)
        m_active.remove(scope);
    changed(key);
}

bool MigrationController::start(const QString &connectionId, const QString &database,
                                const QString &name, const QString &action)
{
    const auto parsed = parseAction(action);
    if (!parsed || !canRun(connectionId, database, name).value(QStringLiteral("ok")).toBool())
        return false;
    const QString key = sourceKey(connectionId, database, name);
    Run &r = run(connectionId, database, name);
    beginRun(r, *parsed);
    m_active.insert(scopeKey(connectionId, database), key);
    m_immediate.reset();
    // Read-only: what database would artisan use? Nothing else happens before this answers.
    const quint64 id = m_runner.resolveTarget(r.settings);
    if (id == 0) {
        const QString why = m_immediate ? m_immediate->message : tr("PHP could not be started.");
        say(r, QStringLiteral("error"), tr("Error"), why,
            m_immediate ? m_immediate->detail.left(kMaxDetailChars) : QString());
        finishRun(key, QStringLiteral("failed"), why,
                  m_immediate ? m_immediate->detail.left(kMaxDetailChars) : QString());
        return true;
    }
    r.request = id;
    m_steps.insert(id, key);
    changed(key);
    return true;
}

void MigrationController::onTargetResolved(quint64 id, const ArtisanTarget &target)
{
    const QString key = m_steps.take(id);
    const auto it = m_runs.find(key);
    if (key.isEmpty() || it == m_runs.end() || it->second.executing)
        return;
    Run &r = it->second;
    r.request = 0;
    r.target = target;
    r.targetKnown = true;
    const auto connection = connectionTargetOf(r.connectionId, r.database);
    QString why;
    const bool same = connection && sameDatabase(target, *connection, &why);
    if (!connection)
        why = tr("The connection is no longer available.");
    const QString database = target.database;
    const QString host = target.driver == QLatin1String("sqlite")
        ? tr("this computer")
        : hostText(target.host, target.port);

    if (r.action == QLatin1String("dryRun")) {
        if (!same) {
            r.warning = tr("This dry run would use database %1 on host %2, not the one of this "
                           "connection.")
                            .arg(database, host);
            say(r, QStringLiteral("notice"), tr("Warning"), r.warning, why);
        }
        execute(key, r);
        return;
    }
    if (!same) {
        r.why = why;
        const QString message
            = tr("Nothing was run: the database artisan uses is not the one of this connection.");
        say(r, QStringLiteral("error"), tr("Error"), message, why);
        finishRun(key, QStringLiteral("blocked"), message, why);
        return;
    }
    r.state = QStringLiteral("awaitingConfirm");
    r.message = tr("Waiting for confirmation");
    changed(key);
}

void MigrationController::confirm(const QString &connectionId, const QString &database,
                                  const QString &name)
{
    Run *r = findRun(connectionId, database, name);
    if (r == nullptr || r->state != QLatin1String("awaitingConfirm")
        || r->action == QLatin1String("dryRun"))
        return;
    r->confirmed = true;
    execute(sourceKey(connectionId, database, name), *r);
}

void MigrationController::decline(const QString &connectionId, const QString &database,
                                  const QString &name)
{
    Run *r = findRun(connectionId, database, name);
    if (r == nullptr || r->state != QLatin1String("awaitingConfirm"))
        return;
    say(*r, QStringLiteral("cancelled"), tr("Cancelled"),
        tr("Nothing was run: it was not confirmed."));
    finishRun(sourceKey(connectionId, database, name), QStringLiteral("idle"), QString());
}

// The only place that starts a migration. Run and Rollback get here from confirm() alone.
void MigrationController::execute(const QString &key, Run &r)
{
    const auto action = parseAction(r.action);
    if (!action || (*action != MigrationAction::DryRun && !r.confirmed))
        return;
    r.executing = true;
    r.state = QStringLiteral("running");
    r.message = tr("Running…");
    m_immediate.reset();
    const QStringList args = MigrationRunner::arguments(r.settings, r.name, *action);
    say(r, QStringLiteral("notice"), tr("Command"),
        commandText(r.settings.phpPath.isEmpty() ? QStringLiteral("php") : r.settings.phpPath,
                    args));
    const quint64 id = m_runner.execute(r.settings, r.name, *action);
    if (id == 0) {
        const QString why = m_immediate ? m_immediate->message : tr("PHP could not be started.");
        const QString detail = m_immediate ? m_immediate->detail.left(kMaxDetailChars) : QString();
        say(r, QStringLiteral("error"), tr("Error"), why, detail);
        finishRun(key, QStringLiteral("failed"), why, detail);
        return;
    }
    r.request = id;
    m_steps.insert(id, key);
    changed(key);
}

void MigrationController::onRunnerFailed(quint64 id, const Error &error)
{
    if (id == 0) {
        m_immediate = error; // inside start() or execute(), which report it
        return;
    }
    const QString key = m_steps.take(id);
    const auto it = m_runs.find(key);
    if (key.isEmpty() || it == m_runs.end())
        return;
    Run &r = it->second;
    const QString detail
        = (error.detail.isEmpty() ? error.message
                                  : error.message + QLatin1Char('\n') + error.detail)
              .left(kMaxDetailChars);
    if (!r.executing) {
        if (r.action == QLatin1String("dryRun")) {
            // A dry run changes nothing: it goes on, and says that it could not compare.
            r.warning = tr("The database artisan uses could not be verified.");
            say(r, QStringLiteral("notice"), tr("Warning"), r.warning, detail);
            execute(key, r);
            return;
        }
        const QString message
            = tr("The database artisan uses could not be verified, so nothing was run.");
        say(r, QStringLiteral("error"), tr("Error"), message, detail);
        finishRun(key, QStringLiteral("blocked"), message, detail);
        return;
    }
    say(r, QStringLiteral("error"), tr("Error"), error.message, error.detail.left(kMaxDetailChars));
    finishRun(key, QStringLiteral("failed"), error.message, error.detail.left(kMaxDetailChars));
    if (r.action != QLatin1String("dryRun"))
        inspect(r.connectionId, r.database); // it may have changed something before failing
}

void MigrationController::onRunnerOutput(quint64 id, const QString &text, bool isError)
{
    const auto it = m_runs.find(m_steps.value(id));
    if (it != m_runs.end() && it->second.executing)
        addLine(it->second, text, isError);
}

void MigrationController::onRunnerFinished(quint64 id, int exitCode)
{
    const QString key = m_steps.take(id);
    const auto it = m_runs.find(key);
    if (key.isEmpty() || it == m_runs.end())
        return;
    Run &r = it->second;
    r.exitCode = exitCode;
    if (exitCode == 0) {
        say(r, QStringLiteral("statement"), tr("Finished"), tr("Finished"));
        finishRun(key, QStringLiteral("done"), tr("Finished"));
    } else {
        const QString message = tr("Failed with exit code %1").arg(exitCode);
        const QString detail = r.errorTail.join(QLatin1Char('\n'));
        say(r, QStringLiteral("error"), tr("Error"), message, detail);
        finishRun(key, QStringLiteral("failed"), message, detail);
    }
    // What the migration did (or did not) is in the table now: read it again for the tree and tab.
    if (r.action != QLatin1String("dryRun"))
        inspect(r.connectionId, r.database);
}

void MigrationController::onRunnerCancelled(quint64 id)
{
    const QString key = m_steps.take(id);
    const auto it = m_runs.find(key);
    if (key.isEmpty() || it == m_runs.end())
        return;
    Run &r = it->second;
    r.cancelled = true;
    const bool changesDatabase = r.executing && r.action != QLatin1String("dryRun");
    const QString message = changesDatabase
        ? tr("Cancelled. The database may be left half migrated.")
        : tr("Cancelled");
    say(r, QStringLiteral("cancelled"), tr("Cancelled"), message);
    finishRun(key, QStringLiteral("failed"), message);
    if (changesDatabase)
        inspect(r.connectionId, r.database);
}

void MigrationController::cancel(const QString &connectionId, const QString &database,
                                 const QString &name)
{
    Run *r = findRun(connectionId, database, name);
    if (r == nullptr)
        return;
    if (r->state == QLatin1String("awaitingConfirm"))
        decline(connectionId, database, name);
    else if ((r->state == QLatin1String("checking") || r->state == QLatin1String("running"))
             && r->request != 0)
        m_runner.cancel(r->request); // cancelled(id) follows
}

QVariantMap MigrationController::targetMap(const Run &r) const
{
    QVariantMap target { { QStringLiteral("known"), r.targetKnown } };
    if (r.targetKnown) {
        target.insert(QStringLiteral("driver"), r.target.driver);
        target.insert(QStringLiteral("name"), r.target.connectionName);
        target.insert(QStringLiteral("database"), r.target.database);
        target.insert(QStringLiteral("host"),
                      r.target.driver == QLatin1String("sqlite")
                          ? tr("this computer")
                          : hostText(r.target.host, r.target.port));
    }
    return target;
}

QVariantMap MigrationController::runOf(const QString &connectionId, const QString &database,
                                       const QString &name) const
{
    QVariantMap out { { QStringLiteral("state"), QStringLiteral("idle") },
                      { QStringLiteral("action"), QString() },
                      { QStringLiteral("message"), QString() },
                      { QStringLiteral("detail"), QString() },
                      { QStringLiteral("warning"), QString() },
                      { QStringLiteral("why"), QString() },
                      { QStringLiteral("exitCode"), -1 },
                      { QStringLiteral("cancelled"), false },
                      { QStringLiteral("output"), QString() },
                      { QStringLiteral("lines"), 0 },
                      { QStringLiteral("truncated"), false },
                      { QStringLiteral("target"),
                        QVariantMap { { QStringLiteral("known"), false } } },
                      { QStringLiteral("connection"), QVariantMap() } };
    const Run *r = findRun(connectionId, database, name);
    if (r == nullptr)
        return out;
    out[QStringLiteral("state")] = r->state;
    out[QStringLiteral("action")] = r->action;
    out[QStringLiteral("message")] = r->message;
    out[QStringLiteral("detail")] = r->detail;
    out[QStringLiteral("warning")] = r->warning;
    out[QStringLiteral("why")] = r->why;
    out[QStringLiteral("exitCode")] = r->exitCode;
    out[QStringLiteral("cancelled")] = r->cancelled;
    out[QStringLiteral("output")] = r->lines.join(QLatin1Char('\n'));
    out[QStringLiteral("lines")] = static_cast<int>(r->lines.size());
    out[QStringLiteral("truncated")] = r->truncated;
    out[QStringLiteral("target")] = targetMap(*r);
    if (const auto c = connectionTargetOf(connectionId, database)) {
        out[QStringLiteral("connection")] = QVariantMap {
            { QStringLiteral("database"), c->database },
            { QStringLiteral("host"),
              c->providerId == QLatin1String("sqlite") ? tr("this computer")
                                                       : hostText(c->host, c->port) },
        };
    }
    return out;
}

QVariantMap MigrationController::confirmationOf(const QString &connectionId,
                                                const QString &database, const QString &name) const
{
    QVariantMap out;
    const Run *r = findRun(connectionId, database, name);
    const auto action = r != nullptr ? parseAction(r->action) : std::nullopt;
    if (!action || !m_connections)
        return out;
    const auto profile = m_connections->model()->profile(QUuid::fromString(connectionId));
    const auto connection = connectionTargetOf(connectionId, database);
    if (!profile || !connection)
        return out;
    const bool rollback = *action == MigrationAction::Rollback;
    const QString dbName = connection->database;
    // What to type on a production connection: the database name, or the file name for SQLite
    // (a whole path is no name to type).
    QString word
        = connection->providerId == QLatin1String("sqlite") ? QFileInfo(dbName).fileName() : dbName;
    if (word.isEmpty())
        word = profile->name;
    const QStringList args = MigrationRunner::arguments(r->settings, name, *action);
    out.insert(QStringLiteral("action"), r->action);
    out.insert(QStringLiteral("title"), rollback ? tr("Roll back migration") : tr("Run migration"));
    out.insert(QStringLiteral("confirmText"), rollback ? tr("Roll back") : tr("Run"));
    out.insert(QStringLiteral("name"), name);
    out.insert(QStringLiteral("connectionName"), profile->name);
    out.insert(QStringLiteral("production"), profile->production);
    out.insert(QStringLiteral("server"), ConnectionListModel::summaryOf(*profile));
    out.insert(QStringLiteral("database"), dbName);
    out.insert(QStringLiteral("host"),
               connection->providerId == QLatin1String("sqlite")
                   ? tr("this computer")
                   : hostText(connection->host, connection->port));
    out.insert(QStringLiteral("typeToConfirm"), word);
    out.insert(
        QStringLiteral("phrase"),
        tr("This runs php artisan from your project: it executes your project's PHP code and "
           "changes the database."));
    out.insert(QStringLiteral("rollbackNote"),
               rollback ? tr("Laravel only rolls back migrations of the latest batch; if this one "
                             "is not in it, nothing happens.")
                        : QString());
    out.insert(
        QStringLiteral("command"),
        commandText(r->settings.phpPath.isEmpty() ? QStringLiteral("php") : r->settings.phpPath,
                    args));
    const auto env = MigrationRunner::resolve(r->settings);
    out.insert(QStringLiteral("workingDirectory"),
               env ? QDir::toNativeSeparators(env->projectRoot) : QString());
    return out;
}

void MigrationController::setMessages(const QString &connectionId, const QString &database,
                                      const QString &name, QueryMessagesModel *messages)
{
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDelete): QPointer false positive
    run(connectionId, database, name).messages = messages;
}

bool MigrationController::isBusy(const QString &connectionId, const QString &database) const
{
    return m_active.contains(scopeKey(connectionId, database));
}

bool MigrationController::isRunning(const QString &connectionId, const QString &database,
                                    const QString &name) const
{
    const Run *r = findRun(connectionId, database, name);
    return r != nullptr
        && (r->state == QLatin1String("checking") || r->state == QLatin1String("running"));
}

bool MigrationController::anyRunning() const
{
    return std::ranges::any_of(m_runs, [](const auto &entry) {
        return entry.second.state == QLatin1String("checking")
            || entry.second.state == QLatin1String("running");
    });
}

void MigrationController::cancelAll()
{
    for (auto &entry : m_runs) {
        const Run &r = entry.second;
        if (r.state == QLatin1String("checking") || r.state == QLatin1String("running")
            || r.state == QLatin1String("awaitingConfirm"))
            cancel(r.connectionId, r.database, r.name);
    }
}

} // namespace quarry::ui
