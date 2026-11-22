#include "ai_settings_worker.h"

#include <QJsonArray>
#include <QJsonDocument>

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

// The lists are stored as a JSON array of strings: a name may hold any character, a separator
// would not be safe. A malformed or empty value reads as an empty list.
QString encodeList(const QStringList &list)
{
    return QString::fromUtf8(
        QJsonDocument(QJsonArray::fromStringList(list)).toJson(QJsonDocument::Compact));
}

QStringList decodeList(const QString &text)
{
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    QStringList out;
    if (!doc.isArray())
        return out;
    const QJsonArray array = doc.array();
    out.reserve(array.size());
    for (const auto &value : array) {
        if (value.isString())
            out.append(value.toString());
    }
    return out;
}

} // namespace

AiSettingsWorker::AiSettingsWorker(QString databasePath)
    : m_database(std::move(databasePath))
{ }

void AiSettingsWorker::load()
{
    Error error;
    if (!m_database.open(error)) {
        emit failed(error);
        return;
    }
    sqlite3_stmt *raw = nullptr;
    int rc = sqlite3_prepare_v2(
        m_database.handle(),
        "SELECT connection_id, enabled, excluded_schemas, excluded_tables, send_comments,"
        " send_defaults, send_checks, send_enum_values, send_definitions, mask_literals,"
        " mask_numbers, max_tables FROM ai_settings",
        -1, &raw, nullptr);
    Stmt stmt(raw, &sqlite3_finalize);
    QList<AiConnectionSettings> list;
    if (rc == SQLITE_OK) {
        while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
            sqlite3_stmt *s = stmt.get();
            AiConnectionSettings item;
            item.connectionId = QUuid::fromString(columnText(s, 0));
            item.enabled = sqlite3_column_int(s, 1) != 0;
            item.excludedSchemas = decodeList(columnText(s, 2));
            item.excludedTables = decodeList(columnText(s, 3));
            item.sendComments = sqlite3_column_int(s, 4) != 0;
            item.sendDefaults = sqlite3_column_int(s, 5) != 0;
            item.sendChecks = sqlite3_column_int(s, 6) != 0;
            item.sendEnumValues = sqlite3_column_int(s, 7) != 0;
            item.sendDefinitions = sqlite3_column_int(s, 8) != 0;
            item.maskLiterals = sqlite3_column_int(s, 9) != 0;
            item.maskNumbers = sqlite3_column_int(s, 10) != 0;
            item.maxTables = sqlite3_column_int(s, 11);
            list.append(AiSettingsService::normalized(item));
        }
    }
    if (rc != SQLITE_DONE) {
        emit failed(m_database.error(rc, QStringLiteral("load AI settings")));
        return;
    }
    emit loaded(list);
}

void AiSettingsWorker::save(const AiConnectionSettings &settings)
{
    Error error;
    if (!m_database.open(error)) {
        emit failed(error);
        return;
    }
    const QString id = settings.connectionId.toString(QUuid::WithoutBraces);
    const Error missing { ErrorKind::NotFound, tr("The connection no longer exists."), id, {}, {} };
    if (settings.connectionId.isNull()) {
        emit failed({ ErrorKind::Constraint, tr("Save the connection first."), {}, {}, {} });
        return;
    }

    // Checked first so that deleting the row of a connection that does not exist is an error too.
    sqlite3_stmt *raw = nullptr;
    int rc = sqlite3_prepare_v2(m_database.handle(), "SELECT 1 FROM connections WHERE id=?1", -1,
                                &raw, nullptr);
    {
        const Stmt exists(raw, &sqlite3_finalize);
        if (rc == SQLITE_OK) {
            bindText(exists.get(), 1, id);
            rc = sqlite3_step(exists.get());
        }
    }
    if (rc == SQLITE_DONE) {
        emit failed(missing);
        return;
    }
    if (rc != SQLITE_ROW) {
        emit failed(m_database.error(rc, QStringLiteral("save AI settings")));
        return;
    }

    const bool store = !settings.isDefault();
    const char *sql = store
        ? "INSERT OR REPLACE INTO ai_settings(connection_id, enabled, excluded_schemas,"
          " excluded_tables, send_comments, send_defaults, send_checks, send_enum_values,"
          " send_definitions, mask_literals, mask_numbers, max_tables)"
          " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)"
        : "DELETE FROM ai_settings WHERE connection_id=?1";
    raw = nullptr;
    rc = sqlite3_prepare_v2(m_database.handle(), sql, -1, &raw, nullptr);
    const Stmt stmt(raw, &sqlite3_finalize);
    if (rc == SQLITE_OK) {
        sqlite3_stmt *s = stmt.get();
        bindText(s, 1, id);
        if (store) {
            sqlite3_bind_int(s, 2, settings.enabled ? 1 : 0);
            bindText(s, 3, encodeList(settings.excludedSchemas));
            bindText(s, 4, encodeList(settings.excludedTables));
            sqlite3_bind_int(s, 5, settings.sendComments ? 1 : 0);
            sqlite3_bind_int(s, 6, settings.sendDefaults ? 1 : 0);
            sqlite3_bind_int(s, 7, settings.sendChecks ? 1 : 0);
            sqlite3_bind_int(s, 8, settings.sendEnumValues ? 1 : 0);
            sqlite3_bind_int(s, 9, settings.sendDefinitions ? 1 : 0);
            sqlite3_bind_int(s, 10, settings.maskLiterals ? 1 : 0);
            sqlite3_bind_int(s, 11, settings.maskNumbers ? 1 : 0);
            sqlite3_bind_int(s, 12, settings.maxTables);
        }
        rc = sqlite3_step(s);
    }
    if ((rc & 0xff) == SQLITE_CONSTRAINT) { // the connection was deleted in between
        emit failed(missing);
        return;
    }
    if (rc != SQLITE_DONE) {
        emit failed(m_database.error(rc, QStringLiteral("save AI settings")));
        return;
    }
    emit saved(settings);
}

} // namespace quarry
