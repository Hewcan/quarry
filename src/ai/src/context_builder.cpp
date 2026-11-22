#include <quarry/ai/context_builder.h>

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace quarry::ai {

using namespace Qt::StringLiterals;

namespace {

constexpr qsizetype kMaxCacheEntries = 32;

bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == u'_' || c == u'$';
}

QSet<QString> words(QStringView text)
{
    QSet<QString> out;
    qsizetype start = -1;
    for (qsizetype i = 0; i <= text.size(); ++i) {
        const bool word = i < text.size() && isWordChar(text[i]);
        if (word && start < 0)
            start = i;
        else if (!word && start >= 0) {
            out.insert(text.mid(start, i - start).toString().toLower());
            start = -1;
        }
    }
    return out;
}

// Metadata is untrusted: one line per element, no control characters, so a comment cannot
// open a new line in the prompt or close the block it lives in.
QString clean(QStringView text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text)
        out.append(c.category() == QChar::Other_Control || c == u' ' || c == u' ' ? QChar(u' ')
                                                                                      : c);
    out.replace(u"</"_s, u"<\\/"_s);
    return out.simplified();
}

QString qualified(QStringView schema, QStringView name)
{
    return schema.isEmpty() ? clean(name) : clean(schema) + u'.' + clean(name);
}

QString joinClean(const QStringList &list)
{
    QStringList out;
    out.reserve(list.size());
    for (const QString &s : list)
        out.append(clean(s));
    return out.join(u", ");
}

QString columnType(const AiColumn &column, const ContextOptions &options)
{
    QString type = clean(column.type);
    if (!options.sendEnumValues) {
        static const QRegularExpression kEnumList(uR"(^\s*(enum|set)\s*\(.*\)\s*$)"_s,
                                                  QRegularExpression::CaseInsensitiveOption);
        const auto match = kEnumList.match(type);
        if (match.hasMatch())
            type = match.captured(1) + u"(…)";
    } else if (!column.enumValues.isEmpty()) {
        type += u" /* values: " + joinClean(column.enumValues).replace(u"*/"_s, u"* /"_s) + u" */";
    }
    return type;
}

bool matchesRequest(const QString &name, const QSet<QString> &requestWords)
{
    return requestWords.contains(name) || requestWords.contains(name + u's')
        || requestWords.contains(name + u"es")
        || (name.endsWith(u's') && requestWords.contains(name.chopped(1)));
}

} // namespace

void ContextBuilder::setSchema(SchemaContext schema)
{
    m_schema = std::move(schema);
    m_cache.clear();
}

