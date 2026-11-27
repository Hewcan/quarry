#include "migration_service_worker.h"

#include "ssh/ssh_sftp.h"

#include <quarry/migration_service.h>

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QStringDecoder>

#include <memory>
#include <sqlite3.h>

namespace quarry {

namespace {

using Stmt = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

QString decodeMigrationSource(const QByteArray &bytes)
{
    QStringDecoder decoder(QStringDecoder::Utf8); // invalid bytes become U+FFFD
    QString text = decoder.decode(bytes);
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text;
}

QString columnText(sqlite3_stmt *stmt, int column)
{
    return QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(stmt, column)),
                             sqlite3_column_bytes(stmt, column));
}

void bindText(sqlite3_stmt *stmt, int index, const QString &text)
{
    const QByteArray utf8 = text.toUtf8();
    sqlite3_bind_text(stmt, index, utf8.constData(), static_cast<int>(utf8.size()),
                      SQLITE_TRANSIENT);
}

} // namespace

MigrationServiceWorker::MigrationServiceWorker(QString databasePath)
    : m_database(std::move(databasePath))
{ }

void MigrationServiceWorker::load()
{
    Error error;
    if (!m_database.open(error)) {
        emit failed(error);
        return;
    }
    sqlite3_stmt *raw = nullptr;
    int rc = sqlite3_prepare_v2(m_database.handle(),
                                "SELECT connection_id, kind, folder, table_name, php_path,"
                                " project_root, database, remote FROM migration_settings",
                                -1, &raw, nullptr);
    Stmt stmt(raw, &sqlite3_finalize);
    QList<MigrationSettings> list;
    if (rc == SQLITE_OK) {
        while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
            MigrationSettings s;
            s.connectionId = QUuid::fromString(columnText(stmt.get(), 0));
            s.kind = columnText(stmt.get(), 1);
            s.folder = columnText(stmt.get(), 2);
            s.table = columnText(stmt.get(), 3);
            s.phpPath = columnText(stmt.get(), 4);
            s.projectRoot = columnText(stmt.get(), 5);
            s.database = columnText(stmt.get(), 6);
            s.remote = sqlite3_column_int(stmt.get(), 7) != 0;
            list.append(s);
        }
    }
    if (rc != SQLITE_DONE) {
        emit failed(m_database.error(rc, QStringLiteral("load migration settings")));
        return;
    }
    emit loaded(list);
}

