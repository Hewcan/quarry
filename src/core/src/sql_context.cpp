#include "quarry/sql_context.h"

#include <algorithm>

namespace quarry {
namespace {

using namespace Qt::StringLiterals;

enum class TokKind : uint8_t { Word, Quoted, Dot, Comma, Open, Close, Other };

struct Tok
{
    TokKind kind = TokKind::Other;
    qsizetype begin = 0;
    qsizetype end = 0;
    bool closed = true; // Quoted: the closing quote exists
};

bool isWordStart(QChar c)
{
    return c.isLetter() || c == u'_' || c.isSurrogate();
}

bool isWordPart(QChar c)
{
    return c.isLetterOrNumber() || c == u'_' || c == u'$' || c.isSurrogate();
}

// i is at the opening quote. A doubled closing char is an escape; with backslash, `\x` is too.
qsizetype skipQuoted(QStringView s, qsizetype i, QChar close, bool backslash, bool &closed)
{
    const qsizetype n = s.size();
    ++i;
    while (i < n) {
        const QChar c = s[i];
        if (backslash && c == u'\\') {
            i += 2;
        } else if (c == close) {
            if (i + 1 < n && s[i + 1] == close) {
                i += 2;
            } else {
                closed = true;
                return i + 1;
            }
        } else {
            ++i;
        }
    }
    closed = false;
    return n;
}

// Nested block comment, as in PostgreSQL. i is at the "/*".
qsizetype skipBlockComment(QStringView s, qsizetype i, bool &closed)
{
    const qsizetype n = s.size();
    int depth = 0;
    while (i < n) {
        if (s[i] == u'/' && i + 1 < n && s[i + 1] == u'*') {
            ++depth;
            i += 2;
        } else if (s[i] == u'*' && i + 1 < n && s[i + 1] == u'/') {
            --depth;
            i += 2;
            if (depth == 0) {
                closed = true;
                return i;
            }
        } else {
            ++i;
        }
    }
    closed = false;
    return n;
}

QChar closingQuote(QChar open)
{
    return open == u'[' ? QChar(u']') : open;
}

// A '[' glued to a name or a closing bracket is a subscript (`a[1]`), not a quoted identifier.
bool isSubscript(QStringView s, qsizetype i)
{
    if (i == 0 || s[i - 1].isSpace()) {
        return false;
    }
    const QChar p = s[i - 1];
    return isWordPart(p) || p == u')' || p == u']' || p == u'"';
}

// Tokens of the statement. Comments are skipped. Sets inLiteral when the cursor is strictly inside
// a string, comment, number or parameter (or at the end of an unfinished one).
std::vector<Tok> scan(QStringView s, qsizetype cursor, bool &inLiteral)
{
    std::vector<Tok> toks;
    toks.reserve(static_cast<size_t>(s.size() / 4 + 4));
    const qsizetype n = s.size();
    qsizetype i = 0;
    auto mark = [&](qsizetype b, qsizetype e, bool closed) {
        if (b < cursor && (closed ? cursor < e : cursor <= e)) {
            inLiteral = true;
        }
    };
    auto push = [&](TokKind kind, qsizetype b, qsizetype e, bool closed = true) {
        toks.push_back({ kind, b, e, closed });
    };

    while (i < n) {
        const QChar c = s[i];
        const QChar c1 = i + 1 < n ? s[i + 1] : QChar();
        bool closed = true;
        if (c.isSpace()) {
            ++i;
        } else if (c == u'-' && c1 == u'-') {
            qsizetype e = i;
            while (e < n && s[e] != u'\n' && s[e] != u'\r') {
                ++e;
            }
            mark(i, e, false);
            i = e;
        } else if (c == u'/' && c1 == u'*') {
            const qsizetype e = skipBlockComment(s, i, closed);
            mark(i, e, closed);
            i = e;
        } else if (c == u'\'') {
            // PostgreSQL E'...': the prefix is a lone E glued to the quote.
            const bool backslash = i > 0 && (s[i - 1] == u'e' || s[i - 1] == u'E')
                && (i < 2 || !isWordPart(s[i - 2]));
            const qsizetype e = skipQuoted(s, i, u'\'', backslash, closed);
            mark(i, e, closed);
            push(TokKind::Other, i, e);
            i = e;
        } else if (c == u'"' || c == u'`' || (c == u'[' && !isSubscript(s, i))) {
            qsizetype e = skipQuoted(s, i, closingQuote(c), false, closed);
            if (!closed && cursor > i) {
                e = cursor; // unfinished quote: it ends at the cursor, the rest is scanned normally
            }
            push(TokKind::Quoted, i, e, closed);
            i = e;
        } else if (c == u'$') {
            qsizetype j = i + 1;
            if (j < n && isWordStart(s[j])) {
                while (j < n && s[j] != u'$' && isWordPart(s[j])) {
                    ++j;
                }
            }
            if (j < n && s[j] == u'$') { // $tag$ ... $tag$
                const QStringView tag = s.mid(i, j - i + 1);
                const qsizetype end = s.indexOf(tag, j + 1);
                closed = end >= 0;
                const qsizetype e = closed ? end + tag.size() : n;
                mark(i, e, closed);
                push(TokKind::Other, i, e);
                i = e;
            } else { // parameter $1 (or a lone $)
                j = i + 1;
                while (j < n && s[j].isDigit()) {
                    ++j;
                }
                mark(i, j, false);
                push(TokKind::Other, i, j);
                i = j;
            }
        } else if (c.isDigit()
                   || (c == u'.' && c1.isDigit()
                       && (toks.empty()
                           || (toks.back().kind != TokKind::Word
                               && toks.back().kind != TokKind::Quoted
                               && toks.back().kind != TokKind::Close)))) {
            qsizetype e = i + 1;
            while (e < n
                   && (s[e].isLetterOrNumber() || s[e] == u'_'
                       || (s[e] == u'.' && e + 1 < n && s[e + 1].isDigit()))) {
                ++e;
            }
            mark(i, e, false);
            push(TokKind::Other, i, e);
            i = e;
        } else if (isWordStart(c)) {
            qsizetype e = i + 1;
            while (e < n && isWordPart(s[e])) {
                ++e;
            }
            push(TokKind::Word, i, e);
            i = e;
        } else {
            TokKind kind = TokKind::Other;
            if (c == u'.') {
                kind = TokKind::Dot;
            } else if (c == u',') {
                kind = TokKind::Comma;
            } else if (c == u'(') {
                kind = TokKind::Open;
            } else if (c == u')') {
                kind = TokKind::Close;
            }
            push(kind, i, i + 1);
            ++i;
        }
    }
    return toks;
}

QString unquote(QStringView inner, QChar close)
{
    QString out = inner.toString();
    out.replace(QString(2, close), QString(1, close));
    return out;
}

QStringView wordText(QStringView s, const Tok &t)
{
    return s.mid(t.begin, t.end - t.begin);
}

QString identText(QStringView s, const Tok &t)
{
    if (t.kind == TokKind::Word) {
        return wordText(s, t).toString();
    }
    const qsizetype innerEnd = t.closed ? t.end - 1 : t.end;
    return unquote(s.mid(t.begin + 1, innerEnd - t.begin - 1), closingQuote(s[t.begin]));
}

bool isIdent(const Tok &t)
{
    return t.kind == TokKind::Word || t.kind == TokKind::Quoted;
}

enum class Kw : uint8_t {
    None,
    TableStart, // FROM JOIN UPDATE INTO TABLE: a table name follows
    ColumnStart, // SELECT WHERE ON ...: expressions follow
    Reset, // UNION ...: a new select starts
    Modifier, // ONLY IF EXISTS ...: skipped before a table name
    Stop, // never an alias
};

Kw classify(QStringView w)
{
    struct Entry
    {
        QLatin1StringView word;
        Kw kw;
    };
    static constexpr Entry kEntries[] = {
        { "from"_L1, Kw::TableStart },    { "join"_L1, Kw::TableStart },
        { "update"_L1, Kw::TableStart },  { "into"_L1, Kw::TableStart },
        { "table"_L1, Kw::TableStart },   { "select"_L1, Kw::ColumnStart },
        { "where"_L1, Kw::ColumnStart },  { "on"_L1, Kw::ColumnStart },
        { "and"_L1, Kw::ColumnStart },    { "or"_L1, Kw::ColumnStart },
        { "by"_L1, Kw::ColumnStart },     { "set"_L1, Kw::ColumnStart },
        { "having"_L1, Kw::ColumnStart }, { "returning"_L1, Kw::ColumnStart },
        { "values"_L1, Kw::ColumnStart }, { "when"_L1, Kw::ColumnStart },
        { "then"_L1, Kw::ColumnStart },   { "else"_L1, Kw::ColumnStart },
        { "union"_L1, Kw::Reset },        { "intersect"_L1, Kw::Reset },
        { "except"_L1, Kw::Reset },       { "only"_L1, Kw::Modifier },
        { "if"_L1, Kw::Modifier },        { "exists"_L1, Kw::Modifier },
        { "lateral"_L1, Kw::Modifier },   { "of"_L1, Kw::Modifier },
        { "group"_L1, Kw::Stop },         { "order"_L1, Kw::Stop },
        { "limit"_L1, Kw::Stop },         { "offset"_L1, Kw::Stop },
        { "using"_L1, Kw::Stop },         { "inner"_L1, Kw::Stop },
        { "left"_L1, Kw::Stop },          { "right"_L1, Kw::Stop },
        { "full"_L1, Kw::Stop },          { "cross"_L1, Kw::Stop },
        { "natural"_L1, Kw::Stop },       { "outer"_L1, Kw::Stop },
        { "window"_L1, Kw::Stop },        { "fetch"_L1, Kw::Stop },
        { "for"_L1, Kw::Stop },           { "default"_L1, Kw::Stop },
        { "with"_L1, Kw::Stop },          { "case"_L1, Kw::Stop },
        { "end"_L1, Kw::Stop },           { "not"_L1, Kw::Stop },
    };
    if (w.size() > 9) { // the longest entry
        return Kw::None;
    }
    for (const Entry &e : kEntries) {
        if (w.compare(e.word, Qt::CaseInsensitive) == 0) {
            return e.kw;
        }
    }
    return Kw::None;
}

enum class Clause : uint8_t { Start, Column, Table };

struct Frame
{
    Clause clause = Clause::Start;
    bool expectTable = false; // the next name is a table reference
    bool record = false; // ... and it goes to SqlContext::tables (TABLE does not)
};

CompletionScope scopeOf(const Frame &f)
{
    switch (f.clause) {
    case Clause::Column:
        return CompletionScope::Column;
    case Clause::Table:
        return f.expectTable ? CompletionScope::Table : CompletionScope::Keyword;
    case Clause::Start:
        break;
    }
    return CompletionScope::Keyword;
}

} // namespace

SqlContext analyzeSqlContext(QStringView s, qsizetype cursor)
{
    cursor = std::clamp<qsizetype>(cursor, 0, s.size());
    SqlContext ctx;
    ctx.replaceStart = cursor;
    bool inLiteral = false;
    const std::vector<Tok> toks = scan(s, cursor, inLiteral);
    if (inLiteral) {
        return ctx; // scope None
    }

    // snap: the token being completed, or else the first one after the cursor.
    size_t snap = toks.size();
    bool inWord = false;
    for (size_t k = 0; k < toks.size(); ++k) {
        const Tok &t = toks[k];
        if (t.begin >= cursor) {
            snap = k;
            break;
        }
        if ((t.kind == TokKind::Word && cursor <= t.end)
            || (t.kind == TokKind::Quoted && (cursor < t.end || !t.closed))) {
            snap = k;
            inWord = true;
            break;
        }
    }
    if (inWord) {
        const Tok &t = toks[snap];
        ctx.replaceStart = t.begin;
        ctx.prefix = t.kind == TokKind::Word
            ? s.mid(t.begin, cursor - t.begin).toString()
            : unquote(s.mid(t.begin + 1, cursor - t.begin - 1), closingQuote(s[t.begin]));
    }

    bool member = false;
    if (snap > 0 && toks[snap - 1].kind == TokKind::Dot) {
        // Walk back over `a.b.` collecting identifiers.
        QStringList parts;
        size_t j = snap - 1;
        while (j >= 1 && isIdent(toks[j - 1])) {
            parts.prepend(identText(s, toks[j - 1]));
            --j;
            if (j >= 1 && toks[j - 1].kind == TokKind::Dot) {
                --j;
            } else {
                break;
            }
        }
        if (parts.isEmpty()) {
            return ctx; // a dot after something that is not a name: no suggestions
        }
        member = true;
        ctx.scope = CompletionScope::Member;
        ctx.qualifier = parts;
    }

    std::vector<Frame> frames(1);
    bool captured = false;
    auto capture = [&] {
        if (!captured && !member) {
            ctx.scope = scopeOf(frames.back());
        }
        captured = true;
    };

    // Reads `name(.name)* [[AS] alias]` starting at toks[start]; returns the last token used. The
    // typed word (snap) is never taken as part of the reference unless it starts it.
    auto parseRef = [&](size_t start, bool record) {
        const size_t limit = start < snap ? snap : toks.size();
        QStringList parts { identText(s, toks[start]) };
        size_t j = start + 1;
        while (j + 1 < limit && toks[j].kind == TokKind::Dot && isIdent(toks[j + 1])) {
            parts.push_back(identText(s, toks[j + 1]));
            j += 2;
        }
        frames.back().expectTable = false;
        if (j < limit && toks[j].kind == TokKind::Dot) { // `a.` still being typed
            return j;
        }
        QString alias;
        if (j < limit && toks[j].kind == TokKind::Word
            && wordText(s, toks[j]).compare(u"as", Qt::CaseInsensitive) == 0) {
            if (j + 1 < limit && isIdent(toks[j + 1])) {
                alias = identText(s, toks[j + 1]);
                j += 2;
            } else {
                ++j;
            }
        } else if (j < limit
                   && (toks[j].kind == TokKind::Quoted
                       || (toks[j].kind == TokKind::Word
                           && classify(wordText(s, toks[j])) == Kw::None))) {
            alias = identText(s, toks[j]);
            ++j;
        }
        if (record && !(start == snap && inWord)) { // the word being typed is not a reference yet
            ctx.tables.push_back({ std::move(parts), std::move(alias) });
        }
        return j - 1;
    };

    for (size_t i = 0; i < toks.size(); ++i) {
        if (i == snap) {
            capture();
        }
        const Tok &t = toks[i];
        switch (t.kind) {
        case TokKind::Word:
        case TokKind::Quoted: {
            const QStringView word = wordText(s, t);
            const Kw kw = t.kind == TokKind::Word ? classify(word) : Kw::None;
            Frame &f = frames.back();
            if (f.expectTable) {
                if (kw == Kw::Modifier) {
                    break;
                }
                if (kw == Kw::None) {
                    i = parseRef(i, f.record);
                    break;
                }
                f.expectTable = false;
            }
            if (kw == Kw::TableStart) {
                f.clause = Clause::Table;
                f.expectTable = true;
                f.record = word.compare(u"table", Qt::CaseInsensitive) != 0;
            } else if (kw == Kw::ColumnStart) {
                f.clause = Clause::Column;
            } else if (kw == Kw::Reset) {
                f.clause = Clause::Start;
            }
            break;
        }
        case TokKind::Comma:
            if (frames.back().clause == Clause::Table) {
                frames.back().expectTable = true;
            }
            break;
        case TokKind::Open: {
            Frame &f = frames.back();
            Frame inner;
            if (f.clause == Clause::Column || (f.clause == Clause::Table && !f.expectTable)) {
                inner.clause = Clause::Column; // call, IN list, column list, VALUES
            }
            f.expectTable = false;
            frames.push_back(inner);
            break;
        }
        case TokKind::Close:
            if (frames.size() > 1) {
                frames.pop_back();
            }
            break;
        case TokKind::Dot:
            break;
        case TokKind::Other:
            frames.back().expectTable = false;
            break;
        }
    }
    capture();
    return ctx;
}

} // namespace quarry
