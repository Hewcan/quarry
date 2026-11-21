#include "schema_tree_model.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace quarry::ui {

namespace {

QString toId(const QUuid &id)
{
    return id.toString(QUuid::WithoutBraces);
}

const char *kindName(ObjectKind kind)
{
    switch (kind) {
    case ObjectKind::Database:
        return "database";
    case ObjectKind::Schema:
        return "schema";
    case ObjectKind::Table:
        return "table";
    case ObjectKind::View:
        return "view";
    case ObjectKind::MaterializedView:
        return "materializedView";
    case ObjectKind::Column:
        return "column";
    case ObjectKind::Index:
        return "index";
    case ObjectKind::ForeignKey:
        return "foreignKey";
    case ObjectKind::Function:
        return "function";
    case ObjectKind::Procedure:
        return "procedure";
    case ObjectKind::Sequence:
        return "sequence";
    case ObjectKind::Trigger:
        return "trigger";
    case ObjectKind::Folder:
        return "folder";
    case ObjectKind::User:
        return "user";
    }
    return "folder";
}

// ASCII is by far the common case and Qt compares it case-insensitively without allocating.
bool isAscii(QStringView s)
{
    return std::ranges::all_of(s, [](QChar c) { return c.unicode() < 0x80; });
}

// Lower case without accents, so "cafe" finds "Café". shortcut: only what Unicode decomposes
// (no "ß" -> "ss", no "ø" -> "o"); the search is a convenience, not a collation.
QString fold(const QString &s)
{
    QString d = s.normalized(QString::NormalizationForm_D);
    d.removeIf([](QChar c) { return c.category() == QChar::Mark_NonSpacing; });
    return d.toCaseFolded();
}

bool containsFolded(const QString &haystack, const QString &foldedNeedle, bool needleAscii)
{
    if (needleAscii && isAscii(haystack))
        return haystack.contains(foldedNeedle, Qt::CaseInsensitive);
    return fold(haystack).contains(foldedNeedle);
}

} // namespace

struct SchemaTreeModel::ConnData
{
    enum class Stage { None, ReadingPassword, AwaitingPassword, Preflight, Opening };

    QUuid id;
    Stage stage = Stage::None;
    quint64 token = 0; // identifies the current Session in m_requests and in its lambdas
    Session *session = nullptr; // child of the model
    bool connected = false;
    bool authFailed = false; // the last attempt was refused: the prompt says so
    int preflight = 0; // ConnectionPreflight request in flight, 0 = none
    bool quiet = false; // opened for autocompletion: never prompts (ensureSession)
    bool interactive = true; // the attempt in progress may prompt (quiet, consumed by connectRoot)
    std::vector<Node *>
        deferred; // nodes of this branch marked Loading whose request is not sent yet
    // SQLite's single database "main" is not shown: its parts, while the second answer (the
    // children of that database) is awaited and while it is loaded. Empty otherwise.
    QStringList flatParts;
    // The Tables folder (its path) that hosts the Migrations folder; empty until one is chosen.
    QStringList migrationHost;
};

struct SchemaTreeModel::Node
{
    enum class Type : uint8_t { Connection, Object, Loading, Error, Empty };

    Type type = Type::Object;
    LoadState state = Idle;
    bool hasChildren = false;
    bool hidden = false; // by the search; only meaningful while it is active
    bool matched = false; // by the search itself, not only because a descendant did
    Flavor flavor = Flavor::Plain; // folders only
    ObjectKind kind = ObjectKind::Table;
    int row = 0; // among the visible siblings while a search is active
    int sourceRow = 0; // connections: the row in ConnectionListModel (the search moves `row`)
    Node *parent = nullptr;
    QString name; // error rows: the message
    QString detail; // error rows: the technical text; folders: how many items
    MigrationState migState = MigrationState::Pending; // migration leaves
    qint64 batch = -1; // migration leaves: -1 = none
    ObjectPath path;
    std::vector<std::unique_ptr<Node>> children;
    std::vector<Node *> shown; // children that pass the search (only while it is active)
    std::unique_ptr<ConnData> conn; // connection nodes only
};

struct SchemaTreeModel::FilterCtx
{
    QString needle; // folded
    bool ascii = true;
    int scope = Everything;
    int count = 0;
    bool incomplete = false;
};

SchemaTreeModel::SchemaTreeModel(ConnectionListModel *connections,
                                 quarry::ProviderRegistry *registry, QObject *parent)
    : QAbstractItemModel(parent)
    , m_list(connections)
    , m_registry(registry)
{
    connect(m_list, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex &, int first, int last) { onSourceInserted(first, last); });
    connect(m_list, &QAbstractItemModel::rowsRemoved, this,
            [this](const QModelIndex &, int first, int last) { onSourceRemoved(first, last); });
    connect(m_list, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &first, const QModelIndex &last) {
                onSourceChanged(first.row(), last.row());
            });
    connect(m_list, &QAbstractItemModel::modelReset, this, [this] { onSourceReset(); });
    fillRoots();
    m_filterTimer = new QTimer(this);
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(100);
    connect(m_filterTimer, &QTimer::timeout, this, &SchemaTreeModel::retryFilter);
}

SchemaTreeModel::~SchemaTreeModel()
{
    // Close every session first so they wind down in parallel; their destructors then wait.
    const auto closeAll = [this](std::vector<std::unique_ptr<Node>> &nodes) {
        for (auto &r : nodes)
            if (r->conn->session != nullptr) {
                r->conn->session->disconnect(this);
                r->conn->session->close();
            }
    };
    closeAll(m_roots);
    closeAll(m_parked);
}

// ---- helpers -----------------------------------------------------------------------------------

SchemaTreeModel::Node *SchemaTreeModel::node(const QModelIndex &index) const
{
    return static_cast<Node *>(index.internalPointer());
}

SchemaTreeModel::Node *SchemaTreeModel::rootOf(Node *n) const
{
    while (n->parent != nullptr)
        n = n->parent;
    return n;
}

bool SchemaTreeModel::attached(const Node *root) const
{
    return std::ranges::any_of(m_roots, [&](const auto &r) { return r.get() == root; });
}

// Invalid for a node the search hides: its row is stale, and a signal must not point at it.
QModelIndex SchemaTreeModel::indexFor(const Node *n) const
{
    if (m_filter.active && !isShown(n))
        return {};
    return createIndex(n->row, 0, const_cast<Node *>(n));
}

void SchemaTreeModel::renumberRoots()
{
    for (size_t i = 0; i < m_roots.size(); ++i) {
        m_roots[i]->row = static_cast<int>(i);
        m_roots[i]->sourceRow = static_cast<int>(i);
    }
}

SchemaTreeModel::Node *SchemaTreeModel::rootById(const QUuid &id) const
{
    const auto it = std::ranges::find_if(m_roots, [&](const auto &r) { return r->conn->id == id; });
    return it == m_roots.end() ? nullptr : it->get();
}

SchemaTreeModel::Node *SchemaTreeModel::rootByToken(quint64 token) const
{
    const auto it
        = std::ranges::find_if(m_roots, [&](const auto &r) { return r->conn->token == token; });
    return it == m_roots.end() || token == 0 ? nullptr : it->get();
}

std::unique_ptr<SchemaTreeModel::Node> SchemaTreeModel::makeRoot(const QUuid &id) const
{
    auto r = std::make_unique<Node>();
    r->type = Node::Type::Connection;
    r->hasChildren = true;
    r->kind = ObjectKind::Database;
    r->path = ObjectPath { {}, ObjectKind::Database };
    r->conn = std::make_unique<ConnData>();
    r->conn->id = id;
    return r;
}

QStringList SchemaTreeModel::connectedIds() const
{
    QStringList out;
    for (const auto &r : m_roots)
        if (r->conn->connected)
            out.append(toId(r->conn->id));
    return out;
}

// ---- mirroring the saved connections -----------------------------------------------------------

void SchemaTreeModel::fillRoots()
{
    for (int r = 0; r < m_list->rowCount(); ++r)
        m_roots.push_back(makeRoot(QUuid::fromString(
            m_list->data(m_list->index(r), ConnectionListModel::IdRole).toString())));
    renumberRoots();
}

void SchemaTreeModel::onSourceInserted(int first, int last)
{
    std::vector<std::unique_ptr<Node>> fresh;
    for (int r = first; r <= last; ++r) {
        const QUuid id = QUuid::fromString(
            m_list->data(m_list->index(r), ConnectionListModel::IdRole).toString());
        const auto parked
            = std::ranges::find_if(m_parked, [&](const auto &p) { return p->conn->id == id; });
        if (parked != m_parked.end()) {
            if (!(*parked)->conn->deferred.empty())
                queueDrain();
            fresh.push_back(std::move(*parked));
            m_parked.erase(parked);
        } else {
            fresh.push_back(makeRoot(id));
        }
    }
    if (m_filter.active) { // the visible rows are not the source rows: show the tree again
        beginResetModel();
        m_roots.insert(m_roots.begin() + first, std::make_move_iterator(fresh.begin()),
                       std::make_move_iterator(fresh.end()));
        renumberRoots();
        applyFilter();
        endResetModel();
        emit connectedChanged();
        emit filterChanged();
        return;
    }
    beginInsertRows({}, first, last);
    m_roots.insert(m_roots.begin() + first, std::make_move_iterator(fresh.begin()),
                   std::make_move_iterator(fresh.end()));
    renumberRoots();
    endInsertRows();
    emit dataChanged(index(0, 0), index(rowCount() - 1, 0), { PositionRole, SetSizeRole });
    emit connectedChanged();
}

