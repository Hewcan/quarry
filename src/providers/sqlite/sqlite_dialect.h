#pragma once

#include <quarry/provider_api.h>

namespace quarry::sqlite {

class SqliteDialect : public ISqlDialect
{
public:
    QString quoteIdentifier(QStringView name) const override;
    QString quoteLiteral(QStringView text) const override;
    QString highlightingDefinition() const override;
    QStringList keywords() const override;
    QStringList builtinFunctions() const override;
    // Ranges are half-open, trimmed of surrounding whitespace, exclude the terminating ';',
    // and skip fragments with nothing but comments/whitespace.
    std::vector<StatementRange> splitStatements(QStringView script) const override;
    QString limitClause(qint64 limit, qint64 offset) const override;
    // Plan only: SQLite has no EXPLAIN ANALYZE, so Analyze gives an empty statement.
    QString explainStatement(const QString &sql, ExplainMode mode,
                             const QString &product) const override;
    Result<ExplainPlan> parseExplain(const QString &output, ExplainMode mode,
                                     const QString &product) const override;
    QString placeholder(int index) const override;
    // Path is {database, table}; "main" is left out so the usual name stays short.
    QString qualifiedName(const ObjectPath &table) const override;
    // "rowid". shortcut: a real column named rowid shadows it; callers must then pick "_rowid_"
    // or "oid" (whichever is not a column name).
    std::optional<QString> rowIdentifier() const override;
};

} // namespace quarry::sqlite
