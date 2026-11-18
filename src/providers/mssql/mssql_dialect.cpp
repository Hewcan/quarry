#include "mssql_dialect.h"

namespace quarry::mssql {

namespace {

// Index just past the closing quote of a '...' or "..." run that opens at `i`; n if
// unterminated. Only a doubled quote escapes inside.
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

// Index just past the [...] identifier that opens at `i`; ]] is an escaped bracket.
qsizetype skipBracketed(QStringView s, qsizetype i)
{
    const qsizetype n = s.size();
    ++i;
    while (i < n) {
        if (s[i] == QLatin1Char(']')) {
            if (i + 1 < n && s[i + 1] == QLatin1Char(']'))
                i += 2;
            else
                return i + 1;
        } else {
            ++i;
        }
    }
    return n;
}

// Index just past the line comment that starts at `i` (at the line break, not after it).
qsizetype skipLine(QStringView s, qsizetype i)
{
    while (i < s.size() && s[i] != QLatin1Char('\n'))
        ++i;
    return i;
}

// Index just past the nested block comment that starts at `i`; n if unterminated.
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

bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('@')
        || c == QLatin1Char('#') || c == QLatin1Char('$');
}

// Index just past the word that starts at `i`.
qsizetype wordEnd(QStringView s, qsizetype i)
{
    while (i < s.size() && isWordChar(s[i]))
        ++i;
    return i;
}

// Only whitespace between the start of the line and `i`.
bool atLineStart(QStringView s, qsizetype i)
{
    for (qsizetype j = i; j > 0 && s[j - 1] != QLatin1Char('\n'); --j) {
        if (!s[j - 1].isSpace())
            return false;
    }
    return true;
}

// The word that follows `i`, in upper case, after whitespace and comments; empty if none.
QString nextWord(QStringView s, qsizetype i)
{
    const qsizetype n = s.size();
    while (i < n) {
        if (s[i].isSpace())
            ++i;
        else if (s[i] == QLatin1Char('-') && i + 1 < n && s[i + 1] == QLatin1Char('-'))
            i = skipLine(s, i);
        else if (s[i] == QLatin1Char('/') && i + 1 < n && s[i + 1] == QLatin1Char('*'))
            i = skipBlock(s, i);
        else
            break;
    }
    return s.mid(i, wordEnd(s, i) - i).toString().toUpper();
}

// If a batch separator starts at `i` (the word GO alone on its line, an optional count and a
// comment allowed after it), returns the index of the end of that line; otherwise -1.
qsizetype goCommand(QStringView s, qsizetype i)
{
    const qsizetype end = wordEnd(s, i);
    if (end - i != 2 || s.mid(i, 2).compare(QLatin1String("go"), Qt::CaseInsensitive) != 0)
        return -1;
    const qsizetype n = s.size();
    auto blank
        = [&s](qsizetype j) { return s[j] == QLatin1Char(' ') || s[j] == QLatin1Char('\t'); };
    qsizetype j = end;
    while (j < n && blank(j))
        ++j;
    while (j < n && s[j].isDigit())
        ++j;
    while (j < n && blank(j))
        ++j;
    if (j < n && s[j] == QLatin1Char('\r'))
        ++j;
    if (j >= n || s[j] == QLatin1Char('\n'))
        return j;
    if (s[j] == QLatin1Char('-') && j + 1 < n && s[j + 1] == QLatin1Char('-'))
        return skipLine(s, j);
    return -1;
}

bool opensBlock(const QString &next)
{
    return next != QLatin1String("TRAN") && next != QLatin1String("TRANSACTION")
        && next != QLatin1String("DISTRIBUTED") && next != QLatin1String("CONVERSATION")
        && next != QLatin1String("DIALOG");
}

} // namespace

QString MssqlDialect::quoteIdentifier(QStringView name) const
{
    QString out;
    out.reserve(name.size() + 2);
    out += QLatin1Char('[');
    for (const QChar c : name) {
        if (c == QLatin1Char(']'))
            out += c;
        out += c;
    }
    out += QLatin1Char(']');
    return out;
}

