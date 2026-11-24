#include "mssql_connection.h"
#include "mssql_support.h"
#include "mssql_text.h"
#include "odbc_diag.h"
#include "odbc_state.h"

namespace quarry::mssql {

namespace {

const QString kColumns = QStringLiteral("Columns");
const QString kIndexes = QStringLiteral("Indexes");
const QString kForeignKeys = QStringLiteral("Foreign keys");
const QString kUsers = QStringLiteral("Users");

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

// The databases that belong to the server itself are hidden; so are the ones that are not
// online or that the login cannot open.
const QString kDatabasesSql
    = QStringLiteral("SELECT name FROM sys.databases "
                     "WHERE state = 0 AND HAS_DBACCESS(name) = 1 "
                     "AND name NOT IN ('master', 'tempdb', 'model', 'msdb') ORDER BY name");

// %1 is the quoted database name in every query below.
const QString kSchemasSql = QStringLiteral(
    "SELECT name FROM %1.sys.schemas "
    "WHERE name NOT IN ('sys', 'INFORMATION_SCHEMA', 'guest') AND LEFT(name, 3) <> 'db_' "
    "ORDER BY name");

// ? = schema name, twice: tables first, then views.
const QString kRelationsSql = QStringLiteral(
    "SELECT t.name, 'U' FROM %1.sys.tables t JOIN %1.sys.schemas s ON s.schema_id = t.schema_id "
    "WHERE s.name = ? AND t.is_ms_shipped = 0 "
    "UNION ALL "
    "SELECT v.name, 'V' FROM %1.sys.views v JOIN %1.sys.schemas s ON s.schema_id = v.schema_id "
    "WHERE s.name = ? AND v.is_ms_shipped = 0 "
    "ORDER BY 2, 1");

// Every query below takes ? = schema name and ? = relation name. The base type of a column of an
// alias type decides the normalized type; the alias name is what the user sees.
const QString kColumnsSql = QStringLiteral(
    "SELECT c.name, ty.name, COALESCE(bt.name, ty.name), c.max_length, c.precision, c.scale, "
    "c.is_nullable, dc.definition, c.is_identity, "
    "CAST(ep.value AS nvarchar(4000)) "
    "FROM %1.sys.columns c "
    "JOIN %1.sys.objects o ON o.object_id = c.object_id "
    "JOIN %1.sys.schemas s ON s.schema_id = o.schema_id "
    "JOIN %1.sys.types ty ON ty.user_type_id = c.user_type_id "
    "LEFT JOIN %1.sys.types bt ON bt.user_type_id = ty.system_type_id "
    "LEFT JOIN %1.sys.default_constraints dc ON dc.object_id = c.default_object_id "
    "LEFT JOIN %1.sys.extended_properties ep ON ep.class = 1 AND ep.major_id = c.object_id "
    "AND ep.minor_id = c.column_id AND ep.name = 'MS_Description' "
    "WHERE s.name = ? AND o.name = ? AND o.type IN ('U', 'V') ORDER BY c.column_id");

// The properties of the table (QUA-23); ? = schema name and ? = relation name.
const QString kTableCommentSql
    = QStringLiteral("SELECT CAST(ep.value AS nvarchar(4000)) FROM %1.sys.extended_properties ep "
                     "JOIN %1.sys.objects o ON o.object_id = ep.major_id "
                     "JOIN %1.sys.schemas s ON s.schema_id = o.schema_id "
                     "WHERE ep.class = 1 AND ep.minor_id = 0 AND ep.name = 'MS_Description' "
                     "AND s.name = ? AND o.name = ?");

const QString kTriggersSql = QStringLiteral(
    "SELECT t.name, CASE WHEN t.is_instead_of_trigger = 1 THEN 'INSTEAD OF' ELSE 'AFTER' END, "
    "(SELECT STRING_AGG(te.type_desc, ' OR ') FROM %1.sys.trigger_events te "
    " WHERE te.object_id = t.object_id), "
    "CAST(m.definition AS nvarchar(4000)) "
    "FROM %1.sys.triggers t "
    "JOIN %1.sys.objects o ON o.object_id = t.parent_id "
    "JOIN %1.sys.schemas s ON s.schema_id = o.schema_id "
    "LEFT JOIN %1.sys.sql_modules m ON m.object_id = t.object_id "
    "WHERE s.name = ? AND o.name = ? ORDER BY t.name");

// The foreign keys of other tables that point at this one.
const QString kReferencesSql = QStringLiteral(
    "SELECT fk.name, pc.name, s.name, o.name, rc.name FROM %1.sys.foreign_keys fk "
    "JOIN %1.sys.foreign_key_columns fkc ON fkc.constraint_object_id = fk.object_id "
    "JOIN %1.sys.objects o ON o.object_id = fk.parent_object_id "
    "JOIN %1.sys.schemas s ON s.schema_id = o.schema_id "
    "JOIN %1.sys.columns pc ON pc.object_id = fkc.parent_object_id "
    "AND pc.column_id = fkc.parent_column_id "
    "JOIN %1.sys.objects ro ON ro.object_id = fk.referenced_object_id "
    "JOIN %1.sys.schemas rs ON rs.schema_id = ro.schema_id "
    "JOIN %1.sys.columns rc ON rc.object_id = fkc.referenced_object_id "
    "AND rc.column_id = fkc.referenced_column_id "
    "WHERE rs.name = ? AND ro.name = ? ORDER BY s.name, o.name, fk.name, fkc.constraint_column_id");

const QString kPrimaryKeySql = QStringLiteral(
    "SELECT c.name FROM %1.sys.indexes i "
    "JOIN %1.sys.index_columns ic ON ic.object_id = i.object_id AND ic.index_id = i.index_id "
    "JOIN %1.sys.columns c ON c.object_id = ic.object_id AND c.column_id = ic.column_id "
    "JOIN %1.sys.objects o ON o.object_id = i.object_id "
    "JOIN %1.sys.schemas s ON s.schema_id = o.schema_id "
    "WHERE s.name = ? AND o.name = ? AND i.is_primary_key = 1 ORDER BY ic.key_ordinal");

// Key columns of the indexes that are not the primary key (that is TableDetails::primaryKey).
const QString kIndexesSql = QStringLiteral(
    "SELECT i.name, i.is_unique, c.name FROM %1.sys.indexes i "
    "JOIN %1.sys.index_columns ic ON ic.object_id = i.object_id AND ic.index_id = i.index_id "
    "JOIN %1.sys.columns c ON c.object_id = ic.object_id AND c.column_id = ic.column_id "
    "JOIN %1.sys.objects o ON o.object_id = i.object_id "
    "JOIN %1.sys.schemas s ON s.schema_id = o.schema_id "
    "WHERE s.name = ? AND o.name = ? AND i.is_primary_key = 0 AND i.type > 0 "
    "AND i.is_hypothetical = 0 AND ic.is_included_column = 0 ORDER BY i.name, ic.key_ordinal");

// Logins of the server (SQL, Windows user, Windows group) without the internal ##...## ones and
// the service accounts. The server catalog only shows what the login may see.
const QString kLoginsSql = QStringLiteral(
    "SELECT name, type FROM sys.server_principals WHERE type IN ('S', 'U', 'G') "
    "AND name NOT LIKE '##%' AND name NOT LIKE 'NT SERVICE\\%' AND name NOT LIKE 'NT AUTHORITY\\%' "
    "ORDER BY name");

// Fallback without a view of the server principals: the users of the current database.
const QString kDatabaseUsersSql
    = QStringLiteral("SELECT name, type FROM sys.database_principals WHERE type IN ('S', 'U', 'G') "
                     "AND name NOT IN ('dbo', 'guest', 'INFORMATION_SCHEMA', 'sys') ORDER BY name");

// ? = schema name. Key columns of the nonclustered and clustered indexes of its tables that are not
// the primary key, like the per-table folder.
const QString kSchemaIndexesSql = QStringLiteral(
    "SELECT o.name, i.name, i.is_unique, c.name FROM %1.sys.indexes i "
    "JOIN %1.sys.index_columns ic ON ic.object_id = i.object_id AND ic.index_id = i.index_id "
    "JOIN %1.sys.columns c ON c.object_id = ic.object_id AND c.column_id = ic.column_id "
    "JOIN %1.sys.objects o ON o.object_id = i.object_id "
    "JOIN %1.sys.schemas s ON s.schema_id = o.schema_id "
    "WHERE s.name = ? AND o.type = 'U' AND i.is_primary_key = 0 AND i.type > 0 "
    "AND i.is_hypothetical = 0 AND ic.is_included_column = 0 "
    "ORDER BY o.name, i.name, ic.key_ordinal");

const QString kForeignKeysSql = QStringLiteral(
    "SELECT fk.name, pc.name, rs.name, ro.name, rc.name FROM %1.sys.foreign_keys fk "
    "JOIN %1.sys.foreign_key_columns fkc ON fkc.constraint_object_id = fk.object_id "
    "JOIN %1.sys.objects o ON o.object_id = fk.parent_object_id "
    "JOIN %1.sys.schemas s ON s.schema_id = o.schema_id "
    "JOIN %1.sys.columns pc ON pc.object_id = fkc.parent_object_id "
    "AND pc.column_id = fkc.parent_column_id "
    "JOIN %1.sys.objects ro ON ro.object_id = fk.referenced_object_id "
    "JOIN %1.sys.schemas rs ON rs.schema_id = ro.schema_id "
    "JOIN %1.sys.columns rc ON rc.object_id = fkc.referenced_object_id "
    "AND rc.column_id = fkc.referenced_column_id "
    "WHERE s.name = ? AND o.name = ? ORDER BY fk.name, fkc.constraint_column_id");

} // namespace

Result<std::vector<SchemaNode>> MssqlConnection::children(const ObjectPath &parent,
                                                          const CancellationToken &token)
{
    odbc::CallScope scope(*state());
    if (!state()->dbc)
        return fail(odbc::notOpenError());
    if (token.isCancelled())
        return fail(odbc::cancelledError());

    const QStringList &parts = parent.parts;
    if (parts.isEmpty())
        return databaseItems();
    switch (parent.kind) {
    case ObjectKind::Database:
        if (parts.size() == 1)
            return schemaItems(parts.at(0));
        break;
    case ObjectKind::Schema:
        if (parts.size() == 2)
            return relationItems(parts);
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
        if (parts.size() == 1 && parts.at(0) == kUsers)
            return userItems();
        if (parts.size() == 3 && parts.at(2) == kIndexes)
            return schemaIndexItems(parts);
        break;
    default:
        break;
    }
    return std::vector<SchemaNode> {};
}

Result<std::vector<SchemaNode>> MssqlConnection::userItems()
{
    auto rows = queryRows(kLoginsSql);
    // No view of the server principals (an error or nothing at all): the users of this database.
    const bool noLogins = rows ? rows.value().empty() : rows.error().kind != ErrorKind::Cancelled;
    if (noLogins)
        rows = queryRows(kDatabaseUsersSql);
    if (!rows)
        return fail(std::move(rows).error());
    std::vector<SchemaNode> out;
    out.reserve(rows.value().size());
    for (const QVariantList &row : rows.value()) {
        const QString name = row.value(0).toString();
        const QString type = row.value(1).toString().trimmed();
        const QString detail = type == QLatin1String("U") ? Text::tr("Windows login")
            : type == QLatin1String("G")                  ? Text::tr("group")
                                                          : Text::tr("SQL login");
        out.push_back(node({ kUsers, name }, ObjectKind::User, name, false, detail));
    }
    return out;
}

Result<std::vector<SchemaNode>> MssqlConnection::schemaIndexItems(const QStringList &parts)
{
    auto rows = queryRows(inDatabase(kSchemaIndexesSql, parts.at(0)), { parts.at(1) });
    if (!rows)
        return fail(std::move(rows).error());
    constexpr size_t kMaxIndexes = 5000; // shortcut: no paging, a huge schema shows the first ones
    std::vector<SchemaNode> out;
    QString table;
    QString index;
    QStringList columns;
    bool unique = false;
    const auto flush = [&] {
        if (index.isEmpty() || out.size() >= kMaxIndexes)
            return;
        out.push_back(node({ parts.at(0), parts.at(1), table, index }, ObjectKind::Index, index,
                           false,
                           QStringLiteral("%1 (%2)%3")
                               .arg(table, columns.join(QLatin1String(", ")),
                                    unique ? QStringLiteral(" unique") : QString())));
    };
    for (const QVariantList &row : rows.value()) {
        const QString rowTable = row.value(0).toString();
        const QString rowIndex = row.value(1).toString();
        if (rowTable != table || rowIndex != index) {
            flush();
            table = rowTable;
            index = rowIndex;
            columns.clear();
            unique = row.value(2).toString() == QLatin1String("1");
        }
        columns << row.value(3).toString();
    }
    flush();
    return out;
}

Result<std::vector<SchemaNode>> MssqlConnection::databaseItems()
{
    auto rows = queryRows(kDatabasesSql);
    if (!rows)
        return fail(std::move(rows).error());
    std::vector<SchemaNode> out;
    out.reserve(rows.value().size());
    for (const QVariantList &row : rows.value()) {
        const QString name = row.value(0).toString();
        out.push_back(node({ name }, ObjectKind::Database, name, true));
    }
    return out;
}

Result<std::vector<SchemaNode>> MssqlConnection::schemaItems(const QString &database)
{
    auto rows = queryRows(inDatabase(kSchemasSql, database));
    if (!rows)
        return fail(std::move(rows).error());
    std::vector<SchemaNode> out;
    out.reserve(rows.value().size());
    for (const QVariantList &row : rows.value()) {
        const QString name = row.value(0).toString();
        out.push_back(node({ database, name }, ObjectKind::Schema, name, true));
    }
    return out;
}

Result<std::vector<SchemaNode>> MssqlConnection::relationItems(const QStringList &parts)
{
    const QString &database = parts.at(0);
    const QString &schema = parts.at(1);
    auto rows = queryRows(inDatabase(kRelationsSql, database), { schema, schema });
    if (!rows)
        return fail(std::move(rows).error());
    std::vector<SchemaNode> out;
    out.reserve(rows.value().size());
    for (const QVariantList &row : rows.value()) {
        const QString name = row.value(0).toString();
        const ObjectKind kind
            = row.value(1).toString() == QLatin1String("V") ? ObjectKind::View : ObjectKind::Table;
        out.push_back(node({ database, schema, name }, kind, name, true));
    }
    return out;
}

Result<std::vector<SchemaNode>> MssqlConnection::folderItems(const QStringList &parts)
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
            const QString target = fk.referencedTable.parts.mid(1).join(QLatin1Char('.'));
            out.push_back(node(
                owner + QStringList { fk.name }, ObjectKind::ForeignKey, fk.name, false,
                QStringLiteral("-> ") + target
                    + QStringLiteral("(%1)").arg(fk.referencedColumns.join(QLatin1String(", ")))));
        }
    }
    return out;
}