void SchemaTreeModel::onSourceRemoved(int first, int last)
{
    const bool filtering = m_filter.active;
    if (filtering)
        beginResetModel();
    else
        beginRemoveRows({}, first, last);
    for (int r = first; r <= last; ++r)
        m_parked.push_back(std::move(m_roots[static_cast<size_t>(r)]));
    m_roots.erase(m_roots.begin() + first, m_roots.begin() + last + 1);
    renumberRoots();
    if (filtering) {
        applyFilter();
        endResetModel();
        emit filterChanged();
    } else {
        endRemoveRows();
        if (!m_roots.empty())
            emit dataChanged(index(0, 0), index(rowCount() - 1, 0), { PositionRole, SetSizeRole });
    }
    emit connectedChanged();
    QTimer::singleShot(0, this, &SchemaTreeModel::flushParked);
}

void SchemaTreeModel::onSourceChanged(int first, int last)
{
    if (first < 0 || last >= static_cast<int>(m_roots.size()))
        return;
    if (m_filter.active) {
        // shortcut: a rename in place keeps its old match until the next search; opening a
        // connection touches its row too, and a reset then would collapse the view each time.
        for (Node *r : m_shownRoots)
            if (r->sourceRow >= first && r->sourceRow <= last)
                emit dataChanged(indexFor(r), indexFor(r));
        return;
    }
    emit dataChanged(index(first, 0), index(last, 0)); // all roles: the language may have changed
}

void SchemaTreeModel::onSourceReset()
{
    beginResetModel();
    m_requests.clear();
    for (auto *list : { &m_roots, &m_parked })
        for (auto &r : *list)
            disposeSession(*r->conn);
    m_roots.clear();
    m_parked.clear();
    m_shownRoots.clear();
    fillRoots();
    if (m_filter.active)
        applyFilter();
    endResetModel();
    emit connectedChanged();
    if (m_filter.active)
        emit filterChanged();
}

void SchemaTreeModel::flushParked()
{
    for (auto &r : m_parked)
        disposeSession(*r->conn);
    m_parked.clear();
}

// ---- model interface ---------------------------------------------------------------------------

QModelIndex SchemaTreeModel::index(int row, int column, const QModelIndex &parent) const
{
    if (column != 0 || row < 0)
        return {};
    const Node *p = parent.isValid() ? node(parent) : nullptr;
    const Node *n = row < visibleCount(p) ? visibleAt(p, row) : nullptr;
    return n != nullptr ? createIndex(row, 0, const_cast<Node *>(n)) : QModelIndex();
}

// Without a search every child is visible and the lists are the same.
int SchemaTreeModel::visibleCount(const Node *parent) const
{
    if (m_filter.active)
        return static_cast<int>(parent != nullptr ? parent->shown.size() : m_shownRoots.size());
    return static_cast<int>(parent != nullptr ? parent->children.size() : m_roots.size());
}

SchemaTreeModel::Node *SchemaTreeModel::visibleAt(const Node *parent, int row) const
{
    const auto r = static_cast<size_t>(row);
    if (m_filter.active)
        return parent != nullptr ? parent->shown[r] : m_shownRoots[r];
    return parent != nullptr ? parent->children[r].get() : m_roots[r].get();
}

bool SchemaTreeModel::isShown(const Node *n) const
{
    if (!m_filter.active)
        return true;
    for (; n != nullptr; n = n->parent)
        if (n->hidden)
            return false;
    return true;
}

QModelIndex SchemaTreeModel::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return {};
    const Node *p = node(child)->parent;
    return p == nullptr ? QModelIndex() : indexFor(p);
}

int SchemaTreeModel::rowCount(const QModelIndex &parent) const
{
    return visibleCount(parent.isValid() ? node(parent) : nullptr);
}

int SchemaTreeModel::columnCount(const QModelIndex &) const
{
    return 1;
}

bool SchemaTreeModel::hasChildren(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return visibleCount(nullptr) > 0;
    const Node *n = node(parent);
    if (n->type != Node::Type::Connection && n->type != Node::Type::Object)
        return false;
    if (m_filter.active) // an unloaded node stays expandable: expanding it is how to search more
        return !n->shown.empty() || (n->state == Idle && (n->hasChildren || n->conn != nullptr));
    return n->type == Node::Type::Connection || n->hasChildren || !n->children.empty();
}

bool SchemaTreeModel::canFetchMore(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return false;
    const Node *n = node(parent);
    return (n->type == Node::Type::Connection || n->type == Node::Type::Object) && n->hasChildren
        && n->state == Idle;
}

void SchemaTreeModel::fetchMore(const QModelIndex &parent)
{
    if (canFetchMore(parent))
        defer(node(parent));
}

void SchemaTreeModel::defer(Node *n)
{
    n->state
        = Loading; // silent: canFetchMore() is false from now on; the views hear of it in load()
    rootOf(n)->conn->deferred.push_back(n);
    queueDrain();
}

void SchemaTreeModel::queueDrain()
{
    if (!m_drainQueued) {
        m_drainQueued = true;
        QMetaObject::invokeMethod(this, &SchemaTreeModel::drainDeferred, Qt::QueuedConnection);
    }
}

void SchemaTreeModel::drainDeferred()
{
    m_drainQueued = false;
    std::vector<Node *> todo;
    for (auto &r : m_roots)
        todo.insert(todo.end(), std::make_move_iterator(r->conn->deferred.begin()),
                    std::make_move_iterator(r->conn->deferred.end())),
            r->conn->deferred.clear();
    for (Node *n : todo) {
        n->state = Idle; // load() sets it again, announcing it
        load(n);
    }
}

QVariant SchemaTreeModel::rootData(const Node *root, int role) const
{
    static const QHash<int, int> map {
        { NameRole, ConnectionListModel::NameRole },
        { ProviderNameRole, ConnectionListModel::ProviderNameRole },
        { SummaryRole, ConnectionListModel::SummaryRole },
        { ColorIndexRole, ConnectionListModel::ColorIndexRole },
        { ColorNameRole, ConnectionListModel::ColorNameRole },
        { ProductionRole, ConnectionListModel::ProductionRole },
    };
    return m_list->data(m_list->index(root->sourceRow), map.value(role));
}

QVariant SchemaTreeModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::DoNotUseParent))
        return {};
    const Node *n = node(index);
    const bool isRoot = n->type == Node::Type::Connection;
    switch (role) {
    case NameRole:
        return isRoot ? rootData(n, role) : QVariant(n->name);
    case KindRole:
        switch (n->type) {
        case Node::Type::Connection:
            return QStringLiteral("connection");
        case Node::Type::Loading:
            return QStringLiteral("loading");
        case Node::Type::Error:
            return QStringLiteral("error");
        case Node::Type::Empty:
            return QStringLiteral("empty");
        case Node::Type::Object:
            if (n->kind == ObjectKind::Folder) {
                switch (n->flavor) {
                case Flavor::Databases:
                    return QStringLiteral("databasesFolder");
                case Flavor::Users:
                    return QStringLiteral("usersFolder");
                case Flavor::Tables:
                    return QStringLiteral("tablesFolder");
                case Flavor::Views:
                    return QStringLiteral("viewsFolder");
                case Flavor::Indexes:
                    return QStringLiteral("indexesFolder");
                case Flavor::Other:
                    return QStringLiteral("otherFolder");
                case Flavor::Migrations:
                    return QStringLiteral("migrationsFolder");
                case Flavor::MigrationsPending:
                    return QStringLiteral("pendingFolder");
                case Flavor::MigrationsApplied:
                    return QStringLiteral("appliedFolder");
                case Flavor::MigrationsOrphans:
                    return QStringLiteral("orphansFolder");
                case Flavor::Migration:
                    return QStringLiteral("migration");
                case Flavor::Plain:
                    break;
                }
            }
            return QString::fromLatin1(kindName(n->kind));
        }
        return {};
    case DetailRole:
        return n->detail;
    case ConnectionIdRole: {
        const Node *r = n;
        while (r->parent != nullptr)
            r = r->parent;
        return toId(r->conn->id);
    }
    case ProviderNameRole:
    case SummaryRole:
    case ColorIndexRole:
    case ColorNameRole:
    case ProductionRole:
        return isRoot ? rootData(n, role) : QVariant();
    case ConnectedRole:
        return isRoot && n->conn->connected;
    case LoadStateRole:
        return static_cast<int>(n->state);
    case PositionRole:
        return n->row + 1;
    case SetSizeRole:
        return visibleCount(n->parent);
    case MigrationStateRole:
        if (n->flavor != Flavor::Migration || n->type != Node::Type::Object)
            return QString();
        return n->migState == MigrationState::Applied ? QStringLiteral("applied")
            : n->migState == MigrationState::Orphan   ? QStringLiteral("orphan")
                                                      : QStringLiteral("pending");
    case BatchRole:
        return n->flavor == Flavor::Migration && n->type == Node::Type::Object ? n->batch : -1;
    default:
        return {};
    }
}

QHash<int, QByteArray> SchemaTreeModel::roleNames() const
{
    return {
        { NameRole, "name" },
        { KindRole, "kind" },
        { DetailRole, "detail" },
        { ConnectionIdRole, "connectionId" },
        { ProviderNameRole, "providerName" },
        { SummaryRole, "summary" },
        { ColorIndexRole, "colorIndex" },
        { ColorNameRole, "colorName" },
        { ProductionRole, "production" },
        { ConnectedRole, "connected" },
        { LoadStateRole, "loadState" },
        { PositionRole, "position" },
        { SetSizeRole, "setSize" },
        { MigrationStateRole, "migrationState" },
        { BatchRole, "batch" },
    };
}