QString MssqlDialect::quoteLiteral(QStringView text) const
{
    QString out;
    out.reserve(text.size() + 3);
    out += QLatin1String("N'");
    for (const QChar c : text) {
        if (c == QLatin1Char('\''))
            out += c;
        out += c;
    }
    out += QLatin1Char('\'');
    return out;
}

QString MssqlDialect::highlightingDefinition() const
{
    return QStringLiteral("SQL (Transact-SQL)");
}

QStringList MssqlDialect::keywords() const
{
    static const QStringList kKeywords = [] {
        const char *words[] = {
            "ADD",       "ALL",          "ALTER",       "AND",      "ANY",       "AS",
            "ASC",       "BACKUP",       "BEGIN",       "BETWEEN",  "BIGINT",    "BINARY",
            "BIT",       "BREAK",        "BY",          "CASCADE",  "CASE",      "CAST",
            "CATCH",     "CHAR",         "CHECK",       "CLOSE",    "CLUSTERED", "COLLATE",
            "COLUMN",    "COMMIT",       "CONSTRAINT",  "CONTINUE", "CREATE",    "CROSS",
            "CURSOR",    "DATABASE",     "DATE",        "DATETIME", "DATETIME2", "DATETIMEOFFSET",
            "DBCC",      "DEALLOCATE",   "DECIMAL",     "DECLARE",  "DEFAULT",   "DELETE",
            "DENY",      "DESC",         "DISTINCT",    "DROP",     "ELSE",      "END",
            "EXCEPT",    "EXEC",         "EXECUTE",     "EXISTS",   "FETCH",     "FILLFACTOR",
            "FLOAT",     "FOR",          "FOREIGN",     "FROM",     "FULL",      "FUNCTION",
            "GO",        "GOTO",         "GRANT",       "GROUP",    "HAVING",    "IDENTITY",
            "IF",        "IN",           "INDEX",       "INNER",    "INSERT",    "INT",
            "INTERSECT", "INTO",         "IS",          "JOIN",     "KEY",       "LEFT",
            "LIKE",      "MERGE",        "MONEY",       "NCHAR",    "NEXT",      "NOCOUNT",
            "NOLOCK",    "NONCLUSTERED", "NOT",         "NULL",     "NUMERIC",   "NVARCHAR",
            "OFFSET",    "ON",           "ONLY",        "OPEN",     "OPTION",    "OR",
            "ORDER",     "OUTER",        "OUTPUT",      "OVER",     "PARTITION", "PERCENT",
            "PIVOT",     "PRIMARY",      "PRINT",       "PROC",     "PROCEDURE", "RAISERROR",
            "READONLY",  "REAL",         "REFERENCES",  "RETURN",   "RETURNS",   "REVOKE",
            "RIGHT",     "ROLLBACK",     "ROW",         "ROWS",     "SCHEMA",    "SELECT",
            "SET",       "SMALLINT",     "TABLE",       "THEN",     "THROW",     "TIME",
            "TOP",       "TRAN",         "TRANSACTION", "TRIGGER",  "TRUNCATE",  "TRY",
            "UNION",     "UNIQUE",       "UNPIVOT",     "UPDATE",   "USE",       "VALUES",
            "VARBINARY", "VARCHAR",      "VIEW",        "WAITFOR",  "WHEN",      "WHERE",
            "WHILE",     "WITH",
        };
        QStringList out;
        for (const char *word : words)
            out.push_back(QString::fromLatin1(word));
        return out;
    }();
    return kKeywords;
}

