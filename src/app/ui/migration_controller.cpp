#include "migration_controller.h"

#include <quarry/connection_store.h>

#include <QDir>
#include <QFileInfo>

namespace quarry::ui {

namespace {

MigrationSettings makeSettings(const QString &connectionId, const QString &kind,
                               const QString &folder, const QString &table)
{
    MigrationSettings s;
    s.connectionId = QUuid::fromString(connectionId);
    s.kind = kind;
    s.folder = QDir::fromNativeSeparators(folder.trimmed());
    s.table = table.trimmed();
    return s;
}

} // namespace

MigrationController::MigrationController(QObject *parent)
    : QObject(parent)
{ }

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
                        m_settings.insert(s.connectionId.toString(QUuid::WithoutBraces), s);
                    for (const QString &id : m_settings.keys() + before)
                        pushFeed(id); // the tree grows (or drops) its Migrations folders
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

QVariantMap MigrationController::settingsOf(const QString &connectionId) const
{
    const MigrationSettings s = m_settings.value(connectionId);
    return { { QStringLiteral("kind"), s.kind },
             { QStringLiteral("folder"), QDir::toNativeSeparators(s.folder) },
             { QStringLiteral("table"), s.table } };
}

bool MigrationController::isConfigured(const QString &connectionId) const
{
    return m_settings.contains(connectionId);
}

QString MigrationController::validate(const QString &kind, const QString &folder,
                                      const QString &table) const
{
    if (kind.isEmpty())
        return {};
    const IMigrationSource *source = findMigrationSource(kind);
    if (source == nullptr)
        return tr("Choose the type of project.");
    const auto error = source->validate(makeSettings({}, kind, folder, table));
    return error ? error->message : QString();
}

void MigrationController::checkFolder(const QString &kind, const QString &folder,
                                      const QString &table)
{
    if (kind.isEmpty()) {
        resetFolderCheck();
        return;
    }
    m_folderCheck = service()->checkFolder(makeSettings({}, kind, folder, table));
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

void MigrationController::saveSettings(const QString &connectionId, const QString &kind,
                                       const QString &folder, const QString &table)
{
    const MigrationSettings s = makeSettings(connectionId, kind, folder, table);
    if (s.connectionId.isNull())
        return;
    if (s.isEnabled() && validate(kind, folder, table).isEmpty())
        m_settings.insert(connectionId, s);
    else
        m_settings.remove(connectionId);
    service()->saveSettings(m_settings.value(connectionId, makeSettings(connectionId, {}, {}, {})));
    if (const auto it = m_requests.find(connectionId); it != m_requests.end()) {
        service()->cancel(*it);
        m_requests.erase(it);
    }
    m_reports.remove(connectionId);
    m_failures.remove(connectionId);
    emit reportsChanged();
    if (m_counts.remove(connectionId) > 0)
        emit countsChanged();
    emit settingsChanged();
    pushFeed(connectionId);
    if (m_connected.contains(connectionId))
        inspect(connectionId);
}

void MigrationController::inspect(const QString &connectionId)
{
    const auto settings = m_settings.constFind(connectionId);
    if (settings == m_settings.cend()) {
        pushFeed(connectionId);
        return;
    }
    if (const auto it = m_requests.find(connectionId); it != m_requests.end())
        service()->cancel(*it);
    Session *session = nullptr;
    if (m_connections) {
        SchemaTreeModel *tree = m_connections->tree();
        if (tree->ensureSession(connectionId) == SchemaTreeModel::Access::Connected)
            session = tree->metadataSession(connectionId);
    }
    if (session == nullptr) {
        // ensureSession() may be opening it: onConnected() reads the migrations once it is.
        m_requests.remove(connectionId);
        pushFeed(connectionId);
        return;
    }
    m_requests.insert(connectionId, service()->inspect(session, *settings));
    pushFeed(connectionId);
}

void MigrationController::onConnected()
{
    if (!m_connections)
        return;
    const QStringList now = m_connections->tree()->connectedIds();
    for (const QString &id : now) {
        if (!m_connected.contains(id) && m_settings.contains(id))
            inspect(id);
    }
    for (const QString &id : std::as_const(m_connected)) {
        if (!now.contains(id)) {
            if (const auto it = m_requests.find(id); it != m_requests.end()) {
                service()->cancel(*it);
                m_requests.erase(it);
            }
            // Disconnected: what was read is no longer what the tree shows.
            m_reports.remove(id);
            m_failures.remove(id);
            emit reportsChanged();
            pushFeed(id);
        }
    }
    m_connected = now;
}

QString MigrationController::idOfRequest(quint64 id) const
{
    for (auto it = m_requests.cbegin(); it != m_requests.cend(); ++it) {
        if (it.value() == id)
            return it.key();
    }
    return {};
}

void MigrationController::onInspected(quint64 id, const MigrationReport &report)
{
    const QString connectionId = idOfRequest(id);
    if (connectionId.isEmpty())
        return;
    m_requests.remove(connectionId);
    m_failures.remove(connectionId);
    m_reports.insert(connectionId, report);
    m_counts.insert(connectionId, report.pending);
    emit countsChanged();
    pushFeed(connectionId);
    emit reportsChanged();
}

void MigrationController::onFailed(quint64 id, const Error &error)
{
    const QString connectionId = idOfRequest(id);
    if (connectionId.isEmpty())
        return;
    m_requests.remove(connectionId);
    m_reports.remove(connectionId);
    m_failures.insert(connectionId, error.message);
    if (m_counts.remove(connectionId) > 0)
        emit countsChanged();
    pushFeed(connectionId);
    emit reportsChanged();
}

void MigrationController::pushFeed(const QString &connectionId)
{
    if (!m_connections)
        return;
    MigrationFeed feed;
    if (m_settings.contains(connectionId)) {
        if (const auto report = m_reports.constFind(connectionId); report != m_reports.cend()) {
            feed.state = MigrationFeed::State::Ready;
            feed.report = *report;
        } else if (m_requests.contains(connectionId)) {
            feed.state = MigrationFeed::State::Reading;
        } else if (const auto failure = m_failures.constFind(connectionId);
                   failure != m_failures.cend()) {
            feed.state = MigrationFeed::State::Failed;
            feed.message = *failure;
        } else {
            feed.state = MigrationFeed::State::NotRead;
        }
    }
    m_connections->tree()->setMigrations(connectionId, feed);
}

QVariantMap MigrationController::entryOf(const QString &connectionId, const QString &name) const
{
    QVariantMap out { { QStringLiteral("found"), false } };
    const auto report = m_reports.constFind(connectionId);
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

QString MigrationController::sourceKey(const QString &connectionId, const QString &name)
{
    return connectionId + QLatin1Char('/') + name;
}

void MigrationController::readSource(const QString &connectionId, const QString &name)
{
    const QString key = sourceKey(connectionId, name);
    const auto settings = m_settings.constFind(connectionId);
    if (settings == m_settings.cend()) {
        m_sources[key] = Source {
            QStringLiteral("failed"), {}, tr("This connection has no migrations configured."), {}, 0
        };
        emit sourceChanged(connectionId, name);
        return;
    }
    Source &source = m_sources[key];
    if (source.state == QLatin1String("loading"))
        return;
    source
        = Source { QStringLiteral("loading"), {}, {}, {}, service()->readSource(*settings, name) };
    emit sourceChanged(connectionId, name);
}

QVariantMap MigrationController::sourceOf(const QString &connectionId, const QString &name) const
{
    QVariantMap out { { QStringLiteral("state"), QStringLiteral("none") },
                      { QStringLiteral("text"), QString() },
                      { QStringLiteral("message"), QString() },
                      { QStringLiteral("detail"), QString() },
                      { QStringLiteral("file"), QString() },
                      { QStringLiteral("definition"), QStringLiteral("PHP") } };
    const auto settings = m_settings.constFind(connectionId);
    if (settings != m_settings.cend()) {
        if (const IMigrationSource *source = findMigrationSource(settings->kind)) {
            out.insert(QStringLiteral("file"),
                       QFileInfo(source->filePath(*settings, name)).fileName());
            out.insert(QStringLiteral("definition"), source->highlightingDefinition());
        }
    }
    const auto it = m_sources.constFind(sourceKey(connectionId, name));
    if (it != m_sources.cend()) {
        out.insert(QStringLiteral("state"), it->state);
        out.insert(QStringLiteral("text"), it->text);
        out.insert(QStringLiteral("message"), it->message);
        out.insert(QStringLiteral("detail"), it->detail);
    }
    return out;
}

void MigrationController::releaseSource(const QString &connectionId, const QString &name)
{
    m_sources.remove(sourceKey(connectionId, name));
}

void MigrationController::onSourceRead(quint64 id, const QUuid &connectionId, const QString &name,
                                       const QString &text)
{
    const QString cid = connectionId.toString(QUuid::WithoutBraces);
    const auto it = m_sources.find(sourceKey(cid, name));
    if (it == m_sources.end() || it->request != id)
        return; // closed meanwhile, or asked again
    it->state = QStringLiteral("ready");
    it->text = text;
    emit sourceChanged(cid, name);
}

void MigrationController::onSourceFailed(quint64 id, const QUuid &connectionId, const QString &name,
                                         const Error &error)
{
    const QString cid = connectionId.toString(QUuid::WithoutBraces);
    const auto it = m_sources.find(sourceKey(cid, name));
    if (it == m_sources.end() || it->request != id)
        return;
    it->state = QStringLiteral("failed");
    it->message = error.message;
    it->detail = error.detail;
    emit sourceChanged(cid, name);
}

} // namespace quarry::ui
