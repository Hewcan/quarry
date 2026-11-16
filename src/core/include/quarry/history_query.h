#pragma once

#include <QMetaType>
#include <QString>
#include <QUuid>

#include <optional>

namespace quarry {

// Filter for QueryHistoryStore::search(). Newest first (executedAt, then id, descending).
struct HistoryQuery
{
    quint64 requestId = 0; // echoed by searched(); lets the UI drop stale answers
    // Empty = no text filter; otherwise every word must match, the last one as a prefix.
    QString text;
    std::optional<QUuid> connectionId; // nullopt = all connections
    bool onlyErrors = false;
    int limit = 200; // 1..1000
    int offset = 0;
};

} // namespace quarry

Q_DECLARE_METATYPE(quarry::HistoryQuery)
