#pragma once

#include "mssql_dialect.h"
#include "odbc_connection.h"

#include <vector>

namespace quarry::mssql {

// A SQL Server connection through the Microsoft ODBC driver. The tree comes from sys.* of each
// database, read with parameterized queries: the names of databases only reach SQL quoted with
// [...].
class MssqlConnection : public odbc::OdbcConnection
{
public:
    MssqlConnection(ConnectionConfig config, QString password);

    Result<std::vector<SchemaNode>> children(const ObjectPath &parent,
                                             const CancellationToken &) override;
    Result<TableDetails> describe(const ObjectPath &table, const CancellationToken &) override;

protected:
    Result<ConnectRequest> connectRequest() override;
    const ISqlDialect &sqlDialect() const override { return m_dialect; }
    // The driver reports exact sizes, so text and binary columns are read in blocks.
    bool bindsVariableLength() const override { return true; }
    QString productName(const QString &dbmsName) const override;

private:
    Result<std::vector<SchemaNode>> databaseItems();
    Result<std::vector<SchemaNode>> schemaItems(const QString &database);
    Result<std::vector<SchemaNode>> relationItems(const QStringList &parts);
    Result<std::vector<SchemaNode>> folderItems(const QStringList &parts);
    Result<std::vector<SchemaNode>> userItems();
    Result<std::vector<SchemaNode>> schemaIndexItems(const QStringList &parts);
    Result<TableDetails> describeTable(const ObjectPath &table);
    // `sql` with %1 replaced by the quoted database name.
    QString inDatabase(const QString &sql, const QString &database) const;

    ConnectionConfig m_config;
    QString m_password;
    MssqlDialect m_dialect;
};

} // namespace quarry::mssql
