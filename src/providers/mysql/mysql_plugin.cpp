#include "mysql_provider.h"

#include <QObject>

class MysqlPlugin : public QObject, public quarry::IProviderPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QUARRY_PROVIDER_IID FILE "mysql.json")
    Q_INTERFACES(quarry::IProviderPlugin)

public:
    std::unique_ptr<quarry::IDatabaseProvider> createProvider() override
    {
        return std::make_unique<quarry::mysql::MysqlProvider>();
    }
};

#include "mysql_plugin.moc"
