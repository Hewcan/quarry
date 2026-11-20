#pragma once

#include <quarry/object_path.h>
#include <quarry/provider_api.h>
#include <quarry/result.h>

#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

namespace quarry {

// One condition of the filter bar of a data tab (H11). The value travels as a parameter, never
// inside the SQL text.
struct ColumnFilter
{
    enum class Op : uint8_t {
        Equals,
        NotEquals,
        Contains, // LIKE '%value%' with % and _ taken literally
        StartsWith,
        Less,
        LessOrEqual,
        Greater,
        GreaterOrEqual,
        IsNull,
        IsNotNull,
    };

    QString column;
    Op op = Op::Equals;
    QString value; // ignored by IsNull / IsNotNull
};

struct SortKey
{
    QString column;
    bool descending = false;
};

// What a data tab reads: a table, an optional hidden key column (rowid / ctid) in front of the
// rest, the filters (joined with AND) and the order.
struct TableQuery
{
    ObjectPath table;
    std::optional<QString> hiddenColumn;
    std::vector<ColumnFilter> filters;
    std::optional<SortKey> sort;
};

// Builds the SELECT behind a data tab, so sorting and filtering run on the server (ARCHITECTURE
// §6.10). Identifiers go through quoteIdentifier / qualifiedName and the values are parameters
// with the dialect's placeholders.
class TableQueryBuilder
{
public:
    // SELECT [hidden,] * FROM t [WHERE …] [ORDER BY c ASC|DESC]. An empty column name is an error.
    static Result<Statement> select(const ISqlDialect &dialect, const TableQuery &query);
    // SELECT COUNT(*) FROM t [WHERE …]: the same filters, no order.
    static Result<Statement> count(const ISqlDialect &dialect, const TableQuery &query);
    // The pattern for Contains / StartsWith: % _ and the escape character are escaped with
    // kLikeEscape, which the clause declares with ESCAPE.
    static QString likePattern(QStringView value, bool startsWith);
    static constexpr QChar kLikeEscape = u'!';
};

} // namespace quarry
