#include "postgres_connection.h"
#include "postgres_errors.h"
#include "postgres_support.h"
#include "postgres_text.h"

namespace quarry::postgres {

namespace {

const QString kColumns = QStringLiteral("Columns");
const QString kIndexes = QStringLiteral("Indexes");
const QString kForeignKeys = QStringLiteral("Foreign keys");
const QString kUsers = QStringLiteral("Users");

// Separator for lists aggregated on the server (ASCII unit separator, never in a name).
const QChar kSep = QChar(31);

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

QStringList splitList(const QVariant &value)
{
    return value.isNull() ? QStringList() : value.toString().split(kSep);
}

bool isTrue(const QVariant &value)
{
    return value.toString() == QLatin1String("t");
}

// Every query below takes $1 = schema name and $2 = relation name.
const QString kColumnsSql = QStringLiteral(
    "SELECT a.attname, pg_catalog.format_type(a.atttypid, a.atttypmod), a.attnotnull, "
    "pg_catalog.pg_get_expr(d.adbin, d.adrelid), a.atttypid::bigint "
    "FROM pg_catalog.pg_class c "
    "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
    "JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped "
    "LEFT JOIN pg_catalog.pg_attrdef d ON d.adrelid = a.attrelid AND d.adnum = a.attnum "
    "WHERE n.nspname = $1 AND c.relname = $2 ORDER BY a.attnum");

const QString kPrimaryKeySql = QStringLiteral(
    "SELECT string_agg(att.attname::text, chr(31) ORDER BY k.ord) "
    "FROM pg_catalog.pg_class c "
    "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
    "JOIN pg_catalog.pg_index i ON i.indrelid = c.oid AND i.indisprimary "
    "CROSS JOIN LATERAL unnest(i.indkey::int2[]) WITH ORDINALITY AS k(attnum, ord) "
    "JOIN pg_catalog.pg_attribute att ON att.attrelid = c.oid AND att.attnum = k.attnum "
    "WHERE n.nspname = $1 AND c.relname = $2");

// The primary key index is reported through TableDetails::primaryKey, not as an index.
const QString kIndexesSql = QStringLiteral(
    "SELECT ic.relname, i.indisunique, "
    "(SELECT string_agg(pg_catalog.pg_get_indexdef(i.indexrelid, k.n, true), chr(31) "
    "ORDER BY k.n) FROM generate_series(1, i.indnkeyatts::int) AS k(n)) "
    "FROM pg_catalog.pg_class c "
    "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
    "JOIN pg_catalog.pg_index i ON i.indrelid = c.oid AND NOT i.indisprimary "
    "JOIN pg_catalog.pg_class ic ON ic.oid = i.indexrelid "
    "WHERE n.nspname = $1 AND c.relname = $2 ORDER BY ic.relname");

// Roles of the server, without the predefined pg_* ones. No password data is read.
const QString kRolesSql
    = QStringLiteral("SELECT rolname, rolcanlogin, rolsuper FROM pg_catalog.pg_roles "
                     "WHERE rolname NOT LIKE 'pg\\_%' ORDER BY rolname");

// $1 = schema name. Every index of the relations of the schema but the primary keys, like the
// per-table folder. shortcut: capped at 5000 indexes, no paging; raise or page it if it matters.
const QString kSchemaIndexesSql = QStringLiteral(
    "SELECT c.relname, ic.relname, i.indisunique, "
    "(SELECT string_agg(pg_catalog.pg_get_indexdef(i.indexrelid, k.n, true), chr(31) "
    "ORDER BY k.n) FROM generate_series(1, i.indnkeyatts::int) AS k(n)) "
    "FROM pg_catalog.pg_class c "
    "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
    "JOIN pg_catalog.pg_index i ON i.indrelid = c.oid AND NOT i.indisprimary "
    "JOIN pg_catalog.pg_class ic ON ic.oid = i.indexrelid "
    "WHERE n.nspname = $1 AND c.relkind IN ('r', 'p', 'm') "
    "ORDER BY c.relname, ic.relname LIMIT 5000");

const QString kForeignKeysSql = QStringLiteral(
    "SELECT con.conname, "
    "(SELECT string_agg(att.attname::text, chr(31) ORDER BY k.ord) "
    " FROM unnest(con.conkey) WITH ORDINALITY AS k(attnum, ord) "
    " JOIN pg_catalog.pg_attribute att ON att.attrelid = con.conrelid AND att.attnum = k.attnum), "
    "rn.nspname, rc.relname, "
    "(SELECT string_agg(att.attname::text, chr(31) ORDER BY k.ord) "
    " FROM unnest(con.confkey) WITH ORDINALITY AS k(attnum, ord) "
    " JOIN pg_catalog.pg_attribute att ON att.attrelid = con.confrelid AND att.attnum = k.attnum) "
    "FROM pg_catalog.pg_class c "
    "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
    "JOIN pg_catalog.pg_constraint con ON con.conrelid = c.oid AND con.contype = 'f' "
    "JOIN pg_catalog.pg_class rc ON rc.oid = con.confrelid "
    "JOIN pg_catalog.pg_namespace rn ON rn.oid = rc.relnamespace "
    "WHERE n.nspname = $1 AND c.relname = $2 ORDER BY con.conname");

} // namespace

Result<std::vector<SchemaNode>> PostgresConnection::children(const ObjectPath &parent,
                                                             const CancellationToken &token)
{
    CallScope scope(*m_state);
    if (!m_state->conn)
        return fail(notOpenError());
    if (token.isCancelled())
        return fail(cancelledError());

    const QStringList &parts = parent.parts;
    if (parts.isEmpty()) { // only the database of the profile
        const QString database = QString::fromUtf8(PQdb(m_state->conn));
        return std::vector<SchemaNode> { node({ database }, ObjectKind::Database, database, true) };
    }
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
    case ObjectKind::MaterializedView:
        if (parts.size() == 3) {
            std::vector<SchemaNode> out { folder(parts, kColumns, Text::tr("Columns")) };
            if (parent.kind != ObjectKind::View) {
                out.push_back(folder(parts, kIndexes, Text::tr("Indexes")));
                if (parent.kind == ObjectKind::Table)
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

Result<std::vector<SchemaNode>> PostgresConnection::userItems()
{
    auto rows = queryRows(kRolesSql);
    if (!rows)
        return fail(std::move(rows).error());
    std::vector<SchemaNode> out;
    out.reserve(rows.value().size());
    for (const QVariantList &row : rows.value()) {
        const QString name = row.at(0).toString();
        QStringList detail;
        if (isTrue(row.at(2)))
            detail << Text::tr("superuser");
        detail << (isTrue(row.at(1)) ? Text::tr("can log in") : Text::tr("role"));
        out.push_back(node({ kUsers, name }, ObjectKind::User, name, false,
                           detail.join(QLatin1String(", "))));
    }
    return out;
}

Result<std::vector<SchemaNode>> PostgresConnection::schemaIndexItems(const QStringList &parts)
{
    auto rows = queryRows(kSchemaIndexesSql, { parts.at(1) });
    if (!rows)
        return fail(std::move(rows).error());
    std::vector<SchemaNode> out;
    out.reserve(rows.value().size());
    for (const QVariantList &row : rows.value()) {
        const QString table = row.at(0).toString();
        const QString name = row.at(1).toString();
        const QString detail = QStringLiteral("%1 (%2)%3")
                                   .arg(table, splitList(row.at(3)).join(QLatin1String(", ")),
                                        isTrue(row.at(2)) ? QStringLiteral(" unique") : QString());
        out.push_back(node({ parts.at(0), parts.at(1), table, name }, ObjectKind::Index, name,
                           false, detail));
    }
    return out;
}

Result<std::vector<SchemaNode>> PostgresConnection::schemaItems(const QString &database)
{
    auto rows = queryRows(
        QStringLiteral("SELECT nspname FROM pg_catalog.pg_namespace "
                       "WHERE nspname <> 'information_schema' AND nspname <> 'pg_catalog' "
                       "AND nspname NOT LIKE 'pg\\_toast%' AND nspname NOT LIKE 'pg\\_temp\\_%' "
                       "ORDER BY nspname"));
    if (!rows)
        return fail(std::move(rows).error());
    std::vector<SchemaNode> out;
    for (const QVariantList &row : rows.value()) {
        const QString name = row.front().toString();
        out.push_back(node({ database, name }, ObjectKind::Schema, name, true));
    }
    return out;
}

Result<std::vector<SchemaNode>> PostgresConnection::relationItems(const QStringList &parts)
{
    auto rows = queryRows(
        QStringLiteral("SELECT c.relname, c.relkind FROM pg_catalog.pg_class c "
                       "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                       "WHERE n.nspname = $1 AND c.relkind IN ('r', 'p', 'f', 'v', 'm') "
                       "ORDER BY CASE c.relkind WHEN 'v' THEN 1 WHEN 'm' THEN 2 ELSE 0 END, "
                       "c.relname"),
        { parts.at(1) });
    if (!rows)
        return fail(std::move(rows).error());
    std::vector<SchemaNode> out;
    for (const QVariantList &row : rows.value()) {
        const QString name = row.at(0).toString();
        const QString relkind = row.at(1).toString();
        ObjectKind kind = ObjectKind::Table;
        if (relkind == QLatin1String("v"))
            kind = ObjectKind::View;
        else if (relkind == QLatin1String("m"))
            kind = ObjectKind::MaterializedView;
        out.push_back(node({ parts.at(0), parts.at(1), name }, kind, name, true));
    }
    return out;
}

Result<std::vector<SchemaNode>> PostgresConnection::folderItems(const QStringList &parts)
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
        for (const IndexDef &index : t.indexes)
            out.push_back(node(owner + QStringList { index.name }, ObjectKind::Index, index.name,
                               false, index.unique ? QStringLiteral("UNIQUE") : QString()));
    } else if (key == kForeignKeys) {
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

Result<TableDetails> PostgresConnection::describe(const ObjectPath &table,
                                                  const CancellationToken &token)
{
    CallScope scope(*m_state);
    if (!m_state->conn)
        return fail(notOpenError());
    if (token.isCancelled())
        return fail(cancelledError());
    return describeTable(table);
}

Result<TableDetails> PostgresConnection::describeTable(const ObjectPath &table)
{
    if (table.parts.size() != 3)
        return fail(Error { ErrorKind::NotFound,
                            Text::tr("The path does not point to a table or view."),
                            {},
                            {},
                            {} });
    const QString &database = table.parts.at(0);
    const QString &name = table.parts.at(2);
    const QStringList args { table.parts.at(1), name };
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
    for (const QVariantList &row : columns.value()) {
        ColumnDef def;
        def.name = row.at(0).toString();
        def.nativeType = row.at(1).toString();
        def.nullable = !isTrue(row.at(2));
        def.defaultExpression = row.at(3).toString();
        def.type = valueTypeForOid(row.at(4).toUInt());
        details.columns.push_back(std::move(def));
    }

    auto pk = queryRows(kPrimaryKeySql, args);
    if (!pk)
        return fail(std::move(pk).error());
    if (const QStringList key = splitList(pk.value().front().front()); !key.isEmpty())
        details.primaryKey = key;

    auto indexes = queryRows(kIndexesSql, args);
    if (!indexes)
        return fail(std::move(indexes).error());
    for (const QVariantList &row : indexes.value())
        details.indexes.push_back(
            { row.at(0).toString(), splitList(row.at(2)), isTrue(row.at(1)) });

    auto fks = queryRows(kForeignKeysSql, args);
    if (!fks)
        return fail(std::move(fks).error());
    for (const QVariantList &row : fks.value())
        details.foreignKeys.push_back(
            { row.at(0).toString(), splitList(row.at(1)),
              ObjectPath { { database, row.at(2).toString(), row.at(3).toString() },
                           ObjectKind::Table },
              splitList(row.at(4)) });
    return details;
}

} // namespace quarry::postgres
