#include "saved_query_store_worker.h"

#include <memory>
#include <sqlite3.h>

namespace quarry {

namespace {

constexpr const char *kQueryColumns = "q.id, q.folder_id, q.connection_id, q.name, q.sql,"
                                      " q.position, q.created_at, q.updated_at";

using Stmt = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

QString columnText(sqlite3_stmt *stmt, int column)
{
    return QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(stmt, column)),
                             sqlite3_column_bytes(stmt, column));
}

// Same text format as connections.id, or the foreign key would never match.
QString idText(const QUuid &id)
{
    return id.toString(QUuid::WithoutBraces);
}

void bindText(sqlite3_stmt *stmt, int index, const QString &text)
{
    const QByteArray utf8 = text.toUtf8();
    sqlite3_bind_text(stmt, index, utf8.constData(), static_cast<int>(utf8.size()),
                      SQLITE_TRANSIENT);
}

void bindOptionalId(sqlite3_stmt *stmt, int index, const std::optional<QUuid> &id)
{
    if (id.has_value())
        bindText(stmt, index, idText(id.value()));
    else
        sqlite3_bind_null(stmt, index);
}

QDateTime fromIso(const QString &text)
{
    return QDateTime::fromString(text, Qt::ISODateWithMs).toUTC();
}

SavedQuery readQuery(sqlite3_stmt *stmt)
{
    SavedQuery q;
    q.id = QUuid::fromString(columnText(stmt, 0));
    if (sqlite3_column_type(stmt, 1) != SQLITE_NULL)
        q.folderId = QUuid::fromString(columnText(stmt, 1));
    if (sqlite3_column_type(stmt, 2) != SQLITE_NULL)
        q.connectionId = QUuid::fromString(columnText(stmt, 2));
    q.name = columnText(stmt, 3);
    q.sql = columnText(stmt, 4);
    q.position = sqlite3_column_int(stmt, 5);
    q.createdAt = fromIso(columnText(stmt, 6));
    q.updatedAt = fromIso(columnText(stmt, 7));
    return q;
}

Stmt prepare(sqlite3 *db, const QByteArray &sql, int &rc)
{
    sqlite3_stmt *raw = nullptr;
    rc = sqlite3_prepare_v2(db, sql.constData(), -1, &raw, nullptr);
    return { raw, &sqlite3_finalize };
}

// Folders by position then name; queries grouped top level first, then by folder order.
constexpr const char *kQueryOrder = " ORDER BY q.folder_id IS NOT NULL, f.position,"
                                    " f.name COLLATE NOCASE, q.position, q.name COLLATE NOCASE,"
                                    " q.id";

} // namespace

SavedQueryStoreWorker::SavedQueryStoreWorker(QString databasePath)
    : m_database(std::move(databasePath))
{ }

void SavedQueryStoreWorker::closeDatabase()
{
    m_database.close();
}

bool SavedQueryStoreWorker::selectQueries(const char *where, const QUuid *param,
                                          QList<SavedQuery> &out, Error &error)
{
    const QByteArray sql = QByteArray("SELECT ") + kQueryColumns
        + " FROM saved_queries q LEFT JOIN saved_folders f ON f.id=q.folder_id WHERE " + where
        + kQueryOrder;
    int rc = SQLITE_OK;
    Stmt stmt = prepare(m_database.handle(), sql, rc);
    if (rc == SQLITE_OK) {
        if (param)
            bindText(stmt.get(), 1, idText(*param));
        while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW)
            out.append(readQuery(stmt.get()));
    }
    if (rc != SQLITE_DONE) {
        error = m_database.error(rc, QStringLiteral("select"));
        return false;
    }
    return true;
}

