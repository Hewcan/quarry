#pragma once

#include "completion_model.h"
#include "editor_tabs_model.h"
#include "schema_tree_model.h"

#include <quarry/metadata_cache.h>

#include <QHash>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <optional>

namespace quarry::ui {

class ConnectionController;

// Autocompletion (ARCHITECTURE section 6.5): one MetadataCache per connection, attached to the
// metadata Session of the schema tree. It opens that session when the user asks for suggestions
// (the way expanding the connection does) but never asks for a password: without a session the
// suggestions are keywords and functions, and the popup says "Connect to suggest tables".
//
// query() fills `model` for the cursor of an editor tab; searchObjects() fills `objects` for the
// palette's "@" search. Whenever a cache changes (a load ended, columns arrived) changed() tells
// the popup to ask again.
class CompletionController : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(quarry::ui::EditorTabsModel *tabs READ tabs WRITE setTabs NOTIFY tabsChanged FINAL)
    Q_PROPERTY(quarry::ui::CompletionModel *model READ model CONSTANT FINAL)
    Q_PROPERTY(quarry::ui::CompletionModel *objects READ objects CONSTANT FINAL)
    // Open the popup by itself after "." and two identifier characters. Ctrl+Space always opens it.
    Q_PROPERTY(bool autoOpen READ autoOpen WRITE setAutoOpen NOTIFY autoOpenChanged FINAL)

public:
    static constexpr int kLimit = 200;

    explicit CompletionController(QObject *parent = nullptr);

    EditorTabsModel *tabs() const { return m_tabs; }
    void setTabs(EditorTabsModel *tabs);
    CompletionModel *model() { return &m_model; }
    CompletionModel *objects() { return &m_objects; }
    bool autoOpen() const;
    void setAutoOpen(bool on);

    // The suggestions at `cursor` (document offset) of the editor tab `row`, into `model`:
    // {count, scope (CompletionScope), replaceStart, replaceEnd (document offsets: what an
    // accepted suggestion replaces, the closing quote after the cursor included), notice ("" or
    // why there are no tables), loading (tables are still being read)}.
    Q_INVOKABLE QVariantMap query(int row, int cursor);
    // The tables and views of a connection whose name contains `text`, into `objects`:
    // {state ("none": no connection or no session without a prompt | "loading" | "ready" |
    // "failed"), count}.
    Q_INVOKABLE QVariantMap searchObjects(const QString &connectionId, const QString &text);
    // Drops what is cached about a connection; the next query reads it again.
    Q_INVOKABLE void invalidate(const QString &connectionId);
    // "none" (no cache yet) | "empty" | "loading" | "ready" | "failed". For tests and the status.
    Q_INVOKABLE QString cacheState(const QString &connectionId) const;
    // Test access.
    quarry::MetadataCache *cache(const QString &connectionId) const;

signals:
    void tabsChanged();
    void autoOpenChanged();
    // A cache changed: ask again.
    void changed();

private:
    struct Entry
    {
        quarry::MetadataCache *cache = nullptr; // child of this
        const quarry::Session *session = nullptr; // only compared, never dereferenced
        bool wanted = false; // load as soon as a session arrives
        bool columnsFailed
            = false; // a read failed: columns are not asked for again until invalidate()
    };

    ConnectionController *connections() const;
    SchemaTreeModel *tree() const;
    void bindTree();
    Entry &entryFor(const QString &connectionId);
    // Opens the session if it can be done without a prompt and attaches the cache; true when the
    // cache is attached and loading or loaded.
    bool prepare(const QString &connectionId, SchemaTreeModel::Access *access = nullptr);
    void onConnectedChanged();
    const quarry::ISqlDialect *dialectOf(const QString &connectionId) const;

    EditorTabsModel *m_tabs = nullptr; // cleared by its destroyed() signal
    SchemaTreeModel *m_tree = nullptr; // cleared by its destroyed() signal
    QList<QMetaObject::Connection> m_treeLinks;
    CompletionModel m_model;
    CompletionModel m_objects;
    QHash<QString, Entry> m_caches;
    mutable std::optional<bool> m_autoOpen;
};

} // namespace quarry::ui
