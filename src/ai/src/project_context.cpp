#include <quarry/ai/project_context.h>

#include <QRegularExpression>
#include <QSet>

namespace quarry::ai {

using namespace Qt::StringLiterals;

namespace {

constexpr qsizetype kMaxCases = 60;

QString stripComments(QStringView php)
{
    static const QRegularExpression kBlock(uR"(/\*.*?\*/)"_s,
                                           QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression kLine(uR"(^[ \t]*(//|#(?!\[)).*$)"_s,
                                          QRegularExpression::MultilineOption);
    QString out = php.toString();
    out.remove(kBlock);
    out.remove(kLine);
    return out;
}

QString lastSegment(const QString &name)
{
    return name.mid(name.lastIndexOf(u'\\') + 1);
}

// One line, no control characters: PHP source is untrusted like any metadata.
QString clean(QStringView text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text)
        out.append(c.category() == QChar::Other_Control ? QChar(u' ') : c);
    return out.simplified();
}

QString unquote(QString value)
{
    value = value.trimmed();
    if (value.size() >= 2 && (value.startsWith(u'\'') || value.startsWith(u'"'))
        && value.endsWith(value.front()))
        return value.mid(1, value.size() - 2);
    return value;
}

QString pluralize(const QString &word)
{
    if (word.endsWith(u'y') && word.size() > 1 && !u"aeiou"_s.contains(word[word.size() - 2]))
        return word.chopped(1) + u"ies";
    if (word.endsWith(u's') || word.endsWith(u'x') || word.endsWith(u'z') || word.endsWith(u"ch")
        || word.endsWith(u"sh"))
        return word + u"es";
    return word + u's';
}

const PhpEnum *findEnum(const ProjectContext &project, const QString &name)
{
    for (const PhpEnum &e : project.enums)
        if (e.name == name)
            return &e;
    return nullptr;
}

} // namespace

QString defaultTableName(QStringView className)
{
    QString snake;
    for (qsizetype i = 0; i < className.size(); ++i) {
        const QChar c = className[i];
        if (c.isUpper() && i > 0)
            snake += u'_';
        snake += c.toLower();
    }
    // Eloquent pluralises only the last word: order_item -> order_items.
    const qsizetype cut = snake.lastIndexOf(u'_') + 1;
    return snake.left(cut) + pluralize(snake.mid(cut));
}

std::optional<LaravelModel> parseLaravelModel(QStringView php)
{
    const QString code = stripComments(php);
    static const QRegularExpression kClass(uR"(\bclass\s+(\w+))"_s);
    const auto cls = kClass.match(code);
    if (!cls.hasMatch())
        return std::nullopt;

    LaravelModel model;
    model.className = cls.captured(1);

    static const QRegularExpression kTable(uR"(\$table\s*=\s*['"]([^'"]+)['"])"_s);
    const auto table = kTable.match(code);
    if (table.hasMatch()) {
        model.table = table.captured(1);
    } else {
        model.table = defaultTableName(model.className);
        model.tableInferred = true;
    }

    static const QRegularExpression kCast(
        uR"(['"](\w+)['"]\s*=>\s*(?:[\w\\]+::of\(\s*)?\\?([\w\\]+)::class)"_s);
    auto it = kCast.globalMatch(code);
    while (it.hasNext()) {
        const auto m = it.next();
        model.enumCasts.push_back({ m.captured(1), lastSegment(m.captured(2)) });
    }
    return model;
}

std::optional<PhpEnum> parsePhpEnum(QStringView php)
{
    const QString code = stripComments(php);
    static const QRegularExpression kEnum(uR"(\benum\s+(\w+)\s*(?::\s*(string|int))?)"_s);
    const auto head = kEnum.match(code);
    if (!head.hasMatch())
        return std::nullopt;

    PhpEnum out;
    out.name = head.captured(1);
    out.backingType = head.captured(2);
    static const QRegularExpression kCase(uR"(\bcase\s+(\w+)\s*(?:=\s*([^;]+?))?\s*;)"_s);
    auto it = kCase.globalMatch(code, head.capturedEnd());
    while (it.hasNext() && static_cast<qsizetype>(out.cases.size()) < kMaxCases) {
        const auto m = it.next();
        out.cases.push_back({ m.captured(1), unquote(m.captured(2)) });
    }
    return out;
}

QString renderProjectContext(const ProjectContext &project, const QStringList &tables,
                             bool includeEnumCases, qsizetype maxChars)
{
    QSet<QString> wanted;
    for (const QString &t : tables)
        wanted.insert(t.mid(t.lastIndexOf(u'.') + 1).toLower());

    QString out;
    QStringList enumOrder;
    for (const LaravelModel &m : project.models) {
        if (!wanted.contains(m.table.toLower()))
            continue;
        QString line = u"table " + clean(m.table) + u": model " + clean(m.className);
        QStringList casts;
        for (const EnumCast &c : m.enumCasts) {
            const PhpEnum *e = findEnum(project, c.enumName);
            if (e == nullptr)
                continue;
            casts.append(clean(c.column) + u" -> enum " + clean(e->name));
            if (!enumOrder.contains(e->name))
                enumOrder.append(e->name);
        }
        if (!casts.isEmpty())
            line += u"; " + casts.join(u", ");
        out += line + u'\n';
    }
    if (includeEnumCases) {
        for (const QString &name : std::as_const(enumOrder)) {
            const PhpEnum *e = findEnum(project, name);
            QStringList cases;
            for (const PhpEnumCase &c : e->cases) {
                cases.append(c.value.isEmpty() ? clean(c.name)
                                               : clean(c.name) + u" = " + clean(c.value));
            }
            const QString type
                = e->backingType.isEmpty() ? QString() : u" (" + e->backingType + u')';
            out += u"enum " + clean(e->name) + type + u": " + cases.join(u", ") + u'\n';
        }
    }
    if (out.size() > maxChars) {
        out.truncate(maxChars);
        out.truncate(out.lastIndexOf(u'\n') + 1);
    }
    return out;
}

} // namespace quarry::ai
