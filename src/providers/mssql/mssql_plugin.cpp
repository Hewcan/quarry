#include "mssql_provider.h"

#include <QObject>

class MssqlPlugin : public QObject, public quarry::IProviderPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QUARRY_PROVIDER_IID FILE "mssql.json")
    Q_INTERFACES(quarry::IProviderPlugin)

public:
    std::unique_ptr<quarry::IDatabaseProvider> createProvider() override
    {
        return std::make_unique<quarry::mssql::MssqlProvider>();
    }
};

#include "mssql_plugin.moc"
