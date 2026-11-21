#include "generic_connection.h"

#include "generic_settings.h"
#include "odbc_diag.h"
#include "odbc_support.h"
#include "odbc_text.h"

#include <algorithm>
#include <set>

namespace quarry::odbc {

namespace {

const QString kColumns = QStringLiteral("Columns");
const QString kIndexes = QStringLiteral("Indexes");
const QString kForeignKeys = QStringLiteral("Foreign keys");

SchemaNode node(QStringList parts, ObjectKind kind, const QString &name, bool hasChildren,
                const QString &detail = {})
{
    SchemaNode out;
    out.path = ObjectPath { std::move(parts), kind };
    out.name = name;
    out.hasChildren = hasChildren;
    out.detail = detail;
    return out;
}

SchemaNode folder(const QStringList &parent, const QString &key, const QString &label)
{
    return node(parent + QStringList { key }, ObjectKind::Folder, label, true);
}

// A string argument of a catalog function: absent (null pointer), or text for SQL_NTS.
struct WideArg
{
    explicit WideArg(const std::optional<QString> &value)
        : present(value.has_value())
        , text(toWide(value.value_or(QString())))
    { }
    SQLWCHAR *data() { return present ? text.data() : nullptr; }
    SQLSMALLINT length() const { return present ? SQL_NTS : 0; }