QStringList MssqlDialect::builtinFunctions() const
{
    // shortcut: the common built-ins of SQL Server, not the whole manual; user-defined functions
    // are not included.
    static const QStringList kFunctions = [] {
        const char *names[] = {
            "abs",
            "avg",
            "cast",
            "ceiling",
            "charindex",
            "checksum",
            "coalesce",
            "concat",
            "concat_ws",
            "convert",
            "count",
            "count_big",
            "dateadd",
            "datediff",
            "datefromparts",
            "datename",
            "datepart",
            "day",
            "db_id",
            "db_name",
            "eomonth",
            "floor",
            "format",
            "getdate",
            "getutcdate",
            "grouping",
            "iif",
            "isdate",
            "isnull",
            "isnumeric",
            "json_value",
            "json_query",
            "lag",
            "lead",
            "left",
            "len",
            "lower",
            "ltrim",
            "max",
            "min",
            "month",
            "newid",
            "nullif",
            "object_id",
            "object_name",
            "patindex",
            "power",
            "rank",
            "dense_rank",
            "replace",
            "replicate",
            "reverse",
            "right",
            "round",
            "row_number",
            "rtrim",
            "scope_identity",
            "sign",
            "sqrt",
            "str",
            "string_agg",
            "string_split",
            "stuff",
            "substring",
            "sum",
            "sysdatetime",
            "sysutcdatetime",
            "trim",
            "try_cast",
            "try_convert",
            "upper",
            "year",
        };
        QStringList out;
        for (const char *name : names)
            out.push_back(QString::fromLatin1(name));
        return out;
    }();
    return kFunctions;
}

std::vector<StatementRange> MssqlDialect::splitStatements(QStringView script) const
{
    std::vector<StatementRange> out;
    const qsizetype n = script.size();
    qsizetype start = 0;
    bool hasToken = false;
    int depth = 0; // open BEGIN ... END and CASE ... END blocks

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
            if (depth > 0) {
                hasToken = true;
            } else {
                flush(i);
                start = i + 1;
            }
            ++i;
        } else if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            hasToken = true;
            i = skipQuoted(script, i, c);
        } else if (c == QLatin1Char('[')) {
            hasToken = true;
            i = skipBracketed(script, i);
        } else if (isWordChar(c)) {
            if (atLineStart(script, i)) {
                if (const qsizetype lineEnd = goCommand(script, i); lineEnd >= 0) {
                    flush(i);
                    depth = 0;
                    i = lineEnd;
                    start = i;
                    continue;
                }
            }
            const qsizetype end = wordEnd(script, i);
            // A word after a dot is a name (t.end), not a keyword.
            const bool qualified = i > 0 && script[i - 1] == QLatin1Char('.');
            if (!qualified) {
                const QString word = script.mid(i, end - i).toString().toUpper();
                if (word == QLatin1String("BEGIN")) {
                    if (opensBlock(nextWord(script, end)))
                        ++depth;
                } else if (word == QLatin1String("CASE")) {
                    ++depth;
                } else if (word == QLatin1String("END")) {
                    // END CONVERSATION is a statement of Service Broker, not the end of a block.
                    if (depth > 0 && nextWord(script, end) != QLatin1String("CONVERSATION"))
                        --depth;
                }
            }
            hasToken = true;
            i = end;
        } else {
            hasToken = true;
            ++i;
        }
    }
    flush(n);
    return out;
}

QString MssqlDialect::limitClause(qint64 limit, qint64 offset) const
{
    if (limit < 0 && offset <= 0)
        return {};
    QString out = QStringLiteral("OFFSET %1 ROWS").arg(offset > 0 ? offset : 0);
    if (limit >= 0)
        out += QStringLiteral(" FETCH NEXT %1 ROWS ONLY").arg(limit);
    return out;
}

QString MssqlDialect::placeholder(int index) const
{
    Q_UNUSED(index)
    return QStringLiteral("?");
}

QString MssqlDialect::qualifiedName(const ObjectPath &table) const
{
    QStringList parts;
    parts.reserve(table.parts.size());
    for (const QString &part : table.parts) {
        if (!part.isEmpty())
            parts << quoteIdentifier(part);
    }
    return parts.join(QLatin1Char('.'));
}

std::optional<QString> MssqlDialect::rowIdentifier() const
{
    return std::nullopt;
}

} // namespace quarry::mssql