// ---- lookups for the UI ------------------------------------------------------------------------

QModelIndex SchemaTreeModel::indexOfConnection(const QString &connectionId) const
{
    const Node *r = rootById(QUuid::fromString(connectionId));
    return r == nullptr ? QModelIndex() : indexFor(r);
}

QModelIndex SchemaTreeModel::connectionOf(const QModelIndex &index) const
{
    return index.isValid() && index.model() == this ? indexFor(rootOf(node(index))) : QModelIndex();
}

QString SchemaTreeModel::connectionIdOf(const QModelIndex &index) const
{
    return index.isValid() && index.model() == this ? toId(rootOf(node(index))->conn->id)
                                                    : QString();
}

QVariantMap SchemaTreeModel::objectOf(const QModelIndex &index) const
{
    QVariantMap out { { QStringLiteral("openable"), false } };
    if (!index.isValid() || index.model() != this)
        return out;
    const Node *n = node(index);
    if (n->type != Node::Type::Object
        || (n->kind != ObjectKind::Table && n->kind != ObjectKind::View
            && n->kind != ObjectKind::MaterializedView))
        return out;
    out.insert(QStringLiteral("openable"), true);
    out.insert(QStringLiteral("connectionId"), connectionIdOf(index));
    out.insert(QStringLiteral("parts"), n->path.parts);
    out.insert(QStringLiteral("kind"), QString::fromLatin1(kindName(n->kind)));
    out.insert(QStringLiteral("name"), n->path.parts.value(n->path.parts.size() - 1));
    return out;
}

QVariantMap SchemaTreeModel::migrationOf(const QModelIndex &index) const
{
    QVariantMap out { { QStringLiteral("valid"), false } };
    if (!index.isValid() || index.model() != this)
        return out;
    const Node *n = node(index);
    if (n->type != Node::Type::Object || n->flavor != Flavor::Migration)
        return out;
    out.insert(QStringLiteral("valid"), true);
    out.insert(QStringLiteral("connectionId"), connectionIdOf(index));
    out.insert(QStringLiteral("name"), n->name);
    out.insert(QStringLiteral("state"), data(index, MigrationStateRole));
    out.insert(QStringLiteral("batch"), n->batch);
    return out;
}

bool SchemaTreeModel::isConnected(const QString &connectionId) const
{
    const Node *r = rootById(QUuid::fromString(connectionId));
    return r != nullptr && r->conn->connected;
}

// ---- loading -----------------------------------------------------------------------------------

namespace {
template<typename T> std::vector<std::unique_ptr<T>> one(std::unique_ptr<T> n)
{
    std::vector<std::unique_ptr<T>> v;
    v.push_back(std::move(n));
    return v;
}
} // namespace

void SchemaTreeModel::setChildren(Node *n, std::vector<std::unique_ptr<Node>> kids)
{
    const bool live = attached(rootOf(n));
    if (live && m_filter.active) {
        // The search keeps its own lists of visible rows. "Loading…", errors and "(empty)" never
        // pass it (except inside a connection the search matched as a whole), so swapping them
        // needs no signal; anything else is shown again from scratch.
        const auto synthetic = [](const auto &nodes) {
            return std::ranges::all_of(nodes,
                                       [](const auto &k) { return k->type != Node::Type::Object; });
        };
        const bool quiet
            = m_filter.scope != Connections && synthetic(n->children) && synthetic(kids);
        if (!quiet)
            beginResetModel();
        n->children.clear();
        n->shown.clear();
        for (size_t i = 0; i < kids.size(); ++i) {
            kids[i]->parent = n;
            kids[i]->row = static_cast<int>(i);
            kids[i]->hidden = quiet;
        }
        n->children = std::move(kids);
        if (!quiet) {
            applyFilter();
            endResetModel();
            emit filterChanged();
        }
        return;
    }
    if (!n->children.empty()) {
        if (live)
            beginRemoveRows(indexFor(n), 0, static_cast<int>(n->children.size()) - 1);
        n->children.clear();
        if (live)
            endRemoveRows();
    }
    if (kids.empty())
        return;
    for (size_t i = 0; i < kids.size(); ++i) {
        kids[i]->parent = n;
        kids[i]->row = static_cast<int>(i);
    }
    if (live)
        beginInsertRows(indexFor(n), 0, static_cast<int>(kids.size()) - 1);
    n->children = std::move(kids);
    if (live)
        endInsertRows();
}

void SchemaTreeModel::setState(Node *n, LoadState state)
{
    if (n->state == state)
        return;
    n->state = state;
    if (attached(rootOf(n)))
        if (const QModelIndex i = indexFor(n); i.isValid())
            emit dataChanged(i, i, { LoadStateRole });
}

void SchemaTreeModel::setConnected(Node *root, bool connected)
{
    if (root->conn->connected == connected)
        return;
    root->conn->connected = connected;
    if (attached(root))
        if (const QModelIndex i = indexFor(root); i.isValid())
            emit dataChanged(i, i, { ConnectedRole });
    emit connectedChanged();
}

void SchemaTreeModel::load(Node *n)
{
    if (n->type == Node::Type::Connection) {
        connectRoot(n);
        return;
    }
    ConnData &c = *rootOf(n)->conn;
    if (c.session == nullptr) {
        failNode(n, Error { ErrorKind::Connection, tr("The connection is closed."), {}, {}, {} });
        return;
    }
    setState(n, Loading);
    auto row = std::make_unique<Node>();
    row->type = Node::Type::Loading;
    row->name = tr("Loading…");
    setChildren(n, one(std::move(row)));
    m_requests.insert({ c.token, c.session->children(n->path) }, n);
    emit announcement(tr("Loading…"), false);
}

void SchemaTreeModel::connectRoot(Node *root)
{
    ConnData &c = *root->conn;
    const auto profile = m_list->profile(c.id);
    if (!profile) {
        failNode(
            root,
            Error { ErrorKind::NotFound, tr("This connection no longer exists."), {}, {}, {} });
        return;
    }
    setState(root, Loading);
    auto row = std::make_unique<Node>();
    row->type = Node::Type::Loading;
    row->name = tr("Loading…");
    setChildren(root, one(std::move(row)));
    emit announcement(tr("Loading…"), false);
    c.interactive = !std::exchange(c.quiet, false);

    switch (profile->passwordMode) {
    case PasswordMode::None:
        proceedConnect(root, {});
        break;
    case PasswordMode::Saved:
        if (!m_credentials.read) {
            failNode(root,
                     Error { ErrorKind::Unsupported,
                             tr("The system keychain is not available."),
                             {},
                             {},
                             {} });
            break;
        }
        c.stage = ConnData::Stage::ReadingPassword;
        m_credentials.read(c.id);
        break;
    case PasswordMode::AskEveryTime:
        if (const auto pw = m_credentials.session ? m_credentials.session(c.id) : std::nullopt) {
            proceedConnect(root, *pw);
        } else {
            c.stage = ConnData::Stage::AwaitingPassword;
            emit passwordRequested(toId(c.id), profile->name, c.authFailed);
        }
        break;
    }
}

void SchemaTreeModel::setPreflight(ConnectionPreflight *preflight)
{
    m_preflight = preflight;
    if (preflight == nullptr)
        return;
    connect(preflight, &ConnectionPreflight::ready, this, &SchemaTreeModel::onPreflightReady);
    connect(preflight, &ConnectionPreflight::failed, this, &SchemaTreeModel::onPreflightFailed);
    connect(preflight, &ConnectionPreflight::cancelled, this,
            &SchemaTreeModel::onPreflightCancelled);
}

SchemaTreeModel::Node *SchemaTreeModel::rootByPreflight(int request) const
{
    const auto it = std::ranges::find_if(
        m_roots, [&](const auto &r) { return r->conn->preflight == request; });
    return it == m_roots.end() || request == 0 ? nullptr : it->get();
}

void SchemaTreeModel::proceedConnect(Node *root, const QString &password)
{
    ConnData &c = *root->conn;
    c.stage = ConnData::Stage::Opening;
    const auto profile = m_list->profile(c.id);
    if (!profile) {
        c.stage = ConnData::Stage::None;
        failNode(
            root,
            Error { ErrorKind::NotFound, tr("This connection no longer exists."), {}, {}, {} });
        return;
    }
    if (m_preflight != nullptr && ConnectionPreflight::usesSsh(*profile)) {
        c.stage = ConnData::Stage::Preflight;
        c.preflight = m_preflight->begin(*profile, password, c.interactive);
        return;
    }
    connectWith(root, *profile, Secrets { password });
}

void SchemaTreeModel::onPreflightReady(int request, const Secrets &secrets)
{
    Node *root = rootByPreflight(request);
    if (root == nullptr)
        return;
    ConnData &c = *root->conn;
    c.preflight = 0;
    c.stage = ConnData::Stage::Opening;
    const auto profile = m_list->profile(c.id);
    if (!profile) {
        c.stage = ConnData::Stage::None;
        failNode(
            root,
            Error { ErrorKind::NotFound, tr("This connection no longer exists."), {}, {}, {} });
        return;
    }
    connectWith(root, *profile, secrets);
}

