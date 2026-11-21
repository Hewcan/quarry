#pragma once

#include "generic_dialect.h"
#include "odbc_connection.h"

#include <optional>
#include <vector>

namespace quarry::odbc {

// The connection of the generic provider: a data source, a driver or a full connection string.
// The schema tree comes from the ODBC catalog functions, so it works with any driver that
// implements them: catalog -> schema -> table, each level only when the driver has it.
class GenericConnection : public OdbcConnection
{
public:
    GenericConnection(ConnectionConfig config, QString password);

    Result<std::vector<SchemaNode>> children(const ObjectPath &parent,
                                             const CancellationToken &) override;
    Result<TableDetails> describe(const ObjectPath &table, const CancellationToken &) override;

protected:
    Result<ConnectRequest> connectRequest() override;
    const ISqlDialect &sqlDialect() const override { return m_dialect; }

private:
    struct TableRow
    {
        QString catalog;
        QString schema;
        QString name;
        QString type;
    };
    using Name = std::optional<QString>; // nullopt: not specified (the driver does not filter)

    Result<Rows> tablesCall(const Name &catalog, const Name &schema, const Name &table,
                            const QString &types);
    Result<std::vector<QString>> listCatalogs();
    // Tables and views (never system tables). Empty catalog and schema mean "any".
    Result<std::vector<TableRow>> listTables(const QString &catalog, const QString &schema);
    Result<std::vector<SchemaNode>> tableLevel(const QString &catalog, const QString &schema,
                                               bool groupBySchema);
    Result<std::vector<SchemaNode>> folderItems(const QStringList &parts);
    Result<TableDetails> describeTable(const ObjectPath &table);
    // The indexes of one table without the primary key (named `primaryName`).
    Result<std::vector<IndexDef>> tableIndexes(const QString &catalog, const QString &schema,
                                               const QString &name, const QString &primaryName);
    // `parts` is the Database ({catalog}) or Schema ({catalog, schema}) that holds the tables.
    Result<std::vector<SchemaNode>> allIndexes(const QStringList &parts, const CancellationToken &);
    // Rows of a catalog function that the driver may not implement: any failure but a cancel
    // gives no rows.
    Result<Rows> optionalRows(const CatalogCall &call);
    QString pattern(const QString &name) const;

    ConnectionConfig m_config;
    QString m_password;
    GenericDialect m_dialect;
};

} // namespace quarry::odbc
