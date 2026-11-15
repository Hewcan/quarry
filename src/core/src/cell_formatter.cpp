#include <quarry/cell_formatter.h>

#include <QLocale>

namespace quarry {

namespace {

QString numberText(const ColumnBuffer &column, qsizetype row, ValueType type)
{
    switch (type) {
    case ValueType::Bool:
        return column.int64At(row) != 0 ? QStringLiteral("true") : QStringLiteral("false");
    case ValueType::Int64:
        return QString::number(static_cast<qlonglong>(column.int64At(row)));
    default:
        return QString::number(column.doubleAt(row), 'g', QLocale::FloatingPointShortest);
    }
}

QString binaryLabel(qsizetype size)
{
    if (size < 1024) {
        return QStringLiteral("<binary %1 B>").arg(size);
    }
    double value = static_cast<double>(size);
    const char *unit = "KB";
    value /= 1024.0;
    for (const char *next : { "MB", "GB" }) {
        if (value < 1023.95) { // would round to "1024.0": show the next unit
            break;
        }
        value /= 1024.0;
        unit = next;
    }
    return QStringLiteral("<binary %1 %2>").arg(value, 0, 'f', 1).arg(QLatin1String(unit));
}

QString hexText(QByteArrayView bytes)
{
    // toHex works on QByteArray; one temporary is fine for the (rare) blob copy.
    return QStringLiteral("0x") + QString::fromLatin1(bytes.toByteArray().toHex());
}

// Line breaks are ASCII, so searching bytes never lands inside a multi-byte sequence.
qsizetype firstBreak(QByteArrayView bytes)
{
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == '\n' || bytes[i] == '\r') {
            return i;
        }
    }
    return -1;
}

QString displayText(QByteArrayView bytes)
{
    // 4 bytes per char is the UTF-8 maximum, so this window always holds >= kDisplayChars units.
    constexpr qsizetype kWindow = CellFormatter::kDisplayChars * 4 + 4;
    bool cut = bytes.size() > kWindow;
    QByteArrayView head = cut ? bytes.first(kWindow) : bytes;
    const qsizetype brk = firstBreak(head);
    if (brk >= 0) {
        head = head.first(brk);
        cut = true;
    }
    QString text = QString::fromUtf8(head);
    if (text.size() > CellFormatter::kDisplayChars) {
        qsizetype n = CellFormatter::kDisplayChars;
        if (text.at(n - 1).isHighSurrogate() && text.at(n).isLowSurrogate()) {
            --n;
        }
        text.truncate(n);
        cut = true;
    }
    if (cut) {
        text += QChar(0x2026);
    }
    return text;
}

} // namespace

QString CellFormatter::display(const ColumnBuffer &column, qsizetype row)
{
    if (column.isNull(row)) {
        return QStringLiteral("NULL");
    }
    const ValueType type = column.typeAt(row);
    switch (type) {
    case ValueType::Null:
        return QStringLiteral("NULL");
    case ValueType::Bool:
    case ValueType::Int64:
    case ValueType::Double:
        return numberText(column, row, type);
    case ValueType::Bytes:
        return binaryLabel(column.bytesAt(row).size());
    default:
        return displayText(column.bytesAt(row));
    }
}

QString CellFormatter::copy(const ColumnBuffer &column, qsizetype row)
{
    if (column.isNull(row)) {
        return {};
    }
    const ValueType type = column.typeAt(row);
    switch (type) {
    case ValueType::Null:
        return {};
    case ValueType::Bool:
    case ValueType::Int64:
    case ValueType::Double:
        return numberText(column, row, type);
    case ValueType::Bytes:
        return hexText(column.bytesAt(row));
    default:
        return QString::fromUtf8(column.bytesAt(row));
    }
}

} // namespace quarry
