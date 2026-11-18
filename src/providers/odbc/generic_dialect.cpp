#include "generic_dialect.h"

namespace quarry::odbc {

namespace {

// Index just past the closing quote of a '...' or "..." run that opens at `i`; n if
// unterminated. A doubled quote is an escaped quote.
qsizetype skipQuoted(QStringView s, qsizetype i, QChar quote)
{
    const qsizetype n = s.size();
    ++i;
    while (i < n) {
        if (s[i] == quote) {
            if (i + 1 < n && s[i + 1] == quote)
                i += 2;
            else
                return i + 1;
        } else {
            ++i;
        }
    }
    return n;
}

// Index just past the line comment that starts at `i`.
qsizetype skipLine(QStringView s, qsizetype i)
{
    while (i < s.size() && s[i] != QLatin1Char('\n'))
        ++i;
    return i;
}

// Index just past the (nested) block comment that starts at `i`; n if unterminated.
qsizetype skipBlock(QStringView s, qsizetype i)
{
    const qsizetype n = s.size();
    int depth = 0;
    while (i < n) {
        if (s[i] == QLatin1Char('/') && i + 1 < n && s[i + 1] == QLatin1Char('*')) {
            ++depth;
            i += 2;
        } else if (s[i] == QLatin1Char('*') && i + 1 < n && s[i + 1] == QLatin1Char('/')) {
            i += 2;
            if (--depth == 0)
                return i;
        } else {
            ++i;
        }
    }
    return n;
}

} // namespace

QString GenericDialect::quoteIdentifier(QStringView name) const
{
    QString out;
    out.reserve(name.size() + 2);
    out += QLatin1Char('"');
    for (const QChar c : name) {
        if (c == QLatin1Char('"'))
            out += c;
        out += c;
    }
    out += QLatin1Char('"');
    return out;
}

QString GenericDialect::quoteLiteral(QStringView text) const
{
    QString out;
    out.reserve(text.size() + 2);
    out += QLatin1Char('\'');
    for (const QChar c : text) {
        if (c == QLatin1Char('\''))
            out += c;
        out += c;
    }
    out += QLatin1Char('\'');
    return out;
}

QString GenericDialect::highlightingDefinition() const
{
    return QStringLiteral("SQL");
}

QStringList GenericDialect::keywords() const
{
    static const QStringList kKeywords = [] {
        const char *words[] = {
            "ADD",
            "ALL",
            "ALTER",
            "AND",
            "ANY",
            "AS",
            "ASC",
            "BETWEEN",
            "BIGINT",
            "BOOLEAN",
            "BY",
            "CASE",
            "CAST",
            "CHAR",
            "CHECK",
            "COLUMN",
            "COMMIT",
            "CONSTRAINT",
            "CREATE",
            "CROSS",
            "CURRENT_DATE",
            "CURRENT_TIME",
            "CURRENT_TIMESTAMP",
            "DATE",
            "DECIMAL",
            "DEFAULT",
            "DELETE",
            "DESC",
            "DISTINCT",
            "DOUBLE",
            "DROP",
            "ELSE",
            "END",
            "EXCEPT",
            "EXISTS",
            "FALSE",
            "FETCH",
            "FIRST",
            "FLOAT",
            "FOREIGN",
            "FROM",
            "FULL",
            "GRANT",
            "GROUP",
            "HAVING",
            "IN",
            "INDEX",
            "INNER",
            "INSERT",
            "INT",
            "INTEGER",
            "INTERSECT",
            "INTO",
            "IS",
            "JOIN",
            "KEY",
            "LEFT",
            "LIKE",
            "NEXT",
            "NOT",
            "NULL",
            "NUMERIC",
            "OFFSET",
            "ON",
            "ONLY",
            "OR",
            "ORDER",
            "OUTER",
            "OVER",
            "PARTITION",
            "PRIMARY",
            "REAL",
            "REFERENCES",
            "REVOKE",
            "RIGHT",
            "ROLLBACK",
            "ROW",
            "ROWS",
            "SCHEMA",
            "SELECT",
            "SET",
            "SMALLINT",
            "TABLE",
            "THEN",
            "TIME",
            "TIMESTAMP",
            "TO",
            "TRANSACTION",
            "TRUE",
            "TRUNCATE",
            "UNION",
            "UNIQUE",
            "UPDATE",
            "USING",
            "VALUES",
            "VARCHAR",
            "VIEW",
            "WHEN",
            "WHERE",
            "WITH",
        };
        QStringList out;
        for (const char *word : words)
            out.push_back(QString::fromLatin1(word));
        return out;
    }();
    return kKeywords;
}

QStringList GenericDialect::builtinFunctions() const
{
    // shortcut: the functions of standard SQL that nearly every driver has, not the manual of any
    // engine.
    static const QStringList kFunctions = [] {
        const char *names[] = {
            "abs",      "avg",      "cast",  "ceil",         "ceiling",      "char_length",
            "coalesce", "concat",   "count", "current_date", "current_time", "current_timestamp",
            "extract",  "floor",    "lower", "max",          "min",          "mod",
            "nullif",   "position", "power", "round",        "sqrt",         "substring",
            "sum",      "trim",     "upper",
        };
        QStringList out;
        for (const char *name : names)
            out.push_back(QString::fromLatin1(name));
        return out;
    }();
    return kFunctions;
}

std::vector<StatementRange> GenericDialect::splitStatements(QStringView script) const
{
    std::vector<StatementRange> out;
    const qsizetype n = script.size();
    qsizetype start = 0;
    bool hasToken = false;

    auto flush = [&](qsizetype end) {
        if (hasToken) {
            qsizetype b = start;
            qsizetype e = end;
            while (b < e && script[b].isSpace())
                ++b;
            while (e > b && script[e - 1].isSpace())
                --e;
            out.push_back({ b, e });
        }
        hasToken = false;
    };

    qsizetype i = 0;
    while (i < n) {
        const QChar c = script[i];
        if (c.isSpace()) {
            ++i;
        } else if (c == QLatin1Char('-') && i + 1 < n && script[i + 1] == QLatin1Char('-')) {
            i = skipLine(script, i);
        } else if (c == QLatin1Char('/') && i + 1 < n && script[i + 1] == QLatin1Char('*')) {
            i = skipBlock(script, i);
        } else if (c == QLatin1Char(';')) {
            flush(i);
            ++i;
            start = i;
        } else {
            hasToken = true;
            if (c == QLatin1Char('\'') || c == QLatin1Char('"'))
                i = skipQuoted(script, i, c);
            else
                ++i;
        }
    }
    flush(n);
    return out;
}

QString GenericDialect::limitClause(qint64 limit, qint64 offset) const
{
    if (limit < 0 && offset <= 0)
        return {};
    QString out;
    if (offset > 0)
        out = QStringLiteral("OFFSET %1 ROWS").arg(offset);
    if (limit >= 0) {
        if (!out.isEmpty())
            out += QLatin1Char(' ');
        out += offset > 0 ? QStringLiteral("FETCH NEXT %1 ROWS ONLY").arg(limit)
                          : QStringLiteral("FETCH FIRST %1 ROWS ONLY").arg(limit);
    }
    return out;
}

QString GenericDialect::placeholder(int index) const
{
    Q_UNUSED(index)
    return QStringLiteral("?");
}

QString GenericDialect::qualifiedName(const ObjectPath &table) const
{
    QStringList parts;
    parts.reserve(table.parts.size());
    for (const QString &part : table.parts) {
        if (!part.isEmpty())
            parts << quoteIdentifier(part);
    }
    return parts.join(QLatin1Char('.'));
}

std::optional<QString> GenericDialect::rowIdentifier() const
{
    return std::nullopt;
}

} // namespace quarry::odbc
