#include "export_text.h"

#include <quarry/value_type.h>

#include <charconv>
#include <cmath>

namespace quarry::exporttext {

namespace {

constexpr int kMaxJsonDepth = 256;

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

bool isHexDigit(char c)
{
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Returns the index after the number that starts at `pos`, or -1.
qsizetype scanNumber(QByteArrayView in, qsizetype pos)
{
    const qsizetype size = in.size();
    if (pos < size && in[pos] == '-')
        ++pos;
    if (pos >= size || !isDigit(in[pos]))
        return -1;
    if (in[pos] == '0') {
        ++pos;
    } else {
        while (pos < size && isDigit(in[pos]))
            ++pos;
    }
    if (pos < size && in[pos] == '.') {
        ++pos;
        if (pos >= size || !isDigit(in[pos]))
            return -1;
        while (pos < size && isDigit(in[pos]))
            ++pos;
    }
    if (pos < size && (in[pos] == 'e' || in[pos] == 'E')) {
        ++pos;
        if (pos < size && (in[pos] == '+' || in[pos] == '-'))
            ++pos;
        if (pos >= size || !isDigit(in[pos]))
            return -1;
        while (pos < size && isDigit(in[pos]))
            ++pos;
    }
    return pos;
}

class JsonCompactor
{
public:
    JsonCompactor(QByteArrayView in, QByteArray &out)
        : m_in(in)
        , m_out(out)
    { }

    bool run()
    {
        m_out.clear();
        m_out.reserve(m_in.size());
        skipSpace();
        if (!value(0))
            return false;
        skipSpace();
        return m_pos == m_in.size();
    }

private:
    bool atEnd() const { return m_pos >= m_in.size(); }
    void skipSpace()
    {
        while (!atEnd()
               && (m_in[m_pos] == ' ' || m_in[m_pos] == '\t' || m_in[m_pos] == '\n'
                   || m_in[m_pos] == '\r'))
            ++m_pos;
    }

    bool literal(QByteArrayView word)
    {
        if (m_in.sliced(m_pos).startsWith(word)) {
            m_out.append(word);
            m_pos += word.size();
            return true;
        }
        return false;
    }

    bool string()
    {
        const qsizetype begin = m_pos;
        ++m_pos; // opening quote
        while (!atEnd()) {
            const char c = m_in[m_pos];
            if (c == '"') {
                ++m_pos;
                m_out.append(m_in.sliced(begin, m_pos - begin));
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20)
                return false;
            if (c == '\\') {
                ++m_pos;
                if (atEnd())
                    return false;
                const char e = m_in[m_pos];
                if (e == 'u') {
                    for (int i = 1; i <= 4; ++i) {
                        if (m_pos + i >= m_in.size() || !isHexDigit(m_in[m_pos + i]))
                            return false;
                    }
                    m_pos += 4;
                } else if (e != '"' && e != '\\' && e != '/' && e != 'b' && e != 'f' && e != 'n'
                           && e != 'r' && e != 't') {
                    return false;
                }
            }
            ++m_pos;
        }
        return false;
    }

    bool value(int depth)
    {
        if (depth > kMaxJsonDepth || atEnd())
            return false;
        const char c = m_in[m_pos];
        if (c == '{')
            return object(depth);
        if (c == '[')
            return array(depth);
        if (c == '"')
            return string();
        if (c == 't')
            return literal("true");
        if (c == 'f')
            return literal("false");
        if (c == 'n')
            return literal("null");
        const qsizetype end = scanNumber(m_in, m_pos);
        if (end < 0)
            return false;
        m_out.append(m_in.sliced(m_pos, end - m_pos));
        m_pos = end;
        return true;
    }

    bool array(int depth)
    {
        ++m_pos;
        m_out.append('[');
        skipSpace();
        if (!atEnd() && m_in[m_pos] == ']') {
            ++m_pos;
            m_out.append(']');
            return true;
        }
        for (;;) {
            skipSpace();
            if (!value(depth + 1))
                return false;
            skipSpace();
            if (atEnd())
                return false;
            const char c = m_in[m_pos++];
            if (c == ']') {
                m_out.append(']');
                return true;
            }
            if (c != ',')
                return false;
            m_out.append(',');
        }
    }

    bool object(int depth)
    {
        ++m_pos;
        m_out.append('{');
        skipSpace();
        if (!atEnd() && m_in[m_pos] == '}') {
            ++m_pos;
            m_out.append('}');
            return true;
        }
        for (;;) {
            skipSpace();
            if (atEnd() || m_in[m_pos] != '"' || !string())
                return false;
            skipSpace();
            if (atEnd() || m_in[m_pos++] != ':')
                return false;
            m_out.append(':');
            skipSpace();
            if (!value(depth + 1))
                return false;
            skipSpace();
            if (atEnd())
                return false;
            const char c = m_in[m_pos++];
            if (c == '}') {
                m_out.append('}');
                return true;
            }
            if (c != ',')
                return false;
            m_out.append(',');
        }
    }

    QByteArrayView m_in;
    QByteArray &m_out;
    qsizetype m_pos = 0;
};

} // namespace

QByteArrayView formatInt64(char (&buffer)[kNumberBufferSize], int64_t value)
{
    const auto result = std::to_chars(buffer, buffer + kNumberBufferSize, value);
    return QByteArrayView(buffer, result.ptr - buffer);
}

QByteArrayView formatDouble(char (&buffer)[kNumberBufferSize], double value)
{
    const auto result = std::to_chars(buffer, buffer + kNumberBufferSize, value);
    return QByteArrayView(buffer, result.ptr - buffer);
}

bool isJsonNumber(QByteArrayView text)
{
    return !text.isEmpty() && scanNumber(text, 0) == text.size();
}

bool compactJson(QByteArrayView text, QByteArray &out)
{
    return JsonCompactor(text, out).run();
}

void appendJsonString(ByteSink &out, QByteArrayView text)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    out.append('"');
    qsizetype runStart = 0;
    const qsizetype size = text.size();
    for (qsizetype i = 0; i < size; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c >= 0x20 && c != '"' && c != '\\')
            continue;
        if (i > runStart)
            out.append(text.sliced(runStart, i - runStart));
        runStart = i + 1;
        switch (c) {
        case '"':
            out.append(QByteArrayView("\\\""));
            break;
        case '\\':
            out.append(QByteArrayView("\\\\"));
            break;
        case '\n':
            out.append(QByteArrayView("\\n"));
            break;
        case '\r':
            out.append(QByteArrayView("\\r"));
            break;
        case '\t':
            out.append(QByteArrayView("\\t"));
            break;
        case '\b':
            out.append(QByteArrayView("\\b"));
            break;
        case '\f':
            out.append(QByteArrayView("\\f"));
            break;
        default: {
            const char escaped[6]
                = { '\\', 'u', '0', '0', kHexDigits[c >> 4], kHexDigits[c & 0xF] };
            out.append(QByteArrayView(escaped, 6));
        }
        }
    }
    if (size > runStart)
        out.append(text.sliced(runStart, size - runStart));
    out.append('"');
}

