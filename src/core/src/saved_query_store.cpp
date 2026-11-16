#include "saved_query_store_worker.h"

#include <QLoggingCategory>
#include <QThread>

namespace quarry {

Q_LOGGING_CATEGORY(lcSavedQueryStore, "quarry.savedquerystore")

namespace {
constexpr int kShutdownTimeoutMs = 5000;
}

SavedQueryStore::SavedQueryStore(QString databasePath, QObject *parent)
    : QObject(parent)
    , m_thread(new QThread) // no parent: must outlive this object if the join times out
{
    qRegisterMetaType<quarry::Error>();
    qRegisterMetaType<quarry::SavedFolder>();
    qRegisterMetaType<QList<quarry::SavedFolder>>();
    qRegisterMetaType<quarry::SavedQuery>();
    qRegisterMetaType<QList<quarry::SavedQuery>>();

    m_worker = new SavedQueryStoreWorker(std::move(databasePath));
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    const auto q = Qt::QueuedConnection;
    connect(m_worker, &SavedQueryStoreWorker::loaded, this, &SavedQueryStore::loaded, q);
    connect(m_worker, &SavedQueryStoreWorker::folderSaved, this, &SavedQueryStore::folderSaved, q);
    connect(m_worker, &SavedQueryStoreWorker::folderRemoved, this, &SavedQueryStore::folderRemoved,
            q);
    connect(m_worker, &SavedQueryStoreWorker::querySaved, this, &SavedQueryStore::querySaved, q);
    connect(m_worker, &SavedQueryStoreWorker::queryRemoved, this, &SavedQueryStore::queryRemoved,
            q);
    connect(m_worker, &SavedQueryStoreWorker::failed, this, &SavedQueryStore::failed, q);
    m_thread->start();
}

SavedQueryStore::~SavedQueryStore()
{
    // Queued after every pending operation, so nothing already requested is lost.
    post([w = m_worker] {
        w->closeDatabase();
        QThread::currentThread()->quit();
    });
    if (m_thread->wait(kShutdownTimeoutMs)) {
        delete m_thread;
    } else {
        qCWarning(lcSavedQueryStore) << "Store thread did not stop within" << kShutdownTimeoutMs
                                     << "ms; leaking it until the disk call returns";
        connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    }
}

template<typename F> void SavedQueryStore::post(F &&task)
{
    QMetaObject::invokeMethod(m_worker, std::forward<F>(task), Qt::QueuedConnection);
}

void SavedQueryStore::load()
{
    post([w = m_worker] { w->load(); });
}

void SavedQueryStore::saveFolder(SavedFolder folder)
{
    post([w = m_worker, folder = std::move(folder)] { w->saveFolder(folder); });
}

void SavedQueryStore::removeFolder(QUuid id)
{
    post([w = m_worker, id] { w->removeFolder(id); });
}

void SavedQueryStore::saveQuery(SavedQuery query)
{
    post([w = m_worker, query = std::move(query)] { w->saveQuery(query); });
}

void SavedQueryStore::removeQuery(QUuid id)
{
    post([w = m_worker, id] { w->removeQuery(id); });
}

} // namespace quarry
