#pragma once

#include <QString>
#include <QStringList>

#include <vector>

// The only input of the AI module: database structure, never data. There is deliberately no
// field that could hold a row, a cell or a result set; the app fills these from introspection.
// Every string here is untrusted (a column comment can carry a prompt injection).
namespace quarry::ai {

struct AiColumn
{
    QString name;
    QString type; // as the engine reports it; enum/set value lists are stripped unless allowed
    bool nullable = true;
    bool primaryKey = false;
    QString comment; // sent only with ContextOptions::sendComments
    QString defaultValue; // sent only with ContextOptions::sendDefaults
    QStringList enumValues; // sent only with ContextOptions::sendEnumValues
};

struct AiForeignKey
{
    QStringList columns;
    QString refSchema;
    QString refTable;
    QStringList refColumns;
};

struct AiIndex
{
    QString name;
    QStringList columns;
    bool unique = false;
};

struct AiTable
{
    QString schema; // empty for engines without schemas
    QString name;
    bool isView = false;
    std::vector<AiColumn> columns;
    std::vector<AiForeignKey> foreignKeys;
    std::vector<AiIndex> indexes;
    QStringList checks; // CHECK expressions, sent only with sendChecks
    QString comment; // sent only with sendComments
    QString definition; // view or routine code, sent only with sendDefinitions
};

struct SchemaContext
{
    QString dialect; // "sqlite", "postgresql", "mysql", "sqlserver"...
    std::vector<AiTable> tables;
};

struct ContextOptions
{
    QStringList excludedSchemas; // case-insensitive
    QStringList excludedTables; // "table" or "schema.table", case-insensitive
    bool sendComments = false;
    bool sendDefaults = false;
    bool sendChecks = false;
    bool sendEnumValues = false;
    bool sendDefinitions = false;
    int maxTables = 40;
};

} // namespace quarry::ai
