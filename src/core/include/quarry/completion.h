#pragma once

#include <quarry/metadata_cache.h>
#include <quarry/object_path.h>
#include <quarry/provider_api.h>
#include <quarry/sql_context.h>

#include <QString>

#include <cstdint>
#include <vector>

namespace quarry {

enum class SuggestionKind : uint8_t { Keyword, Function, Schema, Table, View, Column };

struct Suggestion
{
    QString text; // inserted in [replaceStart, cursor), already quoted when needed
    QString label; // shown, without quotes
    SuggestionKind kind;
    // Column: the type (Member scope) or "table: type" (other scopes); Table/View: the schema.
    QString detail;
};

class CompletionEngine
{
public:
    // Case-insensitive: names that start with the prefix first, then those that contain it; within
    // each group by kind according to the scope, then alphabetically. At most `limit`.
    // Identifiers that are not [A-Za-z_][A-Za-z0-9_]* or that equal a dialect keyword are quoted
    // with ISqlDialect::quoteIdentifier; keywords are inserted in uppercase. Relations are found
    // through the cache's sorted index: binary search for the prefix, a linear scan for substrings
    // only when the prefix matches do not fill `limit`.
    static std::vector<Suggestion> complete(const SqlContext &, const MetadataCache &,
                                            const ISqlDialect &, int limit = 200);
    // Tables of the context (and the table named by a Member qualifier) whose columns are not in
    // the cache yet: the app calls ensureColumns() for each.
    static std::vector<ObjectPath> missingColumns(const SqlContext &, const MetadataCache &);
};

} // namespace quarry
