#include "postgres_dialect.h"

#include "postgres_explain.h"

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

QStringList PostgresDialect::builtinFunctions() const
{
    // shortcut: static list of common built-ins (not the whole catalog); user functions and
    // extensions are not included.
    static const QStringList kFunctions = [] {
        const char *names[] = {
            "abs",
            "avg",
            "bool_and",
            "bool_or",
            "count",
            "every",
            "max",
            "min",
            "string_agg",
            "sum",
            "array_agg",
            "json_agg",
            "jsonb_agg",
            "json_object_agg",
            "jsonb_object_agg",
            "xmlagg",
            "bit_and",
            "bit_or",
            "stddev",
            "stddev_pop",
            "stddev_samp",
            "variance",
            "var_pop",
            "var_samp",
            "corr",
            "covar_pop",
            "covar_samp",
            "regr_slope",
            "regr_intercept",
            "mode",
            "percentile_cont",
            "percentile_disc",
            "ascii",
            "bit_length",
            "btrim",
            "char_length",
            "character_length",
            "chr",
            "concat",
            "concat_ws",
            "format",
            "initcap",
            "left",
            "length",
            "lower",
            "lpad",
            "ltrim",
            "md5",
            "octet_length",
            "overlay",
            "position",
            "quote_ident",
            "quote_literal",
            "quote_nullable",
            "regexp_match",
            "regexp_matches",
            "regexp_replace",
            "regexp_split_to_array",
            "regexp_split_to_table",
            "repeat",
            "replace",
            "reverse",
            "right",
            "rpad",
            "rtrim",
            "split_part",
            "starts_with",
            "strpos",
            "substr",
            "substring",
            "translate",
            "trim",
            "upper",
            "to_hex",
            "encode",
            "decode",
            "ceil",
            "ceiling",
            "div",
            "exp",
            "floor",
            "ln",
            "log",
            "log10",
            "mod",
            "pi",
            "power",
            "random",
            "round",
            "scale",
            "setseed",
            "sign",
            "sqrt",
            "trunc",
            "width_bucket",
            "greatest",
            "least",
            "sin",
            "cos",
            "tan",
            "asin",
            "acos",
            "atan",
            "atan2",
            "degrees",
            "radians",
            "cbrt",
            "factorial",
            "gcd",
            "lcm",
            "age",
            "clock_timestamp",
            "current_date",
            "current_time",
            "current_timestamp",
            "date_part",
            "date_trunc",
            "extract",
            "isfinite",
            "justify_days",
            "justify_hours",
            "justify_interval",
            "localtime",
            "localtimestamp",
            "make_date",
            "make_interval",
            "make_time",
            "make_timestamp",
            "make_timestamptz",
            "now",
            "statement_timestamp",
            "timeofday",
            "to_char",
            "to_date",
            "to_number",
            "to_timestamp",
            "transaction_timestamp",
            "array_append",
            "array_cat",
            "array_dims",
            "array_fill",
            "array_length",
            "array_lower",
            "array_ndims",
            "array_position",
            "array_positions",
            "array_prepend",
            "array_remove",
            "array_replace",
            "array_to_string",
            "array_upper",
            "cardinality",
            "string_to_array",
            "unnest",
            "to_json",
            "to_jsonb",
            "json_array_elements",
            "json_array_elements_text",
            "json_array_length",
            "json_build_array",
            "json_build_object",
            "json_each",
            "json_each_text",
            "json_extract_path",
            "json_extract_path_text",
            "json_object",
            "json_object_keys",
            "json_populate_record",
            "json_strip_nulls",
            "json_typeof",
            "jsonb_array_elements",
            "jsonb_array_elements_text",
            "jsonb_array_length",
            "jsonb_build_array",
            "jsonb_build_object",
            "jsonb_each",
            "jsonb_each_text",
            "jsonb_extract_path",
            "jsonb_extract_path_text",
            "jsonb_insert",
            "jsonb_object_keys",
            "jsonb_path_exists",
            "jsonb_path_query",
            "jsonb_pretty",
            "jsonb_set",
            "jsonb_strip_nulls",
            "jsonb_typeof",
            "row_to_json",
            "row_number",
            "rank",
            "dense_rank",
            "percent_rank",
            "cume_dist",
            "ntile",
            "lag",
            "lead",
            "first_value",
            "last_value",
            "nth_value",
            "coalesce",
            "nullif",
            "generate_series",
            "generate_subscripts",
            "gen_random_uuid",
            "uuid_generate_v4",
            "pg_typeof",
            "current_database",
            "current_schema",
            "current_user",
            "session_user",
            "version",
            "pg_size_pretty",
            "pg_relation_size",
            "pg_total_relation_size",
            "pg_table_size",
            "pg_database_size",
            "set_config",
            "current_setting",
            "to_tsvector",
            "to_tsquery",
            "plainto_tsquery",
            "ts_rank",
            "txid_current",
            "inet_client_addr",
        };
        QStringList out;
        for (const char *name : names)
            out.push_back(QString::fromLatin1(name));
        return out;
    }();
    return kFunctions;
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

QString PostgresDialect::explainStatement(const QString &sql, ExplainMode mode,
                                          const QString &product) const
{
    Q_UNUSED(product)
    QString body = sql.trimmed();
    while (body.endsWith(QLatin1Char(';')))
        body = body.chopped(1).trimmed();
    if (body.isEmpty())
        return {};
    return (mode == ExplainMode::Plan ? QStringLiteral("EXPLAIN (FORMAT JSON) ")
                                      : QStringLiteral("EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) "))
        + body;
}

Result<ExplainPlan> PostgresDialect::parseExplain(const QString &output, ExplainMode mode,
                                                  const QString &product) const
{
    Q_UNUSED(product)
    return parseExplainJson(output, mode);
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
