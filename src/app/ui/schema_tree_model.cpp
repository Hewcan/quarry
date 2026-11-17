#include "schema_tree_model.h"

#include <QDebug>
#include <QElapsedTimer>
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
    }
    return "folder";
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
};

struct SchemaTreeModel::Node
{
    enum class Type : uint8_t { Connection, Object, Loading, Error, Empty };

    Type type = Type::Object;
    LoadState state = Idle;
    bool hasChildren = false;
    ObjectKind kind = ObjectKind::Table;
    int row = 0;
    Node *parent = nullptr;
    QString name; // error rows: the message
    QString detail; // error rows: the technical text
    ObjectPath path;
    std::vector<std::unique_ptr<Node>> children;
    std::unique_ptr<ConnData> conn; // connection nodes only
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
    return root->row >= 0 && static_cast<size_t>(root->row) < m_roots.size()
        && m_roots[static_cast<size_t>(root->row)].get() == root;
}

QModelIndex SchemaTreeModel::indexFor(const Node *n) const
{
    return createIndex(n->row, 0, const_cast<Node *>(n));
}

void SchemaTreeModel::renumberRoots()
{
    for (size_t i = 0; i < m_roots.size(); ++i)
        m_roots[i]->row = static_cast<int>(i);
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
    beginRemoveRows({}, first, last);
    for (int r = first; r <= last; ++r)
        m_parked.push_back(std::move(m_roots[static_cast<size_t>(r)]));
    m_roots.erase(m_roots.begin() + first, m_roots.begin() + last + 1);
    renumberRoots();
    endRemoveRows();
    if (!m_roots.empty())
        emit dataChanged(index(0, 0), index(rowCount() - 1, 0), { PositionRole, SetSizeRole });
    emit connectedChanged();
    QTimer::singleShot(0, this, &SchemaTreeModel::flushParked);
}

void SchemaTreeModel::onSourceChanged(int first, int last)
{
    if (first < 0 || last >= static_cast<int>(m_roots.size()))
        return;
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
    fillRoots();
    endResetModel();
    emit connectedChanged();
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
    if (!parent.isValid()) {
        return static_cast<size_t>(row) < m_roots.size()
            ? createIndex(row, 0, m_roots[static_cast<size_t>(row)].get())
            : QModelIndex();
    }
    const Node *p = node(parent);
    return static_cast<size_t>(row) < p->children.size()
        ? createIndex(row, 0, p->children[static_cast<size_t>(row)].get())
        : QModelIndex();
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
    if (!parent.isValid())
        return static_cast<int>(m_roots.size());
    return static_cast<int>(node(parent)->children.size());
}

int SchemaTreeModel::columnCount(const QModelIndex &) const
{
    return 1;
}

bool SchemaTreeModel::hasChildren(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return !m_roots.empty();
    const Node *n = node(parent);
    switch (n->type) {
    case Node::Type::Connection:
        return true;
    case Node::Type::Object:
        return n->hasChildren || !n->children.empty();
    default:
        return false;
    }
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
    return m_list->data(m_list->index(root->row), map.value(role));
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
        return static_cast<int>(n->parent != nullptr ? n->parent->children.size() : m_roots.size());
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
        emit dataChanged(indexFor(n), indexFor(n), { LoadStateRole });
}

void SchemaTreeModel::setConnected(Node *root, bool connected)
{
    if (root->conn->connected == connected)
        return;
    root->conn->connected = connected;
    if (attached(root))
        emit dataChanged(indexFor(root), indexFor(root), { ConnectedRole });
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
    if (n == root) {
        root->conn->stage = ConnData::Stage::None;
        setConnected(root, true); // the answer proves the session is open
    }
    std::vector<std::unique_ptr<Node>> kids;
    kids.reserve(std::max<size_t>(nodes.size(), 1));
    for (auto &sn : nodes)
        kids.push_back(makeObject(std::move(sn)));
    if (kids.empty()) {
        auto empty = std::make_unique<Node>();
        empty->type = Node::Type::Empty;
        empty->name = tr("(empty)");
        kids.push_back(std::move(empty));
    }
    setChildren(n, std::move(kids));
    setState(n, Loaded);
    if (n == root)
        emit announcement(tr("Connected to %1").arg(data(indexFor(n), NameRole).toString()), false);
    if (m_reveal && root->conn->id == m_reveal->id)
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
    setState(root, Idle);
    setConnected(root, false);
    emit announcement(tr("Disconnected from %1").arg(data(indexFor(root), NameRole).toString()),
                      false);
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
        // Databases and schemas are on the way only when they start the path; folders hold the
        // tables or the views, so every folder is tried.
        const bool container = child->kind == ObjectKind::Folder
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
