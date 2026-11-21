#pragma once

#include "app_database.h"

#include <quarry/cancellation.h>
#include <quarry/migration_source.h>
#include <quarry/session.h>

#include <QList>
#include <QObject>
#include <QStringList>

namespace quarry {

// Lives on MigrationService's thread: quarry.db (`migration_settings`) and the folder scans.
class MigrationServiceWorker : public QObject
{
    Q_OBJECT

public:
    explicit MigrationServiceWorker(QString databasePath);

    void closeDatabase() { m_database.close(); }
    void load();
    void save(const MigrationSettings &settings);
    void scan(quint64 id, const MigrationSettings &settings, const CancellationToken &cancel);
    void readSource(quint64 id, const MigrationSettings &settings, const QString &name);

signals:
    // ok = false: `error` says why and `text` is empty.
    void sourceDone(quint64 id, QUuid connectionId, QString database, QString name, QString text,
                    quarry::Error error, bool ok);
    void loaded(QList<quarry::MigrationSettings> settings);
    void saved(quarry::MigrationSettings settings);
    void failed(quarry::Error error);
    // ok = false: `error` says why and `files` is empty.
    void scanned(quint64 id, QStringList files, quarry::Error error, bool ok);

private:
    AppDatabase m_database;
};

} // namespace quarry
