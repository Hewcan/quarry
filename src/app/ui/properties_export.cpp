#include "properties_export.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace quarry::ui::properties_export {

namespace {

struct Row
{
    QString name;
    QString type;
    QString comment;
    bool nullable = true;
    bool primary = false;
    bool foreign = false;

    QString key() const
    {
        return primary ? QStringLiteral("PRIMARY")
                       : (foreign ? QStringLiteral("FOREIGN") : QString());
    }
};

QString tr(const char *text)
{
    return QCoreApplication::translate("quarry::ui::properties_export", text);
}

QList<Row> rowsOf(const QVariantMap &info)
{
    QList<Row> rows;
    const QVariantList columns = info.value(QStringLiteral("columns")).toList();
    rows.reserve(columns.size());
    for (const QVariant &v : columns) {
        const QVariantMap c = v.toMap();
        Row r;
        r.name = c.value(QStringLiteral("name")).toString();
        r.type = c.value(QStringLiteral("type")).toString();
        r.comment = c.value(QStringLiteral("description")).toString();
        r.nullable = c.value(QStringLiteral("nullable")).toBool();
        r.primary = c.value(QStringLiteral("primaryKey")).toBool();
        r.foreign = !c.value(QStringLiteral("foreignKeys")).toList().isEmpty();
        rows.append(r);
    }
    return rows;
}

// The tokens do not change with the language: other tools may read them.
QString nullability(const Row &r)
{
    return r.nullable ? QStringLiteral("NULLABLE") : QStringLiteral("REQUIRED");
}

QString csvField(QString v)
{
    // A cell that starts like a formula would run in a spreadsheet.
    if (!v.isEmpty() && QStringLiteral("=+@").contains(v.at(0)))
        v.prepend(QLatin1Char('\''));
    if (v.contains(QLatin1Char(',')) || v.contains(QLatin1Char('"'))
        || v.contains(QLatin1Char('\n')) || v.contains(QLatin1Char('\r')))
        return QLatin1Char('"') + v.replace(QLatin1Char('"'), QStringLiteral("\"\""))
            + QLatin1Char('"');
    return v;
}

QString formatCsv(const QVariantMap &info)
{
    QString out = QStringLiteral("Field,Type,Comment,Nullability,Key\r\n");
    for (const Row &r : rowsOf(info))
        out += csvField(r.name) + QLatin1Char(',') + csvField(r.type) + QLatin1Char(',')
            + csvField(r.comment) + QLatin1Char(',') + nullability(r) + QLatin1Char(',') + r.key()
            + QStringLiteral("\r\n");
    return out;
}

QString formatJson(const QVariantMap &info)
{
    QJsonArray columns;
    for (const Row &r : rowsOf(info))
        columns.append(QJsonObject { { QStringLiteral("name"), r.name },
                                     { QStringLiteral("type"), r.type },
                                     { QStringLiteral("comment"), r.comment },
                                     { QStringLiteral("nullable"), r.nullable },
                                     { QStringLiteral("primaryKey"), r.primary },
                                     { QStringLiteral("foreignKey"), r.foreign } });
    const QJsonObject root {
        { QStringLiteral("table"), info.value(QStringLiteral("name")).toString() },
        { QStringLiteral("type"), info.value(QStringLiteral("tableType")).toString() },
        { QStringLiteral("comment"), info.value(QStringLiteral("description")).toString() },
        { QStringLiteral("columns"), columns }
    };
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

QString formatHtml(const QVariantMap &info)
{
    const auto esc = [](const QString &s) { return s.toHtmlEscaped(); };
    const QString name = info.value(QStringLiteral("name")).toString();
    const QString comment = info.value(QStringLiteral("description")).toString();
    QString out
        = QStringLiteral("<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<title>")
        + esc(name)
        + QStringLiteral("</title>\n<style type=\"text/css\">\n"
                         "  table { width: 100% }\n"
                         "  th { background-color: #000000; color: white; }\n"
                         "  .prim { background-color: #FFC000; }\n"
                         "  .sec { background-color: #ADD8E6; }\n"
                         "</style>\n</head>\n<body>\n<h1>")
        + esc(name) + QStringLiteral("</h1>\n");
    if (!comment.isEmpty())
        out += QStringLiteral("<h2>") + esc(comment) + QStringLiteral("</h2>\n");
    out += QStringLiteral("<table class=\"table\">\n  <thead>\n    <tr><th>") + esc(tr("Field"))
        + QStringLiteral("</th><th>") + esc(tr("Type")) + QStringLiteral("</th><th>")
        + esc(tr("Comment")) + QStringLiteral("</th><th>") + esc(tr("Nullability"))
        + QStringLiteral("</th></tr>\n  </thead>\n  <tbody>\n");
    for (const Row &r : rowsOf(info)) {
        // Primary wins over foreign. The colour is only decoration: CSV, JSON and Markdown carry
        // the key as data.
        const QString cls = r.primary ? QStringLiteral(" class=\"prim\"")
                                      : (r.foreign ? QStringLiteral(" class=\"sec\"") : QString());
        out += QStringLiteral("    <tr") + cls + QStringLiteral(">\n      <td>") + esc(r.name)
            + QStringLiteral("</td><td>") + esc(r.type) + QStringLiteral("</td><td>")
            + esc(r.comment) + QStringLiteral("</td><td>") + nullability(r)
            + QStringLiteral("</td>\n    </tr>\n");
    }
    out += QStringLiteral("  </tbody>\n</table>\n</body>\n</html>\n");
    return out;
}

QString mdCell(QString v)
{
    v.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    v.replace(QLatin1Char('|'), QStringLiteral("\\|"));
    v.replace(QStringLiteral("\r\n"), QStringLiteral("<br>"));
    v.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return v;
}

QString formatMarkdown(const QVariantMap &info)
{
    const QString name = info.value(QStringLiteral("name")).toString();
    const QString comment = info.value(QStringLiteral("description")).toString();
    QString out = QStringLiteral("# ") + name + QStringLiteral("\n\n");
    if (!comment.isEmpty())
        out += comment + QStringLiteral("\n\n");
    out += QStringLiteral(
        "| Field | Type | Comment | Nullability | Key |\n|---|---|---|---|---|\n");
    for (const Row &r : rowsOf(info))
        out += QStringLiteral("| ") + mdCell(r.name) + QStringLiteral(" | ") + mdCell(r.type)
            + QStringLiteral(" | ") + mdCell(r.comment) + QStringLiteral(" | ") + nullability(r)
            + QStringLiteral(" | ") + r.key() + QStringLiteral(" |\n");
    return out;
}

} // namespace

bool parseFormat(const QString &name, Format *out)
{
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("csv"))
        *out = Format::Csv;
    else if (n == QLatin1String("json"))
        *out = Format::Json;
    else if (n == QLatin1String("html"))
        *out = Format::Html;
    else if (n == QLatin1String("markdown") || n == QLatin1String("md"))
        *out = Format::Markdown;
    else
        return false;
    return true;
}

QString extensionOf(Format f)
{
    switch (f) {
    case Format::Csv:
        return QStringLiteral("csv");
    case Format::Json:
        return QStringLiteral("json");
    case Format::Html:
        return QStringLiteral("html");
    case Format::Markdown:
        return QStringLiteral("md");
    }
    return {};
}

QString format(const QVariantMap &info, Format f)
{
    switch (f) {
    case Format::Csv:
        return formatCsv(info);
    case Format::Json:
        return formatJson(info);
    case Format::Html:
        return formatHtml(info);
    case Format::Markdown:
        return formatMarkdown(info);
    }
    return {};
}

} // namespace quarry::ui::properties_export
