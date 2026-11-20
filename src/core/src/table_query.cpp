#include <quarry/table_query.h>

#include <QCoreApplication>
#include <QStringList>

namespace quarry {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("quarry::TableQueryBuilder", text);
}

Error internal(const QString &message)
{
    return Error { ErrorKind::Internal, message, {}, {}, {} };
}

QString comparison(ColumnFilter::Op op)
{
    switch (op) {
    case ColumnFilter::Op::Equals:
        return QStringLiteral(" = ");
    case ColumnFilter::Op::NotEquals:
        return QStringLiteral(" <> ");
    case ColumnFilter::Op::Less:
        return QStringLiteral(" < ");
    case ColumnFilter::Op::LessOrEqual:
        return QStringLiteral(" <= ");
    case ColumnFilter::Op::Greater:
        return QStringLiteral(" > ");
    case ColumnFilter::Op::GreaterOrEqual:
        return QStringLiteral(" >= ");
    case ColumnFilter::Op::Contains:
    case ColumnFilter::Op::StartsWith:
    case ColumnFilter::Op::IsNull:
    case ColumnFilter::Op::IsNotNull:
        break;
    }
    return {};
}

// Appends " WHERE …" for the filters; the values go to `params` in order.
Result<QString> whereClause(const ISqlDialect &dialect, const TableQuery &query,
                            QVariantList &params)
{
    QStringList terms;
    for (const ColumnFilter &f : query.filters) {
        if (f.column.isEmpty())
            return fail(internal(tr("A filter has no column.")));
        const QString column = dialect.quoteIdentifier(f.column);
        switch (f.op) {
        case ColumnFilter::Op::IsNull:
            terms << column + QStringLiteral(" IS NULL");
            break;
        case ColumnFilter::Op::IsNotNull:
            terms << column + QStringLiteral(" IS NOT NULL");
            break;
        case ColumnFilter::Op::Contains:
        case ColumnFilter::Op::StartsWith:
            params << TableQueryBuilder::likePattern(f.value, f.op == ColumnFilter::Op::StartsWith);
            // The literal is ASCII without quotes or backslashes, valid in every dialect.
            terms << column + QStringLiteral(" LIKE ") + dialect.placeholder(int(params.size()))
                    + QStringLiteral(" ESCAPE '") + TableQueryBuilder::kLikeEscape
                    + QLatin1Char('\'');
            break;
        default:
            params << f.value;
            terms << column + comparison(f.op) + dialect.placeholder(int(params.size()));
            break;
        }
    }
    if (terms.isEmpty())
        return QString();
    return QStringLiteral(" WHERE ") + terms.join(QStringLiteral(" AND "));
}

} // namespace

QString TableQueryBuilder::likePattern(QStringView value, bool startsWith)
{
    QString out;
    out.reserve(value.size() + 4);
    if (!startsWith)
        out += QLatin1Char('%');
    for (const QChar c : value) {
        if (c == u'%' || c == u'_' || c == kLikeEscape)
            out += kLikeEscape;
        out += c;
    }
    out += QLatin1Char('%');
    return out;
}

Result<Statement> TableQueryBuilder::select(const ISqlDialect &dialect, const TableQuery &query)
{
    Statement statement;
    QString sql = QStringLiteral("SELECT ");
    if (query.hiddenColumn) {
        if (query.hiddenColumn->isEmpty())
            return fail(internal(tr("The hidden key column has no name.")));
        sql += dialect.quoteIdentifier(*query.hiddenColumn) + QStringLiteral(", ");
    }
    sql += QStringLiteral("* FROM ") + dialect.qualifiedName(query.table);
    auto where = whereClause(dialect, query, statement.parameters);
    if (!where)
        return fail(where.error());
    sql += *where;
    if (query.sort) {
        if (query.sort->column.isEmpty())
            return fail(internal(tr("The sort has no column.")));
        sql += QStringLiteral(" ORDER BY ") + dialect.quoteIdentifier(query.sort->column)
            + (query.sort->descending ? QStringLiteral(" DESC") : QStringLiteral(" ASC"));
    }
    statement.sql = sql;
    return statement;
}

Result<Statement> TableQueryBuilder::count(const ISqlDialect &dialect, const TableQuery &query)
{
    Statement statement;
    auto where = whereClause(dialect, query, statement.parameters);
    if (!where)
        return fail(where.error());
    statement.sql
        = QStringLiteral("SELECT COUNT(*) FROM ") + dialect.qualifiedName(query.table) + *where;
    return statement;
}

} // namespace quarry
