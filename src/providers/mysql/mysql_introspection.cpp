#include "mysql_connection.h"
#include "mysql_errors.h"
#include "mysql_support.h"
#include "mysql_text.h"

namespace quarry::mysql {

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

// The databases that belong to the server itself. information_schema is matched
// case-insensitively because lower_case_table_names can change how it is spelled.
const QString kDatabasesSql = QStringLiteral(
    "SELECT SCHEMA_NAME FROM information_schema.SCHEMATA "
    "WHERE LOWER(SCHEMA_NAME) NOT IN ('mysql', 'sys', 'information_schema', 'performance_schema') "
    "ORDER BY SCHEMA_NAME");

// SYSTEM VIEW is information_schema itself; MariaDB reports sequences as SEQUENCE.
const QString kRelationsSql
    = QStringLiteral("SELECT TABLE_NAME, TABLE_TYPE FROM information_schema.TABLES "
                     "WHERE TABLE_SCHEMA = ? AND TABLE_TYPE <> 'SYSTEM VIEW' "
                     "ORDER BY CASE WHEN TABLE_TYPE = 'VIEW' THEN 1 ELSE 0 END, TABLE_NAME");

// Every query below takes ? = database name and ? = relation name.
const QString kColumnsSql
    = QStringLiteral("SELECT COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE, COLUMN_DEFAULT, DATA_TYPE "
                     "FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = ? AND TABLE_NAME = ? "
                     "ORDER BY ORDINAL_POSITION");

// One row per column of every index, PRIMARY included (it becomes TableDetails::primaryKey).
const QString kIndexesSql = QStringLiteral(
    "SELECT INDEX_NAME, NON_UNIQUE, COLUMN_NAME FROM information_schema.STATISTICS "
    "WHERE TABLE_SCHEMA = ? AND TABLE_NAME = ? ORDER BY INDEX_NAME, SEQ_IN_INDEX");

// Accounts of the server. Needs SELECT on mysql.user; kGranteesSql is the fallback without it.
const QString kAccountsSql
    = QStringLiteral("SELECT DISTINCT User, Host FROM mysql.user ORDER BY User, Host");
const QString kGranteesSql = QStringLiteral(
    "SELECT DISTINCT GRANTEE FROM information_schema.USER_PRIVILEGES ORDER BY GRANTEE");

// ? = database name. Every index of its tables but PRIMARY, like the per-table folder.
const QString kDatabaseIndexesSql = QStringLiteral(
    "SELECT TABLE_NAME, INDEX_NAME, NON_UNIQUE, COLUMN_NAME FROM information_schema.STATISTICS "
    "WHERE TABLE_SCHEMA = ? AND INDEX_NAME <> 'PRIMARY' "
    "ORDER BY TABLE_NAME, INDEX_NAME, SEQ_IN_INDEX");

const QString kForeignKeysSql = QStringLiteral(
    "SELECT CONSTRAINT_NAME, COLUMN_NAME, REFERENCED_TABLE_SCHEMA, REFERENCED_TABLE_NAME, "
    "REFERENCED_COLUMN_NAME FROM information_schema.KEY_COLUMN_USAGE "
    "WHERE TABLE_SCHEMA = ? AND TABLE_NAME = ? AND REFERENCED_TABLE_NAME IS NOT NULL "
    "ORDER BY CONSTRAINT_NAME, ORDINAL_POSITION");

} // namespace

