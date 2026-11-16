#pragma once

#include "app_database.h"

#include <quarry/saved_query_store.h>

#include <QObject>

namespace quarry {

// Private to SavedQueryStore. Lives on the store thread and owns the sqlite3 handle.
class SavedQueryStoreWorker : public QObject
{
    Q_OBJECT

public:
    explicit SavedQueryStoreWorker(QString databasePath);

    void load();
    void saveFolder(const SavedFolder &folder);
    void removeFolder(const QUuid &id);
    void saveQuery(const SavedQuery &query);
    void removeQuery(const QUuid &id);
    void closeDatabase();

signals:
    void loaded(QList<quarry::SavedFolder> folders, QList<quarry::SavedQuery> queries);
    void folderSaved(quarry::SavedFolder folder);
    void folderRemoved(QUuid id, QList<quarry::SavedQuery> movedQueries);
    void querySaved(quarry::SavedQuery query);
    void queryRemoved(QUuid id);
    void failed(quarry::Error error);

private:
    // Reads rows of saved_queries matching `where` (a fragment using ?1), ordered for display.
    bool selectQueries(const char *where, const QUuid *param, QList<SavedQuery> &out, Error &error);

    AppDatabase m_database;
};

} // namespace quarry
