#pragma once

#include <QString>
#include <QVariantMap>

namespace quarry::ui {

// Formats the columns of a table's properties (the map StructureController::infoOf() returns) as
// text: one row per column with name, type, comment, nullability and the key marks. Pure and
// synchronous, so it is tested without a session.
namespace properties_export {

enum class Format { Csv, Json, Html, Markdown };

// "csv" | "json" | "html" | "markdown" (case-insensitive). False when unknown.
bool parseFormat(const QString &name, Format *out);
// File extension without the dot, for the save dialog.
QString extensionOf(Format format);

QString format(const QVariantMap &info, Format format);

} // namespace properties_export

} // namespace quarry::ui