void SavedQueryStoreWorker::load()
{
    Error error;
    if (!m_database.open(error)) {
        emit failed(error);
        return;
    }

    QList<SavedFolder> folders;
    int rc = SQLITE_OK;
    {
        Stmt stmt = prepare(m_database.handle(),
                            "SELECT id, name, position FROM saved_folders"
                            " ORDER BY position, name COLLATE NOCASE, id",
                            rc);
        if (rc == SQLITE_OK) {
            while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
                SavedFolder f;
                f.id = QUuid::fromString(columnText(stmt.get(), 0));
                f.name = columnText(stmt.get(), 1);
                f.position = sqlite3_column_int(stmt.get(), 2);
                folders.append(f);
            }
        }
        if (rc != SQLITE_DONE) {
            emit failed(m_database.error(rc, QStringLiteral("load")));
            return;
        }
    }
    QList<SavedQuery> queries;
    if (!selectQueries("1=1", nullptr, queries, error)) {
        emit failed(error);
        return;
    }
    emit loaded(folders, queries);
}

void SavedQueryStoreWorker::saveFolder(const SavedFolder &input)
{
    if (input.name.trimmed().isEmpty()) {
        emit failed({ ErrorKind::Constraint, tr("Enter a name for the folder."), {}, {}, {} });
        return;
    }
    Error error;
    if (!m_database.open(error)) {
        emit failed(error);
        return;
    }
    SavedFolder f = input;
    if (f.id.isNull())
        f.id = QUuid::createUuid();

    int rc = SQLITE_OK;
    Stmt stmt = prepare(m_database.handle(),
                        "INSERT INTO saved_folders(id, name, position) VALUES(?1,?2,?3)"
                        " ON CONFLICT(id) DO UPDATE SET name=excluded.name,"
                        " position=excluded.position",
                        rc);
    if (rc == SQLITE_OK) {
        bindText(stmt.get(), 1, idText(f.id));
        bindText(stmt.get(), 2, f.name);
        sqlite3_bind_int(stmt.get(), 3, f.position);
        rc = sqlite3_step(stmt.get());
    }
    if (rc != SQLITE_DONE) {
        emit failed(m_database.error(rc, QStringLiteral("saveFolder")));
        return;
    }
    emit folderSaved(f);
}

void SavedQueryStoreWorker::removeFolder(const QUuid &id)
{
    Error error;
    if (!m_database.open(error) || !m_database.exec("BEGIN IMMEDIATE", error)) {
        emit failed(error);
        return;
    }
    sqlite3 *db = m_database.handle();
    const auto rollback = [&] { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); };

    // Read first: after the move the queries no longer match the folder.
    QList<SavedQuery> moved;
    if (!selectQueries("q.folder_id=?1", &id, moved, error)) {
        rollback();
        emit failed(error);
        return;
    }
    for (SavedQuery &q : moved)
        q.folderId.reset();

    // Moving explicitly (instead of relying on ON DELETE SET NULL) keeps this step visible.
    constexpr const char *kStatements[] = { "UPDATE saved_queries SET folder_id=NULL"
                                            " WHERE folder_id=?1",
                                            "DELETE FROM saved_folders WHERE id=?1" };
    for (const char *sql : kStatements) {
        int rc = SQLITE_OK;
        Stmt stmt = prepare(db, sql, rc);
        if (rc == SQLITE_OK) {
            bindText(stmt.get(), 1, idText(id));
            rc = sqlite3_step(stmt.get());
        }
        if (rc != SQLITE_DONE) {
            const Error err = m_database.error(rc, QStringLiteral("removeFolder"));
            rollback();
            emit failed(err);
            return;
        }
    }
    if (!m_database.exec("COMMIT", error)) {
        rollback();
        emit failed(error);
        return;
    }
    emit folderRemoved(id, moved);
}

