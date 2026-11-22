#include <quarry/ai/ai_sanitizer.h>

#include <QRegularExpression>

namespace quarry::ai {

using namespace Qt::StringLiterals;

QString sanitizeErrorMessage(QStringView message)
{
    QString out = message.toString();
    // PostgreSQL: Key (email)=(ana@x.com) / Failing row contains (1, ana, ...)
    static const QRegularExpression kKeyValue(uR"(\)=\((?:[^()]|\([^()]*\))*\))"_s);
    out.replace(kKeyValue, u")=(?)"_s);
    static const QRegularExpression kRowValues(
        uR"(\b(row contains|value is|values?)\s*\((?:[^()]|\([^()]*\))*\))"_s,
        QRegularExpression::CaseInsensitiveOption);
    out.replace(kRowValues, u"\\1 (?)"_s);
    // MySQL "Duplicate entry 'x' for key", SQLite and others: any single-quoted text.
    static const QRegularExpression kQuoted(uR"('(?:[^']|'')*')"_s);
    out.replace(kQuoted, u"'?'"_s);
    // Numbers not glued to an identifier (values, ids, amounts).
    static const QRegularExpression kNumber(uR"((?<![\w.])[-+]?\d+(?:\.\d+)?(?![\w.]))"_s);
    out.replace(kNumber, u"?"_s);
    return out;
}

namespace {

bool isIdentChar(QChar c)
{
    return c.isLetterOrNumber() || c == u'_' || c == u'$' || c == u'@' || c == u'#';
}

// End (exclusive) of a quoted run starting at `from` (the opening quote); doubled quote escapes.
qsizetype quotedEnd(QStringView sql, qsizetype from, QChar close)
{
    qsizetype i = from + 1;
    while (i < sql.size()) {
        if (sql[i] == close) {
            if (i + 1 < sql.size() && sql[i + 1] == close) {
                i += 2;
                continue;
            }
            return i + 1;
        }
        if (close == u'\'' && sql[i] == u'\\' && i + 1 < sql.size()) {
            i += 2; // MySQL/E'' backslash escape: never ends the literal early
            continue;
        }
        ++i;
    }
    return sql.size();
}

} // namespace

MaskedScript maskLiterals(QStringView sql, bool maskNumbers)
{
    MaskedScript out;
    out.text.reserve(sql.size());
    auto placeholder = [&](QStringView literal) {
        out.values.append(literal.toString());
        out.text += u"__V" + QString::number(out.values.size()) + u"__";
    };
    qsizetype i = 0;
    const qsizetype n = sql.size();
    while (i < n) {
        const QChar c = sql[i];
        const QChar next = i + 1 < n ? sql[i + 1] : QChar();
        if (c == u'-' && next == u'-') {
            const qsizetype end = sql.indexOf(u'\n', i);
            const qsizetype stop = end < 0 ? n : end;
            out.text += sql.mid(i, stop - i);
            i = stop;
        } else if (c == u'/' && next == u'*') {
            const qsizetype end = sql.indexOf(u"*/", i + 2);
            const qsizetype stop = end < 0 ? n : end + 2;
            out.text += sql.mid(i, stop - i);
            i = stop;
        } else if (c == u'"' || c == u'`' || c == u'[') {
            const qsizetype stop = quotedEnd(sql, i, c == u'[' ? QChar(u']') : c);
            out.text += sql.mid(i, stop - i);
            i = stop;
        } else if (c == u'\'') {
            // A prefix such as N'..', E'..', X'..' belongs to the literal.
            qsizetype start = i;
            if (!out.text.isEmpty() && out.text.back().isLetter()
                && (out.text.size() == 1 || !isIdentChar(out.text[out.text.size() - 2]))) {
                start = i - 1;
                out.text.chop(1);
            }
            const qsizetype stop = quotedEnd(sql, i, u'\'');
            placeholder(sql.mid(start, stop - start));
            i = stop;
        } else if (c == u'$' && (i == 0 || !isIdentChar(sql[i - 1]))) {
            // Dollar quoting: $$...$$ or $tag$...$tag$ (a lone $1 parameter is kept).
            qsizetype j = i + 1;
            while (j < n && (sql[j].isLetterOrNumber() || sql[j] == u'_') && !sql[j].isDigit())
                ++j;
            if (j < n && sql[j] == u'$') {
                const QStringView tag = sql.mid(i, j - i + 1);
                const qsizetype end = sql.indexOf(tag, j + 1);
                const qsizetype stop = end < 0 ? n : end + tag.size();
                placeholder(sql.mid(i, stop - i));
                i = stop;
            } else {
                out.text += c;
                ++i;
            }
        } else if (c.isDigit() && (i == 0 || !isIdentChar(sql[i - 1])) && maskNumbers) {
            qsizetype j = i;
            while (j < n && (sql[j].isDigit() || sql[j] == u'.'))
                ++j;
            if (j < n && isIdentChar(sql[j])) { // 1abc, 2e5...: leave as is
                out.text += sql.mid(i, j - i);
            } else {
                placeholder(sql.mid(i, j - i));
            }
            i = j;
        } else if (isIdentChar(c)) {
            qsizetype j = i;
            while (j < n && isIdentChar(sql[j]))
                ++j;
            out.text += sql.mid(i, j - i);
            i = j;
        } else {
            out.text += c;
            ++i;
        }
    }
    return out;
}

QString unmaskLiterals(QStringView text, const QStringList &values)
{
    static const QRegularExpression kPlaceholder(uR"(__V(\d+)__)"_s);
    QString out;
    qsizetype last = 0;
    auto it = kPlaceholder.globalMatchView(text);
    while (it.hasNext()) {
        const auto match = it.next();
        const qsizetype index = match.capturedView(1).toLongLong() - 1;
        out += text.mid(last, match.capturedStart() - last);
        out += index >= 0 && index < values.size() ? values[index] : match.captured();
        last = match.capturedEnd();
    }
    out += text.mid(last);
    return out;
}

} // namespace quarry::ai
