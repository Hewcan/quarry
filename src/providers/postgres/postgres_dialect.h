#pragma once

#include <quarry/provider_api.h>

namespace quarry::postgres {

class PostgresDialect : public ISqlDialect
{
public:
    QString quoteIdentifier(QStringView name) const override;
    // Uses E'...' when the text has a backslash, so the result does not depend on
    // standard_conforming_strings.
    QString quoteLiteral(QStringView text) const override;
    QString highlightingDefinition() const override;
    QStringList keywords() const override;
    QStringList builtinFunctions() const override;
    // Ranges are half-open, trimmed, exclude the terminating ';' and skip comment-only
    // fragments. Understands '...', E'...', "...", $tag$...$tag$, -- and nested /* */.
    // shortcut: BEGIN ATOMIC ... END function bodies split at their inner ';', when needed add
    // a body tracker like SQLite's trigger scanner.
    std::vector<StatementRange> splitStatements(QStringView script) const override;
    QString limitClause(qint64 limit, qint64 offset) const override;
    QString explainPrefix(ExplainMode mode) const override;
    QString placeholder(int index) const override;
    // Path is {database, schema, relation}: the database is not part of the name.
    QString qualifiedName(const ObjectPath &table) const override;
    // "ctid": changes on UPDATE/VACUUM FULL, so only valid for a short edit session.
    std::optional<QString> rowIdentifier() const override;
};

} // namespace quarry::postgres