bool ContextBuilder::isExcluded(QStringView schema, QStringView table,
                                const ContextOptions &options)
{
    for (const QString &s : options.excludedSchemas)
        if (!schema.isEmpty() && schema.compare(s, Qt::CaseInsensitive) == 0)
            return true;
    const QString full = schema.isEmpty() ? table.toString() : schema.toString() + u'.' + table;
    for (const QString &t : options.excludedTables) {
        if (t.contains(u'.') ? full.compare(t, Qt::CaseInsensitive) == 0
                             : table.compare(t, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

QString ContextBuilder::renderTable(const AiTable &table, const ContextOptions &options,
                                    const SchemaContext &all)
{
    Q_UNUSED(all);
    const QString name = qualified(table.schema, table.name);
    QString out;
    if (options.sendComments && !table.comment.isEmpty())
        out += u"-- " + clean(table.comment) + u'\n';
    out += (table.isView ? u"CREATE VIEW "_s : u"CREATE TABLE "_s) + name + u" (\n";

    QStringList lines;
    QStringList comments;
    QStringList pk;
    for (const AiColumn &c : table.columns)
        if (c.primaryKey)
            pk.append(clean(c.name));
    for (const AiColumn &c : table.columns) {
        QString line = u"  " + clean(c.name) + u' ' + columnType(c, options);
        if (!c.nullable)
            line += u" NOT NULL";
        if (c.primaryKey && pk.size() == 1)
            line += u" PRIMARY KEY";
        if (options.sendDefaults && !c.defaultValue.isEmpty())
            line += u" DEFAULT " + clean(c.defaultValue);
        lines.append(line);
        comments.append(options.sendComments && !c.comment.isEmpty() ? clean(c.comment)
                                                                     : QString());
    }
    if (pk.size() > 1) {
        lines.append(u"  PRIMARY KEY (" + pk.join(u", ") + u')');
        comments.append(QString());
    }
    if (options.sendChecks) {
        for (const QString &check : table.checks) {
            lines.append(u"  CHECK (" + clean(check) + u')');
            comments.append(QString());
        }
    }
    for (const AiForeignKey &fk : table.foreignKeys) {
        if (isExcluded(fk.refSchema, fk.refTable, options))
            continue; // even the name of an excluded table stays out
        lines.append(u"  FOREIGN KEY (" + joinClean(fk.columns) + u") REFERENCES "
                     + qualified(fk.refSchema, fk.refTable) + u" (" + joinClean(fk.refColumns)
                     + u')');
        comments.append(QString());
    }
    for (qsizetype i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size())
            out += u',';
        if (!comments[i].isEmpty())
            out += u" -- " + comments[i];
        out += u'\n';
    }
    out += u");\n";
    if (table.isView) {
        if (options.sendDefinitions && !table.definition.isEmpty())
            out += u"-- definition: " + clean(table.definition) + u'\n';
    }
    for (const AiIndex &index : table.indexes) {
        out += (index.unique ? u"CREATE UNIQUE INDEX "_s : u"CREATE INDEX "_s) + clean(index.name)
            + u" ON " + name + u" (" + joinClean(index.columns) + u");\n";
    }
    return out;
}

SelectedContext ContextBuilder::build(QStringView script, QStringView request,
                                      const ContextOptions &options) const
{
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(script.toUtf8());
    hash.addData(QByteArrayView("\x1f"));
    hash.addData(request.toUtf8());
    hash.addData(QByteArrayView("\x1f"));
    hash.addData(
        QStringList { options.excludedSchemas.join(u'\x1e'), options.excludedTables.join(u'\x1e') }
            .join(u'\x1f')
            .toUtf8());
    const char flags[] = { char('0' + options.sendComments), char('0' + options.sendDefaults),
                           char('0' + options.sendChecks), char('0' + options.sendEnumValues),
                           char('0' + options.sendDefinitions) };
    hash.addData(QByteArrayView(flags, sizeof flags));
    hash.addData(QByteArray::number(options.maxTables));
    const QByteArray key = hash.result();
    if (const auto it = m_cache.constFind(key); it != m_cache.constEnd())
        return *it;

    const QSet<QString> scriptWords = words(script);
    const QSet<QString> requestWords = words(request);
    const auto &tables = m_schema.tables;
    const auto count = static_cast<qsizetype>(tables.size());

    // Priority: 0 named in the script, 1 named in the request, 2 one FK hop away.
    std::vector<int> priority(tables.size(), -1);
    for (qsizetype i = 0; i < count; ++i) {
        const AiTable &t = tables[i];
        if (isExcluded(t.schema, t.name, options))
            continue;
        const QString lower = t.name.toLower();
        if (scriptWords.contains(lower))
            priority[i] = 0;
        else if (matchesRequest(lower, requestWords))
            priority[i] = 1;
    }
    auto indexOf = [&](const QString &schema, const QString &name) -> qsizetype {
        for (qsizetype i = 0; i < count; ++i) {
            if (tables[i].name.compare(name, Qt::CaseInsensitive) == 0
                && (schema.isEmpty() || tables[i].schema.compare(schema, Qt::CaseInsensitive) == 0))
                return i;
        }
        return -1;
    };
    const std::vector<int> direct = priority;
    for (qsizetype i = 0; i < count; ++i) {
        const AiTable &t = tables[i];
        for (const AiForeignKey &fk : t.foreignKeys) {
            const qsizetype target = indexOf(fk.refSchema, fk.refTable);
            if (target < 0 || isExcluded(tables[target].schema, tables[target].name, options)
                || isExcluded(t.schema, t.name, options))
                continue;
            if (direct[i] >= 0 && priority[target] < 0)
                priority[target] = 2;
            if (direct[target] >= 0 && priority[i] < 0)
                priority[i] = 2;
        }
    }

    std::vector<qsizetype> chosen;
    for (qsizetype i = 0; i < count; ++i)
        if (priority[i] >= 0)
            chosen.push_back(i);
    std::stable_sort(chosen.begin(), chosen.end(),
                     [&](qsizetype a, qsizetype b) { return priority[a] < priority[b]; });
    if (static_cast<qsizetype>(chosen.size()) > options.maxTables)
        chosen.resize(static_cast<size_t>(std::max(0, options.maxTables)));
    std::sort(chosen.begin(), chosen.end());

    SelectedContext out;
    out.dialect = clean(m_schema.dialect);
    for (const qsizetype i : chosen) {
        out.tables.append(qualified(tables[i].schema, tables[i].name));
        out.ddl += renderTable(tables[i], options, m_schema);
    }
    if (m_cache.size() >= kMaxCacheEntries)
        m_cache.clear();
    m_cache.insert(key, out);
    return out;
}

} // namespace quarry::ai