void SchemaTreeModel::onPreflightFailed(int request, const Error &error)
{
    Node *root = rootByPreflight(request);
    if (root == nullptr)
        return;
    root->conn->preflight = 0;
    root->conn->stage = ConnData::Stage::None;
    failNode(root, error);
}

// The user dismissed the SSH secret or the host key prompt: the branch stays closed, as with the
// password prompt.
void SchemaTreeModel::onPreflightCancelled(int request)
{
    Node *root = rootByPreflight(request);
    if (root == nullptr)
        return;
    root->conn->preflight = 0;
    root->conn->stage = ConnData::Stage::None;
    emit collapseRequested(indexFor(root));
    setChildren(root, {});
    setState(root, Idle);
}

void SchemaTreeModel::connectWith(Node *root, const ConnectionProfile &profile,
                                  const Secrets &secrets)
{
    ConnData &c = *root->conn;
    // shortcut: loads the provider plugin on the UI thread (a few ms, once per engine), like
    // ConnectionTester; move it to the session thread when plugins grow heavy.
    auto connection = m_registry->createConnection(profile, secrets);
    if (!connection) {
        c.stage = ConnData::Stage::None;
        failNode(root, connection.error());
        return;
    }
    openSession(root, std::move(connection).value());
}

void SchemaTreeModel::openSession(Node *root, std::unique_ptr<IConnection> connection)
{
    ConnData &c = *root->conn;
    c.token = ++m_nextToken;
    c.flatParts.clear();
    c.session = new Session(std::move(connection), this);
    const quint64 t = c.token;
    Session *s = c.session;
    connect(s, &Session::opened, this, [this, t] { onSessionOpened(t); });
    connect(s, &Session::failed, this, [this, t](const Error &e) { onSessionFailed(t, e); });
    connect(s, &Session::childrenReady, this,
            [this, t](quint64 id, const ObjectPath &, std::vector<SchemaNode> nodes) {
                onChildrenReady(t, id, std::move(nodes));
            });
    connect(
        s, &Session::childrenFailed, this,
        [this, t](quint64 id, const ObjectPath &, const Error &e) { onChildrenFailed(t, id, e); });
    s->open();
    m_requests.insert({ t, s->children(root->path) }, root);
}

void SchemaTreeModel::onSessionOpened(quint64 token)
{
    Node *root = rootByToken(token);
    if (root == nullptr)
        return;
    setConnected(root, true);
    root->conn->authFailed = false;
    if (m_preflight != nullptr)
        m_preflight->openSucceeded(root->conn->id);
    m_list->store()->touch(root->conn->id);
}

void SchemaTreeModel::onSessionFailed(quint64 token, const Error &error)
{
    Node *root = rootByToken(token);
    if (root == nullptr || root->conn->stage != ConnData::Stage::Opening || root->conn->connected)
        return;
    // Open failed: the request queued behind it will fail with a generic message; the real one is
    // here.
    ConnData &c = *root->conn;
    Error shown = error;
    bool sshAuth = false;
    if (const auto profile = m_list->profile(c.id); profile && m_preflight != nullptr) {
        sshAuth = ConnectionPreflight::isSshAuthError(*profile, error);
        shown = m_preflight->openFailed(*profile, error);
    }
    if (error.kind == ErrorKind::Authentication && !sshAuth) {
        c.authFailed = true;
        if (m_credentials.forget) // a wrong password must not be tried again silently
            m_credentials.forget(c.id);
    }
    disposeSession(c);
    c.stage = ConnData::Stage::None;
    failNode(root, shown);
}

void SchemaTreeModel::onChildrenReady(quint64 token, quint64 requestId,
                                      std::vector<SchemaNode> nodes)
{
    Node *n = m_requests.take({ token, requestId });
    if (n == nullptr)
        return; // stale: the session was closed or the connection removed
    Node *root = rootOf(n);
    ConnData &conn = *root->conn;
    if (n == root && conn.flatParts.isEmpty()) {
        conn.stage = ConnData::Stage::None;
        setConnected(root, true); // the answer proves the session is open
        // shortcut: SQLite's single database "main" is not shown. The connection row already
        // names the file, so its children (Tables, Views, Indexes) are listed directly under it;
        // the paths keep "main", only the level is left out. Another engine, or SQLite with
        // attached databases, lists its databases as always. Revisit if another engine has one
        // default.
        if (nodes.size() == 1 && nodes.front().path.kind == ObjectKind::Database
            && nodes.front().path.parts == QStringList { QStringLiteral("main") }) {
            conn.flatParts = nodes.front().path.parts;
            m_requests.insert({ conn.token, conn.session->children(nodes.front().path) }, root);
            return; // the "Loading…" row stays until the second answer
        }
    }
    const size_t count = nodes.size();
    std::vector<std::unique_ptr<Node>> kids;
    // The relations of a database without schemas or of a schema, and the second answer of the
    // flattened SQLite root, are regrouped into Tables, Views, Indexes...
    const bool relations = n != root && n->type == Node::Type::Object
        && (n->kind == ObjectKind::Database || n->kind == ObjectKind::Schema);
    if (n == root && !conn.flatParts.isEmpty())
        kids = relationFolders(root, conn.flatParts, std::move(nodes));
    else if (n == root)
        kids = connectionFolders(root, std::move(nodes));
    else if (relations)
        kids = relationFolders(root, n->path.parts, std::move(nodes));
    else {
        kids.reserve(std::max<size_t>(nodes.size(), 1));
        // A folder's own parent (Database, Schema or the connection) names what it holds.
        const bool listing = n->kind == ObjectKind::Folder && n->type == Node::Type::Object
            && n->flavor != Flavor::Plain;
        for (auto &sn : nodes)
            kids.push_back(makeObject(std::move(sn)));
        if (kids.empty()) {
            auto empty = std::make_unique<Node>();
            empty->type = Node::Type::Empty;
            empty->name = tr("(empty)");
            kids.push_back(std::move(empty));
        }
        if (listing)
            n->detail = QString::number(count);
        if (listing && n->flavor == Flavor::Tables)
            addMigrations(root, n->path.parts, kids); // after the count: it is not a table
    }
    setChildren(n, std::move(kids));
    setState(n, Loaded);
    if (n->type == Node::Type::Object && n->flavor != Flavor::Plain && !n->detail.isEmpty())
        if (const QModelIndex i = indexFor(n); i.isValid())
            emit dataChanged(i, i, { DetailRole });
    if (n == root)
        emit announcement(tr("Connected to %1").arg(rootData(root, NameRole).toString()), false);
    if (m_reveal && conn.id == m_reveal->id)
        advanceReveal();
}

std::unique_ptr<SchemaTreeModel::Node> SchemaTreeModel::makeObject(SchemaNode &&sn)
{
    auto k = std::make_unique<Node>();
    k->kind = sn.path.kind;
    k->hasChildren = sn.hasChildren;
    k->name = std::move(sn.name);
    k->detail = std::move(sn.detail);
    k->path = std::move(sn.path);
    return k;
}

std::unique_ptr<SchemaTreeModel::Node>
SchemaTreeModel::makeFolder(Flavor flavor, const QString &name, QStringList parts) const
{
    auto f = std::make_unique<Node>();
    f->kind = ObjectKind::Folder;
    f->flavor = flavor;
    f->hasChildren = true;
    f->name = name;
    f->path = ObjectPath { std::move(parts), ObjectKind::Folder };
    return f;
}

// A folder the app fills itself: loaded already, with the count as its secondary text.
void SchemaTreeModel::fillFolder(Node *folder, std::vector<std::unique_ptr<Node>> kids)
{
    folder->detail = QString::number(kids.size());
    if (kids.empty()) {
        auto empty = std::make_unique<Node>();
        empty->type = Node::Type::Empty;
        empty->name = tr("(empty)");
        kids.push_back(std::move(empty));
    }
    for (size_t i = 0; i < kids.size(); ++i) {
        kids[i]->parent = folder;
        kids[i]->row = static_cast<int>(i);
    }
    folder->children = std::move(kids);
    folder->state = Loaded;
}

bool SchemaTreeModel::supportsUsers(const Node *root) const
{
    const auto profile = m_list->profile(root->conn->id);
    if (!profile)
        return false;
    // The plugin is loaded already: the connection was created from it.
    const IDatabaseProvider *provider = m_registry->provider(profile->providerId, nullptr);
    return provider != nullptr && provider->info().caps.testFlag(Capability::Users);
}

// A connection's own children: Databases (what the provider listed) and Users.
std::vector<std::unique_ptr<SchemaTreeModel::Node>>
SchemaTreeModel::connectionFolders(Node *root, std::vector<SchemaNode> &&nodes)
{
    std::vector<std::unique_ptr<Node>> out;
    auto databases
        = makeFolder(Flavor::Databases, tr("Databases"), { QStringLiteral("Databases") });
    std::vector<std::unique_ptr<Node>> kids;
    kids.reserve(nodes.size());
    for (auto &sn : nodes)
        kids.push_back(makeObject(std::move(sn)));
    fillFolder(databases.get(), std::move(kids));
    out.push_back(std::move(databases));
    if (supportsUsers(root)) // loaded when expanded, like any provider folder
        out.push_back(makeFolder(Flavor::Users, tr("Users"), { QStringLiteral("Users") }));
    return out;
}

