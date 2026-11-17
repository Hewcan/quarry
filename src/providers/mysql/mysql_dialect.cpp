#include "mysql_dialect.h"

namespace quarry::mysql {

namespace {

// Index just past the closing quote of a '...' or "..." run that opens at `i`; n if
// unterminated. A doubled quote and \x are escapes inside the string; a backtick run only has the
// doubled form.
qsizetype skipQuoted(QStringView s, qsizetype i, QChar quote)
{
    const qsizetype n = s.size();
    const bool backslash = quote != QLatin1Char('`');
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

// Index just past the line comment that starts at `i`.
qsizetype skipLine(QStringView s, qsizetype i)
{
    while (i < s.size() && s[i] != QLatin1Char('\n'))
        ++i;
    return i;
}

// "-- " starts a comment only when a space or control character (or the end) follows.
bool isDashComment(QStringView s, qsizetype i)
{
    return s[i] == QLatin1Char('-') && i + 1 < s.size() && s[i + 1] == QLatin1Char('-')
        && (i + 2 >= s.size() || s[i + 2].isSpace() || s[i + 2].unicode() < 32);
}

// Only whitespace between the start of the line and `i`: the mysql client recognizes its own
// commands there and nowhere else.
bool atLineStart(QStringView s, qsizetype i)
{
    for (qsizetype j = i; j > 0 && s[j - 1] != QLatin1Char('\n'); --j) {
        if (!s[j - 1].isSpace())
            return false;
    }
    return true;
}

// If the DELIMITER command starts at `i`, returns the index of the end of its line and sets
// `delimiter`; otherwise -1.
qsizetype delimiterCommand(QStringView s, qsizetype i, QString &delimiter)
{
    static constexpr QLatin1String kWord("delimiter");
    if (s.mid(i, kWord.size()).compare(kWord, Qt::CaseInsensitive) != 0)
        return -1;
    qsizetype j = i + kWord.size();
    if (j >= s.size() || (s[j] != QLatin1Char(' ') && s[j] != QLatin1Char('\t')))
        return -1;
    const qsizetype lineEnd = skipLine(s, j);
    const QStringView argument = s.mid(j, lineEnd - j).trimmed();
    if (argument.isEmpty())
        return -1;
    const qsizetype space = argument.indexOf(QLatin1Char(' '));
    delimiter = argument.left(space < 0 ? argument.size() : space).toString();
    return lineEnd;
}

} // namespace

QString MysqlDialect::quoteIdentifier(QStringView name) const
{
    QString out;
    out.reserve(name.size() + 2);
    out += QLatin1Char('`');
    for (const QChar c : name) {
        if (c == QLatin1Char('`'))
            out += c;
        out += c;
    }
    out += QLatin1Char('`');
    return out;
}

QString MysqlDialect::quoteLiteral(QStringView text) const
{
    QString out;
    out.reserve(text.size() + 2);
    out += QLatin1Char('\'');
    for (const QChar c : text) {
        if (c == QLatin1Char('\''))
            out += QLatin1Char('\'');
        else if (c == QLatin1Char('\\'))
            out += QLatin1Char('\\');
        else if (c.unicode() == 0) {
            out += QLatin1String("\\0");
            continue;
        }
        out += c;
    }
    out += QLatin1Char('\'');
    return out;
}

QString MysqlDialect::highlightingDefinition() const
{
    return QStringLiteral("SQL (MySQL)");
}

QStringList MysqlDialect::keywords() const
{
    static const QStringList kKeywords = [] {
        const char *words[] = {
            "ADD",          "ALL",
            "ALTER",        "ANALYZE",
            "AND",          "AS",
            "ASC",          "AUTO_INCREMENT",
            "BEGIN",        "BETWEEN",
            "BIGINT",       "BINARY",
            "BLOB",         "BOOLEAN",
            "BY",           "CALL",
            "CASCADE",      "CASE",
            "CHANGE",       "CHAR",
            "CHARACTER",    "CHECK",
            "COLLATE",      "COLUMN",
            "COLUMNS",      "COMMENT",
            "COMMIT",       "CONSTRAINT",
            "CREATE",       "CROSS",
            "CURRENT_DATE", "CURRENT_TIMESTAMP",
            "CURRENT_USER", "DATABASE",
            "DATABASES",    "DATE",
            "DATETIME",     "DECIMAL",
            "DEFAULT",      "DELETE",
            "DELIMITER",    "DESC",
            "DESCRIBE",     "DISTINCT",
            "DIV",          "DOUBLE",
            "DROP",         "DUPLICATE",
            "ELSE",         "END",
            "ENGINE",       "ENUM",
            "EXCEPT",       "EXISTS",
            "EXPLAIN",      "FALSE",
            "FLOAT",        "FOR",
            "FORCE",        "FOREIGN",
            "FROM",         "FULL",
            "FULLTEXT",     "FUNCTION",
            "GRANT",        "GROUP",
            "HAVING",       "IF",
            "IGNORE",       "IN",
            "INDEX",        "INNER",
            "INSERT",       "INT",
            "INTEGER",      "INTERSECT",
            "INTO",         "IS",
            "JOIN",         "JSON",
            "KEY",          "KEYS",
            "KILL",         "LEFT",
            "LIKE",         "LIMIT",
            "LOCK",         "LONGTEXT",
            "MEDIUMTEXT",   "MODIFY",
            "NATURAL",      "NOT",
            "NULL",         "NUMERIC",
            "OFFSET",       "ON",
            "OPTIMIZE",     "OR",
            "ORDER",        "OUTER",
            "OVER",         "PARTITION",
            "PRIMARY",      "PROCEDURE",
            "REFERENCES",   "RENAME",
            "REPLACE",      "RETURNING",
            "RETURNS",      "REVOKE",
            "RIGHT",        "RLIKE",
            "ROLLBACK",     "ROW",
            "SCHEMA",       "SELECT",
            "SEQUENCE",     "SET",
            "SHOW",         "SMALLINT",
            "START",        "STRAIGHT_JOIN",
            "TABLE",        "TEMPORARY",
            "TEXT",         "THEN",
            "TIME",         "TIMESTAMP",
            "TINYINT",      "TO",
            "TRANSACTION",  "TRIGGER",
            "TRUE",         "TRUNCATE",
            "UNION",        "UNIQUE",
            "UNLOCK",       "UNSIGNED",
            "UPDATE",       "USE",
            "USER",         "USING",
            "VALUES",       "VARBINARY",
            "VARCHAR",      "VIEW",
            "WHEN",         "WHERE",
            "WINDOW",       "WITH",
            "XOR",          "YEAR",
            "ZEROFILL",
        };
        QStringList out;
        for (const char *word : words)
            out.push_back(QString::fromLatin1(word));
        return out;
    }();
    return kKeywords;
}

QStringList MysqlDialect::builtinFunctions() const
{
    // shortcut: static list of common built-ins of both engines (not the whole manual); stored
    // and user-defined functions are not included.
    static const QStringList kFunctions = [] {
        const char *names[] = {
            "abs",
            "acos",
            "adddate",
            "addtime",
            "aes_decrypt",
            "aes_encrypt",
            "ascii",
            "asin",
            "atan",
            "atan2",
            "avg",
            "benchmark",
            "bin",
            "bit_and",
            "bit_count",
            "bit_length",
            "bit_or",
            "bit_xor",
            "cast",
            "ceil",
            "ceiling",
            "char_length",
            "character_length",
            "charset",
            "coalesce",
            "collation",
            "concat",
            "concat_ws",
            "connection_id",
            "conv",
            "convert",
            "convert_tz",
            "cos",
            "cot",
            "count",
            "crc32",
            "cume_dist",
            "curdate",
            "current_date",
            "current_time",
            "current_timestamp",
            "current_user",
            "curtime",
            "database",
            "date",
            "date_add",
            "date_format",
            "date_sub",
            "datediff",
            "day",
            "dayname",
            "dayofmonth",
            "dayofweek",
            "dayofyear",
            "decode",
            "default",
            "degrees",
            "dense_rank",
            "elt",
            "encode",
            "exp",
            "export_set",
            "extract",
            "field",
            "find_in_set",
            "first_value",
            "floor",
            "format",
            "found_rows",
            "from_days",
            "from_unixtime",
            "get_format",
            "get_lock",
            "greatest",
            "group_concat",
            "hex",
            "hour",
            "if",
            "ifnull",
            "inet_aton",
            "inet_ntoa",
            "insert",
            "instr",
            "is_free_lock",
            "isnull",
            "json_array",
            "json_arrayagg",
            "json_contains",
            "json_extract",
            "json_keys",
            "json_length",
            "json_merge_patch",
            "json_object",
            "json_objectagg",
            "json_query",
            "json_remove",
            "json_search",
            "json_set",
            "json_table",
            "json_type",
            "json_unquote",
            "json_valid",
            "json_value",
            "lag",
            "last_day",
            "last_insert_id",
            "last_value",
            "lcase",
            "lead",
            "least",
            "left",
            "length",
            "ln",
            "load_file",
            "locate",
            "log",
            "log10",
            "log2",
            "lower",
            "lpad",
            "ltrim",
            "make_set",
            "makedate",
            "maketime",
            "max",
            "md5",
            "microsecond",
            "mid",
            "min",
            "minute",
            "mod",
            "month",
            "monthname",
            "now",
            "nth_value",
            "ntile",
            "nullif",
            "oct",
            "octet_length",
            "ord",
            "percent_rank",
            "period_add",
            "period_diff",
            "pi",
            "position",
            "pow",
            "power",
            "quarter",
            "quote",
            "radians",
            "rand",
            "rank",
            "regexp_instr",
            "regexp_replace",
            "regexp_substr",
            "release_lock",
            "repeat",
            "replace",
            "reverse",
            "right",
            "round",
            "row_count",
            "row_number",
            "rpad",
            "rtrim",
            "schema",
            "sec_to_time",
            "second",
            "session_user",
            "sha1",
            "sha2",
            "sign",
            "sin",
            "sleep",
            "soundex",
            "space",
            "sqrt",
            "std",
            "stddev",
            "stddev_pop",
            "stddev_samp",
            "str_to_date",
            "strcmp",
            "subdate",
            "substr",
            "substring",
            "substring_index",
            "subtime",
            "sum",
            "sysdate",
            "system_user",
            "tan",
            "time",
            "time_format",
            "time_to_sec",
            "timediff",
            "timestamp",
            "timestampadd",
            "timestampdiff",
            "to_base64",
            "to_days",
            "to_seconds",
            "trim",
            "truncate",
            "ucase",
            "uncompress",
            "unhex",
            "unix_timestamp",
            "upper",
            "user",
            "utc_date",
            "utc_timestamp",
            "uuid",
            "uuid_short",
            "values",
            "var_pop",
            "var_samp",
            "variance",
            "version",
            "week",
            "weekday",
            "weekofyear",
            "year",
            "yearweek",
            "bin_to_uuid",
            "uuid_to_bin",
            "json_arrayagg",
            "json_overlaps",
            "json_pretty",
            "json_storage_size",
            "json_compact",
            "json_detailed",
            "json_loose",
            "json_exists",
            "json_equals",
            "json_normalize",
            "natural_sort_key",
            "regexp_like",
            "sformat",
            "lastval",
            "nextval",
            "setval",
            "median",
            "percentile_cont",
            "percentile_disc",
        };
        QStringList out;
        for (const char *name : names)
            out.push_back(QString::fromLatin1(name));
        return out;
    }();
    return kFunctions;
}

std::vector<StatementRange> MysqlDialect::splitStatements(QStringView script) const
{
    std::vector<StatementRange> out;
    const qsizetype n = script.size();
    QString delimiter = QStringLiteral(";");
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
        } else if (c == QLatin1Char('#') || isDashComment(script, i)) {
            i = skipLine(script, i);
        } else if (c == QLatin1Char('/') && i + 1 < n && script[i + 1] == QLatin1Char('*')) {
            // /*! ... */ and /*M! ... */ are executable: they make the statement non-empty.
            const bool code = (i + 2 < n && script[i + 2] == QLatin1Char('!'))
                || (i + 3 < n && script[i + 2] == QLatin1Char('M')
                    && script[i + 3] == QLatin1Char('!'));
            const qsizetype close = script.indexOf(QLatin1String("*/"), i + 2);
            i = close < 0 ? n : close + 2;
            hasToken = hasToken || code;
        } else if (script.mid(i, delimiter.size()) == QStringView(delimiter)) {
            flush(i);
            i += delimiter.size();
            start = i;
        } else {
            QString newDelimiter;
            if (!hasToken && atLineStart(script, i)) {
                if (const qsizetype end = delimiterCommand(script, i, newDelimiter); end >= 0) {
                    delimiter = newDelimiter;
                    i = end;
                    start = i;
                    continue;
                }
            }
            hasToken = true;
            if (c == QLatin1Char('\'') || c == QLatin1Char('"') || c == QLatin1Char('`'))
                i = skipQuoted(script, i, c);
            else
                ++i;
        }
    }
    flush(n);
    return out;
}

