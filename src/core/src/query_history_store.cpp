#include "query_history_store_worker.h"

#include <QLoggingCategory>
#include <QThread>

namespace quarry {

Q_LOGGING_CATEGORY(lcQueryHistoryStore, "quarry.queryhistorystore")

namespace {
constexpr int kShutdownTimeoutMs = 5000;
}

QueryHistoryStore::QueryHistoryStore(QString databasePath, QObject *parent)
    : QObject(parent)
    , m_thread(new QThread) // no parent: must outlive this object if the join times out
{
    qRegisterMetaType<quarry::Error>();
    qRegisterMetaType<quarry::HistoryEntry>();
    qRegisterMetaType<QList<quarry::HistoryEntry>>();
    qRegisterMetaType<std::optional<QUuid>>();

    m_worker = new QueryHistoryStoreWorker(std::move(databasePath));
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    const auto q = Qt::QueuedConnection;
    connect(m_worker, &QueryHistoryStoreWorker::recorded, this, &QueryHistoryStore::recorded, q);
    connect(m_worker, &QueryHistoryStoreWorker::searched, this, &QueryHistoryStore::searched, q);
    connect(m_worker, &QueryHistoryStoreWorker::cleared, this, &QueryHistoryStore::cleared, q);
    connect(m_worker, &QueryHistoryStoreWorker::removed, this, &QueryHistoryStore::removed, q);
    connect(m_worker, &QueryHistoryStoreWorker::failed, this, &QueryHistoryStore::failed, q);
    m_thread->start();
}

QueryHistoryStore::~QueryHistoryStore()
{
    // Queued after every pending operation, so nothing already requested is lost.
    post([w = m_worker] {
        w->closeDatabase();
        QThread::currentThread()->quit();
    });
    if (m_thread->wait(kShutdownTimeoutMs)) {
        delete m_thread;
    } else {
        qCWarning(lcQueryHistoryStore) << "Store thread did not stop within" << kShutdownTimeoutMs
                                       << "ms; leaking it until the disk call returns";
        connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    }
}

template<typename F> void QueryHistoryStore::post(F &&task)
{
    QMetaObject::invokeMethod(m_worker, std::forward<F>(task), Qt::QueuedConnection);
}

void QueryHistoryStore::record(HistoryEntry entry)
{
    post([w = m_worker, entry = std::move(entry)] { w->record(entry); });
}

void QueryHistoryStore::search(HistoryQuery query)
{
    post([w = m_worker, query = std::move(query)] { w->search(query); });
}

void QueryHistoryStore::clear(std::optional<QUuid> connectionId)
{
    post([w = m_worker, connectionId] { w->clear(connectionId); });
}

void QueryHistoryStore::remove(qint64 id)
{
    post([w = m_worker, id] { w->remove(id); });
}

} // namespace quarry
