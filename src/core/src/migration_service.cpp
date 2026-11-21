#include "migration_service_worker.h"

#include <quarry/migration_service.h>

#include <QLoggingCategory>
#include <QThread>

namespace quarry {

Q_LOGGING_CATEGORY(lcMigrations, "quarry.migrations")

namespace {

constexpr int kShutdownTimeoutMs = 5000;
constexpr qsizetype kFetchRows = 500;

QString cellText(const ColumnBuffer &column, qsizetype row)
{
    switch (column.typeAt(row)) {
    case ValueType::Null:
        return {};
    case ValueType::Int64:
    case ValueType::Bool:
        return QString::number(column.int64At(row));
    case ValueType::Double:
        return QString::number(column.doubleAt(row));
    default:
        return QString::fromUtf8(column.bytesAt(row));
    }
}

std::optional<qint64> cellInteger(const ColumnBuffer &column, qsizetype row)
{
    switch (column.typeAt(row)) {
    case ValueType::Null:
        return std::nullopt;
    case ValueType::Int64:
    case ValueType::Bool:
        return column.int64At(row);
    case ValueType::Double:
        return static_cast<qint64>(column.doubleAt(row));
    default: {
        bool ok = false;
        const qint64 v = QString::fromUtf8(column.bytesAt(row)).toLongLong(&ok);
        return ok ? std::optional<qint64>(v) : std::nullopt;
    }
    }
}

// The engines word "table not found" differently; NotFound is the mapped kind, the texts are the
// fallback for drivers that report it as a generic error.
bool isMissingTable(const Error &error)
{
    if (error.kind == ErrorKind::NotFound || error.sqlState == QLatin1String("42P01")
        || error.sqlState == QLatin1String("42S02"))
        return true;
    const QString d = error.detail + QLatin1Char(' ') + error.message;
    return d.contains(QLatin1String("no such table"), Qt::CaseInsensitive)
        || d.contains(QLatin1String("doesn't exist"), Qt::CaseInsensitive)
        || d.contains(QLatin1String("does not exist"), Qt::CaseInsensitive)
        || d.contains(QLatin1String("Invalid object name"), Qt::CaseInsensitive);
}

} // namespace

struct MigrationService::Run
{
    QPointer<Session> session;
    MigrationSettings settings;
    const IMigrationSource *source = nullptr;
    CancellationSource cancel;
    QStringList files;
    std::vector<AppliedMigration> rows;
    std::vector<QMetaObject::Connection> connections;
    bool started = false; // the session started our SELECT: signals before that are not ours
    bool querying = false;

