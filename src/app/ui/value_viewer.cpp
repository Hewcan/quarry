#include "value_viewer.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>

namespace quarry::ui {

namespace {

constexpr int kIndent = 2;

QStringView trimmedStart(QStringView text)
{
    qsizetype i = 0;
    while (i < text.size() && text.at(i).isSpace())
        ++i;
    return text.mid(i);
}

bool validJson(QStringView text, QString *error = nullptr)
{
    QJsonParseError parse;
    QJsonDocument::fromJson(text.toUtf8(), &parse);
    if (error != nullptr)
        *error = parse.errorString();
    return parse.error == QJsonParseError::NoError;
}

bool wellFormedXml(QStringView text)
{
    QXmlStreamReader reader(text);
    bool element = false;
    while (!reader.atEnd()) {
        if (reader.readNext() == QXmlStreamReader::StartElement)
            element = true;
    }
    return element && !reader.hasError();
}

void newline(QString &out, int depth)
{
    out += QLatin1Char('\n');
    out += QString(static_cast<qsizetype>(depth) * kIndent, QLatin1Char(' '));
}

// Re-indents text that is valid JSON without building a tree, so the keys keep their order and the
// numbers their spelling (QJsonObject would sort the keys).
QString indentJson(QStringView text)
{
    QString out;
    out.reserve(text.size() + text.size() / 4);
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (inString) {
            out += c;
            if (escaped)
                escaped = false;
            else if (c == QLatin1Char('\\'))
                escaped = true;
            else if (c == QLatin1Char('"'))
                inString = false;
            continue;
        }
        if (c.isSpace())
            continue;
        switch (c.unicode()) {
        case '"':
            inString = true;
            out += c;
            break;
        case '{':
        case '[': {
            const QChar close = c == QLatin1Char('{') ? QLatin1Char('}') : QLatin1Char(']');
            qsizetype j = i + 1;
            while (j < text.size() && text.at(j).isSpace())
                ++j;
            if (j < text.size() && text.at(j) == close) { // empty: stays on one line
                out += c;
                out += close;
                i = j;
            } else {
                out += c;
                newline(out, ++depth);
            }
            break;
        }
        case '}':
        case ']':
            newline(out, --depth);
            out += c;
            break;
        case ',':
            out += c;
            newline(out, depth);
            break;
        case ':':
            out += QLatin1String(": ");
            break;
        default:
            out += c;
        }
    }
    return out;
}

} // namespace

QString ValueViewer::detect(ValueType type, QStringView nativeType, QStringView text)
{
    if (type == ValueType::Bytes)
        return QStringLiteral("blob");
    if (type == ValueType::Json || nativeType.contains(u"json", Qt::CaseInsensitive))
        return QStringLiteral("json");
    if (nativeType.compare(u"xml", Qt::CaseInsensitive) == 0
        || nativeType.compare(u"sqlxml", Qt::CaseInsensitive) == 0)
        return QStringLiteral("xml");
    const bool textual = type == ValueType::Text || type == ValueType::Other;
    if (!textual)
        return {};
    const QStringView head = trimmedStart(text);
    if (text.size() <= kMaxChars && !head.isEmpty()) {
        if ((head.front() == QLatin1Char('{') || head.front() == QLatin1Char('['))
            && validJson(text))
            return QStringLiteral("json");
        if (head.front() == QLatin1Char('<') && wellFormedXml(text))
            return QStringLiteral("xml");
    }
    if (text.size() >= kLongTextChars || text.contains(QLatin1Char('\n')))
        return QStringLiteral("text");
    return {};
}

ValueViewer::Formatted ValueViewer::formatText(QStringView text)
{
    Formatted out;
    if (text.size() > kMaxChars) {
        out.text = text.left(kMaxChars).toString();
        out.notice = tr("%1 more characters not shown. Copy takes the whole value.")
                         .arg(text.size() - kMaxChars);
    } else {
        out.text = text.toString();
    }
    return out;
}

