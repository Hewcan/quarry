#include "query_history_store_worker.h"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <memory>
#include <sqlite3.h>

namespace quarry {

namespace {

constexpr int kMaxLimit = 1000;

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

HistoryEntry readRow(sqlite3_stmt *stmt)
{
    HistoryEntry e;
    e.id = sqlite3_column_int64(stmt, 0);
    if (sqlite3_column_type(stmt, 1) != SQLITE_NULL)
        e.connectionId = QUuid::fromString(columnText(stmt, 1));
    e.sql = columnText(stmt, 2);
    e.executedAt = QDateTime::fromString(columnText(stmt, 3), Qt::ISODateWithMs).toUTC();
    e.durationMs = sqlite3_column_int64(stmt, 4);
    e.rows = sqlite3_column_int64(stmt, 5);
    e.success = sqlite3_column_int(stmt, 6) != 0;
    e.errorMessage = columnText(stmt, 7);
    return e;
}

Stmt prepare(sqlite3 *db, const QByteArray &sql, int &rc)
{
    sqlite3_stmt *raw = nullptr;
    rc = sqlite3_prepare_v2(db, sql.constData(), -1, &raw, nullptr);
    return { raw, &sqlite3_finalize };
}

} // namespace

QueryHistoryStoreWorker::QueryHistoryStoreWorker(QString databasePath)
    : m_database(std::move(databasePath))
{ }

void QueryHistoryStoreWorker::closeDatabase()
{
    m_database.close();
}

QString QueryHistoryStoreWorker::matchExpression(const QString &text)
{
    // Every word becomes one FTS5 string, so user input can never act as an FTS operator.
    QStringList terms;
    const QStringList words
        = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    terms.reserve(words.size());
    for (const QString &word : words) {
        const bool searchable
            = std::any_of(word.cbegin(), word.cend(), [](QChar c) { return c.isLetterOrNumber(); });
        if (!searchable) // would tokenize to nothing
            continue;
        QString quoted = word;
        quoted.replace(QLatin1Char('"'), QLatin1String("\"\""));
        terms.append(QLatin1Char('"') + quoted + QLatin1Char('"'));
    }
    if (terms.isEmpty())
        return {};
    terms.last() += QLatin1Char('*');
    return terms.join(QLatin1Char(' '));
}

void QueryHistoryStoreWorker::record(const HistoryEntry &input)
{
    if (input.sql.trimmed().isEmpty()) {
        emit failed({ ErrorKind::Constraint,
                      tr("There is no statement to add to the history."),
                      {},
                      {},
                      {} });
        return;
    }
    Error error;
    if (!m_database.open(error)) {
        emit failed(error);
        return;
    }
    sqlite3 *db = m_database.handle();

    HistoryEntry e = input;
    if (!e.executedAt.isValid())
        e.executedAt = QDateTime::currentDateTimeUtc();
    e.executedAt = e.executedAt.toUTC();

    if (!m_database.exec("BEGIN IMMEDIATE", error)) {
        emit failed(error);
        return;
    }
    const auto rollback = [&] { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); };
    const auto fail = [&](int rc, const char *context) {
        const Error err = m_database.error(rc, QString::fromUtf8(context));
        rollback();
        emit failed(err);
    };

    int rc = SQLITE_OK;
    if (e.connectionId.has_value()) {
        Stmt check = prepare(db, "SELECT record_history FROM connections WHERE id=?1", rc);
        if (rc == SQLITE_OK) {
            bindText(check.get(), 1, idText(e.connectionId.value()));
            rc = sqlite3_step(check.get());
        }
        if (rc == SQLITE_DONE) {
            rollback();
            emit failed({ ErrorKind::Constraint,
                          tr("The connection of this history entry no longer exists."),
                          idText(e.connectionId.value()),
                          {},
                          {} });
            return;
        }
        if (rc != SQLITE_ROW) {
            fail(rc, "record");
            return;
        }
        const bool recordHistory = sqlite3_column_int(check.get(), 0) != 0;
        check.reset();
        if (!recordHistory) {
            rollback();
            return;
        }
    }

    {
        Stmt insert = prepare(db,
                              "INSERT INTO query_history(connection_id, sql, executed_at,"
                              " duration_ms, rows, success, error_message)"
                              " VALUES(?1,?2,?3,?4,?5,?6,?7)",
                              rc);
        if (rc == SQLITE_OK) {
            bindOptionalId(insert.get(), 1, e.connectionId);
            bindText(insert.get(), 2, e.sql);
            bindText(insert.get(), 3, e.executedAt.toString(Qt::ISODateWithMs));
            sqlite3_bind_int64(insert.get(), 4, e.durationMs);
            sqlite3_bind_int64(insert.get(), 5, e.rows);
            sqlite3_bind_int(insert.get(), 6, e.success ? 1 : 0);
            bindText(insert.get(), 7, e.errorMessage);
            rc = sqlite3_step(insert.get());
        }
        if (rc != SQLITE_DONE) {
            fail(rc, "record");
            return;
        }
        e.id = sqlite3_last_insert_rowid(db);
    }

