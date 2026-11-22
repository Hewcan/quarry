#include "app_database.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QThread>

#include <memory>
#include <sqlite3.h>

namespace quarry {

namespace {

constexpr int kBusyTimeoutMs = 3000;

// Migration N (1-based) is kMigrations[N - 1]. Never edit a released one: add a new entry.
// `values` is a reserved word, hence values_json. connection_id uses the same text format as
// connections.id (UUID without braces) so the foreign key matches.
constexpr const char *kMigrations[AppDatabase::kLatestVersion] = {
    "CREATE TABLE connections("
    " id TEXT PRIMARY KEY NOT NULL,"
    " name TEXT NOT NULL,"
    " provider_id TEXT NOT NULL,"
    " values_json TEXT NOT NULL DEFAULT '{}',"
    " color_index INTEGER NOT NULL DEFAULT -1,"
    " production INTEGER NOT NULL DEFAULT 0,"
    " password_mode INTEGER NOT NULL DEFAULT 0,"
    " created_at TEXT NOT NULL,"
    " updated_at TEXT NOT NULL,"
    " last_used_at TEXT)",
    "CREATE TABLE editor_tabs("
    " id TEXT PRIMARY KEY NOT NULL,"
    " connection_id TEXT REFERENCES connections(id) ON DELETE SET NULL,"
    " title TEXT NOT NULL,"
    " text TEXT NOT NULL,"
    " cursor_position INTEGER NOT NULL,"
    " position INTEGER NOT NULL,"
    " updated_at TEXT NOT NULL)",
    "ALTER TABLE connections ADD COLUMN record_history INTEGER NOT NULL DEFAULT 1",
    // History plus its external-content FTS5 index (kept in sync by triggers) and saved queries.
    "CREATE TABLE query_history("
    " id INTEGER PRIMARY KEY,"
    " connection_id TEXT REFERENCES connections(id) ON DELETE CASCADE,"
    " sql TEXT NOT NULL,"
    " executed_at TEXT NOT NULL,"
    " duration_ms INTEGER NOT NULL,"
    " rows INTEGER NOT NULL,"
    " success INTEGER NOT NULL,"
    " error_message TEXT NOT NULL DEFAULT '');"
    "CREATE INDEX query_history_connection ON query_history(connection_id, executed_at);"
    "CREATE INDEX query_history_executed ON query_history(executed_at, id);"
    "CREATE VIRTUAL TABLE query_history_fts USING fts5(sql, content='query_history',"
    " content_rowid='id');"
    "CREATE TRIGGER query_history_ai AFTER INSERT ON query_history BEGIN"
    " INSERT INTO query_history_fts(rowid, sql) VALUES(new.id, new.sql); END;"
    "CREATE TRIGGER query_history_ad AFTER DELETE ON query_history BEGIN"
    " INSERT INTO query_history_fts(query_history_fts, rowid, sql)"
    " VALUES('delete', old.id, old.sql); END;"
    "CREATE TRIGGER query_history_au AFTER UPDATE ON query_history BEGIN"
    " INSERT INTO query_history_fts(query_history_fts, rowid, sql)"
    " VALUES('delete', old.id, old.sql);"
    " INSERT INTO query_history_fts(rowid, sql) VALUES(new.id, new.sql); END;"
    "CREATE TABLE saved_folders("
    " id TEXT PRIMARY KEY NOT NULL,"
    " name TEXT NOT NULL,"
    " position INTEGER NOT NULL);"
    "CREATE TABLE saved_queries("
    " id TEXT PRIMARY KEY NOT NULL,"
    " folder_id TEXT REFERENCES saved_folders(id) ON DELETE SET NULL,"
    " connection_id TEXT REFERENCES connections(id) ON DELETE SET NULL,"
    " name TEXT NOT NULL,"
    " sql TEXT NOT NULL,"
    " position INTEGER NOT NULL,"
    " created_at TEXT NOT NULL,"
    " updated_at TEXT NOT NULL)",
    // H12: link of a connection to an external project's migrations. Its own table so it never
    // travels with the connection; deleted with it.
    "CREATE TABLE migration_settings("
    " connection_id TEXT PRIMARY KEY NOT NULL REFERENCES connections(id) ON DELETE CASCADE,"
    " kind TEXT NOT NULL,"
    " folder TEXT NOT NULL,"
    " table_name TEXT NOT NULL DEFAULT '',"
    " updated_at TEXT NOT NULL)",
    // H12 phase C: the php executable and the project root (the folder with `artisan`) used to run
    // migrations. Empty = php from the PATH / two levels above the migrations folder.
    "ALTER TABLE migration_settings ADD COLUMN php_path TEXT NOT NULL DEFAULT '';"
    "ALTER TABLE migration_settings ADD COLUMN project_root TEXT NOT NULL DEFAULT ''",
    // H12: the settings are per database of a connection, not per connection. SQLite cannot change
    // a primary key in place, so the table is rebuilt; every row keeps database = ''.
    "CREATE TABLE migration_settings_new("
    " connection_id TEXT NOT NULL REFERENCES connections(id) ON DELETE CASCADE,"
    " database TEXT NOT NULL DEFAULT '',"
    " kind TEXT NOT NULL,"
    " folder TEXT NOT NULL,"
    " table_name TEXT NOT NULL DEFAULT '',"
    " updated_at TEXT NOT NULL,"
    " php_path TEXT NOT NULL DEFAULT '',"
    " project_root TEXT NOT NULL DEFAULT '',"
    " PRIMARY KEY(connection_id, database));"
    "INSERT INTO migration_settings_new(connection_id, database, kind, folder, table_name,"
    " updated_at, php_path, project_root)"
    " SELECT connection_id, '', kind, folder, table_name, updated_at, php_path, project_root"
    " FROM migration_settings;"
    "DROP TABLE migration_settings;"
    "ALTER TABLE migration_settings_new RENAME TO migration_settings",
    // H13 phase B: per-connection settings of the Claude Code integration. Its own table so it
    // never travels with the connection; deleted with it. The two lists are JSON arrays of strings.
    // No row = the defaults of AiConnectionSettings (AI off).
    "CREATE TABLE ai_settings("
    " connection_id TEXT PRIMARY KEY NOT NULL REFERENCES connections(id) ON DELETE CASCADE,"
    " enabled INTEGER NOT NULL DEFAULT 0,"
    " excluded_schemas TEXT NOT NULL DEFAULT '',"
    " excluded_tables TEXT NOT NULL DEFAULT '',"
    " send_comments INTEGER NOT NULL DEFAULT 0,"
    " send_defaults INTEGER NOT NULL DEFAULT 0,"
    " send_checks INTEGER NOT NULL DEFAULT 0,"
    " send_enum_values INTEGER NOT NULL DEFAULT 0,"
    " send_definitions INTEGER NOT NULL DEFAULT 0,"
    " mask_literals INTEGER NOT NULL DEFAULT 1,"
    " mask_numbers INTEGER NOT NULL DEFAULT 0,"
    " max_tables INTEGER NOT NULL DEFAULT 40)",
};

QString tr(const char *text)
{
    return QCoreApplication::translate("quarry::AppDatabase", text);
}

} // namespace

AppDatabase::AppDatabase(QString path)
    : m_path(std::move(path))
{ }

AppDatabase::~AppDatabase()
{
    close();
}

void AppDatabase::close()
{
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

Error AppDatabase::error(int rc, const QString &context) const
{
    const QString detail = QStringLiteral("%1 (SQLite %2: %3) [%4]")
                               .arg(context)
                               .arg(rc)
                               .arg(m_db ? QString::fromUtf8(sqlite3_errmsg(m_db))
                                         : QString::fromUtf8(sqlite3_errstr(rc)),
                                    m_path);
    switch (rc & 0xff) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return { ErrorKind::Connection,
                 tr("Quarry's database is in use by another process. Try again in a moment."),
                 detail,
                 {},
                 {} };
    case SQLITE_NOTADB:
    case SQLITE_CORRUPT:
        return { ErrorKind::Driver,
                 tr("Quarry's database file is damaged or is not a database."),
                 detail,
                 {},
                 {} };
    case SQLITE_CANTOPEN:
    case SQLITE_PERM:
    case SQLITE_READONLY:
    case SQLITE_AUTH:
        return { ErrorKind::Permission,
                 tr("Quarry cannot read or write its database file."),
                 detail,
                 {},
                 {} };
    default:
        return { ErrorKind::Internal, tr("Quarry could not access its database."), detail, {}, {} };
    }
}

bool AppDatabase::exec(const char *sql, Error &error)
{
    char *message = nullptr;
    const int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        error = this->error(rc, QString::fromUtf8(sql));
        sqlite3_free(message);
        return false;
    }
    return true;
}

