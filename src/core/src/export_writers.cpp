#include "export_text.h"
#include "export_writer.h"

#include <quarry/provider_api.h>

#include <QBuffer>
#include <QSet>
#include <QString>

#include <algorithm>
#include <cmath>

namespace quarry {

namespace {

using exporttext::CellText;

constexpr char kBom[] = "\xEF\xBB\xBF";

QByteArray utf8(const QString &text)
{
    return text.toUtf8();
}

// CSV (RFC 4180 quoting) and TSV (backslash escapes), one row per line.
class DelimitedWriter final : public ExportWriter
{
public:
    DelimitedWriter(const ExportOptions &options, bool tsv)
        : m_tsv(tsv)
        , m_header(options.header)
        , m_bom(options.bom)
    {
        QChar delimiter = tsv ? QLatin1Char('\t') : options.delimiter;
        if (delimiter.isNull() || delimiter == QLatin1Char('"') || delimiter == QLatin1Char('\n')
            || delimiter == QLatin1Char('\r'))
            delimiter = QLatin1Char(','); // would make the file ambiguous
        m_delimiter = utf8(QString(delimiter));
    }

    void begin(const std::vector<ColumnInfo> &columns, ByteSink &out) override
    {
        m_columnCount = columns.size();
        if (m_bom)
            out.append(QByteArrayView(kBom, 3));
        if (!m_header || columns.empty())
            return;
        for (size_t c = 0; c < columns.size(); ++c) {
            if (c > 0)
                out.append(m_delimiter);
            appendField(out, utf8(columns[c].name));
        }
        out.append('\n');
    }

    void writeBatch(const ResultBatch &batch, ByteSink &out) override
    {
        CellText cells;
        for (qsizetype row = 0; row < batch.rowCount; ++row) {
            for (size_t c = 0; c < m_columnCount; ++c) {
                if (c > 0)
                    out.append(m_delimiter);
                appendField(out, cells.text(batch.columns[c], row));
            }
            out.append('\n');
        }
    }

    void end(ByteSink &) override { }

private:
    void appendField(ByteSink &out, QByteArrayView text) const
    {
        if (m_tsv)
            appendTsv(out, text);
        else
            appendCsv(out, text);
    }

    bool needsQuotes(QByteArrayView text) const
    {
        const char first = m_delimiter[0];
        const bool single = m_delimiter.size() == 1;
        for (const char c : text) {
            if (c == '"' || c == '\n' || c == '\r' || (single && c == first))
                return true;
        }
        return !single && text.contains(QByteArrayView(m_delimiter));
    }

    void appendCsv(ByteSink &out, QByteArrayView text) const
    {
        if (!needsQuotes(text)) {
            out.append(text);
            return;
        }
        out.append('"');
        qsizetype runStart = 0;
        for (qsizetype i = 0; i < text.size(); ++i) {
            if (text[i] != '"')
                continue;
            out.append(text.sliced(runStart, i + 1 - runStart)); // up to and including the quote
            out.append('"');
            runStart = i + 1;
        }
        out.append(text.sliced(runStart));
        out.append('"');
    }

    static void appendTsv(ByteSink &out, QByteArrayView text)
    {
        qsizetype runStart = 0;
        for (qsizetype i = 0; i < text.size(); ++i) {
            const char c = text[i];
            QByteArrayView escape;
            if (c == '\t')
                escape = QByteArrayView("\\t");
            else if (c == '\n')
                escape = QByteArrayView("\\n");
            else if (c == '\r')
                escape = QByteArrayView("\\r");
            else if (c == '\\')
                escape = QByteArrayView("\\\\");
            else
                continue;
            out.append(text.sliced(runStart, i - runStart));
            out.append(escape);
            runStart = i + 1;
        }
        out.append(text.sliced(runStart));
    }

    bool m_tsv;
    bool m_header;
    bool m_bom;
    QByteArray m_delimiter;
    size_t m_columnCount = 0;
};

class MarkdownWriter final : public ExportWriter
{
public:
    explicit MarkdownWriter(const ExportOptions &options)
        : m_header(options.header)
    { }

    void begin(const std::vector<ColumnInfo> &columns, ByteSink &out) override
    {
        m_columnCount = columns.size();
        if (columns.empty())
            return;
        // A Markdown table cannot exist without a header row: without names it is left blank.
        out.append('|');
        for (const ColumnInfo &column : columns) {
            out.append(' ');
            if (m_header)
                appendCell(out, utf8(column.name));
            out.append(QByteArrayView(" |"));
        }
        out.append('\n');
        out.append('|');
        for (size_t c = 0; c < m_columnCount; ++c)
            out.append(QByteArrayView(" --- |"));
        out.append('\n');
    }