    bool present;
    std::vector<SQLWCHAR> text;
};

std::optional<QString> given(const QString &name)
{
    return name.isEmpty() ? std::nullopt : std::optional<QString>(name);
}

QString cell(const QVariantList &row, qsizetype index)
{
    return row.value(index).toString();
}

bool isCancelled(const Error &error)
{
    return error.kind == ErrorKind::Cancelled;
}

Error notATable()
{
    return Error {
        ErrorKind::NotFound, Text::tr("The path does not point to a table or view."), {}, {}, {}
    };
}

} // namespace

GenericConnection::GenericConnection(ConnectionConfig config, QString password)
    : m_config(std::move(config))
    , m_password(std::move(password))
{ }

Result<OdbcConnection::ConnectRequest> GenericConnection::connectRequest()
{
    auto settings = buildGenericSettings(m_config.values, m_password);
    if (!settings)
        return fail(std::move(settings).error());
    return ConnectRequest { buildGenericConnectionString(settings.value()),
                            genericTarget(settings.value()) };
}

QString GenericConnection::pattern(const QString &name) const
{
    return escapePattern(name, state()->searchEscape);
}

Result<OdbcConnection::Rows> GenericConnection::optionalRows(const CatalogCall &call)
{
    auto rows = catalogRows(call);
    if (!rows) {
        if (isCancelled(rows.error()))
            return fail(std::move(rows).error());
        return Rows {};
    }
    return rows;
}

Result<OdbcConnection::Rows> GenericConnection::tablesCall(const Name &catalog, const Name &schema,
                                                           const Name &table, const QString &types)
{
    return catalogRows([&](SQLHSTMT statement) {
        WideArg c { catalog };
        WideArg s { schema };
        WideArg t { table };
        WideArg y { given(types) };
        return SQLTablesW(statement, c.data(), c.length(), s.data(), s.length(), t.data(),
                          t.length(), y.data(), y.length());
    });
}

Result<std::vector<QString>> GenericConnection::listCatalogs()
{
    // SQL_ALL_CATALOGS: "%" as catalog with empty schema and table.
    auto rows = optionalRows([&](SQLHSTMT statement) {
        WideArg c { std::optional<QString>(QStringLiteral("%")) };
        WideArg e { std::optional<QString>(QString()) };
        return SQLTablesW(statement, c.data(), c.length(), e.data(), e.length(), e.data(),
                          e.length(), nullptr, 0);
    });
    if (!rows)
        return fail(std::move(rows).error());
    std::vector<QString> out;
    out.reserve(rows.value().size());
    for (const QVariantList &row : rows.value()) {
        const QString name = cell(row, 0);
        if (!name.isEmpty())
            out.push_back(name);
    }
    return out;
}

Result<std::vector<GenericConnection::TableRow>>
GenericConnection::listTables(const QString &catalog, const QString &schema)
{
    const Name schemaPattern = schema.isEmpty() ? Name() : Name(pattern(schema));
    auto rows = tablesCall(given(catalog), schemaPattern, Name(QStringLiteral("%")),
                           QStringLiteral("TABLE,VIEW"));
    if (!rows)
        return fail(std::move(rows).error());
    std::vector<TableRow> out;
    out.reserve(rows.value().size());
    for (const QVariantList &row : rows.value()) {
        TableRow table { cell(row, 0), cell(row, 1), cell(row, 2), cell(row, 3) };
        // A driver that ignores the type filter or the pattern escape: never system objects, and
        // exactly the schema that was asked for.
        if (table.name.isEmpty()
            || table.type.startsWith(QLatin1String("SYSTEM"), Qt::CaseInsensitive))
            continue;
        if (!schema.isEmpty() && table.schema != schema)
            continue;
        out.push_back(std::move(table));
    }
    return out;
}

Result<std::vector<SchemaNode>>
GenericConnection::tableLevel(const QString &catalog, const QString &schema, bool groupBySchema)
{
    auto tables = listTables(catalog, schema);
    if (!tables)
        return fail(std::move(tables).error());
    std::vector<SchemaNode> out;
    if (groupBySchema) {
        std::set<QString> schemas;
        for (const TableRow &table : tables.value()) {
            if (!table.schema.isEmpty())
                schemas.insert(table.schema);
        }
        for (const QString &name : schemas)
            out.push_back(node({ catalog, name }, ObjectKind::Schema, name, true));
    }
    out.reserve(out.size() + tables.value().size());
    for (const TableRow &table : tables.value()) {
        if (groupBySchema && !table.schema.isEmpty())
            continue;
        const ObjectKind kind = table.type.compare(QLatin1String("VIEW"), Qt::CaseInsensitive) == 0
            ? ObjectKind::View
            : ObjectKind::Table;
        out.push_back(node({ table.catalog, table.schema, table.name }, kind, table.name, true));
    }
    return out;
}

Result<std::vector<SchemaNode>> GenericConnection::children(const ObjectPath &parent,
                                                            const CancellationToken &token)
{
    CallScope scope(*state());
    if (!state()->dbc)
        return fail(notOpenError());
    if (token.isCancelled())
        return fail(cancelledError());

    const QStringList &parts = parent.parts;
    if (parts.isEmpty()) {
        auto catalogs = listCatalogs();
        if (!catalogs)
            return fail(std::move(catalogs).error());
        if (catalogs.value().empty())
            return tableLevel({}, {}, true);
        std::vector<SchemaNode> out;
        out.reserve(catalogs.value().size());
        for (const QString &name : catalogs.value())
            out.push_back(node({ name }, ObjectKind::Database, name, true));
        return out;
    }
    switch (parent.kind) {
    case ObjectKind::Database:
        if (parts.size() == 1)
            return tableLevel(parts.at(0), {}, true);
        break;
    case ObjectKind::Schema:
        if (parts.size() == 2)
            return tableLevel(parts.at(0), parts.at(1), false);
        break;
    case ObjectKind::Table:
    case ObjectKind::View:
        if (parts.size() == 3) {
            std::vector<SchemaNode> out { folder(parts, kColumns, Text::tr("Columns")) };
            if (parent.kind == ObjectKind::Table) {
                out.push_back(folder(parts, kIndexes, Text::tr("Indexes")));
                out.push_back(folder(parts, kForeignKeys, Text::tr("Foreign keys")));
            }
            return out;
        }
        break;
    case ObjectKind::Folder:
        if (parts.size() == 4)
            return folderItems(parts);
        // The "Indexes" folder of a Database or Schema node (there is no Users here).
        if ((parts.size() == 2 || parts.size() == 3) && parts.back() == kIndexes)
            return allIndexes(parts.mid(0, parts.size() - 1), token);
        break;
    default:
        break;
    }
    return std::vector<SchemaNode> {};
}

Result<std::vector<SchemaNode>> GenericConnection::folderItems(const QStringList &parts)
{
    const QString &key = parts.at(3);
    auto details = describeTable(ObjectPath { parts.mid(0, 3), ObjectKind::Table });
    if (!details)
        return fail(std::move(details).error());
    const TableDetails &t = details.value();
    const QStringList owner = parts.mid(0, 3);
    const QStringList primary = t.primaryKey.value_or(QStringList());
    std::vector<SchemaNode> out;

    if (key == kColumns) {
        out.reserve(t.columns.size());
        for (const ColumnDef &column : t.columns) {
            QStringList detail { column.nativeType };
            if (!column.nullable)
                detail << QStringLiteral("NOT NULL");
            if (primary.contains(column.name))
                detail << QStringLiteral("PK");
            out.push_back(node(owner + QStringList { column.name }, ObjectKind::Column, column.name,
                               false, detail.join(QLatin1Char(' '))));
        }
    } else if (key == kIndexes) {
        out.reserve(t.indexes.size());
        for (const IndexDef &index : t.indexes)
            out.push_back(node(owner + QStringList { index.name }, ObjectKind::Index, index.name,
                               false, index.unique ? QStringLiteral("UNIQUE") : QString()));
    } else if (key == kForeignKeys) {
        out.reserve(t.foreignKeys.size());
        for (const ForeignKeyDef &fk : t.foreignKeys) {
            QStringList target;
            for (const QString &part : fk.referencedTable.parts) {
                if (!part.isEmpty())
                    target << part;
            }
            out.push_back(node(
                owner + QStringList { fk.name }, ObjectKind::ForeignKey, fk.name, false,
                QStringLiteral("-> ") + target.join(QLatin1Char('.'))
                    + QStringLiteral("(%1)").arg(fk.referencedColumns.join(QLatin1String(", ")))));
        }
    }
    return out;
}

Result<std::vector<IndexDef>> GenericConnection::tableIndexes(const QString &catalog,
                                                              const QString &schema,
                                                              const QString &name,
                                                              const QString &primaryName)
{
    const Name catalogArg = given(catalog);
    const Name schemaArg = given(schema);
    auto statistics = optionalRows([&](SQLHSTMT statement) {
        WideArg c { catalogArg };
        WideArg s { schemaArg };
        WideArg t { Name(name) };
        return SQLStatisticsW(statement, c.data(), c.length(), s.data(), s.length(), t.data(),
                              t.length(), SQL_INDEX_ALL, SQL_QUICK);
    });
    if (!statistics)
        return fail(std::move(statistics).error());
    struct IndexRow
    {
        QString name;
        int ordinal = 0;
        QString column;
        bool unique = false;
    };
    std::vector<IndexRow> ordered;
    ordered.reserve(statistics.value().size());
    for (const QVariantList &row : statistics.value()) {
        // TYPE 0 (SQL_TABLE_STAT) is the row count of the table, not an index.
        if (cell(row, 6) == QLatin1String("0") || row.value(5).isNull())
            continue;
        const QString indexName = cell(row, 5);
        if (indexName.isEmpty() || indexName == primaryName)
            continue;
        ordered.push_back({ indexName, cell(row, 7).toInt(),
                            row.value(8).isNull() ? QStringLiteral("<expression>") : cell(row, 8),
                            cell(row, 3) == QLatin1String("0") });
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const IndexRow &a, const IndexRow &b) {
        return a.name == b.name ? a.ordinal < b.ordinal : a.name < b.name;
    });
    std::vector<IndexDef> out;
    for (const IndexRow &row : ordered) {
        if (out.empty() || out.back().name != row.name)
            out.push_back({ row.name, {}, row.unique });
        out.back().columns << row.column;
    }
    return out;
}

