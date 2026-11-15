#include "postgres_provider.h"

#include <QObject>

class PostgresPlugin : public QObject, public quarry::IProviderPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QUARRY_PROVIDER_IID FILE "postgres.json")
    Q_INTERFACES(quarry::IProviderPlugin)

public:
    std::unique_ptr<quarry::IDatabaseProvider> createProvider() override
    {
        return std::make_unique<quarry::postgres::PostgresProvider>();
    }
};

#include "postgres_plugin.moc"
