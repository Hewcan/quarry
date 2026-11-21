#include "migration_name.h"

#include <QRegularExpression>

namespace quarry::ui {

QString shortMigrationName(const QString &name)
{
    static const QRegularExpression datePrefix(QStringLiteral("^\\d{4}_\\d{2}_\\d{2}_"));
    const QRegularExpressionMatch m = datePrefix.match(name);
    if (!m.hasMatch() || m.capturedLength() >= name.size())
        return name;
    return name.mid(m.capturedLength());
}

} // namespace quarry::ui