Result<std::vector<SchemaNode>> GenericConnection::allIndexes(const QStringList &parts,
                                                              const CancellationToken &token)
{
    // shortcut: the catalog functions work table by table, so a big schema only shows the indexes
    // of its first kMaxTables tables; a driver without SQLStatistics gives none.
    constexpr size_t kMaxTables = 200;
    const QString schema = parts.size() > 1 ? parts.at(1) : QString();
    auto tables = listTables(parts.at(0), schema);
    if (!tables)
        return fail(std::move(tables).error());
    std::vector<SchemaNode> out;
    size_t visited = 0;
    for (const TableRow &table : tables.value()) {
        // Views have no indexes; under a database without schemas only the tables with no schema.
        if (table.type.compare(QLatin1String("TABLE"), Qt::CaseInsensitive) != 0
            || (parts.size() == 1 && !table.schema.isEmpty()))
            continue;
        if (visited++ >= kMaxTables)
            break;
        if (token.isCancelled())
            return fail(cancelledError());
        QString primaryName;
        auto keys = optionalRows([&](SQLHSTMT statement) {
            WideArg c { given(table.catalog) };
            WideArg s { given(table.schema) };
            WideArg t { Name(table.name) };
            return SQLPrimaryKeysW(statement, c.data(), c.length(), s.data(), s.length(), t.data(),
                                   t.length());
        });
        if (!keys)
            return fail(std::move(keys).error());
        if (!keys.value().empty())
            primaryName = cell(keys.value().front(), 5);
        auto indexes = tableIndexes(table.catalog, table.schema, table.name, primaryName);
        if (!indexes)
            return fail(std::move(indexes).error());
        for (const IndexDef &index : indexes.value()) {
            out.push_back(node({ table.catalog, table.schema, table.name, index.name },
                               ObjectKind::Index, index.name, false,
                               QStringLiteral("%1 (%2)%3")
                                   .arg(table.name, index.columns.join(QLatin1String(", ")),
                                        index.unique ? QStringLiteral(" unique") : QString())));
        }
    }
    return out;
}