void MigrationServiceWorker::save(const MigrationSettings &settings)
{
    Error error;
    if (!m_database.open(error)) {
        emit failed(error);
        return;
    }
    if (settings.connectionId.isNull()) {
        emit failed({ ErrorKind::Constraint, tr("Save the connection first."), {}, {}, {} });
        return;
    }
    const char *sql = settings.isEnabled()
        ? "INSERT OR REPLACE INTO migration_settings(connection_id, kind, folder, table_name,"
          " updated_at, php_path, project_root, database, remote)"
          " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)"
        : "DELETE FROM migration_settings WHERE connection_id=?1 AND database=?8";
    sqlite3_stmt *raw = nullptr;
    int rc = sqlite3_prepare_v2(m_database.handle(), sql, -1, &raw, nullptr);
    Stmt stmt(raw, &sqlite3_finalize);
    if (rc == SQLITE_OK) {
        bindText(stmt.get(), 1, settings.connectionId.toString(QUuid::WithoutBraces));
        bindText(stmt.get(), 8, settings.database);
        if (settings.isEnabled()) {
            bindText(stmt.get(), 2, settings.kind);
            bindText(stmt.get(), 3, settings.folder);
            bindText(stmt.get(), 4, settings.table);
            bindText(stmt.get(), 5, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            bindText(stmt.get(), 6, settings.phpPath);
            bindText(stmt.get(), 7, settings.projectRoot);
            sqlite3_bind_int(stmt.get(), 9, settings.remote ? 1 : 0);
        }
        rc = sqlite3_step(stmt.get());
    }
    if ((rc & 0xff) == SQLITE_CONSTRAINT) { // the only constraint is the foreign key
        emit failed({ ErrorKind::NotFound,
                      tr("The connection no longer exists."),
                      settings.connectionId.toString(QUuid::WithoutBraces),
                      {},
                      {} });
        return;
    }
    if (rc != SQLITE_DONE) {
        emit failed(m_database.error(rc, QStringLiteral("save migration settings")));
        return;
    }
    emit saved(settings);
}

void MigrationServiceWorker::scan(quint64 id, const MigrationSettings &settings,
                                  const CancellationToken &cancel,
                                  const std::optional<RemoteMigrationContext> &remote)
{
    const IMigrationSource *source = findMigrationSource(settings.kind);
    if (source == nullptr) {
        emit scanned(
            id, {},
            { ErrorKind::Unsupported, tr("Choose the type of project."), settings.kind, {}, {} },
            false);
        return;
    }
    if (auto error = source->validate(settings)) {
        emit scanned(id, {}, *error, false);
        return;
    }
    if (settings.remote) {
        if (!remote || !remote->isValid()) {
            emit scanned(id, {},
                         { ErrorKind::Connection,
                           tr("Remote migrations require an SSH tunnel on this connection."),
                           settings.folder,
                           {},
                           {} },
                         false);
            return;
        }
        const QString remoteFolder = settings.folder.trimmed();
        Result<QStringList> rawEntries = m_remoteBackend.listDirectory
            ? m_remoteBackend.listDirectory(*remote, remoteFolder, cancel)
            : ssh::listRemoteDirectory(remote->tunnel, remote->secrets, *remote->knownHosts,
                                       remoteFolder, cancel);
        if (!rawEntries) {
            emit scanned(id, {}, rawEntries.error(), false);
            return;
        }
        emit scanned(id, source->filterFileNames(rawEntries.value()), {}, true);
        return;
    }
    auto files = source->scanFolder(settings.folder, cancel);
    if (!files) {
        emit scanned(id, {}, files.error(), false);
        return;
    }
    emit scanned(id, files.value(), {}, true);
}

void MigrationServiceWorker::readSource(quint64 id, const MigrationSettings &settings,
                                        const QString &name,
                                        const std::optional<RemoteMigrationContext> &remote)
{
    const auto fail = [&](const Error &error) {
        emit sourceDone(id, settings.connectionId, settings.database, name, {}, error, false);
    };
    const IMigrationSource *source = findMigrationSource(settings.kind);
    if (source == nullptr) {
        fail({ ErrorKind::Unsupported, tr("Choose the type of project."), settings.kind, {}, {} });
        return;
    }
    if (auto error = source->validate(settings)) {
        fail(*error);
        return;
    }
    const QString path = source->filePath(settings, name);
    if (path.isEmpty()) {
        fail({ ErrorKind::Syntax, tr("That is not a valid migration name."), name, {}, {} });
        return;
    }
    if (settings.remote) {
        if (!remote || !remote->isValid()) {
            fail({ ErrorKind::Connection,
                   tr("Remote migrations require an SSH tunnel on this connection."),
                   path,
                   {},
                   {} });
            return;
        }
        Result<QByteArray> bytes = m_remoteBackend.readFile
            ? m_remoteBackend.readFile(*remote, path, MigrationService::kMaxSourceBytes, {})
            : ssh::readRemoteFile(remote->tunnel, remote->secrets, *remote->knownHosts, path,
                                  MigrationService::kMaxSourceBytes, {});
        if (!bytes) {
            fail(bytes.error());
            return;
        }
        emit sourceDone(id, settings.connectionId, settings.database, name,
                        decodeMigrationSource(bytes.value()), {}, true);
        return;
    }
    // QFileInfo follows symlinks: a link to a regular file reads, a link to a folder or a device
    // is refused.
    const QFileInfo info(path);
    if (!info.exists()) {
        fail({ ErrorKind::NotFound, tr("The migration file was not found."), path, {}, {} });
        return;
    }
    if (!info.isFile()) {
        fail({ ErrorKind::Constraint,
               tr("The migration path is not a regular file."),
               path,
               {},
               {} });
        return;
    }
    if (info.size() > MigrationService::kMaxSourceBytes) {
        fail({ ErrorKind::Constraint,
               tr("The migration file is larger than 1 MB."),
               tr("%1 bytes").arg(info.size()),
               {},
               {} });
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail({ ErrorKind::Permission,
               tr("The migration file cannot be read."),
               file.errorString(),
               {},
               {} });
        return;
    }
    // One byte more than the limit: a file that grew after the size check is still refused.
    const QByteArray bytes = file.read(MigrationService::kMaxSourceBytes + 1);
    if (file.error() != QFileDevice::NoError) {
        fail({ ErrorKind::Internal,
               tr("The migration file cannot be read."),
               file.errorString(),
               {},
               {} });
        return;
    }
    if (bytes.size() > MigrationService::kMaxSourceBytes) {
        fail({ ErrorKind::Constraint,
               tr("The migration file is larger than 1 MB."),
               tr("%1 bytes").arg(bytes.size()),
               {},
               {} });
        return;
    }
    emit sourceDone(id, settings.connectionId, settings.database, name,
                    decodeMigrationSource(bytes), {}, true);
}

} // namespace quarry