// What a database (no schemas) or a schema holds. `baseParts` is its path: the Indexes request is
// {baseParts + "Indexes", Folder}. A provider that already returns folders called Tables, Views or
// Indexes (SQLite) keeps them; relations that arrive mixed are grouped here in memory.
std::vector<std::unique_ptr<SchemaTreeModel::Node>>
SchemaTreeModel::relationFolders(Node *root, const QStringList &baseParts,
                                 std::vector<SchemaNode> &&nodes)
{
    const bool hasSchemas = std::ranges::any_of(
        nodes, [](const SchemaNode &sn) { return sn.path.kind == ObjectKind::Schema; });
    std::vector<std::unique_ptr<Node>> out;
    if (hasSchemas) { // a database of schemas: each schema has its own Tables, Views, Indexes
        out.reserve(nodes.size());
        for (auto &sn : nodes)
            out.push_back(makeObject(std::move(sn)));
        if (out.empty()) {
            auto empty = std::make_unique<Node>();
            empty->type = Node::Type::Empty;
            empty->name = tr("(empty)");
            out.push_back(std::move(empty));
        }
        return out;
    }

    std::vector<std::unique_ptr<Node>> tables;
    std::vector<std::unique_ptr<Node>> views;
    std::vector<std::unique_ptr<Node>> other;
    std::unique_ptr<Node> tablesFolder;
    std::unique_ptr<Node> viewsFolder;
    std::unique_ptr<Node> indexesFolder;
    std::vector<std::unique_ptr<Node>> otherFolders;
    for (auto &sn : nodes) {
        switch (sn.path.kind) {
        case ObjectKind::Table:
            tables.push_back(makeObject(std::move(sn)));
            break;
        case ObjectKind::View:
        case ObjectKind::MaterializedView:
            views.push_back(makeObject(std::move(sn)));
            break;
        case ObjectKind::Folder: {
            const QString key = sn.path.parts.value(sn.path.parts.size() - 1);
            auto f = makeObject(std::move(sn));
            if (key == QLatin1String("Tables") && !tablesFolder) {
                f->flavor = Flavor::Tables;
                tablesFolder = std::move(f);
            } else if (key == QLatin1String("Views") && !viewsFolder) {
                f->flavor = Flavor::Views;
                viewsFolder = std::move(f);
            } else if (key == QLatin1String("Indexes") && !indexesFolder) {
                f->flavor = Flavor::Indexes;
                indexesFolder = std::move(f);
            } else {
                otherFolders.push_back(std::move(f));
            }
            break;
        }
        default:
            other.push_back(makeObject(std::move(sn)));
            break;
        }
    }
    if (!tablesFolder) {
        tablesFolder = makeFolder(Flavor::Tables, tr("Tables"),
                                  baseParts + QStringList { QStringLiteral("Tables") });
        fillFolder(tablesFolder.get(), std::move(tables));
        // The project's migrations hang here, as one more virtual child of Tables.
        std::vector<std::unique_ptr<Node>> kids = std::move(tablesFolder->children);
        addMigrations(root, tablesFolder->path.parts, kids);
        for (size_t i = 0; i < kids.size(); ++i) {
            kids[i]->parent = tablesFolder.get();
            kids[i]->row = static_cast<int>(i);
        }
        tablesFolder->children = std::move(kids);
    }
    if (!viewsFolder) {
        viewsFolder = makeFolder(Flavor::Views, tr("Views"),
                                 baseParts + QStringList { QStringLiteral("Views") });
        fillFolder(viewsFolder.get(), std::move(views));
    }
    if (!indexesFolder) // real, so the provider lists them when it is expanded
        indexesFolder = makeFolder(Flavor::Indexes, tr("Indexes"),
                                   baseParts + QStringList { QStringLiteral("Indexes") });
    out.push_back(std::move(tablesFolder));
    out.push_back(std::move(viewsFolder));
    out.push_back(std::move(indexesFolder));
    if (!other.empty()) {
        auto folder = makeFolder(Flavor::Other, tr("Other"),
                                 baseParts + QStringList { QStringLiteral("Other") });
        fillFolder(folder.get(), std::move(other));
        out.push_back(std::move(folder));
    }
    for (auto &f : otherFolders)
        out.push_back(std::move(f));
    return out;
}

void SchemaTreeModel::onChildrenFailed(quint64 token, quint64 requestId, const Error &error)
{
    Node *n = m_requests.take({ token, requestId });
    if (n == nullptr)
        return;
    failNode(n, error);
}

void SchemaTreeModel::failNode(Node *n, const Error &error)
{
    auto row = std::make_unique<Node>();
    row->type = Node::Type::Error;
    row->name = error.message.isEmpty() ? tr("Something went wrong.") : error.message;
    row->detail = error.detail;
    const QString message = row->name;
    Node *root = rootOf(n);
    if (m_reveal && root->conn->id == m_reveal->id)
        m_reveal.reset();
    if (n == root) {
        root->conn->stage = ConnData::Stage::None;
        root->conn->migrationHost.clear();
        if (root->conn->session != nullptr && error.kind != ErrorKind::Cancelled) {
            disposeSession(*root->conn);
            setConnected(root, false);
        }
    }
    setChildren(n, one(std::move(row)));
    setState(n, Failed);
    emit announcement(message, true);
}

void SchemaTreeModel::retry(const QModelIndex &index)
{
    if (!index.isValid())
        return;
    Node *n = node(index);
    if (n->type == Node::Type::Error && n->parent != nullptr)
        n = n->parent;
    if (n->state != Failed)
        return;
    emit metadataInvalidated(toId(rootOf(n)->conn->id));
    defer(n);
}

// ---- disconnecting -----------------------------------------------------------------------------

void SchemaTreeModel::dropRequests(quint64 token)
{
    for (auto it = m_requests.begin(); it != m_requests.end();)
        it = it.key().first == token ? m_requests.erase(it) : std::next(it);
}

void SchemaTreeModel::disposeSession(ConnData &c)
{
    c.deferred.clear();
    if (c.preflight != 0 && m_preflight != nullptr)
        m_preflight->cancel(c.preflight);
    c.preflight = 0;
    if (c.session == nullptr)
        return;
    dropRequests(c.token);
    Session *s = c.session;
    c.session = nullptr;
    c.token = 0;
    s->disconnect(this);
    connect(s, &Session::closed, s, &QObject::deleteLater);
    s->close();
}

void SchemaTreeModel::disconnectConnection(const QString &connectionId)
{
    Node *root = rootById(QUuid::fromString(connectionId));
    if (root == nullptr)
        return;
    ConnData &c = *root->conn;
    if (c.session == nullptr && c.stage == ConnData::Stage::None && root->state == Idle)
        return;
    emit collapseRequested(indexFor(root)); // the view first, so it never shows a stale branch
    disposeSession(c);
    c.stage = ConnData::Stage::None;
    setChildren(root, {});
    c.migrationHost.clear();
    setState(root, Idle);
    setConnected(root, false);
    emit announcement(tr("Disconnected from %1").arg(rootData(root, NameRole).toString()), false);
}

// ---- autocompletion and reveal -----------------------------------------------------------------

Session *SchemaTreeModel::metadataSession(const QString &connectionId) const
{
    const Node *r = rootById(QUuid::fromString(connectionId));
    return r != nullptr && r->conn->connected ? r->conn->session : nullptr;
}

SchemaTreeModel::Access SchemaTreeModel::ensureSession(const QString &connectionId)
{
    Node *root = rootById(QUuid::fromString(connectionId));
    if (root == nullptr)
        return Access::Unavailable;
    ConnData &c = *root->conn;
    if (c.connected && c.session != nullptr)
        return Access::Connected;
    if (c.session != nullptr || c.stage != ConnData::Stage::None || root->state == Loading)
        return Access::Opening;
    if (root->state != Idle)
        return Access::Unavailable; // Failed: retrying is the user's call, not every keystroke's
    const auto profile = m_list->profile(c.id);
    if (!profile)
        return Access::Unavailable;
    if (profile->passwordMode == PasswordMode::AskEveryTime
        && !(m_credentials.session && m_credentials.session(c.id)))
        return Access::NeedsPassword;
    c.quiet = true; // autocompletion never opens a prompt: the SSH steps fail instead
    fetchMore(indexFor(root));
    return Access::Opening;
}

void SchemaTreeModel::reveal(const QString &connectionId, const QStringList &parts)
{
    const Node *root = rootById(QUuid::fromString(connectionId));
    if (root == nullptr || !root->conn->connected || parts.isEmpty()) {
        emit announcement(tr("Connect to find %1 in the tree").arg(parts.value(parts.size() - 1)),
                          true);
        return;
    }
    if (m_filter.active || m_filter.pending) { // the node may be hidden by the search
        clearFilter();
        emit filterCleared();
    }
    m_reveal = Reveal { root->conn->id, parts };
    advanceReveal();
}

namespace {
bool isRelation(ObjectKind kind)
{
    return kind == ObjectKind::Table || kind == ObjectKind::View
        || kind == ObjectKind::MaterializedView;
}
} // namespace

// The relation, or null; `toLoad` gets the first branch on the way whose children are not known.
SchemaTreeModel::Node *SchemaTreeModel::findForReveal(Node *n, const QStringList &target,
                                                      Node *&toLoad) const
{
    if ((n->state == Idle && n->hasChildren) || n->state == Loading) {
        toLoad = toLoad != nullptr ? toLoad : n;
        return nullptr;
    }
    for (const auto &child : n->children) {
        if (child->type != Node::Type::Object)
            continue;
        if (isRelation(child->kind) && child->path.parts == target)
            return child.get();
        // Databases and schemas are on the way only when they start the path; the folders of
        // databases, tables and views hold what is looked for, so they are all tried. Users,
        // Indexes and Other never do: they are not opened for this.
        const bool container
            = (child->kind == ObjectKind::Folder
               && (child->flavor == Flavor::Databases || child->flavor == Flavor::Tables
                   || child->flavor == Flavor::Views))
            || ((child->kind == ObjectKind::Database || child->kind == ObjectKind::Schema)
                && target.mid(0, child->path.parts.size()) == child->path.parts);
        if (container)
            if (Node *found = findForReveal(child.get(), target, toLoad))
                return found;
    }
    return nullptr;
}