Result<TableDetails> GenericConnection::describe(const ObjectPath &table,
                                                 const CancellationToken &token)
{
    CallScope scope(*state());
    if (!state()->dbc)
        return fail(notOpenError());
    if (token.isCancelled())
        return fail(cancelledError());
    return describeTable(table);
}

Result<TableDetails> GenericConnection::describeTable(const ObjectPath &table)
{
    if (table.parts.size() != 3)
        return fail(notATable());
    const QString &catalog = table.parts.at(0);
    const QString &schema = table.parts.at(1);
    const QString &name = table.parts.at(2);
    const Name catalogArg = given(catalog);
    const Name schemaArg = given(schema);
    TableDetails details;

    // Columns: schema and table are search patterns, so the names are escaped and the rows
    // filtered again for the exact table.
    auto columns = catalogRows([&](SQLHSTMT statement) {
        WideArg c { catalogArg };
        WideArg s { schemaArg.has_value() ? Name(pattern(schema)) : Name() };
        WideArg t { Name(pattern(name)) };
        WideArg all { Name(QStringLiteral("%")) };
        return SQLColumnsW(statement, c.data(), c.length(), s.data(), s.length(), t.data(),
                           t.length(), all.data(), all.length());
    });
    if (!columns)
        return fail(std::move(columns).error());
    struct ColumnRow
    {
        int ordinal = 0;
        ColumnDef def;
    };
    std::vector<ColumnRow> rows;
    rows.reserve(columns.value().size());
    for (const QVariantList &row : columns.value()) {
        if (cell(row, 2) != name || (!schema.isEmpty() && cell(row, 1) != schema))
            continue;
        ColumnRow out;
        out.ordinal = cell(row, 16).toInt();
        out.def.name = cell(row, 3);
        const int sqlType = cell(row, 4).toInt();
        out.def.type = valueTypeForSqlType(sqlType);
        QString typeName = cell(row, 5);
        if (typeName.isEmpty())
            typeName = genericTypeName(sqlType);
        out.def.nativeType = decoratedTypeName(typeName, out.def.type, cell(row, 6).toULongLong(),
                                               cell(row, 8).toInt());
        out.def.nullable = cell(row, 10) != QLatin1String("0")
            && cell(row, 17).compare(QLatin1String("NO"), Qt::CaseInsensitive) != 0;
        out.def.defaultExpression = cell(row, 12);
        rows.push_back(std::move(out));
    }
    if (rows.empty())
        return fail(Error { ErrorKind::NotFound,
                            Text::tr("The table \"%1\" does not exist.").arg(name),
                            {},
                            {},
                            {} });
    std::stable_sort(rows.begin(), rows.end(),
                     [](const ColumnRow &a, const ColumnRow &b) { return a.ordinal < b.ordinal; });
    details.columns.reserve(rows.size());
    for (ColumnRow &row : rows)
        details.columns.push_back(std::move(row.def));

    // Keys and indexes are optional features: a driver without them just shows none.
    QString primaryName;
    auto keys = optionalRows([&](SQLHSTMT statement) {
        WideArg c { catalogArg };
        WideArg s { schemaArg };
        WideArg t { Name(name) };
        return SQLPrimaryKeysW(statement, c.data(), c.length(), s.data(), s.length(), t.data(),
                               t.length());
    });
    if (!keys)
        return fail(std::move(keys).error());
    {
        std::vector<std::pair<int, QString>> ordered;
        ordered.reserve(keys.value().size());
        for (const QVariantList &row : keys.value()) {
            ordered.emplace_back(cell(row, 4).toInt(), cell(row, 3));
            primaryName = cell(row, 5);
        }
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const auto &a, const auto &b) { return a.first < b.first; });
        QStringList primary;
        for (const auto &entry : ordered)
            primary << entry.second;
        if (!primary.isEmpty())
            details.primaryKey = primary;
    }

    auto indexes = tableIndexes(catalog, schema, name, primaryName);
    if (!indexes)
        return fail(std::move(indexes).error());
    details.indexes = indexes.value();

    auto foreign = optionalRows([&](SQLHSTMT statement) {
        WideArg none { std::optional<QString>() };
        WideArg c { catalogArg };
        WideArg s { schemaArg };
        WideArg t { Name(name) };
        return SQLForeignKeysW(statement, none.data(), none.length(), none.data(), none.length(),
                               none.data(), none.length(), c.data(), c.length(), s.data(),
                               s.length(), t.data(), t.length());
    });
    if (!foreign)
        return fail(std::move(foreign).error());
    {
        struct KeyRow
        {
            QString name;
            int sequence = 0;
            QStringList target;
            QString column;
            QString referencedColumn;
        };
        std::vector<KeyRow> ordered;
        ordered.reserve(foreign.value().size());
        for (const QVariantList &row : foreign.value()) {
            if (cell(row, 6) != name)
                continue;
            QString keyName = cell(row, 11);
            if (keyName.isEmpty())
                keyName = cell(row, 2); // unnamed: one key per referenced table
            ordered.push_back({ keyName,
                                cell(row, 8).toInt(),
                                { cell(row, 0), cell(row, 1), cell(row, 2) },
                                cell(row, 7),
                                cell(row, 3) });
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const KeyRow &a, const KeyRow &b) {
            return a.name == b.name ? a.sequence < b.sequence : a.name < b.name;
        });
        for (const KeyRow &row : ordered) {
            if (details.foreignKeys.empty() || details.foreignKeys.back().name != row.name)
                details.foreignKeys.push_back(
                    { row.name, {}, ObjectPath { row.target, ObjectKind::Table }, {} });
            details.foreignKeys.back().columns << row.column;
            details.foreignKeys.back().referencedColumns << row.referencedColumn;
        }
    }
    return details;
}

} // namespace quarry::odbc
