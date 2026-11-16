#include "completion_controller.h"

#include "connection_controller.h"
#include "editor_tabs_model.h"

#include <quarry/completion.h>
#include <quarry/sql_context.h>

#include <QSettings>

#include <algorithm>

namespace quarry::ui {

namespace {

const QString kAutoOpenKey = QStringLiteral("ui/completion/autoOpen");

// The character that closes the identifier quote `open`, or a null character.
QChar closingOf(QChar open)
{
    if (open == QLatin1Char('"') || open == QLatin1Char('`'))
        return open;
    return open == QLatin1Char('[') ? QLatin1Char(']') : QChar();
}

QString schemaOf(const QStringList &parts)
{
    return parts.size() >= 2 ? parts.at(parts.size() - 2) : QString();
}

} // namespace

CompletionController::CompletionController(QObject *parent)
    : QObject(parent)
    , m_model(this)
    , m_objects(this)
{ }

bool CompletionController::autoOpen() const
{
    if (!m_autoOpen)
        m_autoOpen = QSettings().value(kAutoOpenKey, true).toBool(); // INI gives "true" strings
    return *m_autoOpen;
}

void CompletionController::setAutoOpen(bool on)
{
    if (autoOpen() == on)
        return;
    m_autoOpen = on;
    QSettings().setValue(kAutoOpenKey, on);
    emit autoOpenChanged();
}

void CompletionController::setTabs(EditorTabsModel *tabs)
{
    if (tabs == m_tabs)
        return;
    if (m_tabs)
        m_tabs->disconnect(this);
    m_tabs = tabs;
    if (m_tabs)
        connect(m_tabs, &QObject::destroyed, this, [this] { m_tabs = nullptr; });
    emit tabsChanged();
}

ConnectionController *CompletionController::connections() const
{
    return m_tabs ? m_tabs->connections() : nullptr;
}

SchemaTreeModel *CompletionController::tree() const
{
    ConnectionController *c = connections();
    return c ? c->tree() : nullptr;
}

// The tree belongs to the ConnectionController, which the tabs get after they are created: so the
// links are made the first time they are needed.
void CompletionController::bindTree()
{
    SchemaTreeModel *t = tree();
    if (t == m_tree)
        return;
    for (const QMetaObject::Connection &link : std::as_const(m_treeLinks))
        disconnect(link);
    m_treeLinks.clear();
    m_tree = t;
    if (!m_tree)
        return;
    m_treeLinks << connect(m_tree, &SchemaTreeModel::connectedChanged, this,
                           &CompletionController::onConnectedChanged)
                << connect(m_tree, &SchemaTreeModel::metadataInvalidated, this,
                           &CompletionController::invalidate)
                << connect(m_tree, &QObject::destroyed, this, [this] {
                       m_tree = nullptr;
                       m_treeLinks.clear();
                   });
}

CompletionController::Entry &CompletionController::entryFor(const QString &connectionId)
{
    auto it = m_caches.find(connectionId);
    if (it == m_caches.end()) {
        Entry e;
        e.cache = new MetadataCache(this);
        connect(e.cache, &MetadataCache::changed, this, &CompletionController::changed);
        connect(e.cache, &MetadataCache::failed, this, [this, connectionId] {
            if (const auto it = m_caches.find(connectionId); it != m_caches.end())
                it->columnsFailed = true; // retrying on every keystroke would only repeat the error
            emit changed();
        });
        it = m_caches.insert(connectionId, e);
    }
    return it.value();
}

quarry::MetadataCache *CompletionController::cache(const QString &connectionId) const
{
    const auto it = m_caches.constFind(connectionId);
    return it == m_caches.constEnd() ? nullptr : it->cache;
}

QString CompletionController::cacheState(const QString &connectionId) const
{
    const MetadataCache *c = cache(connectionId);
    if (!c)
        return QStringLiteral("none");
    switch (c->state()) {
    case MetadataCache::State::Empty:
        return QStringLiteral("empty");
    case MetadataCache::State::Loading:
        return QStringLiteral("loading");
    case MetadataCache::State::Ready:
        return QStringLiteral("ready");
    case MetadataCache::State::Failed:
        break;
    }
    return QStringLiteral("failed");
}

bool CompletionController::prepare(const QString &connectionId, SchemaTreeModel::Access *access)
{
    bindTree();
    SchemaTreeModel *t = tree();
    if (!t || connectionId.isEmpty()) {
        if (access)
            *access = SchemaTreeModel::Access::Unavailable;
        return false;
    }
    Entry &e = entryFor(connectionId);
    const SchemaTreeModel::Access a = t->ensureSession(connectionId);
    if (access)
        *access = a;
    e.wanted = a == SchemaTreeModel::Access::Opening;
    if (a != SchemaTreeModel::Access::Connected)
        return false;
    Session *s = t->metadataSession(connectionId);
    if (s != e.session) {
        e.session = s;
        e.cache->attach(s);
        e.columnsFailed = false;
    }
    // A failed walk is not retried by every keystroke: "Refresh metadata" starts it again.
    if (e.cache->state() == MetadataCache::State::Empty)
        e.cache->load();
    return true;
}

void CompletionController::onConnectedChanged()
{
    if (!m_tree)
        return;
    bool any = false;
    for (auto it = m_caches.begin(); it != m_caches.end(); ++it) {
        Entry &e = it.value();
        Session *s = m_tree->metadataSession(it.key());
        if (s == e.session)
            continue;
        e.session = s;
        e.cache->attach(s); // nullptr: drops the data of a connection that is gone
        e.columnsFailed = false;
        if (s && e.wanted)
            e.cache->load();
        any = true;
    }
    if (any)
        emit changed();
}

void CompletionController::invalidate(const QString &connectionId)
{
    const auto it = m_caches.find(connectionId);
    if (it != m_caches.end()) {
        it->cache->invalidate();
        it->columnsFailed = false;
    }
    emit changed();
}

const ISqlDialect *CompletionController::dialectOf(const QString &connectionId) const
{
    ConnectionController *c = connections();
    if (!c || connectionId.isEmpty())
        return nullptr;
    const auto profile = c->model()->profile(QUuid::fromString(connectionId));
    if (!profile)
        return nullptr;
    // shortcut: loads the engine's plugin on the UI thread the first time (a few ms), as the
    // editor tabs do for splitStatements.
    IDatabaseProvider *provider = c->registry().provider(profile->providerId);
    return provider ? &provider->dialect() : nullptr;
}

QVariantMap CompletionController::query(int row, int cursor)
{
    QVariantMap out { { QStringLiteral("count"), 0 },
                      { QStringLiteral("scope"), static_cast<int>(CompletionScope::None) },
                      { QStringLiteral("replaceStart"), cursor },
                      { QStringLiteral("replaceEnd"), cursor },
                      { QStringLiteral("notice"), QString() },
                      { QStringLiteral("loading"), false } };
    const auto none = [&] {
        m_model.setItems({});
        return out;
    };
    if (!m_tabs || m_tabs->isData(row))
        return none();
    const QString text = m_tabs->textOf(row);
    cursor = std::clamp(cursor, 0, static_cast<int>(text.size()));
    out.insert(QStringLiteral("replaceStart"), cursor);
    out.insert(QStringLiteral("replaceEnd"), cursor);

    const QString connectionId = m_tabs->connectionIdOf(row);
    const ISqlDialect *dialect = dialectOf(connectionId);
    if (!dialect) { // no connection, so no dialect: nothing to suggest
        out.insert(QStringLiteral("notice"), tr("Connect to suggest tables"));
        return none();
    }

    // The statement under the cursor. Past its end (the range stops before trailing blanks) the
    // cursor still belongs to it unless a ";" came between; otherwise, or before the first
    // statement, the cursor is in the gap between statements: what is analyzed is the gap up to
    // the cursor (blank, or a comment).
    const QVariantMap st = m_tabs->statementNow(row, cursor);
    int begin = st.value(QStringLiteral("begin")).toInt();
    int end = st.value(QStringLiteral("end")).toInt();
    const bool found = st.value(QStringLiteral("index")).toInt() >= 0;
    QString statement;
    if (found && cursor >= begin && cursor <= end) {
        statement = text.mid(begin, end - begin);
    } else if (found && cursor > end && end > 0 && text.at(end - 1) != QLatin1Char(';')
               && text.mid(end, cursor - end).trimmed().isEmpty()) {
        statement = text.mid(begin, cursor - begin);
    } else {
        begin = found && cursor > end ? end : 0;
        statement = text.mid(begin, cursor - begin);
    }

    const SqlContext ctx = analyzeSqlContext(statement, cursor - begin);
    out.insert(QStringLiteral("scope"), static_cast<int>(ctx.scope));
    if (ctx.scope == CompletionScope::None)
        return none();

    SchemaTreeModel::Access access = SchemaTreeModel::Access::Unavailable;
    const bool attached = prepare(connectionId, &access);
    Entry &e = entryFor(connectionId);
    if (attached) {
        std::vector<ObjectPath> missing;
        if (!e.columnsFailed)
            missing = CompletionEngine::missingColumns(ctx, *e.cache);
        for (const ObjectPath &table : missing)
            e.cache->ensureColumns(table);
        if (e.cache->state() == MetadataCache::State::Failed)
            out.insert(QStringLiteral("notice"), tr("Tables could not be loaded"));
        out.insert(QStringLiteral("loading"),
                   e.cache->state() == MetadataCache::State::Loading || !missing.empty());
    } else if (access == SchemaTreeModel::Access::Opening) {
        out.insert(QStringLiteral("loading"), true);
    } else {
        out.insert(QStringLiteral("notice"), tr("Connect to suggest tables"));
    }

    const std::vector<Suggestion> list
        = CompletionEngine::complete(ctx, *e.cache, *dialect, kLimit);
    m_model.setSuggestions(list);

    const int start = begin + static_cast<int>(ctx.replaceStart);
    int stop = cursor;
    // `"us|"`: the closing quote is already there, so the suggestion (which brings its own
    // quotes when it needs them) must replace it too.
    if (cursor < text.size() && ctx.replaceStart < statement.size()) {
        const QChar closing = closingOf(statement.at(ctx.replaceStart));
        if (!closing.isNull() && text.at(cursor) == closing)
            stop = cursor + 1;
    }
    out.insert(QStringLiteral("count"), static_cast<int>(list.size()));
    out.insert(QStringLiteral("replaceStart"), start);
    out.insert(QStringLiteral("replaceEnd"), stop);
    return out;
}

QVariantMap CompletionController::searchObjects(const QString &connectionId, const QString &text)
{
    QVariantMap out { { QStringLiteral("state"), QStringLiteral("none") },
                      { QStringLiteral("count"), 0 } };
    SchemaTreeModel::Access access = SchemaTreeModel::Access::Unavailable;
    const bool attached = prepare(connectionId, &access);
    m_objects.setItems({});
    if (!attached) {
        if (access == SchemaTreeModel::Access::Opening)
            out.insert(QStringLiteral("state"), QStringLiteral("loading"));
        return out;
    }
    const MetadataCache &c = *entryFor(connectionId).cache;
    switch (c.state()) {
    case MetadataCache::State::Failed:
        out.insert(QStringLiteral("state"), QStringLiteral("failed"));
        return out;
    case MetadataCache::State::Empty:
    case MetadataCache::State::Loading:
        out.insert(QStringLiteral("state"), QStringLiteral("loading"));
        return out;
    case MetadataCache::State::Ready:
        break;
    }
    out.insert(QStringLiteral("state"), QStringLiteral("ready"));

    // Names that start with the text first, then those that contain it; both in name order.
    const QString q = text.trimmed().toLower();
    const auto &relations = c.relations();
    const auto &keys = c.relationKeys();
    std::vector<CompletionModel::Item> items;
    for (const bool prefixPass : { true, false }) {
        for (const qsizetype i : c.relationOrder()) {
            if (static_cast<int>(items.size()) >= kLimit)
                break;
            const QString &key = keys[static_cast<size_t>(i)];
            const bool starts = key.startsWith(q);
            if (prefixPass ? !starts : (starts || !key.contains(q)))
                continue;
            const CachedObject &o = relations[static_cast<size_t>(i)];
            const bool view = o.path.kind != ObjectKind::Table;
            items.push_back(
                { Suggestion { o.name, o.name, view ? SuggestionKind::View : SuggestionKind::Table,
                               schemaOf(o.path.parts) },
                  o.path.parts, o.path.kind == ObjectKind::MaterializedView });
        }
    }
    out.insert(QStringLiteral("count"), static_cast<int>(items.size()));
    m_objects.setItems(std::move(items));
    return out;
}

} // namespace quarry::ui
