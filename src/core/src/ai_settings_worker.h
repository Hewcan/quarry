#pragma once

#include "app_database.h"

#include <quarry/ai_settings.h>

#include <QList>
#include <QObject>

namespace quarry {

// Lives on AiSettingsService's thread: quarry.db (`ai_settings`).
class AiSettingsWorker : public QObject
{
    Q_OBJECT

public:
    explicit AiSettingsWorker(QString databasePath);

    void closeDatabase() { m_database.close(); }
    void load();
    void save(const AiConnectionSettings &settings);

signals:
    void loaded(QList<quarry::AiConnectionSettings> settings);
    void saved(quarry::AiConnectionSettings settings);
    void failed(quarry::Error error);

private:
    AppDatabase m_database;
};

} // namespace quarry
