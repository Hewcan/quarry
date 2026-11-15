#pragma once

#include "postgres_dialect.h"

#include <quarry/provider_api.h>

namespace quarry::postgres {

class PostgresProvider : public IDatabaseProvider
{
public:
    ProviderInfo info() const override;
    std::vector<ConnectionField> connectionFields() const override;
    const ISqlDialect &dialect() const override { return m_dialect; }
    std::unique_ptr<IConnection> createConnection(const ConnectionConfig &config,
                                                  const Secrets &secrets) const override;

private:
    PostgresDialect m_dialect;
};

} // namespace quarry::postgres
