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

Unexpected syntaxError(const QString &message, qsizetype position, const QString &detail = {})
{
    return fail(Error { ErrorKind::Syntax, message, detail, {}, int(position) });
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

// " WHERE …" for the filters ("" without any); the values go to `params` in order.
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
        case ColumnFilter::Op::StartsWith: {
            const bool startsWith = f.op == ColumnFilter::Op::StartsWith;
            params << TableQueryBuilder::likePattern(f.value, startsWith);
            const QString marker = dialect.placeholder(int(params.size()));
            // The literal is ASCII without quotes or backslashes, valid in every dialect.
            const QString escape = QStringLiteral(" ESCAPE '!'");
            terms << column + QStringLiteral(" LIKE ") + marker + escape;
            break;
        }
        default: {
            params << f.value;
            const QString marker = dialect.placeholder(int(params.size()));
            terms << column + comparison(f.op) + marker;
            break;
        }
        }
    }
    auto expression = TableQueryBuilder::validateWhere(query.whereExpression);
    if (!expression)
        return fail(expression.error());
    if (!expression->isEmpty())
        terms << QLatin1Char('(') + *expression + QLatin1Char(')');
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

Result<QString> TableQueryBuilder::validateWhere(QStringView expression)
{
    const QStringView trimmed = expression.trimmed();
    if (trimmed.isEmpty())
        return QString();

    // Outside = plain SQL; inside a '…' literal, a "…" or a `…` identifier. A doubled closing
    // quote is the escape and just re-enters the same state.
    QChar quote;
    qsizetype quoteStart = 0;
    QList<qsizetype> open; // positions of the `(` still waiting for their `)`
    const qsizetype size = expression.size();
    for (qsizetype i = 0; i < size; ++i) {
        const QChar c = expression[i];
        if (c.isNull())
            return syntaxError(tr("The filter contains an invalid character."), i,
                               QStringLiteral("NUL"));
        if (!quote.isNull()) {
            if (c == quote) {
                if (i + 1 < size && expression[i + 1] == quote)
                    ++i;
                else
                    quote = QChar();
            }
            continue;
        }
        const QChar next = i + 1 < size ? expression[i + 1] : QChar();
        switch (c.unicode()) {
        case u'\'':
        case u'"':
        case u'`':
            quote = c;
            quoteStart = i;
            break;
        case u';':
            return syntaxError(tr("Only one condition is allowed: remove the ';'."), i);
        case u'-':
            if (next == u'-')
                return syntaxError(tr("Comments are not allowed in the filter."), i,
                                   QStringLiteral("--"));
            break;
        case u'/':
            if (next == u'*')
                return syntaxError(tr("Comments are not allowed in the filter."), i,
                                   QStringLiteral("/*"));
            break;
        case u'?':
            return syntaxError(tr("The '?' placeholder is not allowed: write the value in the "
                                  "condition."),
                               i);
        case u'$':
            if (next.isDigit())
                return syntaxError(tr("Numbered placeholders such as $1 are not allowed: write "
                                      "the value in the condition."),
                                   i);
            break;
        case u'(':
            open.append(i);
            break;
        case u')':
            if (open.isEmpty())
                return syntaxError(tr("A closing parenthesis has no opening one."), i);
            open.removeLast();
            break;
        default:
            break;
        }
    }
    if (!quote.isNull())
        return syntaxError(tr("A quote is not closed."), quoteStart);
    if (!open.isEmpty())
        return syntaxError(tr("A parenthesis is not closed."), open.constFirst());
    return trimmed.toString();
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
        const QString column = dialect.quoteIdentifier(query.sort->column);
        const bool descending = query.sort->descending;
        const QString direction = descending ? QStringLiteral(" DESC") : QStringLiteral(" ASC");
        sql += QStringLiteral(" ORDER BY ") + column + direction;
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
    const QString table = dialect.qualifiedName(query.table);
    statement.sql = QStringLiteral("SELECT COUNT(*) FROM ") + table + *where;
    return statement;
}

} // namespace quarry
