#pragma once

#include "odbc_state.h"

#include <quarry/provider_api.h>

#include <functional>
#include <memory>
#include <vector>

namespace quarry::odbc {

// Everything of an ODBC connection that does not depend on the engine: connecting, statements,
// cancellation, transactions. A plugin derives from it and adds the connection string, the
// dialect and the schema tree (children() and describe()).
class OdbcConnection : public IConnection
{
public:
    ~OdbcConnection() override;
    OdbcConnection(const OdbcConnection &) = delete;
    OdbcConnection &operator=(const OdbcConnection &) = delete;

    Result<ServerInfo> open(const CancellationToken &) override;
    void close() override;
    bool isOpen() const override { return m_state->dbc != SQL_NULL_HDBC; }
    Result<std::unique_ptr<IResultStream>> execute(const Statement &,
                                                   const CancellationToken &) override;
    void requestCancel() override { m_state->requestCancel(); }
    Result<void> begin() override;
    Result<void> commit() override { return endTransaction(SQL_COMMIT); }
    Result<void> rollback() override { return endTransaction(SQL_ROLLBACK); }

protected:
    OdbcConnection();

    // What open() connects with. `target` is the name the user asked for (data source or driver):
    // it goes into the message when the driver manager cannot find it.
    struct ConnectRequest
    {
        QString connectionString;
        QString target;
    };
    virtual Result<ConnectRequest> connectRequest() = 0;
    virtual const ISqlDialect &sqlDialect() const = 0;
    // Whether text, binary and decimal columns are read in blocks at the size the driver reports.
    virtual bool bindsVariableLength() const { return false; }
    // ServerInfo::product from SQLGetInfo(SQL_DBMS_NAME).
    virtual QString productName(const QString &dbmsName) const { return dbmsName; }

    using Rows = std::vector<QVariantList>;
    // Runs a statement with `?` parameters and returns every row of its first result as text
    // (NULL as an invalid QVariant). The caller holds a CallScope.
    Result<Rows> queryRows(const QString &sql, const QVariantList &arguments = {});
    // Same for a catalog function (SQLTables, SQLColumns...): `call` runs it on a fresh statement.
    using CatalogCall = std::function<SQLRETURN(SQLHSTMT)>;
    Result<Rows> catalogRows(const CatalogCall &call);

    const std::shared_ptr<OdbcState> &state() const { return m_state; }

private:
    Result<void> endTransaction(SQLSMALLINT completion);
    Result<Rows> readRows(SQLHSTMT statement);

    std::shared_ptr<OdbcState> m_state;
};

} // namespace quarry::odbc
