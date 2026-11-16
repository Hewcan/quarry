#pragma once

#include "app_database.h"

#include <quarry/query_history_store.h>

#include <QObject>

namespace quarry {

// Private to QueryHistoryStore. Lives on the store thread and owns the sqlite3 handle.
class QueryHistoryStoreWorker : public QObject
{
    Q_OBJECT

public:
    explicit QueryHistoryStoreWorker(QString databasePath);

    void record(const HistoryEntry &entry);
    void search(const HistoryQuery &query);
    void clear(const std::optional<QUuid> &connectionId);
    void remove(qint64 id);
    void closeDatabase();

    // Builds the FTS5 MATCH expression for free text; empty when the text has no searchable word.
    static QString matchExpression(const QString &text);

signals:
    void recorded(quarry::HistoryEntry entry);
    void searched(quint64 requestId, QList<quarry::HistoryEntry> entries, bool hasMore);
    void cleared(std::optional<QUuid> connectionId, qint64 deletedCount);
    void removed(qint64 id);
    void failed(quarry::Error error);

private:
    AppDatabase m_database;
};

} // namespace quarry