QString MysqlDialect::limitClause(qint64 limit, qint64 offset) const
{
    if (limit < 0 && offset <= 0)
        return {};
    // OFFSET needs a LIMIT in MySQL: use the documented "all rows" value.
    const QString count
        = limit >= 0 ? QString::number(limit) : QStringLiteral("18446744073709551615");
    QString out = QStringLiteral("LIMIT %1").arg(count);
    if (offset > 0)
        out += QStringLiteral(" OFFSET %1").arg(offset);
    return out;
}

QString MysqlDialect::explainPrefix(ExplainMode mode) const
{
    // shortcut: EXPLAIN ANALYZE is MySQL 8.0.18+ syntax; MariaDB spells it "ANALYZE <stmt>".
    // The dialect has no server flavor (see the class comment), when needed let the provider hand
    // out a dialect per connection.
    return mode == ExplainMode::Plan ? QStringLiteral("EXPLAIN ")
                                     : QStringLiteral("EXPLAIN ANALYZE ");
}

QString MysqlDialect::placeholder(int index) const
{
    Q_UNUSED(index)
    return QStringLiteral("?");
}

QString MysqlDialect::qualifiedName(const ObjectPath &table) const
{
    const qsizetype n = table.parts.size();
    const QString relation = quoteIdentifier(n > 0 ? table.parts.at(n - 1) : QString());
    return n >= 2 ? quoteIdentifier(table.parts.at(n - 2)) + QLatin1Char('.') + relation : relation;
}

std::optional<QString> MysqlDialect::rowIdentifier() const
{
    return std::nullopt;
}

} // namespace quarry::mysql