ValueViewer::Formatted ValueViewer::formatJson(QStringView text)
{
    if (text.size() > kMaxChars) {
        Formatted out = formatText(text);
        out.ok = false;
        out.notice = tr("Too large to format. Showing the first %1 characters as they are.")
                         .arg(kMaxChars);
        return out;
    }
    QString error;
    if (!validJson(text, &error)) {
        Formatted out;
        out.text = text.toString();
        out.ok = false;
        out.notice = tr("This is not valid JSON (%1). Showing the text as it is.").arg(error);
        return out;
    }
    Formatted out;
    out.text = indentJson(text);
    return out;
}

ValueViewer::Formatted ValueViewer::formatXml(QStringView text)
{
    if (text.size() > kMaxChars) {
        Formatted out = formatText(text);
        out.ok = false;
        out.notice = tr("Too large to format. Showing the first %1 characters as they are.")
                         .arg(kMaxChars);
        return out;
    }
    QXmlStreamReader reader(text);
    QString formatted;
    QXmlStreamWriter writer(&formatted);
    writer.setAutoFormatting(true);
    writer.setAutoFormattingIndent(kIndent);
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::StartDocument || token == QXmlStreamReader::EndDocument
            || token == QXmlStreamReader::Invalid)
            continue;
        // The indentation is the writer's: the blanks between tags are dropped.
        if (token == QXmlStreamReader::Characters && reader.isWhitespace())
            continue;
        writer.writeCurrentToken(reader);
    }
    if (reader.hasError()) {
        Formatted out;
        out.text = text.toString();
        out.ok = false;
        out.notice = tr("This is not well-formed XML (%1). Showing the text as it is.")
                         .arg(reader.errorString());
        return out;
    }
    Formatted out;
    out.text = formatted;
    return out;
}

QString ValueViewer::sizeText(qint64 bytes)
{
    if (bytes == 1)
        return tr("1 byte");
    QString exact = tr("%1 bytes").arg(bytes);
    if (bytes < 1024)
        return exact;
    const char *unit = "KiB";
    double value = static_cast<double>(bytes) / 1024.0;
    for (const char *next : { "MiB", "GiB" }) {
        if (value < 1023.95)
            break;
        value /= 1024.0;
        unit = next;
    }
    return QStringLiteral("%1 (%2 %3)").arg(exact).arg(value, 0, 'f', 1).arg(QLatin1String(unit));
}

ValueViewer::Formatted ValueViewer::hexDump(QByteArrayView bytes, qint64 totalSize)
{
    const qsizetype shown = std::min<qsizetype>(bytes.size(), kMaxBlobBytes);
    Formatted out;
    out.text.reserve(static_cast<qsizetype>((shown / kBytesPerLine + 1) * 78));
    for (qsizetype offset = 0; offset < shown; offset += kBytesPerLine) {
        const qsizetype n = std::min<qsizetype>(kBytesPerLine, shown - offset);
        QString line
            = QStringLiteral("%1  ").arg(static_cast<qulonglong>(offset), 8, 16, QLatin1Char('0'));
        QString ascii;
        ascii.reserve(kBytesPerLine);
        for (int i = 0; i < kBytesPerLine; ++i) {
            if (i == 8)
                line += QLatin1Char(' ');
            if (i < n) {
                const auto b = static_cast<uchar>(bytes[offset + i]);
                line += QStringLiteral("%1 ").arg(b, 2, 16, QLatin1Char('0'));
                ascii += (b >= 0x20 && b < 0x7f) ? QChar(b) : QLatin1Char('.');
            } else {
                line += QLatin1String("   ");
            }
        }
        out.text += line + QLatin1String(" |") + ascii + QLatin1String("|\n");
    }
    if (totalSize > shown)
        out.notice
            = tr("%1 more bytes not shown. Copy takes the whole value.").arg(totalSize - shown);
    return out;
}

ValueViewer::Formatted ValueViewer::format(const QString &kind, QStringView text)
{
    if (kind == QLatin1String("json"))
        return formatJson(text);
    if (kind == QLatin1String("xml"))
        return formatXml(text);
    return formatText(text);
}

void ValueViewer::copyText(const QString &text) const
{
    QGuiApplication::clipboard()->setText(text);
}

QVariantMap ValueViewer::formatted(const QString &kind, const QString &text) const
{
    const Formatted f = format(kind, text);
    return { { QStringLiteral("text"), f.text },
             { QStringLiteral("notice"), f.notice },
             { QStringLiteral("ok"), f.ok } };
}

} // namespace quarry::ui
