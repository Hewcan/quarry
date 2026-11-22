#pragma once

#include <quarry/ai/schema_context.h>

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QStringView>

namespace quarry::ai {

struct SelectedContext
{
    QString dialect;
    QStringList tables; // qualified names, in schema order
    QString ddl; // compact DDL, exactly what goes into the prompt
};

// Picks the relevant tables (named in the script, named in the request, and one foreign-key hop
// from those) and renders them as compact DDL. Excluded schemas and tables never appear, not even
// as the target of a foreign key. Results are cached per (script, request, options).
class ContextBuilder
{
public:
    void setSchema(SchemaContext schema); // drops the cache
    const SchemaContext &schema() const { return m_schema; }

    SelectedContext build(QStringView script, QStringView request,
                          const ContextOptions &options) const;

    static bool isExcluded(QStringView schema, QStringView table, const ContextOptions &options);
    static QString renderTable(const AiTable &table, const ContextOptions &options,
                               const SchemaContext &all);

private:
    SchemaContext m_schema;
    mutable QHash<QByteArray, SelectedContext> m_cache;
};

} // namespace quarry::ai
