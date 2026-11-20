#include "migration_service_worker.h"

#include <QDateTime>

#include <memory>
#include <sqlite3.h>

namespace quarry {

namespace {

using Stmt = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

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
                                "SELECT connection_id, kind, folder, table_name"
                                " FROM migration_settings",
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
          " updated_at) VALUES(?1, ?2, ?3, ?4, ?5)"
        : "DELETE FROM migration_settings WHERE connection_id=?1";
    sqlite3_stmt *raw = nullptr;
    int rc = sqlite3_prepare_v2(m_database.handle(), sql, -1, &raw, nullptr);
    Stmt stmt(raw, &sqlite3_finalize);
    if (rc == SQLITE_OK) {
        bindText(stmt.get(), 1, settings.connectionId.toString(QUuid::WithoutBraces));
        if (settings.isEnabled()) {
            bindText(stmt.get(), 2, settings.kind);
            bindText(stmt.get(), 3, settings.folder);
            bindText(stmt.get(), 4, settings.table);
            bindText(stmt.get(), 5,
                     QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        }
        rc = sqlite3_step(stmt.get());
    }
    if ((rc & 0xff) == SQLITE_CONSTRAINT) { // the only constraint is the foreign key
        emit failed({ ErrorKind::NotFound, tr("The connection no longer exists."),
                      settings.connectionId.toString(QUuid::WithoutBraces), {}, {} });
        return;
    }
    if (rc != SQLITE_DONE) {
        emit failed(m_database.error(rc, QStringLiteral("save migration settings")));
        return;
    }
    emit saved(settings);
}

void MigrationServiceWorker::scan(quint64 id, const MigrationSettings &settings,
                                  const CancellationToken &cancel)
{
    const IMigrationSource *source = findMigrationSource(settings.kind);
    if (source == nullptr) {
        emit scanned(id, {},
                     { ErrorKind::Unsupported, tr("Choose the type of project."), settings.kind,
                       {}, {} },
                     false);
        return;
    }
    if (auto error = source->validate(settings)) {
        emit scanned(id, {}, *error, false);
        return;
    }
    auto files = source->scanFolder(settings.folder, cancel);
    if (!files) {
        emit scanned(id, {}, files.error(), false);
        return;
    }
    emit scanned(id, files.value(), {}, true);
}

} // namespace quarry
