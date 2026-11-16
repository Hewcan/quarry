#pragma once

#include <quarry/result.h>
#include <quarry/saved_folder.h>
#include <quarry/saved_query.h>
// Declares the Q_DECLARE_METATYPE for quarry::Error used by failed().
#include <quarry/session.h>

#include <QList>
#include <QObject>

class QThread;

namespace quarry {

class SavedQueryStoreWorker;

// UI-thread facade over `saved_folders` and `saved_queries` in quarry.db. Same threading model as
// EditorTabStore.
class SavedQueryStore : public QObject
{
    Q_OBJECT

public:
    explicit SavedQueryStore(QString databasePath, QObject *parent = nullptr);
    ~SavedQueryStore() override;

public slots:
    // -> loaded(folders by position then name, queries by folder, position, name) | failed(Error)
    void load();
    // Insert or update by id. Empty name -> ErrorKind::Constraint. -> folderSaved(folder as stored)
    void saveFolder(quarry::SavedFolder folder);
    // Queries of the folder move to the top level; they are never deleted with it.
    // -> folderRemoved(id, queries moved to top level) (also if it did not exist) | failed(Error)
    void removeFolder(QUuid id);
    // Insert or update by id. Empty name, or a folderId / connectionId that matches nothing, ->
    // ErrorKind::Constraint. Deleting a connection clears connectionId. updatedAt is ignored.
    // -> querySaved(query as stored) | failed(Error)
    void saveQuery(quarry::SavedQuery query);
    void removeQuery(QUuid id); // -> queryRemoved(id) (also if it did not exist) | failed(Error)

signals:
    void loaded(QList<quarry::SavedFolder> folders, QList<quarry::SavedQuery> queries);
    void folderSaved(quarry::SavedFolder folder);
    void folderRemoved(QUuid id, QList<quarry::SavedQuery> movedQueries);
    void querySaved(quarry::SavedQuery query);
    void queryRemoved(QUuid id);
    void failed(quarry::Error error);

private:
    template<typename F> void post(F &&task);

    QThread *m_thread = nullptr;
    SavedQueryStoreWorker *m_worker = nullptr; // lives on m_thread, deleted there
};

} // namespace quarry