Result<TableDetails> MssqlConnection::describe(const ObjectPath &table,
                                               const CancellationToken &token)
{
    odbc::CallScope scope(*state());
    if (!state()->dbc)
        return fail(odbc::notOpenError());
    if (token.isCancelled())
        return fail(odbc::cancelledError());
    auto described = describeTable(table);
    if (!described)
        return described;
    TableDetails details = std::move(described).value();
    // The properties: a part that fails to read stays unknown instead of failing describe().
    const QString &database = table.parts.at(0);
    const QVariantList args { table.parts.at(1), table.parts.at(2) };
    if (auto comment = queryRows(inDatabase(kTableCommentSql, database), args);
        comment && !comment.value().empty())
        details.comment = comment.value().front().value(0).toString();
    if (auto rows = queryRows(inDatabase(kTriggersSql, database), args); rows) {
        std::vector<TriggerDef> triggers;
        for (const QVariantList &row : rows.value())
            triggers.push_back({ row.value(0).toString(), row.value(1).toString(),
                                 row.value(2).toString(), row.value(3).toString() });
        details.triggers = std::move(triggers);
    }
    if (auto rows = queryRows(inDatabase(kReferencesSql, database), args); rows) {
        std::vector<ForeignKeyDef> references;
        QString current;
        QString currentTable;
        for (const QVariantList &row : rows.value()) {
            const QString keyName = row.value(0).toString();
            const QString keyTable
                = row.value(2).toString() + QLatin1Char('.') + row.value(3).toString();
            if (references.empty() || keyName != current || keyTable != currentTable) {
                current = keyName;
                currentTable = keyTable;
                references.push_back(
                    { keyName,
                      {},
                      ObjectPath { { database, row.value(2).toString(), row.value(3).toString() },
                                   ObjectKind::Table },
                      {} });
            }
            references.back().columns << row.value(1).toString();
            references.back().referencedColumns << row.value(4).toString();
        }
        details.references = std::move(references);
    }
    return details;
}

