#pragma once

#include "postgres_state.h"

#include <quarry/provider_api.h>

#include <memory>
#include <vector>

namespace quarry::postgres {

class PostgresConnection : public IConnection
{
public:
    PostgresConnection(ConnectionConfig config, QString password);
    ~PostgresConnection() override;
    PostgresConnection(const PostgresConnection &) = delete;
    PostgresConnection &operator=(const PostgresConnection &) = delete;

    Result<ServerInfo> open(const CancellationToken &) override;
    void close() override;
    bool isOpen() const override;

    Result<std::vector<SchemaNode>> children(const ObjectPath &parent,
                                             const CancellationToken &) override;
    Result<TableDetails> describe(const ObjectPath &table, const CancellationToken &) override;
    Result<std::unique_ptr<IResultStream>> execute(const Statement &,
                                                   const CancellationToken &) override;
    void requestCancel() override { m_state->requestCancel(); }

    Result<void> begin() override { return exec("BEGIN"); }
    Result<void> commit() override { return exec("COMMIT"); }
    Result<void> rollback() override { return exec("ROLLBACK"); }

private:
    using Rows = std::vector<QVariantList>;

    void closeConnection(); // close() without virtual dispatch, for the destructor
    Result<void> exec(const char *sql);
    // Runs a read-only query with text parameters and collects every row. No CallScope.
    Result<Rows> queryRows(const QString &sql, const QStringList &args = {});
    Result<TableDetails> describeTable(const ObjectPath &table); // no CallScope
    Result<std::vector<SchemaNode>> schemaItems(const QString &database);
    Result<std::vector<SchemaNode>> relationItems(const QStringList &parts);
    Result<std::vector<SchemaNode>> folderItems(const QStringList &parts);
    Result<std::vector<SchemaNode>> userItems();
    Result<std::vector<SchemaNode>> schemaIndexItems(const QStringList &parts);

    ConnectionConfig m_config;
    QString m_password;
    std::shared_ptr<PgState> m_state;
};

} // namespace quarry::postgres