void appendHex(ByteSink &out, QByteArrayView bytes)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    char chunk[256];
    qsizetype used = 0;
    for (const char b : bytes) {
        const auto c = static_cast<unsigned char>(b);
        chunk[used++] = kHexDigits[c >> 4];
        chunk[used++] = kHexDigits[c & 0xF];
        if (used == sizeof(chunk)) {
            out.append(QByteArrayView(chunk, used));
            used = 0;
        }
    }
    if (used > 0)
        out.append(QByteArrayView(chunk, used));
}

QByteArrayView CellText::text(const ColumnBuffer &column, qsizetype row)
{
    switch (column.typeAt(row)) {
    case ValueType::Null:
        return {};
    case ValueType::Bool:
        return column.int64At(row) != 0 ? QByteArrayView("true") : QByteArrayView("false");
    case ValueType::Int64:
        return formatInt64(m_number, column.int64At(row));
    case ValueType::Double: {
        const double v = column.doubleAt(row);
        if (std::isnan(v))
            return QByteArrayView("NaN");
        if (std::isinf(v))
            return v > 0 ? QByteArrayView("Infinity") : QByteArrayView("-Infinity");
        return formatDouble(m_number, v);
    }
    case ValueType::Bytes:
        m_scratch = column.bytesAt(row).toByteArray().toBase64();
        return m_scratch;
    default:
        return column.bytesAt(row);
    }
}

} // namespace quarry::exporttext