bool AppDatabase::readVersion(int &version, Error &error)
{
    sqlite3_stmt *raw = nullptr;
    int rc = sqlite3_prepare_v2(m_db, "PRAGMA user_version", -1, &raw, nullptr);
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, &sqlite3_finalize);
    if (rc == SQLITE_OK)
        rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_ROW) {
        error = this->error(rc, QStringLiteral("PRAGMA user_version"));
        return false;
    }
    version = sqlite3_column_int(stmt.get(), 0);
    return true;
}

bool AppDatabase::migrate(Error &error)
{
    const auto tooNew = [&](int version) {
        error = { ErrorKind::Unsupported,
                  tr("This database was created by a newer version of Quarry."),
                  QStringLiteral("Database version %1; this version of Quarry understands up to "
                                 "%2. [%3]")
                      .arg(version)
                      .arg(kLatestVersion)
                      .arg(m_path),
                  {},
                  {} };
        return false;
    };

    int version = 0;
    if (!readVersion(version, error))
        return false;
    if (version > kLatestVersion)
        return tooNew(version);

    // Header-touching pragma only after the version check, so a newer database stays untouched.
    // Switching to WAL can report BUSY without waiting when another opener does the same.
    for (int attempt = 0;; ++attempt) {
        if (exec("PRAGMA journal_mode=WAL", error))
            break;
        if (attempt == 40 || (sqlite3_errcode(m_db) & 0xff) != SQLITE_BUSY)
            return false;
        QThread::msleep(25);
    }
    if (version == kLatestVersion)
        return true;

    if (!exec("BEGIN IMMEDIATE", error))
        return false;
    // Another opener may have migrated while this one waited for the write lock.
    const auto rollback = [&] { sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr); };
    if (!readVersion(version, error)) {
        rollback();
        return false;
    }
    if (version > kLatestVersion) {
        rollback();
        return tooNew(version);
    }
    for (int v = version; v < kLatestVersion; ++v) {
        if (!exec(kMigrations[v], error)) {
            rollback();
            return false;
        }
    }
    const QByteArray setVersion = "PRAGMA user_version=" + QByteArray::number(kLatestVersion);
    if (!exec(setVersion.constData(), error) || !exec("COMMIT", error)) {
        rollback();
        return false;
    }
    return true;
}

bool AppDatabase::open(Error &error)
{
    if (m_db)
        return true;

    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        error = { ErrorKind::Permission,
                  tr("Quarry cannot read or write its database file."),
                  QStringLiteral("Could not create the folder of %1.").arg(m_path),
                  {},
                  {} };
        return false;
    }
    const int rc = sqlite3_open_v2(m_path.toUtf8().constData(), &m_db,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                                   nullptr);
    if (rc != SQLITE_OK) {
        error = this->error(rc, QStringLiteral("open"));
        close();
        return false;
    }
    sqlite3_busy_timeout(m_db, kBusyTimeoutMs);
    if (!exec("PRAGMA foreign_keys=ON", error) || !migrate(error)) {
        close();
        return false;
    }
    return true;
}

} // namespace quarry