Result<std::vector<SchemaNode>> MysqlConnection::children(const ObjectPath &parent,
                                                          const CancellationToken &token)
{
    CallScope scope(*m_state);
    if (!m_state->conn)
        return fail(notOpenError());
    if (token.isCancelled())
        return fail(cancelledError());

    const QStringList &parts = parent.parts;
    if (parts.isEmpty())
        return databaseItems();
    switch (parent.kind) {
    case ObjectKind::Database:
        if (parts.size() == 1)
            return relationItems(parts);
        break;
    case ObjectKind::Table:
    case ObjectKind::View:
        if (parts.size() == 2) {
            std::vector<SchemaNode> out { folder(parts, kColumns, Text::tr("Columns")) };
            if (parent.kind == ObjectKind::Table) {
                out.push_back(folder(parts, kIndexes, Text::tr("Indexes")));
                out.push_back(folder(parts, kForeignKeys, Text::tr("Foreign keys")));
            }
            return out;
        }
        break;
    case ObjectKind::Folder:
        if (parts.size() == 3)
            return folderItems(parts);
        if (parts.size() == 1 && parts.at(0) == kUsers)
            return userItems();
        if (parts.size() == 2 && parts.at(1) == kIndexes)
            return databaseIndexItems(parts.at(0));
        break;
    default:
        break;
    }
    return std::vector<SchemaNode> {};
}

Result<std::vector<SchemaNode>> MysqlConnection::userItems()
{
    auto rows = queryRows(kAccountsSql);
    if (!rows) {
        if (rows.error().kind == ErrorKind::Cancelled)
            return fail(std::move(rows).error());
        // No right on the mysql schema: the accounts that hold some privilege are still visible.
        rows = queryRows(kGranteesSql);
        if (!rows)
            return fail(std::move(rows).error());
        std::vector<SchemaNode> out;
        out.reserve(rows.value().size());
        for (const QVariantList &row : rows.value()) {
            // GRANTEE is 'user'@'host' with the quotes.
            QString name = row.at(0).toString();
            name.replace(QLatin1String("'@'"), QLatin1String("@"));
            if (name.startsWith(QLatin1Char('\'')))
                name.remove(0, 1);
            if (name.endsWith(QLatin1Char('\'')))
                name.chop(1);
            out.push_back(node({ kUsers, name }, ObjectKind::User, name, false, Text::tr("user")));
        }
        return out;
    }
    std::vector<SchemaNode> out;
    out.reserve(rows.value().size());
    for (const QVariantList &row : rows.value()) {
        const QString name = row.at(0).toString() + QLatin1Char('@') + row.at(1).toString();
        out.push_back(node({ kUsers, name }, ObjectKind::User, name, false, Text::tr("user")));
    }
    return out;
}

Result<std::vector<SchemaNode>> MysqlConnection::databaseIndexItems(const QString &database)
{
    auto rows = queryRows(kDatabaseIndexesSql, { database });
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
        out.push_back(node({ database, table, index }, ObjectKind::Index, index, false,
                           QStringLiteral("%1 (%2)%3")
                               .arg(table, columns.join(QLatin1String(", ")),
                                    unique ? QStringLiteral(" unique") : QString())));
    };
    for (const QVariantList &row : rows.value()) {
        const QString rowTable = row.at(0).toString();
        const QString rowIndex = row.at(1).toString();
        if (rowTable != table || rowIndex != index) {
            flush();
            table = rowTable;
            index = rowIndex;
            columns.clear();
            unique = row.at(2).toString() == QLatin1String("0");
        }
        columns << (row.at(3).isNull() ? QStringLiteral("<expression>") : row.at(3).toString());
    }
    flush();
    return out;
}

Result<std::vector<SchemaNode>> MysqlConnection::databaseItems()
{
    auto rows = queryRows(kDatabasesSql);
    if (!rows)
        return fail(std::move(rows).error());
    std::vector<SchemaNode> out;
    out.reserve(rows.value().size());
    for (const QVariantList &row : rows.value()) {
        const QString name = row.front().toString();
        out.push_back(node({ name }, ObjectKind::Database, name, true));
    }
    return out;
}

