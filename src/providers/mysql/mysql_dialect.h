#pragma once

#include <quarry/provider_api.h>

namespace quarry::mysql {

// One dialect for MySQL and MariaDB: IDatabaseProvider::dialect() is asked before any connection
// exists, so the keyword and function lists are the union of both. ServerInfo::product tells
// the two engines apart.
class MysqlDialect : public ISqlDialect
{
public:
    QString quoteIdentifier(QStringView name) const override;
    // Doubles ' and \ (the default sql_mode treats \ as an escape inside strings).
    QString quoteLiteral(QStringView text) const override;
    QString highlightingDefinition() const override;
    QStringList keywords() const override;
    QStringList builtinFunctions() const override;
    // Ranges are half-open, trimmed, exclude the terminating delimiter and skip comment-only
    // fragments. Understands the DELIMITER command, '...' and "..." (with \ and doubled
    // quotes), `...`, '#', '-- ' and /* */ comments; /*! ... */ and /*M! ... */ are code.
    std::vector<StatementRange> splitStatements(QStringView script) const override;
    QString limitClause(qint64 limit, qint64 offset) const override;
    // "MariaDB" in `product` (any case) selects MariaDB's syntax and parser.
    QString explainStatement(const QString &sql, ExplainMode mode,
                             const QString &product) const override;
    Result<ExplainPlan> parseExplain(const QString &output, ExplainMode mode,
                                     const QString &product) const override;
    QString placeholder(int index) const override;
    // Path is {database, relation}: the database is part of the name, as the session may be
    // using another one.
    QString qualifiedName(const ObjectPath &table) const override;
    // MySQL has no hidden row identifier: tables without a primary key are read-only.
    std::optional<QString> rowIdentifier() const override;
};

} // namespace quarry::mysql