    ~Run()
    {
        for (const auto &c : connections)
            QObject::disconnect(c);
    }
};

MigrationService::MigrationService(QString databasePath, QObject *parent)
    : QObject(parent)
    , m_thread(new QThread) // no parent: must outlive this object if the join times out
{
    qRegisterMetaType<quarry::Error>();
    qRegisterMetaType<quarry::MigrationSettings>();
    qRegisterMetaType<QList<quarry::MigrationSettings>>();
    qRegisterMetaType<quarry::MigrationReport>();

    m_worker = new MigrationServiceWorker(std::move(databasePath));
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    const auto q = Qt::QueuedConnection;
    connect(m_worker, &MigrationServiceWorker::loaded, this, &MigrationService::settingsLoaded, q);
    connect(m_worker, &MigrationServiceWorker::saved, this, &MigrationService::settingsSaved, q);
    connect(m_worker, &MigrationServiceWorker::failed, this, &MigrationService::failed, q);
    connect(m_worker, &MigrationServiceWorker::scanned, this, &MigrationService::onScanned, q);
    connect(m_worker, &MigrationServiceWorker::sourceDone, this, &MigrationService::onSourceDone,
            q);
    m_thread->start();
}

MigrationService::~MigrationService()
{
    for (auto &[id, run] : m_runs)
        run->cancel.cancel();
    m_runs.clear();
    post([w = m_worker] {
        w->closeDatabase();
        QThread::currentThread()->quit();
    });
    if (m_thread->wait(kShutdownTimeoutMs)) {
        delete m_thread;
    } else {
        qCWarning(lcMigrations) << "Migrations thread did not stop within" << kShutdownTimeoutMs
                                << "ms; leaking it until the disk call returns";
        connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    }
}

template<typename F> void MigrationService::post(F &&task)
{
    QMetaObject::invokeMethod(m_worker, std::forward<F>(task), Qt::QueuedConnection);
}

void MigrationService::loadSettings()
{
    post([w = m_worker] { w->load(); });
}

void MigrationService::saveSettings(const MigrationSettings &settings)
{
    post([w = m_worker, settings] { w->save(settings); });
}

quint64 MigrationService::checkFolder(const MigrationSettings &settings)
{
    const quint64 id = ++m_lastId;
    m_checks.insert(id);
    post([w = m_worker, id, settings] { w->scan(id, settings, {}); });
    return id;
}

// NOLINTBEGIN(clang-analyzer-cplusplus.NewDelete): QPointer false positive
quint64 MigrationService::inspect(Session *session, const MigrationSettings &settings)
{
    for (auto it = m_runs.begin(); it != m_runs.end();) {
        if (session != nullptr && it->second->session == session) {
            it->second->cancel.cancel();
            it = m_runs.erase(it);
        } else {
            ++it;
        }
    }
    const quint64 id = ++m_lastId;
    auto run = std::make_unique<Run>();
    run->session = session;
    run->settings = settings;
    run->source = findMigrationSource(settings.kind);
    const CancellationToken token = run->cancel.token();
    m_runs.emplace(id, std::move(run));
    post([w = m_worker, id, settings, token] { w->scan(id, settings, token); });
    return id;
}
// NOLINTEND(clang-analyzer-cplusplus.NewDelete)

quint64 MigrationService::readSource(const MigrationSettings &settings, const QString &name)
{
    const quint64 id = ++m_lastId;
    m_reads.insert(id);
    post([w = m_worker, id, settings, name] { w->readSource(id, settings, name); });
    return id;
}

void MigrationService::onSourceDone(quint64 id, const QUuid &connectionId, const QString &name,
                                    const QString &text, const Error &error, bool ok)
{
    if (m_reads.erase(id) == 0)
        return; // cancelled
    if (ok)
        emit sourceRead(id, connectionId, name, text);
    else
        emit sourceFailed(id, connectionId, name, error);
}

void MigrationService::cancel(quint64 id)
{
    m_checks.erase(id);
    m_reads.erase(id);
    const auto it = m_runs.find(id);
    if (it == m_runs.end())
        return;
    it->second->cancel.cancel();
    m_runs.erase(it);
}

void MigrationService::onScanned(quint64 id, const QStringList &files, const Error &error, bool ok)
{
    if (m_checks.erase(id) > 0) {
        if (ok)
            emit folderChecked(id, static_cast<int>(files.size()));
        else
            emit folderCheckFailed(id, error);
        return;
    }
    const auto it = m_runs.find(id);
    if (it == m_runs.end())
        return; // cancelled
    Run &run = *it->second;
    if (!ok) {
        finishRun(id, &error);
        return;
    }
    run.files = files;
    Session *session = run.session.data();
    if (session == nullptr) {
        const Error notOpen { ErrorKind::Connection,
                              tr("Connect to the database to compare its migrations."),
                              {},
                              {},
                              {} };
        finishRun(id, &notOpen);
        return;
    }
    auto query = run.source->appliedQuery(run.settings);
    if (!query) {
        finishRun(id, &query.error());
        return;
    }
    Statement statement = std::move(query).value();
    statement.fetchSize = static_cast<int>(kFetchRows);

    auto &c = run.connections;
    c.push_back(connect(session, &Session::started, this, [this, id] {
        if (const auto r = m_runs.find(id); r != m_runs.end() && r->second->querying)
            r->second->started = true;
    }));
    c.push_back(connect(session, &Session::batchReady, this,
                        [this, id](const std::shared_ptr<const ResultBatch> &batch) {
                            const auto r = m_runs.find(id);
                            if (r != m_runs.end() && r->second->started && batch)
                                onBatch(*r->second, *batch);
                        }));
    c.push_back(connect(session, &Session::finished, this, [this, id](const ExecutionStats &stats) {
        const auto r = m_runs.find(id);
        if (r == m_runs.end() || !r->second->started)
            return;
        if (!stats.atEnd && r->second->session) {
            r->second->session->fetchMore(kFetchRows);
            return;
        }
        finishRun(id, nullptr);
    }));
    c.push_back(connect(session, &Session::failed, this, [this, id](const Error &e) {
        const auto r = m_runs.find(id);
        if (r != m_runs.end() && r->second->started)
            finishRun(id, &e);
    }));
    c.push_back(connect(session, &Session::cancelled, this, [this, id] {
        const auto r = m_runs.find(id);
        if (r == m_runs.end() || !r->second->started)
            return;
        const Error e {
            ErrorKind::Cancelled, tr("Reading the migrations table was cancelled."), {}, {}, {}
        };
        finishRun(id, &e);
    }));
    c.push_back(connect(session, &QObject::destroyed, this, [this, id] {
        if (m_runs.contains(id)) {
            const Error e { ErrorKind::Connection,
                            tr("The connection was closed before the migrations were read."),
                            {},
                            {},
                            {} };
            finishRun(id, &e);
        }
    }));
    run.querying = true;
    session->execute(statement);
}

void MigrationService::onBatch(Run &run, const ResultBatch &batch)
{
    if (batch.columns.empty())
        return;
    const ColumnBuffer &names = batch.columns[0];
    const ColumnBuffer *batches = batch.columns.size() > 1 ? &batch.columns[1] : nullptr;
    for (qsizetype row = 0; row < batch.rowCount; ++row) {
        if (names.isNull(row))
            continue;
        run.rows.push_back(
            { cellText(names, row), batches ? cellInteger(*batches, row) : std::nullopt });
    }
}

void MigrationService::finishRun(quint64 id, const Error *error)
{
    const auto it = m_runs.find(id);
    if (it == m_runs.end())
        return;
    std::unique_ptr<Run> run = std::move(it->second);
    m_runs.erase(it); // `run` disconnects from the session when it goes out of scope

    if (error != nullptr && !(run->querying && isMissingTable(*error))) {
        Error shown = *error;
        if (run->querying && shown.kind != ErrorKind::Cancelled) {
            const QString table
                = run->settings.table.isEmpty() ? run->source->defaultTable() : run->settings.table;
            shown.message = tr("Quarry could not read the migrations table \"%1\": %2")
                                .arg(table, error->message);
        }
        emit inspectionFailed(id, shown);
        return;
    }
    const bool tableMissing = error != nullptr;
    if (tableMissing)
        run->rows.clear(); // nothing is applied without the table
    MigrationReport report = run->source->reconcile(run->files, run->rows);
    report.tableMissing = tableMissing;
    emit inspected(id, std::move(report));
}

} // namespace quarry
