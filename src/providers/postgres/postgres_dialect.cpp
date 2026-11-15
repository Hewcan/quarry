#include "postgres_dialect.h"

namespace quarry::postgres {

namespace {

QString quoteWith(QStringView text, QChar quote)
{
    QString out;
    out.reserve(text.size() + 2);
    out += quote;
    for (const QChar c : text) {
        if (c == quote)
            out += quote;
        out += c;
    }
    out += quote;
    return out;
}

bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('$');
}

bool isTagStart(QChar c)
{
    return c.isLetter() || c == QLatin1Char('_');
}

bool isTagChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

// Index just past the closing quote of a '...' or "..." run that opens at `i`; n if unterminated.
// A doubled quote is an escaped quote; with `backslash`, \x escapes the next character (E'...').
qsizetype skipQuoted(QStringView s, qsizetype i, QChar quote, bool backslash)
{
    const qsizetype n = s.size();
    ++i;
    while (i < n) {
        if (backslash && s[i] == QLatin1Char('\\')) {
            i += 2;
        } else if (s[i] == quote) {
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

// If a dollar-quote tag ($$ or $name$) opens at `i`, returns its length; otherwise 0.
qsizetype dollarTagLength(QStringView s, qsizetype i)
{
    qsizetype j = i + 1;
    if (j < s.size() && isTagStart(s[j])) {
        while (j < s.size() && isTagChar(s[j]))
            ++j;
    }
    return j < s.size() && s[j] == QLatin1Char('$') ? j - i + 1 : 0;
}

// Index just past the nested comment that opens at `i` (script[i..i+1] == "/*"); n if unterminated.
qsizetype skipBlockComment(QStringView s, qsizetype i)
{
    int depth = 1;
    i += 2;
    while (i < s.size() && depth > 0) {
        if (s[i] == QLatin1Char('/') && i + 1 < s.size() && s[i + 1] == QLatin1Char('*')) {
            ++depth;
            i += 2;
        } else if (s[i] == QLatin1Char('*') && i + 1 < s.size() && s[i + 1] == QLatin1Char('/')) {
            --depth;
            i += 2;
        } else {
            ++i;
        }
    }
    return i;
}

} // namespace

QString PostgresDialect::quoteIdentifier(QStringView name) const
{
    return quoteWith(name, QLatin1Char('"'));
}

QString PostgresDialect::quoteLiteral(QStringView text) const
{
    if (!text.contains(QLatin1Char('\\')))
        return quoteWith(text, QLatin1Char('\''));
    QString escaped;
    escaped.reserve(text.size() + 2);
    for (const QChar c : text) {
        if (c == QLatin1Char('\\'))
            escaped += c;
        escaped += c;
    }
    return QLatin1Char('E') + quoteWith(escaped, QLatin1Char('\''));
}

QString PostgresDialect::highlightingDefinition() const
{
    return QStringLiteral("SQL (PostgreSQL)");
}

QStringList PostgresDialect::keywords() const
{
    static const QStringList kKeywords = [] {
        const char *words[] = {
            "ALL",          "ALTER",        "ANALYSE",      "ANALYZE",
            "AND",          "ANY",          "ARRAY",        "AS",
            "ASC",          "BEGIN",        "BETWEEN",      "BIGINT",
            "BOOLEAN",      "BY",           "CASE",         "CAST",
            "CHECK",        "COLLATE",      "COLUMN",       "COMMENT",
            "COMMIT",       "CONCURRENTLY", "CONSTRAINT",   "COPY",
            "CREATE",       "CROSS",        "CURRENT_DATE", "CURRENT_TIMESTAMP",
            "CURRENT_USER", "DATABASE",     "DEFAULT",      "DEFERRABLE",
            "DELETE",       "DESC",         "DISTINCT",     "DO",
            "DROP",         "ELSE",         "END",          "EXCEPT",
            "EXECUTE",      "EXISTS",       "EXPLAIN",      "FALSE",
            "FETCH",        "FILTER",       "FOR",          "FOREIGN",
            "FROM",         "FULL",         "FUNCTION",     "GRANT",
            "GROUP",        "HAVING",       "ILIKE",        "IN",
            "INDEX",        "INNER",        "INSERT",       "INT",
            "INTEGER",      "INTERSECT",    "INTERVAL",     "INTO",
            "IS",           "JOIN",         "JSON",         "JSONB",
            "LATERAL",      "LEFT",         "LIKE",         "LIMIT",
            "LOCK",         "MATERIALIZED", "NATURAL",      "NOT",
            "NULL",         "NUMERIC",      "OFFSET",       "ON",
            "ONLY",         "OR",           "ORDER",        "OUTER",
            "OVER",         "PARTITION",    "PRIMARY",      "PROCEDURE",
            "REFERENCES",   "REFRESH",      "RETURNING",    "RETURNS",
            "REVOKE",       "RIGHT",        "ROLLBACK",     "ROW",
            "SCHEMA",       "SELECT",       "SEQUENCE",     "SERIAL",
            "SET",          "SHOW",         "SIMILAR",      "SOME",
            "TABLE",        "TABLESPACE",   "TEMP",         "TEMPORARY",
            "TEXT",         "THEN",         "TIMESTAMP",    "TIMESTAMPTZ",
            "TO",           "TRANSACTION",  "TRIGGER",      "TRUE",
            "TRUNCATE",     "UNION",        "UNIQUE",       "UPDATE",
            "USER",         "USING",        "UUID",         "VACUUM",
            "VALUES",       "VARCHAR",      "VIEW",         "WHEN",
            "WHERE",        "WINDOW",       "WITH",
        };
        QStringList out;
        for (const char *word : words)
            out.push_back(QString::fromLatin1(word));
        return out;
    }();
    return kKeywords;
}

std::vector<StatementRange> PostgresDialect::splitStatements(QStringView script) const
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
        const bool afterWord = i > 0 && isWordChar(script[i - 1]);
        if (c.isSpace()) {
            ++i;
        } else if (c == QLatin1Char('-') && i + 1 < n && script[i + 1] == QLatin1Char('-')) {
            while (i < n && script[i] != QLatin1Char('\n'))
                ++i;
        } else if (c == QLatin1Char('/') && i + 1 < n && script[i + 1] == QLatin1Char('*')) {
            i = skipBlockComment(script, i);
        } else if (c == QLatin1Char(';')) {
            flush(i);
            start = i + 1;
            ++i;
        } else {
            hasToken = true;
            if ((c == QLatin1Char('E') || c == QLatin1Char('e')) && !afterWord && i + 1 < n
                && script[i + 1] == QLatin1Char('\'')) {
                i = skipQuoted(script, i + 1, QLatin1Char('\''), true);
            } else if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
                i = skipQuoted(script, i, c, false);
            } else if (c == QLatin1Char('$') && !afterWord && dollarTagLength(script, i) > 0) {
                const qsizetype tagLength = dollarTagLength(script, i);
                const qsizetype close = script.indexOf(script.mid(i, tagLength), i + tagLength);
                i = close < 0 ? n : close + tagLength;
            } else {
                ++i;
            }
        }
    }
    flush(n);
    return out;
}

QString PostgresDialect::limitClause(qint64 limit, qint64 offset) const
{
    QString out;
    if (limit >= 0)
        out = QStringLiteral("LIMIT %1").arg(limit);
    if (offset > 0)
        out += QStringLiteral("%1OFFSET %2")
                   .arg(out.isEmpty() ? QString() : QStringLiteral(" "))
                   .arg(offset);
    return out;
}

QString PostgresDialect::explainPrefix(ExplainMode mode) const
{
    return mode == ExplainMode::Plan ? QStringLiteral("EXPLAIN ")
                                     : QStringLiteral("EXPLAIN (ANALYZE, BUFFERS) ");
}

QString PostgresDialect::placeholder(int index) const
{
    return QStringLiteral("$%1").arg(index);
}

QString PostgresDialect::qualifiedName(const ObjectPath &table) const
{
    const qsizetype n = table.parts.size();
    const QString rel = quoteIdentifier(n > 0 ? table.parts.at(n - 1) : QString());
    return n >= 2 ? quoteIdentifier(table.parts.at(n - 2)) + QLatin1Char('.') + rel : rel;
}

std::optional<QString> PostgresDialect::rowIdentifier() const
{
    return QStringLiteral("ctid");
}

} // namespace quarry::postgres
