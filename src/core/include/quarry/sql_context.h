#pragma once

#include <QString>
#include <QStringList>
#include <QStringView>

#include <cstdint>
#include <vector>

namespace quarry {

enum class CompletionScope : uint8_t {
    None, // inside a string, a comment or a number: no suggestions
    Keyword, // statement start or a place without a clear scope: keywords, functions, columns,
             // tables
    Table, // after FROM, JOIN, UPDATE, INTO, TABLE: tables, views and schemas
    Column, // after SELECT, WHERE, ON, AND, OR, BY, SET, HAVING, ',' in those clauses: columns,
            // functions and keywords
    Member, // after `x.`: columns of x (alias or table); if x is a schema, its tables
};

struct TableReference
{
    QStringList parts; // unquoted, as written: {"users"} | {"public", "users"}
    QString alias; // empty if none
};

struct SqlContext
{
    CompletionScope scope = CompletionScope::None;
    qsizetype replaceStart = 0; // UTF-16 offset in the statement where the completed word starts
    QString prefix; // unquoted text from replaceStart to the cursor
    QStringList qualifier; // Member: the parts before the last '.', unquoted
    // Every FROM/JOIN/UPDATE/INTO reference of the statement, also those after the cursor.
    std::vector<TableReference> tables;
};

// Lightweight tokenizer, not a parser: comments, strings, identifiers "x", `x`, [x], parentheses.
// Cursor in [0, statement.size()] (clamped). Pure, no I/O, O(n) in the statement length.
// shortcut: subqueries are not scoped: their table references join the flat list of the whole
// statement, and a derived table alias (`(select ...) x`) is dropped. Upgrade when the app needs
// columns of derived tables or CTEs.
// shortcut: `ON` always means a column clause (also in CREATE INDEX ... ON t), and `[` opens a
// quoted identifier unless it is glued to the previous word (PostgreSQL subscript `a[1]`).
// An unfinished quote at the cursor ends at the cursor: the text after it is still scanned.
SqlContext analyzeSqlContext(QStringView statement, qsizetype cursor);

} // namespace quarry
