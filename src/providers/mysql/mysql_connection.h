#pragma once

#include "mysql_state.h"

#include <quarry/provider_api.h>

#include <memory>
#include <vector>

namespace quarry::mysql {

class MysqlConnection : public IConnection
{
public:
    MysqlConnection(ConnectionConfig config, QString password);
    ~MysqlConnection() override;
    MysqlConnection(const MysqlConnection &) = delete;
    MysqlConnection &operator=(const MysqlConnection &) = delete;

    Result<ServerInfo> open(const CancellationToken &) override;
    void close() override;
    bool isOpen() const override;

    Result<std::vector<SchemaNode>> children(const ObjectPath &parent,
                                             const CancellationToken &) override;
    Result<TableDetails> describe(const ObjectPath &table, const CancellationToken &) override;
    Result<std::unique_ptr<IResultStream>> execute(const Statement &,
                                                   const CancellationToken &) override;
    void requestCancel() override { m_state->requestCancel(); }

    Result<void> begin() override { return exec("START TRANSACTION"); }
    Result<void> commit() override { return exec("COMMIT"); }
    Result<void> rollback() override { return exec("ROLLBACK"); }

private:
    using Rows = std::vector<QVariantList>;

    Result<void> exec(const char *sql);
    // Sends `sql` (UTF-8, already bound) and reads the whole answer. No CallScope.
    Result<Rows> runRows(const QByteArray &sql);
    // Runs a read-only query with `?` parameters and collects every row. No CallScope.
    Result<Rows> queryRows(const QString &sql, const QVariantList &args = {});
    Result<QByteArray> bind(const QString &sql, const QVariantList &args);
    Result<TableDetails> describeTable(const ObjectPath &table); // no CallScope
    Result<std::vector<SchemaNode>> databaseItems();
    Result<std::vector<SchemaNode>> relationItems(const QStringList &parts);
    Result<std::vector<SchemaNode>> folderItems(const QStringList &parts);

    ConnectionConfig m_config;
    QString m_password;
    std::shared_ptr<MyState> m_state;
};

} // namespace quarry::mysql