Result<std::vector<SchemaNode>> MysqlConnection::relationItems(const QStringList &parts)
{
    auto rows = queryRows(kRelationsSql, { parts.at(0) });
    if (!rows)
        return fail(std::move(rows).error());
    std::vector<SchemaNode> out;
    out.reserve(rows.value().size());
    for (const QVariantList &row : rows.value()) {
        const QString name = row.at(0).toString();
        const QString type = row.at(1).toString();
        if (type == QLatin1String("SEQUENCE")) {
            out.push_back(node({ parts.at(0), name }, ObjectKind::Sequence, name, false));
            continue;
        }
        const ObjectKind kind
            = type == QLatin1String("VIEW") ? ObjectKind::View : ObjectKind::Table;
        out.push_back(node({ parts.at(0), name }, kind, name, true));
    }
    return out;
}

Result<std::vector<SchemaNode>> MysqlConnection::folderItems(const QStringList &parts)
{
    const QString &key = parts.at(2);
    auto details = describeTable(ObjectPath { parts.mid(0, 2), ObjectKind::Table });
    if (!details)
        return fail(std::move(details).error());
    const TableDetails &t = details.value();
    const QStringList owner = parts.mid(0, 2);
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
            const QString target = fk.referencedTable.parts.join(QLatin1Char('.'));
            out.push_back(node(
                owner + QStringList { fk.name }, ObjectKind::ForeignKey, fk.name, false,
                QStringLiteral("-> ") + target
                    + QStringLiteral("(%1)").arg(fk.referencedColumns.join(QLatin1String(", ")))));
        }
    }
    return out;
}

Result<TableDetails> MysqlConnection::describe(const ObjectPath &table,
                                               const CancellationToken &token)
{
    CallScope scope(*m_state);
    if (!m_state->conn)
        return fail(notOpenError());
    if (token.isCancelled())
        return fail(cancelledError());
    return describeTable(table);
}

Result<TableDetails> MysqlConnection::describeTable(const ObjectPath &table)
{
    if (table.parts.size() != 2)
        return fail(Error { ErrorKind::NotFound,
                            Text::tr("The path does not point to a table or view."),
                            {},
                            {},
                            {} });
    const QString &database = table.parts.at(0);
    const QString &name = table.parts.at(1);
    const QVariantList args { database, name };
    TableDetails details;

    auto columns = queryRows(kColumnsSql, args);
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
        def.name = row.at(0).toString();
        def.nativeType = row.at(1).toString();
        def.nullable = row.at(2).toString() == QLatin1String("YES");
        def.defaultExpression = row.at(3).toString();
        def.type = valueTypeForDataType(row.at(4).toString());
        details.columns.push_back(std::move(def));
    }

    auto indexes = queryRows(kIndexesSql, args);
    if (!indexes)
        return fail(std::move(indexes).error());
    QStringList primary;
    QString current;
    for (const QVariantList &row : indexes.value()) {
        const QString indexName = row.at(0).toString();
        const QString column
            = row.at(2).isNull() ? QStringLiteral("<expression>") : row.at(2).toString();
        if (indexName == QLatin1String("PRIMARY")) {
            primary << column;
            continue;
        }
        if (details.indexes.empty() || indexName != current) {
            current = indexName;
            details.indexes.push_back(
                { indexName, {}, row.at(1).toString() == QLatin1String("0") });
        }
        details.indexes.back().columns << column;
    }
    if (!primary.isEmpty())
        details.primaryKey = primary;

    auto fks = queryRows(kForeignKeysSql, args);
    if (!fks)
        return fail(std::move(fks).error());
    current.clear();
    for (const QVariantList &row : fks.value()) {
        const QString fkName = row.at(0).toString();
        if (details.foreignKeys.empty() || fkName != current) {
            current = fkName;
            details.foreignKeys.push_back(
                { fkName,
                  {},
                  ObjectPath { { row.at(2).toString(), row.at(3).toString() }, ObjectKind::Table },
                  {} });
        }
        details.foreignKeys.back().columns << row.at(1).toString();
        details.foreignKeys.back().referencedColumns << row.at(4).toString();
    }
    return details;
}

} // namespace quarry::mysql
