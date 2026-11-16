#pragma once

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QUuid>

#include <optional>

namespace quarry {

// One executed statement, persisted in `query_history`.
struct HistoryEntry
{
    qint64 id = 0; // 0 = not stored yet; QueryHistoryStore assigns it
    std::optional<QUuid> connectionId; // nullopt = connection since deleted
    QString sql; // non-empty
    QDateTime executedAt; // UTC; set by the caller (start of the run)
    qint64 durationMs = 0;
    qint64 rows = -1; // rows read or affected; -1 = unknown
    bool success = true;
    QString errorMessage; // plain-language Error::message when !success
};

} // namespace quarry

Q_DECLARE_METATYPE(quarry::HistoryEntry)
Q_DECLARE_METATYPE(QList<quarry::HistoryEntry>)