void SavedQueryStoreWorker::saveQuery(const SavedQuery &input)
{
    if (input.name.trimmed().isEmpty()) {
        emit failed({ ErrorKind::Constraint, tr("Enter a name for the query."), {}, {}, {} });
        return;
    }
    Error error;
    if (!m_database.open(error) || !m_database.exec("BEGIN IMMEDIATE", error)) {
        emit failed(error);
        return;
    }
    sqlite3 *db = m_database.handle();
    const auto rollback = [&] { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); };
    const auto fail = [&](const Error &err) {
        rollback();
        emit failed(err);
    };

    SavedQuery q = input;
    if (q.id.isNull())
        q.id = QUuid::createUuid();

    // Checked here (not left to the foreign key) so the message says which reference is gone.
    const auto exists = [&](const char *sql, const QUuid &key, bool &found) {
        int rc = SQLITE_OK;
        Stmt stmt = prepare(db, sql, rc);
        if (rc == SQLITE_OK) {
            bindText(stmt.get(), 1, idText(key));
            rc = sqlite3_step(stmt.get());
        }
        found = rc == SQLITE_ROW;
        return rc == SQLITE_ROW || rc == SQLITE_DONE ? SQLITE_OK : rc;
    };
    bool found = true;
    if (q.folderId.has_value()) {
        const int rc = exists("SELECT 1 FROM saved_folders WHERE id=?1", q.folderId.value(), found);
        if (rc != SQLITE_OK) {
            fail(m_database.error(rc, QStringLiteral("saveQuery")));
            return;
        }
        if (!found) {
            fail({ ErrorKind::Constraint,
                   tr("The folder of this query no longer exists."),
                   idText(q.folderId.value()),
                   {},
                   {} });
            return;
        }
    }
    if (q.connectionId.has_value()) {
        const int rc
            = exists("SELECT 1 FROM connections WHERE id=?1", q.connectionId.value(), found);
        if (rc != SQLITE_OK) {
            fail(m_database.error(rc, QStringLiteral("saveQuery")));
            return;
        }
        if (!found) {
            fail({ ErrorKind::Constraint,
                   tr("The connection of this query no longer exists."),
                   idText(q.connectionId.value()),
                   {},
                   {} });
            return;
        }
    }

    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    int rc = SQLITE_OK;
    {
        Stmt stmt = prepare(db,
                            "INSERT INTO saved_queries(id, folder_id, connection_id, name, sql,"
                            " position, created_at, updated_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?7)"
                            " ON CONFLICT(id) DO UPDATE SET folder_id=excluded.folder_id,"
                            " connection_id=excluded.connection_id, name=excluded.name,"
                            " sql=excluded.sql, position=excluded.position,"
                            " updated_at=excluded.updated_at",
                            rc);
        if (rc == SQLITE_OK) {
            bindText(stmt.get(), 1, idText(q.id));
            bindOptionalId(stmt.get(), 2, q.folderId);
            bindOptionalId(stmt.get(), 3, q.connectionId);
            bindText(stmt.get(), 4, q.name);
            bindText(stmt.get(), 5, q.sql);
            sqlite3_bind_int(stmt.get(), 6, q.position);
            bindText(stmt.get(), 7, now);
            rc = sqlite3_step(stmt.get());
        }
        if (rc != SQLITE_DONE) {
            fail(m_database.error(rc, QStringLiteral("saveQuery")));
            return;
        }
    }

    QList<SavedQuery> stored;
    if (!selectQueries("q.id=?1", &q.id, stored, error)) {
        fail(error);
        return;
    }
    if (stored.isEmpty() || !m_database.exec("COMMIT", error)) {
        fail(stored.isEmpty() ? m_database.error(SQLITE_ERROR, QStringLiteral("saveQuery"))
                              : error);
        return;
    }
    emit querySaved(stored.first());
}

void SavedQueryStoreWorker::removeQuery(const QUuid &id)
{
    Error error;
    if (!m_database.open(error)) {
        emit failed(error);
        return;
    }
    int rc = SQLITE_OK;
    Stmt stmt = prepare(m_database.handle(), "DELETE FROM saved_queries WHERE id=?1", rc);
    if (rc == SQLITE_OK) {
        bindText(stmt.get(), 1, idText(id));
        rc = sqlite3_step(stmt.get());
    }
    if (rc != SQLITE_DONE) {
        emit failed(m_database.error(rc, QStringLiteral("removeQuery")));
        return;
    }
    emit queryRemoved(id);
}

} // namespace quarry
