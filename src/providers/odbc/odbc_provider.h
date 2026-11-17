#pragma once

#include "generic_dialect.h"

#include <quarry/provider_api.h>

namespace quarry::odbc {

class OdbcProvider : public IDatabaseProvider
{
public:
    ProviderInfo info() const override;
    std::vector<ConnectionField> connectionFields() const override;
    const ISqlDialect &dialect() const override { return m_dialect; }
    std::unique_ptr<IConnection> createConnection(const ConnectionConfig &config,
                                                  const Secrets &secrets) const override;

private:
    GenericDialect m_dialect;
};

} // namespace quarry::odbc
