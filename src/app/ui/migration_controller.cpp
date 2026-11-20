#include "migration_controller.h"

#include <quarry/connection_store.h>

#include <QDir>

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
                    m_settings.clear();
                    for (const MigrationSettings &s : list)
                        m_settings.insert(s.connectionId.toString(QUuid::WithoutBraces), s);
                    m_connected.clear(); // inspect whatever is already connected
                    onConnected();
                });
        connect(m_service.get(), &MigrationService::failed, this, [this](const Error &e) {
            if (!m_viewId.isEmpty())
                setView(QStringLiteral("failed"), e.message, e.detail);
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
    if (m_counts.remove(connectionId) > 0)
        emit countsChanged();
    if (m_connected.contains(connectionId) || m_viewId == connectionId)
        inspect(connectionId);
}

void MigrationController::show(const QString &connectionId)
{
    m_viewId = connectionId;
    m_report = {};
    m_model.clear();
    inspect(connectionId);
}

void MigrationController::refresh()
{
    if (!m_viewId.isEmpty())
        inspect(m_viewId);
}

void MigrationController::cancel()
{
    if (const auto it = m_requests.find(m_viewId); it != m_requests.end()) {
        service()->cancel(*it);
        m_requests.erase(it);
        setView(QStringLiteral("failed"), tr("Reading the migrations was cancelled."));
        emit announcement(m_message);
    }
}

void MigrationController::inspect(const QString &connectionId)
{
    const bool view = connectionId == m_viewId;
    const auto settings = m_settings.constFind(connectionId);
    if (settings == m_settings.cend()) {
        if (view)
            setView(QStringLiteral("unconfigured"),
                    tr("This connection is not linked to a project. Edit the connection and "
                       "choose the type of project and its migrations folder."));
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
        if (view)
            setView(QStringLiteral("disconnected"),
                    tr("Connect to the database (expand it in the sidebar) to compare its "
                       "migrations."));
        return;
    }
    m_requests.insert(connectionId, service()->inspect(session, *settings));
    if (view) {
        setView(QStringLiteral("loading"), tr("Reading the migrations…"));
        emit announcement(m_message);
    }
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
            if (id == m_viewId)
                setView(QStringLiteral("disconnected"),
                        tr("Connect to the database (expand it in the sidebar) to compare its "
                           "migrations."));
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
    m_counts.insert(connectionId, report.pending);
    emit countsChanged();
    if (connectionId != m_viewId)
        return;
    m_report = report;
    m_model.setEntries(report.entries);
    QString note;
    if (report.tableMissing)
        note = tr("The migrations table does not exist yet: no migration has been applied.");
    else if (report.files == 0)
        note = tr("The migrations folder has no migration files.");
    setView(QStringLiteral("ready"), note);
    emit announcement(note.isEmpty() ? summary() : summary() + QLatin1Char(' ') + note);
}

void MigrationController::onFailed(quint64 id, const Error &error)
{
    const QString connectionId = idOfRequest(id);
    if (connectionId.isEmpty())
        return;
    m_requests.remove(connectionId);
    if (m_counts.remove(connectionId) > 0)
        emit countsChanged();
    if (connectionId != m_viewId)
        return;
    m_report = {};
    m_model.clear();
    setView(QStringLiteral("failed"), error.message, error.detail);
    emit announcement(error.message);
}

void MigrationController::setView(const QString &state, const QString &message,
                                  const QString &detail)
{
    m_state = state;
    m_message = message;
    m_detail = detail;
    emit viewChanged();
}

QString MigrationController::summary() const
{
    if (m_state != QLatin1String("ready"))
        return {};
    QString text = tr("%n pending", nullptr, m_report.pending) + QStringLiteral(", ")
        + tr("%n applied", nullptr, m_report.applied);
    if (m_report.orphans > 0)
        text += QStringLiteral(", ") + tr("%n with the file missing", nullptr, m_report.orphans);
    return text;
}

} // namespace quarry::ui
