#pragma once

#include <quarry/history_entry.h>
#include <quarry/history_query.h>
#include <quarry/result.h>
// Declares the Q_DECLARE_METATYPE for quarry::Error used by failed().
#include <quarry/session.h>

#include <QList>
#include <QObject>

#include <optional>

class QThread;

namespace quarry {

class QueryHistoryStoreWorker;

// UI-thread facade over `query_history` (+ its FTS5 index) in quarry.db. Same threading model as
// EditorTabStore: worker thread, lazy open, asynchronous methods, queued signals in call order.
class QueryHistoryStore : public QObject
{
    Q_OBJECT

public:
    static constexpr int kMaxEntries = 10000; // oldest entries are deleted beyond this

    explicit QueryHistoryStore(QString databasePath, QObject *parent = nullptr);
    ~QueryHistoryStore() override;

public slots:
    // Stores the entry and trims to kMaxEntries. Does nothing (no signal) when the entry's
    // connection has recordHistory == false. A connectionId that matches no connection, or an
    // empty sql, fails with ErrorKind::Constraint. -> recorded(entry as stored) | failed(Error)
    void record(quarry::HistoryEntry entry);
    // -> searched(requestId, entries, hasMore) | failed(Error)
    void search(quarry::HistoryQuery query);
    // Deletes the history of one connection, or all of it when connectionId is nullopt.
    // -> cleared(connectionId, deletedCount) | failed(Error)
    void clear(std::optional<QUuid> connectionId);
    void remove(qint64 id); // -> removed(id) (also if it did not exist) | failed(Error)

signals:
    void recorded(quarry::HistoryEntry entry);
    void searched(quint64 requestId, QList<quarry::HistoryEntry> entries, bool hasMore);
    void cleared(std::optional<QUuid> connectionId, qint64 deletedCount);
    void removed(qint64 id);
    void failed(quarry::Error error);

private:
    template<typename F> void post(F &&task);

    QThread *m_thread = nullptr;
    QueryHistoryStoreWorker *m_worker = nullptr; // lives on m_thread, deleted there
};

} // namespace quarry
