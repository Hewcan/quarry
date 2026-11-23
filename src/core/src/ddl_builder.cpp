#include <quarry/ddl_builder.h>

#include <QCoreApplication>
#include <QStringList>

namespace quarry {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("quarry::DdlBuilder", text);
}

// Stands in for a native type the provider did not report.
QString typeName(ValueType type)
{
    switch (type) {
    case ValueType::Null:
        return QStringLiteral("NULL");
    case ValueType::Bool:
        return QStringLiteral("BOOL");
    case ValueType::Int64:
        return QStringLiteral("INT64");
    case ValueType::Double:
        return QStringLiteral("DOUBLE");
    case ValueType::Decimal:
        return QStringLiteral("DECIMAL");
    case ValueType::Text:
        return QStringLiteral("TEXT");
    case ValueType::Bytes:
        return QStringLiteral("BYTES");
    case ValueType::Date:
        return QStringLiteral("DATE");
    case ValueType::Time:
        return QStringLiteral("TIME");
    case ValueType::DateTime:
        return QStringLiteral("DATETIME");
    case ValueType::DateTimeTz:
        return QStringLiteral("DATETIMETZ");
    case ValueType::Uuid:
        return QStringLiteral("UUID");
    case ValueType::Json:
        return QStringLiteral("JSON");
    case ValueType::Other:
        return QStringLiteral("OTHER");
    }
    return {};
}

QString columnType(const ColumnDef &column)
{
    return column.nativeType.isEmpty() ? typeName(column.type) : column.nativeType;
}

QString columnList(const ISqlDialect &dialect, const QStringList &columns)
{
    QStringList quoted;
    quoted.reserve(columns.size());
    for (const QString &c : columns)
        quoted << dialect.quoteIdentifier(c);
    return quoted.join(QStringLiteral(", "));
}

QString columnDefinition(const ISqlDialect &dialect, const ColumnDef &column)
{
    QString line = dialect.quoteIdentifier(column.name) + QLatin1Char(' ') + columnType(column);
    if (!column.nullable)
        line += QStringLiteral(" NOT NULL");
    if (!column.defaultExpression.isEmpty())
        line += QStringLiteral(" DEFAULT ") + column.defaultExpression;
    return line;
}

QString constraintPrefix(const ISqlDialect &dialect, const QString &name)
{
    return name.isEmpty()
        ? QString()
        : QStringLiteral("CONSTRAINT ") + dialect.quoteIdentifier(name) + QLatin1Char(' ');
}

} // namespace

QString DdlBuilder::header()
{
    return tr("-- Reconstructed by Quarry from the table's structure. CHECK constraints, triggers "
              "and engine options are not included.");
}

QString DdlBuilder::build(const ISqlDialect &dialect, const ObjectPath &table,
                          const TableDetails &details, bool isView)
{
    const QString nl = QStringLiteral("\n");
    QString out = header() + nl + nl;

    if (isView) {
        out += tr("-- The definition of a view is not available here.") + nl;
        for (const ColumnDef &c : details.columns)
            out += QStringLiteral("--   ") + c.name + QLatin1Char(' ') + columnType(c) + nl;
        return out;
    }
    if (details.columns.empty())
        return out + tr("-- No columns are known for this table.") + nl;

    QStringList lines;
    lines.reserve(qsizetype(details.columns.size()) + qsizetype(details.foreignKeys.size()) + 1);
    for (const ColumnDef &c : details.columns)
        lines << columnDefinition(dialect, c);
    if (details.primaryKey && !details.primaryKey->isEmpty())
        lines << QStringLiteral("PRIMARY KEY (") + columnList(dialect, *details.primaryKey)
                + QLatin1Char(')');
    for (const ForeignKeyDef &fk : details.foreignKeys)
        lines << constraintPrefix(dialect, fk.name) + QStringLiteral("FOREIGN KEY (")
                + columnList(dialect, fk.columns) + QStringLiteral(") REFERENCES ")
                + dialect.qualifiedName(fk.referencedTable) + QStringLiteral(" (")
                + columnList(dialect, fk.referencedColumns) + QLatin1Char(')');

    const QString qualified = dialect.qualifiedName(table);
    out += QStringLiteral("CREATE TABLE ") + qualified + QStringLiteral(" (") + nl;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        out += QStringLiteral("    ") + lines.at(i);
        if (i + 1 < lines.size())
            out += QLatin1Char(',');
        out += nl;
    }
    out += QStringLiteral(");") + nl;

    for (const IndexDef &index : details.indexes) {
        if (index.unique && details.primaryKey && index.columns == *details.primaryKey)
            continue;
        out += nl + QStringLiteral("CREATE ")
            + (index.unique ? QStringLiteral("UNIQUE ") : QString()) + QStringLiteral("INDEX ")
            + dialect.quoteIdentifier(index.name) + QStringLiteral(" ON ") + qualified
            + QStringLiteral(" (") + columnList(dialect, index.columns) + QStringLiteral(");") + nl;
    }
    return out;
}

} // namespace quarry
