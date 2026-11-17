#pragma once

#include <quarry/export_format.h>

#include <QMetaType>
#include <QString>

namespace quarry {

class ISqlDialect;

// What to write and where. Value rules per format are in ARCHITECTURE section 14.
struct ExportOptions
{
    ExportFormat format = ExportFormat::Csv;
    QString filePath; // required; the final file, written through a temporary sibling
    // Csv only. Tsv always uses a tab and ignores this.
    QChar delimiter = QLatin1Char(',');
    bool header = true; // Csv, Tsv and Markdown: first row with the column names
    bool bom = false; // Csv and Tsv: UTF-8 byte order mark at the start
    // SqlInsert only.
    QString tableName = QStringLiteral("export_table"); // quoted with the dialect
    int rowsPerInsert = 1; // 1..1000 rows in each INSERT statement
    const ISqlDialect *dialect = nullptr; // quoting and hex literals; null = ANSI ("" and X'..')
};

} // namespace quarry

Q_DECLARE_METATYPE(quarry::ExportOptions)
