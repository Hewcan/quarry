#pragma once

#include <quarry/object_path.h>
#include <quarry/provider_api.h>
#include <quarry/result.h>

#include <QStringList>
#include <QVariant>
#include <QVariantList>

#include <cstdint>
#include <vector>

namespace quarry {

struct RowKey
{
    enum class Kind : uint8_t { PrimaryKey, RowIdentifier };

    Kind kind = Kind::PrimaryKey;
    QStringList columns; // the primary key, or {rowIdentifier()}
};

struct EditTarget
{
    ObjectPath table;
    QStringList columns; // table columns in grid order (without the hidden key column)
    RowKey key;
};

struct CellChange
{
    int column = 0; // index into EditTarget::columns
    QVariant value; // QVariant() = NULL
};

struct RowEdit
{
    enum class Kind : uint8_t { Update, Insert, Delete };

    Kind kind = Kind::Update;
    QVariantList key; // original values of key.columns (Update/Delete)
    std::vector<CellChange>
        cells; // Update: changed cells; Insert: cells with a value (rest DEFAULT)
};

struct PlannedStatement
{
    Statement statement; // sql + parameters, exactly as they are executed
    int editIndex = 0; // the RowEdit it comes from
    bool expectOneRow = true; // Update/Delete: must affect exactly 1 row
};

class ChangeSqlBuilder
{
public:
    // One statement per RowEdit, in order. Values are always parameters (numbered from 1 per
    // statement); identifiers go through quoteIdentifier, the table through qualifiedName.
    // Errors (ErrorKind::Internal): empty key columns, key size mismatch, NULL key value, Update
    // without cells, column index out of range. The message names the offending edit.
    static Result<std::vector<PlannedStatement>>
    build(const ISqlDialect &dialect, const EditTarget &target, const std::vector<RowEdit> &edits);
};

} // namespace quarry

Q_DECLARE_METATYPE(quarry::PlannedStatement)
Q_DECLARE_METATYPE(std::vector<quarry::PlannedStatement>)
