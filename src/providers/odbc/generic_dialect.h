#pragma once

#include <quarry/provider_api.h>

namespace quarry::odbc {

// ANSI SQL for a driver the provider knows nothing about: "..." identifiers, OFFSET ... FETCH.
// shortcut: the dialect is static; when a connection exists, read SQL_IDENTIFIER_QUOTE_CHAR and
// the driver's reserved words (SQLGetInfo) and hand out a dialect per connection.
class GenericDialect : public ISqlDialect
{
public:
    QString quoteIdentifier(QStringView name) const override;
    QString quoteLiteral(QStringView text) const override;
    QString highlightingDefinition() const override;
    QStringList keywords() const override;
    QStringList builtinFunctions() const override;
    // Ranges are half-open, trimmed, exclude the terminating ';' and skip comment-only
    // fragments. Understands '...' and "..." (doubled quote), -- and nested /* */ comments.
    // shortcut: no dollar quoting ($$...$$) or backslash escapes; they are PostgreSQL and MySQL
    // extensions and the driver behind the connection is unknown.
    std::vector<StatementRange> splitStatements(QStringView script) const override;
    QString limitClause(qint64 limit, qint64 offset) const override;
    QString placeholder(int index) const override;
    // Path is {catalog, schema, relation}; the parts a driver does not have are empty and left out.
    QString qualifiedName(const ObjectPath &table) const override;
    std::optional<QString> rowIdentifier() const override;
};

} // namespace quarry::odbc