Result<TableDetails> MssqlConnection::describeTable(const ObjectPath &table)
{
    if (table.parts.size() != 3)
        return fail(Error { ErrorKind::NotFound,
                            Text::tr("The path does not point to a table or view."),
                            {},
                            {},
                            {} });
    const QString &database = table.parts.at(0);
    const QString &schema = table.parts.at(1);
    const QString &name = table.parts.at(2);
    const QVariantList args { schema, name };
    TableDetails details;

    auto columns = queryRows(inDatabase(kColumnsSql, database), args);
    if (!columns)
        return fail(std::move(columns).error());
    if (columns.value().empty())
        return fail(Error { ErrorKind::NotFound,
                            Text::tr("The table \"%1\" does not exist.").arg(name),
                            {},
                            {},
                            {} });
    details.columns.reserve(columns.value().size());
    for (const QVariantList &row : columns.value()) {
        ColumnDef def;
        def.name = row.value(0).toString();
        const QString typeName = row.value(1).toString();
        const QString baseName = row.value(2).toString();
        def.type = valueTypeForTypeName(baseName);
        def.nativeType
            = nativeTypeName(typeName == baseName ? baseName : typeName, row.value(3).toInt(),
                             row.value(4).toInt(), row.value(5).toInt());
        def.nullable = row.value(6).toString() != QLatin1String("0");
        def.defaultExpression = row.value(7).toString();
        def.autoIncrement = row.value(8).toString() == QLatin1String("1")
            || row.value(8).toString() == QLatin1String("true");
        def.comment = row.value(9).toString();
        details.columns.push_back(std::move(def));
    }

    auto keys = queryRows(inDatabase(kPrimaryKeySql, database), args);
    if (!keys)
        return fail(std::move(keys).error());
    QStringList primary;
    for (const QVariantList &row : keys.value())
        primary << row.value(0).toString();
    if (!primary.isEmpty())
        details.primaryKey = primary;

    auto indexes = queryRows(inDatabase(kIndexesSql, database), args);
    if (!indexes)
        return fail(std::move(indexes).error());
    QString current;
    for (const QVariantList &row : indexes.value()) {
        const QString indexName = row.value(0).toString();
        if (details.indexes.empty() || indexName != current) {
            current = indexName;
            details.indexes.push_back(
                { indexName, {}, row.value(1).toString() == QLatin1String("1") });
        }
        details.indexes.back().columns << row.value(2).toString();
    }

    auto foreign = queryRows(inDatabase(kForeignKeysSql, database), args);
    if (!foreign)
        return fail(std::move(foreign).error());
    current.clear();
    for (const QVariantList &row : foreign.value()) {
        const QString keyName = row.value(0).toString();
        if (details.foreignKeys.empty() || keyName != current) {
            current = keyName;
            details.foreignKeys.push_back(
                { keyName,
                  {},
                  ObjectPath { { database, row.value(2).toString(), row.value(3).toString() },
                               ObjectKind::Table },
                  {} });
        }
        details.foreignKeys.back().columns << row.value(1).toString();
        details.foreignKeys.back().referencedColumns << row.value(4).toString();
    }
    return details;
}

} // namespace quarry::mssql