void SchemaTreeModel::advanceReveal()
{
    if (!m_reveal)
        return;
    const Reveal reveal = *m_reveal;
    Node *root = rootById(reveal.id);
    if (root == nullptr || !attached(root)) {
        m_reveal.reset();
        return;
    }
    Node *toLoad = nullptr;
    if (Node *found = findForReveal(root, reveal.parts, toLoad)) {
        QVariantList ancestors;
        for (Node *p = found->parent; p != nullptr; p = p->parent)
            ancestors.prepend(QVariant::fromValue(indexFor(p)));
        m_reveal.reset();
        emit revealed(indexFor(found), ancestors);
        return;
    }
    if (toLoad == nullptr) {
        const QString name = reveal.parts.value(reveal.parts.size() - 1);
        m_reveal.reset();
        emit announcement(tr("%1 was not found in the tree").arg(name), true);
        return;
    }
    if (toLoad->state == Idle)
        defer(toLoad); // its answer calls advanceReveal() again
}

// ---- migrations (H12 phase B) ------------------------------------------------------------------

// A line of text where children would be ("Not read yet", or "(empty)" with a better word).
std::unique_ptr<SchemaTreeModel::Node> SchemaTreeModel::makeNote(const QString &text)
{
    auto n = std::make_unique<Node>();
    n->type = Node::Type::Empty;
    n->name = text;
    return n;
}

// Whether the Tables folder with this path is where the Migrations folder hangs: the one of the
// connection's configured database, else the first that is loaded; once chosen, only that one.
bool SchemaTreeModel::hostsMigrations(Node *root, const QStringList &tablesParts)
{
    if (!m_feeds.contains(root->conn->id))
        return false;
    ConnData &c = *root->conn;
    if (!c.migrationHost.isEmpty())
        return c.migrationHost == tablesParts;
    const auto profile = m_list->profile(c.id);
    const QString database = profile
        ? profile->values.value(QStringLiteral("database")).toString().trimmed()
        : QString();
    if (!database.isEmpty() && tablesParts.value(0) != database)
        return false;
    c.migrationHost = tablesParts;
    return true;
}

namespace {
MigrationState stateOfGroup(int group, int pending, int applied)
{
    return group == pending ? MigrationState::Pending
        : group == applied  ? MigrationState::Applied
                            : MigrationState::Orphan;
}
} // namespace

std::vector<std::unique_ptr<SchemaTreeModel::Node>>
SchemaTreeModel::groupChildren(Flavor group, const QStringList &groupParts,
                               const MigrationFeed &feed) const
{
    const MigrationState wanted
        = stateOfGroup(static_cast<int>(group), static_cast<int>(Flavor::MigrationsPending),
                       static_cast<int>(Flavor::MigrationsApplied));
    std::vector<std::unique_ptr<Node>> out;
    for (const MigrationEntry &e : feed.report.entries) {
        if (e.state != wanted)
            continue;
        auto leaf = std::make_unique<Node>();
        leaf->kind = ObjectKind::Folder;
        leaf->flavor = Flavor::Migration;
        leaf->name = e.name;
        leaf->migState = e.state;
        leaf->batch = e.batch.value_or(-1);
        if (e.batch)
            leaf->detail = tr("batch %1").arg(*e.batch);
        leaf->path = ObjectPath { groupParts + QStringList { e.name }, ObjectKind::Folder };
        out.push_back(std::move(leaf));
    }
    if (out.empty()) {
        out.push_back(makeNote(group == Flavor::MigrationsPending ? tr("Nothing pending")
                                   : feed.report.tableMissing
                                   ? tr("Migrations table not found: everything is pending")
                                   : tr("Nothing applied yet")));
    }
    return out;
}

std::vector<std::unique_ptr<SchemaTreeModel::Node>>
SchemaTreeModel::migrationChildren(const Node *folder, const MigrationFeed &feed) const
{
    std::vector<std::unique_ptr<Node>> out;
    switch (feed.state) {
    case MigrationFeed::State::Ready:
        break;
    case MigrationFeed::State::Reading:
        out.push_back(makeNote(tr("Reading…")));
        return out;
    case MigrationFeed::State::Failed:
        out.push_back(makeNote(feed.message.isEmpty() ? tr("The migrations could not be read.")
                                                      : feed.message));
        return out;
    default: // NotRead (Unconfigured never gets here)
        out.push_back(makeNote(tr("Not read yet: connect to compare with the migrations table")));
        return out;
    }
    struct Group
    {
        Flavor flavor;
        QString name;
        QString key;
        MigrationState state;
    };
    const Group groups[] = {
        { Flavor::MigrationsPending, tr("Pending"), QStringLiteral("Pending"),
          MigrationState::Pending },
        { Flavor::MigrationsApplied, tr("Applied"), QStringLiteral("Applied"),
          MigrationState::Applied },
        { Flavor::MigrationsOrphans, tr("Orphans"), QStringLiteral("Orphans"),
          MigrationState::Orphan },
    };
    for (const Group &g : groups) {
        if (g.state == MigrationState::Orphan && feed.report.orphans == 0)
            continue;
        auto f = makeFolder(g.flavor, g.name, folder->path.parts + QStringList { g.key });
        std::vector<std::unique_ptr<Node>> kids = groupChildren(g.flavor, f->path.parts, feed);
        f->detail = QString::number(std::ranges::count_if(
            feed.report.entries, [&](const MigrationEntry &e) { return e.state == g.state; }));
        for (size_t i = 0; i < kids.size(); ++i) {
            kids[i]->parent = f.get();
            kids[i]->row = static_cast<int>(i);
        }
        f->children = std::move(kids);
        f->state = Loaded;
        out.push_back(std::move(f));
    }
    return out;
}

std::unique_ptr<SchemaTreeModel::Node>
SchemaTreeModel::makeMigrationsNode(const QStringList &tablesParts, const MigrationFeed &feed) const
{
    auto m = makeFolder(Flavor::Migrations, tr("Migrations"),
                        tablesParts + QStringList { QStringLiteral("Migrations") });
    auto kids = migrationChildren(m.get(), feed);
    for (size_t i = 0; i < kids.size(); ++i) {
        kids[i]->parent = m.get();
        kids[i]->row = static_cast<int>(i);
    }
    m->children = std::move(kids);
    m->state = Loaded;
    if (feed.state == MigrationFeed::State::Ready)
        m->detail = QString::number(feed.report.entries.size());
    return m;
}

// A Tables folder that is about to be (re)filled: puts the Migrations folder first, in place of its
// "(empty)" row.
void SchemaTreeModel::addMigrations(Node *root, const QStringList &tablesParts,
                                    std::vector<std::unique_ptr<Node>> &kids)
{
    const auto feed = m_feeds.constFind(root->conn->id);
    if (feed == m_feeds.constEnd() || !hostsMigrations(root, tablesParts))
        return;
    if (kids.size() == 1 && kids.front()->type == Node::Type::Empty)
        kids.clear();
    kids.insert(kids.begin(), makeMigrationsNode(tablesParts, *feed));
}

SchemaTreeModel::Node *SchemaTreeModel::findMigrations(Node *n) const
{
    for (const auto &c : n->children) {
        if (c->type != Node::Type::Object)
            continue;
        if (c->flavor == Flavor::Migrations)
            return c.get();
        const bool container = c->kind == ObjectKind::Database || c->kind == ObjectKind::Schema
            || (c->kind == ObjectKind::Folder
                && (c->flavor == Flavor::Databases || c->flavor == Flavor::Tables));
        if (container)
            if (Node *found = findMigrations(c.get()))
                return found;
    }
    return nullptr;
}

// A loaded Tables folder that may host the Migrations folder (chooses it when none was chosen).
SchemaTreeModel::Node *SchemaTreeModel::findMigrationsHost(Node *root, Node *n)
{
    for (const auto &c : n->children) {
        if (c->type != Node::Type::Object)
            continue;
        if (c->kind == ObjectKind::Folder && c->flavor == Flavor::Tables) {
            if (c->state == Loaded && hostsMigrations(root, c->path.parts))
                return c.get();
            continue;
        }
        const bool container = c->kind == ObjectKind::Database || c->kind == ObjectKind::Schema
            || (c->kind == ObjectKind::Folder && c->flavor == Flavor::Databases);
        if (container)
            if (Node *found = findMigrationsHost(root, c.get()))
                return found;
    }
    return nullptr;
}