    void writeBatch(const ResultBatch &batch, ByteSink &out) override
    {
        CellText cells;
        for (qsizetype row = 0; row < batch.rowCount; ++row) {
            out.append('|');
            for (size_t c = 0; c < m_columnCount; ++c) {
                out.append(' ');
                appendCell(out, cells.text(batch.columns[c], row));
                out.append(QByteArrayView(" |"));
            }
            out.append('\n');
        }
    }

    void end(ByteSink &) override { }

private:
    static void appendCell(ByteSink &out, QByteArrayView text)
    {
        qsizetype runStart = 0;
        for (qsizetype i = 0; i < text.size(); ++i) {
            const char c = text[i];
            QByteArrayView replacement;
            qsizetype skip = 1;
            if (c == '|') {
                replacement = QByteArrayView("\\|");
            } else if (c == '\n') {
                replacement = QByteArrayView("<br>");
            } else if (c == '\r') {
                replacement = QByteArrayView("<br>");
                if (i + 1 < text.size() && text[i + 1] == '\n')
                    skip = 2; // CRLF is one line break
            } else {
                continue;
            }
            out.append(text.sliced(runStart, i - runStart));
            out.append(replacement);
            i += skip - 1;
            runStart = i + 1;
        }
        out.append(text.sliced(runStart));
    }

    bool m_header;
    size_t m_columnCount = 0;
};

// Array of objects (streamed) or one object per line.
class JsonWriter final : public ExportWriter
{
public:
    explicit JsonWriter(bool lines)
        : m_lines(lines)
    { }

    void begin(const std::vector<ColumnInfo> &columns, ByteSink &out) override
    {
        QSet<QString> used;
        used.reserve(static_cast<qsizetype>(columns.size()));
        m_keys.reserve(columns.size());
        for (const ColumnInfo &column : columns) {
            QString name = column.name;
            for (int n = 2; used.contains(name); ++n)
                name = column.name + QLatin1Char('_') + QString::number(n);
            used.insert(name);
            m_keys.push_back(jsonKey(name));
        }
        if (!m_lines)
            out.append('[');
    }

    void writeBatch(const ResultBatch &batch, ByteSink &out) override
    {
        for (qsizetype row = 0; row < batch.rowCount; ++row) {
            if (!m_lines)
                out.append(m_any ? QByteArrayView(",\n") : QByteArrayView("\n"));
            m_any = true;
            out.append('{');
            for (size_t c = 0; c < m_keys.size(); ++c) {
                if (c > 0)
                    out.append(',');
                out.append(m_keys[c]);
                appendValue(out, batch.columns[c], row);
            }
            out.append('}');
            if (m_lines)
                out.append('\n');
        }
    }

    void end(ByteSink &out) override
    {
        if (!m_lines)
            out.append(m_any ? QByteArrayView("\n]\n") : QByteArrayView("]\n"));
    }

private:
    static QByteArray jsonKey(const QString &name)
    {
        QBuffer buffer;
        buffer.open(QIODevice::WriteOnly);
        ByteSink sink(buffer);
        exporttext::appendJsonString(sink, utf8(name));
        sink.append(':');
        sink.flush();
        return buffer.data();
    }

    void appendValue(ByteSink &out, const ColumnBuffer &column, qsizetype row)
    {
        char number[exporttext::kNumberBufferSize];
        switch (column.typeAt(row)) {
        case ValueType::Null:
            out.append(QByteArrayView("null"));
            break;
        case ValueType::Bool:
            out.append(column.int64At(row) != 0 ? QByteArrayView("true") : QByteArrayView("false"));
            break;
        case ValueType::Int64:
            out.append(exporttext::formatInt64(number, column.int64At(row)));
            break;
        case ValueType::Double: {
            const double v = column.doubleAt(row);
            out.append(std::isfinite(v) ? exporttext::formatDouble(number, v)
                                        : QByteArrayView("null"));
            break;
        }
        case ValueType::Decimal: {
            const QByteArrayView text = column.bytesAt(row);
            if (exporttext::isJsonNumber(text))
                out.append(text);
            else
                exporttext::appendJsonString(out, text);
            break;
        }
        case ValueType::Json:
            if (exporttext::compactJson(column.bytesAt(row), m_scratch))
                out.append(m_scratch);
            else
                exporttext::appendJsonString(out, column.bytesAt(row));
            break;
        case ValueType::Bytes:
            out.append('"');
            out.append(column.bytesAt(row).toByteArray().toBase64());
            out.append('"');
            break;
        default:
            exporttext::appendJsonString(out, column.bytesAt(row));
        }
    }

    bool m_lines;
    bool m_any = false;
    std::vector<QByteArray> m_keys;
    QByteArray m_scratch;
};

// INSERT INTO <table> (<columns>) VALUES (...), (...); with rowsPerInsert rows per statement.
class SqlInsertWriter final : public ExportWriter
{
public:
    explicit SqlInsertWriter(const ExportOptions &options)
        : m_options(options)
        , m_rowsPerInsert(std::clamp(options.rowsPerInsert, 1, 1000))
    { }

