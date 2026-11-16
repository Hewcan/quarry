#pragma once

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QUuid>

#include <optional>

namespace quarry {

// A named, saved statement. Global: the connection is an optional association, not ownership.
struct SavedQuery
{
    QUuid id; // null = new; SavedQueryStore generates one
    std::optional<QUuid> folderId; // nullopt = top level
    std::optional<QUuid> connectionId; // nullopt = any connection
    QString name; // non-empty
    QString sql;
    int position = 0; // order inside its folder (or top level)
    QDateTime createdAt, updatedAt; // UTC, set by SavedQueryStore
};

} // namespace quarry

Q_DECLARE_METATYPE(quarry::SavedQuery)
Q_DECLARE_METATYPE(QList<quarry::SavedQuery>)