void SchemaTreeModel::insertFirst(Node *parent, std::unique_ptr<Node> child)
{
    const bool live = attached(rootOf(parent));
    const auto place = [&] {
        child->parent = parent;
        parent->children.insert(parent->children.begin(), std::move(child));
        for (size_t i = 0; i < parent->children.size(); ++i)
            parent->children[i]->row = static_cast<int>(i);
    };
    const bool onlyNote
        = parent->children.size() == 1 && parent->children.front()->type == Node::Type::Empty;
    if (live && m_filter.active) { // the search keeps lists of visible rows: show the tree again
        beginResetModel();
        if (onlyNote)
            parent->children.clear();
        place();
        applyFilter();
        endResetModel();
        emit filterChanged();
        return;
    }
    if (onlyNote) {
        if (live)
            beginRemoveRows(indexFor(parent), 0, 0);
        parent->children.clear();
        if (live)
            endRemoveRows();
    }
    if (live)
        beginInsertRows(indexFor(parent), 0, 0);
    place();
    if (live) {
        endInsertRows();
        if (parent->children.size() > 1) { // the others moved down one place
            const QModelIndex p = indexFor(parent);
            emit dataChanged(index(1, 0, p),
                             index(static_cast<int>(parent->children.size()) - 1, 0, p),
                             { PositionRole, SetSizeRole });
        }
    }
}

void SchemaTreeModel::removeChild(Node *child)
{
    Node *parent = child->parent;
    const bool live = attached(rootOf(parent));
    const auto pos = static_cast<int>(
        std::ranges::find_if(parent->children, [&](const auto &c) { return c.get() == child; })
        - parent->children.begin());
    const auto drop = [&] {
        parent->children.erase(parent->children.begin() + pos);
        for (size_t i = 0; i < parent->children.size(); ++i)
            parent->children[i]->row = static_cast<int>(i);
    };
    if (live && m_filter.active) {
        beginResetModel();
        drop();
        if (parent->children.empty()) {
            auto none = makeNote(tr("(empty)"));
            none->parent = parent;
            parent->children.push_back(std::move(none));
        }
        applyFilter();
        endResetModel();
        emit filterChanged();
        return;
    }
    if (live)
        beginRemoveRows(indexFor(parent), pos, pos);
    drop();
    if (live)
        endRemoveRows();
    if (parent->children.empty()) { // a Tables folder without tables says so
        auto none = makeNote(tr("(empty)"));
        none->parent = parent;
        if (live)
            beginInsertRows(indexFor(parent), 0, 0);
        parent->children.push_back(std::move(none));
        if (live)
            endInsertRows();
    } else if (live) {
        const QModelIndex p = indexFor(parent);
        emit dataChanged(index(0, 0, p), index(static_cast<int>(parent->children.size()) - 1, 0, p),
                         { PositionRole, SetSizeRole });
    }
}

namespace {
// What a node shows, to tell whether it changed.
template<typename N> QStringList signatures(const std::vector<std::unique_ptr<N>> &nodes)
{
    QStringList out;
    for (const auto &n : nodes)
        out.append(n->name + QLatin1Char('\x1f') + n->detail);
    return out;
}
} // namespace

// The leaves of a group that stays on screen: the ones that left are removed, the new ones inserted
// and the others kept (so the current row and the view's state survive), all in report order.
void SchemaTreeModel::mergeLeaves(Node *group, std::vector<std::unique_ptr<Node>> fresh)
{
    const QModelIndex gi = indexFor(group);
    QSet<QString> wanted;
    for (const auto &f : fresh)
        wanted.insert(f->name);
    const auto renumber = [&] {
        for (size_t i = 0; i < group->children.size(); ++i)
            group->children[i]->row = static_cast<int>(i);
    };
    for (int i = static_cast<int>(group->children.size()) - 1; i >= 0; --i) {
        if (wanted.contains(group->children[static_cast<size_t>(i)]->name))
            continue;
        beginRemoveRows(gi, i, i);
        group->children.erase(group->children.begin() + i);
        renumber();
        endRemoveRows();
    }
    // What is left is in the order of `fresh`, so each new leaf goes in at its own place.
    for (size_t i = 0; i < fresh.size(); ++i) {
        if (i < group->children.size() && group->children[i]->name == fresh[i]->name) {
            Node &old = *group->children[i];
            if (old.detail != fresh[i]->detail || old.migState != fresh[i]->migState) {
                old.detail = fresh[i]->detail;
                old.migState = fresh[i]->migState;
                old.batch = fresh[i]->batch;
                const QModelIndex at = index(static_cast<int>(i), 0, gi);
                emit dataChanged(at, at, { DetailRole, MigrationStateRole, BatchRole });
            }
            continue;
        }
        beginInsertRows(gi, static_cast<int>(i), static_cast<int>(i));
        fresh[i]->parent = group;
        group->children.insert(group->children.begin() + static_cast<std::ptrdiff_t>(i),
                               std::move(fresh[i]));
        renumber();
        endInsertRows();
    }
    if (!group->children.empty())
        emit dataChanged(index(0, 0, gi),
                         index(static_cast<int>(group->children.size()) - 1, 0, gi),
                         { PositionRole, SetSizeRole });
}

// The leaves of one group changed: replaced only when something in them did.
void SchemaTreeModel::syncGroup(Node *group, const MigrationFeed &feed)
{
    auto fresh = groupChildren(group->flavor, group->path.parts, feed);
    const MigrationState state
        = stateOfGroup(static_cast<int>(group->flavor), static_cast<int>(Flavor::MigrationsPending),
                       static_cast<int>(Flavor::MigrationsApplied));
    const QString count = QString::number(std::ranges::count_if(
        feed.report.entries, [&](const MigrationEntry &e) { return e.state == state; }));
    if (signatures(fresh) != signatures(group->children)) {
        const auto leaves = [](const auto &nodes) {
            return !nodes.empty() && std::ranges::all_of(nodes, [](const auto &n) {
                return n->flavor == Flavor::Migration;
            });
        };
        if (!m_filter.active && attached(rootOf(group)) && leaves(fresh) && leaves(group->children))
            mergeLeaves(group, std::move(fresh));
        else
            setChildren(group, std::move(fresh));
    }
    if (group->detail != count) {
        group->detail = count;
        if (attached(rootOf(group)))
            if (const QModelIndex i = indexFor(group); i.isValid())
                emit dataChanged(i, i, { DetailRole });
    }
}

void SchemaTreeModel::refreshMigrations(Node *folder, const MigrationFeed &feed)
{
    std::vector<Flavor> shape;
    if (feed.state == MigrationFeed::State::Ready) {
        shape = { Flavor::MigrationsPending, Flavor::MigrationsApplied };
        if (feed.report.orphans > 0)
            shape.push_back(Flavor::MigrationsOrphans);
    }
    bool same = !shape.empty() && shape.size() == folder->children.size();
    for (size_t i = 0; same && i < shape.size(); ++i)
        same = folder->children[i]->flavor == shape[i];
    if (same) {
        for (const auto &g : folder->children)
            syncGroup(g.get(), feed);
    } else {
        setChildren(folder, migrationChildren(folder, feed));
    }
    const QString count = feed.state == MigrationFeed::State::Ready
        ? QString::number(feed.report.entries.size())
        : QString();
    if (folder->detail != count) {
        folder->detail = count;
        if (attached(rootOf(folder)))
            if (const QModelIndex i = indexFor(folder); i.isValid())
                emit dataChanged(i, i, { DetailRole });
    }
}

void SchemaTreeModel::setMigrations(const QString &connectionId, const MigrationFeed &feed)
{
    const QUuid id = QUuid::fromString(connectionId);
    if (id.isNull())
        return;
    const bool configured = feed.state != MigrationFeed::State::Unconfigured;
    if (configured)
        m_feeds.insert(id, feed);
    else
        m_feeds.remove(id);
    Node *root = rootById(id);
    if (root == nullptr)
        return;
    Node *existing = findMigrations(root);
    if (!configured) {
        root->conn->migrationHost.clear();
        if (existing != nullptr)
            removeChild(existing);
        return;
    }
    if (existing != nullptr)
        refreshMigrations(existing, feed);
    else if (Node *host = findMigrationsHost(root, root))
        insertFirst(host, makeMigrationsNode(host->path.parts, feed));
}

// ---- search ------------------------------------------------------------------------------------

// A request is in flight or about to be sent: the tree is about to change under a new search.
bool SchemaTreeModel::busy() const
{
    return !m_requests.isEmpty() || m_drainQueued;
}

void SchemaTreeModel::setFilter(const QString &text, int scope)
{
    const QString t = text.simplified();
    if (t == m_filter.wantedText && scope == m_filter.wantedScope)
        return;
    m_filter.wantedText = t;
    m_filter.wantedScope = scope;
    if (t == m_filter.text && scope == m_filter.scope) { // back to what is shown
        m_filterTimer->stop();
        if (std::exchange(m_filter.pending, false))
            emit filterChanged();
        return;
    }
    if (busy()) {
        if (!std::exchange(m_filter.pending, true))
            emit filterChanged();
        m_filterTimer->start();
        return;
    }
    m_filterTimer->stop();
    commitFilter(t, scope);
}

void SchemaTreeModel::retryFilter()
{
    if (!m_filter.pending)
        return;
    if (busy()) {
        m_filterTimer->start();
        return;
    }
    commitFilter(m_filter.wantedText, m_filter.wantedScope);
}

void SchemaTreeModel::clearFilter()
{
    m_filter.wantedText.clear();
    m_filterTimer->stop();
    if (m_filter.active)
        commitFilter({}, m_filter.scope);
    else if (std::exchange(m_filter.pending, false))
        emit filterChanged();
}

