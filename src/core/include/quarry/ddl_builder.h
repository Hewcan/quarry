#pragma once

#include <quarry/object_path.h>
#include <quarry/provider_api.h>

#include <QString>

namespace quarry {

// The DDL shown in the "Structure" tab of a data tab (H11 phase 4, ARCHITECTURE §6.10). It is
// RECONSTRUCTED from what IConnection::describe gives (columns, primary key, indexes, foreign
// keys), not read from the server: CHECK constraints, triggers, collations, storage options and
// anything else `describe` does not carry are not in it, and the first line says so. It is for
// reading and copying, never for running on its own as a faithful copy of the table.
class DdlBuilder
{
public:
    // The first line of every result: "-- Reconstructed by Quarry from the table's structure. ..."
    // (see ddlHeader()). Then, for a table:
    //
    //   CREATE TABLE <qualified name> (
    //       <col> <nativeType> [NOT NULL] [DEFAULT <expression>],
    //       ...
    //       [CONSTRAINT <name>] PRIMARY KEY (<cols>),
    //       [CONSTRAINT <name>] FOREIGN KEY (<cols>) REFERENCES <qualified target> (<cols>)
    //   );
    //
    // followed by one `CREATE [UNIQUE] INDEX <name> ON <qualified name> (<cols>);` per index that
    // is not the primary key's own (an index whose columns equal the primary key and is unique is
    // taken to be it). Identifiers go through dialect.quoteIdentifier / qualifiedName; the native
    // type and the default expression are the provider's own text, written as they came. A column
    // without a native type is written with its ValueType name in upper case. For a view
    // (`isView`), instead of the CREATE TABLE: the header, a line "-- The definition of a view is
    // not available here." and the list of its columns as comments ("--   <col> <type>").
    // Every statement ends with ";" and each is separated by a blank line; lines use 4 spaces and
    // "\n" line ends. Deterministic: the same input always gives the same text.
    static QString build(const ISqlDialect &dialect, const ObjectPath &table,
                         const TableDetails &details, bool isView = false);
    // The warning line (translatable, in the context "quarry::DdlBuilder").
    static QString header();
};

} // namespace quarry
