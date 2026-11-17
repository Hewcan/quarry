#pragma once

#include <quarry/provider_api.h>

namespace quarry::mssql {

// Transact-SQL.
class MssqlDialect : public ISqlDialect
{
public:
    // [name], doubling every ].
    QString quoteIdentifier(QStringView name) const override;
    // N'...' with the quote doubled: Unicode text, whatever the code page of the column.
    QString quoteLiteral(QStringView text) const override;
    QString highlightingDefinition() const override;
    QStringList keywords() const override;
    QStringList builtinFunctions() const override;
    // Ranges are half-open, trimmed, exclude the terminating ';' and skip comment-only
    // fragments. A line that only has GO (with an optional count, ignored) ends a batch. ';' ends
    // a statement except inside BEGIN ... END and CASE ... END (BEGIN TRAN[SACTION], BEGIN
    // DISTRIBUTED, BEGIN CONVERSATION and BEGIN DIALOG do not open a block; BEGIN TRY and
    // BEGIN CATCH do). Understands -- and nested /* */ comments, '...', N'...' and "..." strings
    // and [...] identifiers.
    // shortcut: IF x stmt; ELSE stmt (no BEGIN) splits at the ';' before ELSE; wrap the branches
    // in BEGIN ... END, as the T-SQL style guides ask.
    std::vector<StatementRange> splitStatements(QStringView script) const override;
    // OFFSET n ROWS FETCH NEXT m ROWS ONLY, which SQL Server only accepts after an ORDER BY: the
    // caller adds ORDER BY (SELECT NULL) when the query has none.
    QString limitClause(qint64 limit, qint64 offset) const override;
    // shortcut: no EXPLAIN prefix; SQL Server plans come from SET SHOWPLAN_XML, a statement of
    // its own (H9).
    QString explainPrefix(ExplainMode mode) const override;
    QString placeholder(int index) const override;
    // Path is {database, schema, relation}: [database].[schema].[relation].
    QString qualifiedName(const ObjectPath &table) const override;
    // SQL Server has no hidden row identifier: tables without a primary key are read-only.
    std::optional<QString> rowIdentifier() const override;
};

} // namespace quarry::mssql
