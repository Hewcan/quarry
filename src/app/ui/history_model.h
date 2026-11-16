#pragma once

#include <quarry/history_entry.h>
#include <quarry/query_history_store.h>

#include <QAbstractListModel>
#include <QString>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <optional>
#include <vector>

namespace quarry::ui {

class ConnectionController;

// The query history (ARCHITECTURE section 11.3): what the History tab lists and what
// QueryController writes. Owns the QueryHistoryStore, created on first use (the first recorded
// statement or the first time the panel opens), so nothing is added to startup.
//
// The list is one page of the newest entries that match text / connection / "only errors";
// loadMore() appends the next page. Every search carries a requestId and an answer to an older
// one is dropped, so slow answers never overwrite the list of a newer filter. Text typing is
// debounced; the other filters search at once.
class HistoryModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(
        QString databasePath READ databasePath WRITE setDatabasePath NOTIFY configChanged FINAL)
    // Gives the connection names.
    Q_PROPERTY(quarry::ui::ConnectionController *connections READ connections WRITE setConnections
                   NOTIFY configChanged FINAL)
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY filterChanged FINAL)
    // "" = every connection.
    Q_PROPERTY(QString connectionFilter READ connectionFilter WRITE setConnectionFilter NOTIFY
                   filterChanged FINAL)
    Q_PROPERTY(bool onlyErrors READ onlyErrors WRITE setOnlyErrors NOTIFY filterChanged FINAL)
    Q_PROPERTY(int debounce READ debounce WRITE setDebounce NOTIFY configChanged FINAL)
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY hasMoreChanged FINAL)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged FINAL)
    // The first search was asked for (the panel was opened).
    Q_PROPERTY(bool started READ started NOTIFY startedChanged FINAL)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        SqlRole,
        PreviewRole, // one line, cut
        ConnectionIdRole, // "" = the connection was deleted
        ConnectionNameRole,
        WhenRole,
        DetailRole, // "15 ms · 120 rows"
        SuccessRole,
        ErrorMessageRole,
    };

    static constexpr int kPageSize = 100;
    static constexpr int kDefaultDebounceMs = 200;

    explicit HistoryModel(QObject *parent = nullptr);

    QString databasePath() const { return m_databasePath; }
    void setDatabasePath(const QString &path);
    ConnectionController *connections() const { return m_connections; }
    void setConnections(ConnectionController *connections);
    QString text() const { return m_text; }
    void setText(const QString &text);
    QString connectionFilter() const { return m_connectionFilter; }
    void setConnectionFilter(const QString &id);
    bool onlyErrors() const { return m_onlyErrors; }
    void setOnlyErrors(bool on);
    int debounce() const { return m_debounce.interval(); }
    void setDebounce(int ms) { m_debounce.setInterval(ms); }
    int count() const { return static_cast<int>(m_rows.size()); }
    bool hasMore() const { return m_hasMore; }
    bool loading() const { return m_loading; }
    bool started() const { return m_started; }

    // Writes one statement. Asynchronous; the store drops it when the connection does not
    // record history. The list is not searched again: a matching entry is added at the top.
    void record(const quarry::HistoryEntry &entry);

    // Lists the first page the first time (the panel does it when it opens). Idempotent.
    Q_INVOKABLE void start();
    // Searches again with the current filters, dropping what is listed.
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE void remove(int row);
    // Clears the history of the connection filter (everything when it is empty).
    Q_INVOKABLE void clear();
    // Clears the history of one connection, whatever the filter (the privacy question).
    Q_INVOKABLE void clearConnection(const QString &connectionId);
    Q_INVOKABLE QString sqlOf(int row) const;
    Q_INVOKABLE QVariantMap get(int row) const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // The store, created on first use. Tests wait on its signals.
    quarry::QueryHistoryStore *store();

signals:
    void configChanged();
    void filterChanged();
    void countChanged();
    void hasMoreChanged();
    void loadingChanged();
    void startedChanged();
    // For the screen reader, without moving the focus.
    void announcement(const QString &text, bool assertive);
    // A search ended (after the stale ones were dropped).
    void searchFinished();
    void cleared(const QString &connectionId, qint64 count);
    // A store operation failed (plain message, technical detail apart).
    void failed(const QString &message, const QString &detail);

private:
    void search(bool append);
    void onSearched(quint64 requestId, const QList<quarry::HistoryEntry> &entries, bool hasMore);
    void onRecorded(const quarry::HistoryEntry &entry);
    void onRemoved(qint64 id);
    void onCleared(const std::optional<QUuid> &connectionId, qint64 deleted);
    void onStoreFailed(const quarry::Error &error);
    void setLoading(bool loading);
    void setHasMore(bool hasMore);
    QString connectionName(const QUuid &id) const;
    bool matches(const quarry::HistoryEntry &entry) const;

    QString m_databasePath;
    ConnectionController *m_connections = nullptr; // cleared by its destroyed() signal
    quarry::QueryHistoryStore *m_store = nullptr;
    std::vector<quarry::HistoryEntry> m_rows;
    QString m_text;
    QString m_connectionFilter;
    bool m_onlyErrors = false;
    bool m_started = false;
    bool m_loading = false;
    bool m_hasMore = false;
    QTimer m_debounce;
    quint64 m_nextRequest = 0;
    quint64 m_latestRequest = 0; // the only answer that is taken
    bool m_latestAppends = false;
};

} // namespace quarry::ui
