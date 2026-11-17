#include "odbc_provider.h"

#include <QObject>

class OdbcPlugin : public QObject, public quarry::IProviderPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QUARRY_PROVIDER_IID FILE "odbc.json")
    Q_INTERFACES(quarry::IProviderPlugin)

public:
    std::unique_ptr<quarry::IDatabaseProvider> createProvider() override
    {
        return std::make_unique<quarry::odbc::OdbcProvider>();
    }
};

#include "odbc_plugin.moc"