    void begin(const std::vector<ColumnInfo> &columns, ByteSink &) override
    {
        m_columnCount = columns.size();
        m_prefix = "INSERT INTO " + quoteIdentifier(m_options.tableName) + " (";
        for (size_t c = 0; c < columns.size(); ++c) {
            if (c > 0)
                m_prefix += ", ";
            m_prefix += quoteIdentifier(columns[c].name);
        }
        m_prefix += ") VALUES ";
    }

    void writeBatch(const ResultBatch &batch, ByteSink &out) override
    {
        if (m_columnCount == 0)
            return;
        for (qsizetype row = 0; row < batch.rowCount; ++row) {
            if (m_inStatement == 0)
                out.append(m_prefix);
            else
                out.append(QByteArrayView(", "));
            out.append('(');
            for (size_t c = 0; c < m_columnCount; ++c) {
                if (c > 0)
                    out.append(QByteArrayView(", "));
                appendValue(out, batch.columns[c], row);
            }
            out.append(')');
            if (++m_inStatement == m_rowsPerInsert)
                closeStatement(out);
        }
    }

    void end(ByteSink &out) override
    {
        if (m_inStatement > 0)
            closeStatement(out);
    }

private:
    void closeStatement(ByteSink &out)
    {
        out.append(QByteArrayView(";\n"));
        m_inStatement = 0;
    }

    QByteArray quoteIdentifier(const QString &name) const
    {
        if (m_options.dialect)
            return utf8(m_options.dialect->quoteIdentifier(name));
        QString quoted = name;
        quoted.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return utf8(QLatin1Char('"') + quoted + QLatin1Char('"'));
    }

    void appendLiteral(ByteSink &out, QByteArrayView text) const
    {
        if (m_options.dialect) {
            out.append(utf8(m_options.dialect->quoteLiteral(QString::fromUtf8(text))));
            return;
        }
        out.append('\'');
        qsizetype runStart = 0;
        for (qsizetype i = 0; i < text.size(); ++i) {
            if (text[i] != '\'')
                continue;
            out.append(text.sliced(runStart, i + 1 - runStart));
            out.append('\'');
            runStart = i + 1;
        }
        out.append(text.sliced(runStart));
        out.append('\'');
    }

    void appendValue(ByteSink &out, const ColumnBuffer &column, qsizetype row) const
    {
        char number[exporttext::kNumberBufferSize];
        switch (column.typeAt(row)) {
        case ValueType::Null:
            out.append(QByteArrayView("NULL"));
            break;
        case ValueType::Bool:
            out.append(column.int64At(row) != 0 ? QByteArrayView("TRUE") : QByteArrayView("FALSE"));
            break;
        case ValueType::Int64:
            out.append(exporttext::formatInt64(number, column.int64At(row)));
            break;
        case ValueType::Double: {
            const double v = column.doubleAt(row);
            // No portable literal for NaN/Infinity.
            out.append(std::isfinite(v) ? exporttext::formatDouble(number, v)
                                        : QByteArrayView("NULL"));
            break;
        }
        case ValueType::Decimal: {
            const QByteArrayView text = column.bytesAt(row);
            if (exporttext::isJsonNumber(text))
                out.append(text);
            else
                appendLiteral(out, text);
            break;
        }
        case ValueType::Bytes:
            // shortcut: X'..' is ANSI/SQLite/MySQL; SQL Server wants 0x.., when a dialect needs it
            // add a hex-literal hook to ISqlDialect.
            out.append(QByteArrayView("X'"));
            exporttext::appendHex(out, column.bytesAt(row));
            out.append('\'');
            break;
        default:
            appendLiteral(out, column.bytesAt(row));
        }
    }

    ExportOptions m_options;
    int m_rowsPerInsert;
    size_t m_columnCount = 0;
    int m_inStatement = 0; // rows already in the open INSERT
    QByteArray m_prefix;
};

} // namespace

std::unique_ptr<ExportWriter> makeExportWriter(const ExportOptions &options)
{
    switch (options.format) {
    case ExportFormat::Csv:
        return std::make_unique<DelimitedWriter>(options, false);
    case ExportFormat::Tsv:
        return std::make_unique<DelimitedWriter>(options, true);
    case ExportFormat::Json:
        return std::make_unique<JsonWriter>(false);
    case ExportFormat::JsonLines:
        return std::make_unique<JsonWriter>(true);
    case ExportFormat::SqlInsert:
        return std::make_unique<SqlInsertWriter>(options);
    case ExportFormat::Markdown:
        return std::make_unique<MarkdownWriter>(options);
    }
    return std::make_unique<DelimitedWriter>(options, false);
}

} // namespace quarry
