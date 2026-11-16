#pragma once

#include <QList>
#include <QMetaType>
#include <QString>
#include <QUuid>

namespace quarry {

// A one-level folder of saved queries (no nesting).
struct SavedFolder
{
    QUuid id; // null = new; SavedQueryStore generates one
    QString name; // non-empty
    int position = 0; // order among folders
};

} // namespace quarry

Q_DECLARE_METATYPE(quarry::SavedFolder)
Q_DECLARE_METATYPE(QList<quarry::SavedFolder>)