void SchemaTreeModel::commitFilter(const QString &text, int scope)
{
    const bool wasActive = m_filter.active;
    const bool pending = std::exchange(m_filter.pending, false);
    const bool nowActive = !text.isEmpty();
    m_filter.wantedText = text;
    m_filter.wantedScope = scope;
    if (!wasActive && !nowActive) {
        m_filter.text = text;
        m_filter.scope = scope;
        if (pending)
            emit filterChanged();
        return;
    }
    // The state changes inside the reset, so a view that hears of it still sees the old one.
    beginResetModel();
    m_filter.text = text;
    m_filter.folded = fold(text);
    m_filter.scope = scope;
    m_filter.active = nowActive;
    if (nowActive) {
        applyFilter();
    } else {
        for (auto *list : { &m_roots, &m_parked })
            for (auto &r : *list)
                restoreRows(r.get());
        for (size_t i = 0; i < m_roots.size(); ++i)
            m_roots[i]->row = static_cast<int>(i);
        m_shownRoots.clear();
        m_filter.count = 0;
        m_filter.incomplete = false;
    }
    endResetModel();
    emit filterChanged();
}

void SchemaTreeModel::restoreRows(Node *n)
{
    n->shown.clear();
    n->hidden = false;
    for (size_t i = 0; i < n->children.size(); ++i) {
        n->children[i]->row = static_cast<int>(i);
        restoreRows(n->children[i].get());
    }
}

// The whole pass is one walk over the nodes already in memory: nothing is allocated per node but
// the lists of visible children, and nothing is loaded.
void SchemaTreeModel::applyFilter()
{
    FilterCtx ctx;
    ctx.needle = m_filter.folded;
    ctx.ascii = isAscii(ctx.needle);
    ctx.scope = m_filter.scope;
    m_shownRoots.clear();
    for (auto &r : m_roots) {
        if (filterWalk(r.get(), ctx, false)) {
            r->row = static_cast<int>(m_shownRoots.size());
            m_shownRoots.push_back(r.get());
        }
    }
    m_filter.count = ctx.count;
    m_filter.incomplete = ctx.incomplete;
}

bool SchemaTreeModel::matches(const Node *n, const FilterCtx &ctx) const
{
    const auto named = [&](const QString &s) { return containsFolded(s, ctx.needle, ctx.ascii); };
    if (n->type == Node::Type::Connection) {
        if (ctx.scope != Everything && ctx.scope != Connections)
            return false;
        // The file or the database as well: that is what the connection is known by.
        return named(rootData(n, NameRole).toString())
            || named(rootData(n, SummaryRole).toString());
    }
    if (n->type != Node::Type::Object)
        return false;
    switch (ctx.scope) {
    case Everything:
        return named(n->name);
    case Databases:
        return n->kind == ObjectKind::Database && named(n->name);
    case Tables:
        return (n->kind == ObjectKind::Table || n->kind == ObjectKind::Index) && named(n->name);
    case Views:
        return (n->kind == ObjectKind::View || n->kind == ObjectKind::MaterializedView)
            && named(n->name);
    case Users:
        return n->kind == ObjectKind::User && named(n->name);
    case Migrations:
        return n->flavor >= Flavor::Migrations && named(n->name);
    default: // Connections: connections only.
        return false;
    }
}

bool SchemaTreeModel::filterWalk(Node *n, FilterCtx &ctx, bool forced)
{
    n->shown.clear();
    n->hidden = false;
    const bool self = !forced && matches(n, ctx);
    n->matched = self;
    if (self)
        ++ctx.count;
    if (!forced && !ctx.incomplete && n->state == Idle && ctx.scope != Connections) {
        // Something the search could not look into: a connection not opened, a folder not expanded.
        // The migrations are inside the connections whose migrations are configured, in Tables.
        const bool all = ctx.scope == Everything;
        const bool mig = ctx.scope == Migrations && m_feeds.contains(rootOf(n)->conn->id);
        bool relevant = n->type == Node::Type::Connection && (ctx.scope != Migrations || mig);
        if (n->type == Node::Type::Object && n->hasChildren) {
            if (n->kind == ObjectKind::Database || n->kind == ObjectKind::Schema) {
                relevant = all || ctx.scope == Tables || ctx.scope == Views || mig;
            } else if (n->kind == ObjectKind::Folder) {
                switch (n->flavor) {
                case Flavor::Tables:
                    relevant = all || ctx.scope == Tables || mig;
                    break;
                case Flavor::Indexes:
                    relevant = all || ctx.scope == Tables;
                    break;
                case Flavor::Views:
                    relevant = all || ctx.scope == Views;
                    break;
                case Flavor::Users:
                    relevant = all || ctx.scope == Users;
                    break;
                default:
                    break;
                }
            }
        }
        if (relevant)
            ctx.incomplete = true;
    }
    // A connection that matches in "connections" shows everything under it, collapsed.
    const bool whole = forced || (self && ctx.scope == Connections);
    if (ctx.scope != Connections || whole) {
        for (auto &c : n->children) {
            if (filterWalk(c.get(), ctx, whole)) {
                c->row = static_cast<int>(n->shown.size());
                n->shown.push_back(c.get());
            }
        }
    }
    n->hidden = !(forced || self || !n->shown.empty());
    return !n->hidden;
}

QVariantList SchemaTreeModel::filterExpansion() const
{
    QVariantList out;
    if (!m_filter.active || m_filter.scope == Connections)
        return out;
    const auto collect = [&](const auto &self, const Node *n) -> void {
        for (const Node *c : n->shown)
            if (!c->shown.empty()) {
                out.append(QVariant::fromValue(createIndex(c->row, 0, const_cast<Node *>(c))));
                self(self, c);
            }
    };
    for (const Node *r : m_shownRoots)
        if (!r->shown.empty()) {
            out.append(QVariant::fromValue(createIndex(r->row, 0, const_cast<Node *>(r))));
            collect(collect, r);
        }
    return out;
}

namespace {
// Generic over the node type, which is private to the model.
template<typename N> QString keyPart(const N *n)
{
    if (n->conn != nullptr)
        return QLatin1Char('c') + toId(n->conn->id);
    return QString::number(static_cast<int>(n->type)) + QLatin1Char('.')
        + QString::number(static_cast<int>(n->kind)) + QLatin1Char('.')
        + QString::number(static_cast<int>(n->flavor)) + QLatin1Char('.')
        + n->path.parts.join(QLatin1Char('\x1e')) + QLatin1Char('|') + n->name;
}
} // namespace

QString SchemaTreeModel::keyOf(const QModelIndex &index) const
{
    if (!index.isValid() || index.model() != this)
        return {};
    QStringList parts;
    for (const Node *n = node(index); n != nullptr; n = n->parent)
        parts.prepend(keyPart(n));
    return parts.join(QLatin1Char('\x1f'));
}

QVariantList SchemaTreeModel::indexesOfKeys(const QStringList &keys) const
{
    const QSet<QString> wanted(keys.begin(), keys.end());
    QVariantList out;
    const auto visit = [&](const auto &self, const Node *parent, const QString &prefix) -> void {
        const int count = visibleCount(parent);
        for (int r = 0; r < count; ++r) {
            const Node *c = visibleAt(parent, r);
            const QString key
                = prefix.isEmpty() ? keyPart(c) : prefix + QLatin1Char('\x1f') + keyPart(c);
            if (!wanted.contains(key))
                continue;
            out.append(QVariant::fromValue(createIndex(c->row, 0, const_cast<Node *>(c))));
            self(self, c, key);
        }
    };
    visit(visit, nullptr, QString());
    return out;
}

QModelIndex SchemaTreeModel::firstMatch() const
{
    if (!m_filter.active)
        return {};
    const auto find = [&](const auto &self, const Node *n) -> const Node * {
        if (n->matched)
            return n;
        for (const Node *c : n->shown)
            if (const Node *found = self(self, c))
                return found;
        return nullptr;
    };
    for (const Node *r : m_shownRoots)
        if (const Node *found = find(find, r))
            return createIndex(found->row, 0, const_cast<Node *>(found));
    return {};
}

QVariantList SchemaTreeModel::ancestorsOf(const QModelIndex &index) const
{
    QVariantList out;
    if (!index.isValid() || index.model() != this)
        return out;
    for (const Node *p = node(index)->parent; p != nullptr; p = p->parent)
        out.prepend(QVariant::fromValue(indexFor(p)));
    return out;
}

// ---- passwords ---------------------------------------------------------------------------------

void SchemaTreeModel::onPasswordRead(QUuid id, const QString &password)
{
    Node *root = rootById(id);
    if (root == nullptr || root->conn->stage != ConnData::Stage::ReadingPassword)
        return;
    proceedConnect(root, password);
}

bool SchemaTreeModel::onPasswordFailed(QUuid id, const Error &error)
{
    Node *root = rootById(id);
    if (root == nullptr || root->conn->stage != ConnData::Stage::ReadingPassword)
        return false;
    root->conn->stage = ConnData::Stage::None;
    failNode(root, error);
    return true;
}

void SchemaTreeModel::providePassword(const QString &connectionId, const QString &password)
{
    Node *root = rootById(QUuid::fromString(connectionId));
    if (root == nullptr || root->conn->stage != ConnData::Stage::AwaitingPassword)
        return;
    if (m_credentials.remember)
        m_credentials.remember(root->conn->id, password);
    proceedConnect(root, password);
}

void SchemaTreeModel::cancelPassword(const QString &connectionId)
{
    Node *root = rootById(QUuid::fromString(connectionId));
    if (root == nullptr || root->conn->stage != ConnData::Stage::AwaitingPassword)
        return;
    root->conn->stage = ConnData::Stage::None;
    emit collapseRequested(indexFor(root));
    setChildren(root, {});
    setState(root, Idle);
}

} // namespace quarry::ui