    {
        Stmt count = prepare(db, "SELECT count(*) FROM query_history", rc);
        if (rc == SQLITE_OK)
            rc = sqlite3_step(count.get());
        if (rc != SQLITE_ROW) {
            fail(rc, "trim");
            return;
        }
        const qint64 excess = sqlite3_column_int64(count.get(), 0) - QueryHistoryStore::kMaxEntries;
        count.reset();
        if (excess > 0) {
            Stmt trim = prepare(db,
                                "DELETE FROM query_history WHERE id IN (SELECT id FROM"
                                " query_history ORDER BY executed_at, id LIMIT ?1)",
                                rc);
            if (rc == SQLITE_OK) {
                sqlite3_bind_int64(trim.get(), 1, excess);
                rc = sqlite3_step(trim.get());
            }
            if (rc != SQLITE_DONE) {
                fail(rc, "trim");
                return;
            }
        }
    }

    if (!m_database.exec("COMMIT", error)) {
        rollback();
        emit failed(error);
        return;
    }
    emit recorded(e);
}

void QueryHistoryStoreWorker::search(const HistoryQuery &query)
{
    Error error;
    if (!m_database.open(error)) {
        emit failed(error);
        return;
    }

    const int limit = std::clamp(query.limit, 1, kMaxLimit);
    const int offset = std::max(query.offset, 0);
    const QString match = matchExpression(query.text);

    QByteArray sql = "SELECT id, connection_id, sql, executed_at, duration_ms, rows, success,"
                     " error_message FROM query_history WHERE 1=1";
    if (!match.isEmpty())
        sql += " AND id IN (SELECT rowid FROM query_history_fts WHERE query_history_fts MATCH ?)";
    if (query.connectionId.has_value())
        sql += " AND connection_id=?";
    if (query.onlyErrors)
        sql += " AND success=0";
    sql += " ORDER BY executed_at DESC, id DESC LIMIT ? OFFSET ?";

    int rc = SQLITE_OK;
    Stmt stmt = prepare(m_database.handle(), sql, rc);
    QList<HistoryEntry> list;
    bool hasMore = false;
    if (rc == SQLITE_OK) {
        int index = 0;
        if (!match.isEmpty())
            bindText(stmt.get(), ++index, match);
        if (query.connectionId.has_value())
            bindText(stmt.get(), ++index, idText(query.connectionId.value()));
        sqlite3_bind_int(stmt.get(), ++index, limit + 1); // one extra row tells hasMore
        sqlite3_bind_int(stmt.get(), ++index, offset);
        list.reserve(limit);
        while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW)
            list.append(readRow(stmt.get()));
    }
    if (rc != SQLITE_DONE) {
        emit failed(m_database.error(rc, QStringLiteral("search")));
        return;
    }
    if (list.size() > limit) {
        hasMore = true;
        list.resize(limit);
    }
    emit searched(query.requestId, list, hasMore);
}

void QueryHistoryStoreWorker::clear(const std::optional<QUuid> &connectionId)
{
    Error error;
    if (!m_database.open(error)) {
        emit failed(error);
        return;
    }
    int rc = SQLITE_OK;
    Stmt stmt
        = prepare(m_database.handle(),
                  connectionId.has_value() ? "DELETE FROM query_history WHERE connection_id=?1"
                                           : "DELETE FROM query_history",
                  rc);
    if (rc == SQLITE_OK) {
        if (connectionId.has_value())
            bindText(stmt.get(), 1, idText(connectionId.value()));
        rc = sqlite3_step(stmt.get());
    }
    if (rc != SQLITE_DONE) {
        emit failed(m_database.error(rc, QStringLiteral("clear")));
        return;
    }
    // Direct changes only: the FTS triggers' own writes are not counted.
    emit cleared(connectionId, sqlite3_changes64(m_database.handle()));
}

void QueryHistoryStoreWorker::remove(qint64 id)
{
    Error error;
    if (!m_database.open(error)) {
        emit failed(error);
        return;
    }
    int rc = SQLITE_OK;
    Stmt stmt = prepare(m_database.handle(), "DELETE FROM query_history WHERE id=?1", rc);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt.get(), 1, id);
        rc = sqlite3_step(stmt.get());
    }
    if (rc != SQLITE_DONE) {
        emit failed(m_database.error(rc, QStringLiteral("remove")));
        return;
    }
    emit removed(id);
}

} // namespace quarry
