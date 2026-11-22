#pragma once

#include <quarry/value_type.h>

#include <QByteArrayView>
#include <QObject>
#include <QString>
#include <QStringView>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

namespace quarry::ui {

// Formats one value for the viewers of the record panel (H11 phase 3, UX.md "Panel de registro y
// visores"): JSON and XML re-indented, long text, and a BLOB as hex plus ASCII. Everything is
// done here, in C++, and capped, so a huge value never freezes the UI thread: the callers hand over
// at most kMaxChars characters / kMaxBlobBytes bytes and the notice says what was left out.
//
// QML uses it as the singleton `ValueViewer`; the static functions are what the tests call.
class ValueViewer : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    // The viewer never shows more text than this; the rest is said in the notice.
    static constexpr qsizetype kMaxChars = 200000;
    // A BLOB is shown as hex up to this many bytes ("N more bytes not shown" after it).
    static constexpr qsizetype kMaxBlobBytes = 64 * 1024;
    // Text of this length or more (or with a line break) gets a viewer, because the grid cuts it.
    static constexpr qsizetype kLongTextChars = 200;
    // Bytes of a BLOB shown per line of the hex view.
    static constexpr int kBytesPerLine = 16;

    struct Formatted
    {
        QString text;
        // Plain-language note shown above the text ("" = none): not valid JSON, cut, ...
        QString notice;
        // The text is the formatted one (false: it was shown as it came).
        bool ok = true;
    };

    explicit ValueViewer(QObject *parent = nullptr)
        : QObject(parent)
    { }

    // "json" | "xml" | "blob" | "text" | "" (no viewer needed). By the type of the value first
    // (Json, Bytes, a native type called xml / json), then by the content when the value is text:
    // it starts with `{` or `[` and is valid JSON, or starts with `<` and is well-formed XML, or it
    // is long (kLongTextChars) or has line breaks.
    static QString detect(ValueType type, QStringView nativeType, QStringView text);
    // Re-indented with two spaces; keys keep the order they came in. Invalid: the text as it is.
    static Formatted formatJson(QStringView text);
    // Re-indented with two spaces. Not well-formed: the text as it is.
    static Formatted formatXml(QStringView text);
    // The text as it is, cut at kMaxChars.
    static Formatted formatText(QStringView text);
    // Offset, 16 bytes in hex and ASCII per line (like `hexdump -C`). `bytes` is what was read
    // (at most kMaxBlobBytes are shown), `totalSize` the size of the whole value.
    static Formatted hexDump(QByteArrayView bytes, qint64 totalSize);
    // "1 byte", "512 bytes", "2,048 bytes (2.0 KiB)".
    static QString sizeText(qint64 bytes);
    // The formatter for `kind` ("json", "xml", "blob" is not here: it has bytes, not text).
    static Formatted format(const QString &kind, QStringView text);

    // For QML: {text, notice, ok}.
    Q_INVOKABLE QVariantMap formatted(const QString &kind, const QString &text) const;
    Q_INVOKABLE QString size(qint64 bytes) const { return sizeText(bytes); }
    // Puts `text` on the system clipboard (the "Copy DDL" button).
    Q_INVOKABLE void copyText(const QString &text) const;
};

} // namespace quarry::ui
