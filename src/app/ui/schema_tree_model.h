#pragma once

#include "connection_list_model.h"
#include "connection_preflight.h"

#include <quarry/object_path.h>
#include <quarry/provider_api.h>
#include <quarry/provider_registry.h>
#include <quarry/result.h>
#include <quarry/session.h>

#include <QAbstractItemModel>
#include <QHash>
#include <QStringList>
#include <QTimer>
#include <QUuid>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace quarry::ui {

// What the tree needs from the keychain and the per-session password memory. The controller
// wires it to CredentialStore (or to the test backend) and forwards the results to
// onPasswordRead() / onPasswordFailed().
struct SchemaCredentials
{
    std::function<void(QUuid)> read;
    std::function<std::optional<QString>(QUuid)> session;
    std::function<void(QUuid, QString)> remember;
    std::function<void(QUuid)> forget;
};

// The sidebar tree. Top level = the saved connections (mirrors ConnectionListModel, which stays
// the single source of truth for name, color, production...). Expanding a connection opens its
// metadata Session and lists its children; every other node loads its children when expanded
// (canFetchMore/fetchMore -> Session::children). While a request is pending the node has one
// synthetic "Loading…" child; on failure one synthetic error child (name = message, detail =
// technical text); a node that answers with no children gets one synthetic "(empty)" child, so
// a screen reader never says "expanded" about a node with nothing under it. Nodes the provider
// reports without children (leaves) have no expander at all.
//
// Object tree v2 (H12 phase A): the app adds folders the providers do not return, grouping what
// they do. Under a connection: Databases (every database) and Users (only with Capability::Users;
// loaded when expanded), or, for SQLite's single database, Tables, Views and Indexes directly.
// Under a database without schemas, or a schema: Tables, Views, Indexes (loaded when expanded) and
// Other (only when something else came back). The ObjectPath of a table or view never changes.
//
// The search box (setFilter) hides, inside this model, every node that neither matches nor has a
// matching descendant. It only looks at what is loaded: it never connects or loads anything.
class SchemaTreeModel : public QAbstractItemModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by ConnectionController")

    // Ids of the connections whose session is open.
    Q_PROPERTY(QStringList connectedIds READ connectedIds NOTIFY connectedChanged FINAL)
    // The search in force (what the model shows), not what the box says while a load is running.
    Q_PROPERTY(bool filterActive READ filterActive NOTIFY filterChanged FINAL)
    Q_PROPERTY(int filterCount READ filterCount NOTIFY filterChanged FINAL)
    // Some connection or folder was not loaded, so the results may be incomplete.
    Q_PROPERTY(bool filterIncomplete READ filterIncomplete NOTIFY filterChanged FINAL)
    // A new text is waiting for the running loads to end.
    Q_PROPERTY(bool filterPending READ filterPending NOTIFY filterChanged FINAL)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        KindRole, // "connection", "database", "table"... or "loading", "error", "empty"
        DetailRole,
        ConnectionIdRole,
        ProviderNameRole,
        SummaryRole,
        ColorIndexRole,
        ColorNameRole,
        ProductionRole,
        ConnectedRole, // connection nodes: the session is open
        LoadStateRole, // LoadState
        PositionRole, // 1-based position among the siblings
        SetSizeRole, // number of siblings
    };
    Q_ENUM(Role)

    enum LoadState { Idle, Loading, Loaded, Failed };
    Q_ENUM(LoadState)

    // Why the search box filters (UX.md, "Tree search"). The numbers are stored in the layout
    // settings: only append.
    enum FilterScope { Everything, Connections, Databases, Tables, Views, Users, Migrations };
    Q_ENUM(FilterScope)

    explicit SchemaTreeModel(ConnectionListModel *connections, quarry::ProviderRegistry *registry,
                             QObject *parent = nullptr);
    // Closes every session.
    ~SchemaTreeModel() override;

    void setCredentials(SchemaCredentials credentials) { m_credentials = std::move(credentials); }
    // The SSH secret and host key step of connections that use a tunnel. Owned by the caller, who
    // keeps it alive as long as the model. Without one, such a connection is opened directly.
    void setPreflight(ConnectionPreflight *preflight);
    QStringList connectedIds() const;

    // Keychain results (forwarded by the controller). onPasswordFailed returns true when the
    // tree was waiting for that read.
    void onPasswordRead(QUuid id, const QString &password);
    bool onPasswordFailed(QUuid id, const quarry::Error &error);

    // The tree and the controller are cheap to ask: row of a connection node, or -1.
    Q_INVOKABLE QModelIndex indexOfConnection(const QString &connectionId) const;
    // The connection node at the top of `index`'s branch (invalid for an invalid index).
    Q_INVOKABLE QModelIndex connectionOf(const QModelIndex &index) const;
    Q_INVOKABLE QString connectionIdOf(const QModelIndex &index) const;
    Q_INVOKABLE bool isConnected(const QString &connectionId) const;
    // What "Open data" needs from a node: {openable, connectionId, parts, kind, name}. Only tables,
    // views and materialized views are openable.
    Q_INVOKABLE QVariantMap objectOf(const QModelIndex &index) const;
    // Error row or its parent: loads the parent's children again (and reconnects a connection).
    Q_INVOKABLE void retry(const QModelIndex &index);
    // Closes the session and returns the connection's subtree to "not loaded".
    Q_INVOKABLE void disconnectConnection(const QString &connectionId);
    // Answer to passwordRequested(). An empty password is allowed.
    Q_INVOKABLE void providePassword(const QString &connectionId, const QString &password);
    // The user dismissed the password prompt.
    Q_INVOKABLE void cancelPassword(const QString &connectionId);

    // ---- for autocompletion (CompletionController) ----
    enum class Access : uint8_t {
        Connected, // the metadata session is open
        Opening, // it is being opened (or the user is already expanding the connection)
        NeedsPassword, // opening it would prompt: nothing was done
        Unavailable, // no such connection, or the last attempt failed: the user retries in the tree
    };
    // The session of a connected connection (owned by the model), else nullptr.
    quarry::Session *metadataSession(const QString &connectionId) const;
    // Opens the session the way expanding the connection does, but never asks for a password.
    Access ensureSession(const QString &connectionId);
    // Finds the table or view `parts` in the tree, loading the branches on the way, and ends with
    // revealed(). Needs a connected connection (never opens one).
    // Also drops the search: the node may be hidden by it (filterCleared() tells the box).
    Q_INVOKABLE void reveal(const QString &connectionId, const QStringList &parts);

    // ---- search ----
    // Filters by `text` (case and accent insensitive, empty = no filter). While a request is in
    // flight the new text waits (filterPending) and is applied when the tree is quiet.
    Q_INVOKABLE void setFilter(const QString &text, int scope);
    Q_INVOKABLE void clearFilter();
    bool filterActive() const { return m_filter.active; }
    int filterCount() const { return m_filter.count; }
    bool filterIncomplete() const { return m_filter.incomplete; }
    bool filterPending() const { return m_filter.pending; }
    // The nodes to expand to show the matches, top first (only loaded ones: never loads).
    Q_INVOKABLE QVariantList filterExpansion() const;
    // A node's identity across model resets and filter changes ("" for an invalid index).
    Q_INVOKABLE QString keyOf(const QModelIndex &index) const;
    // The nodes of `keys` that exist and are shown, top first. Only looks inside nodes whose own
    // key is in `keys`, so give it the expanded nodes plus the one to select.
    Q_INVOKABLE QVariantList indexesOfKeys(const QStringList &keys) const;
    // The first node the search itself matched (not just an ancestor of one), or invalid.
    Q_INVOKABLE QModelIndex firstMatch() const;
    // The branches above `index`, top first.
    Q_INVOKABLE QVariantList ancestorsOf(const QModelIndex &index) const;

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    bool hasChildren(const QModelIndex &parent = {}) const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void connectedChanged();
    // For the screen reader, without moving the focus.
    void announcement(const QString &text, bool assertive);
    // The node must be shown collapsed (disconnected, or the password prompt was dismissed).
    void collapseRequested(const QModelIndex &index);
    // "Ask every time" and no password remembered for this run. `retry` = the last one failed.
    void passwordRequested(const QString &connectionId, const QString &name, bool retry);
    // The tree was loaded again (retry): the metadata cache of the connection is stale.
    void metadataInvalidated(const QString &connectionId);
    // reveal() found the node: `ancestors` (QModelIndex, top first) must be expanded to show it.
    void revealed(const QModelIndex &target, const QVariantList &ancestors);
    // The search was dropped by the model (reveal): the box must empty itself.
    void filterCleared();
    // The search changed or the tree changed under it: the view must expand the matches again.
    void filterChanged();

private:
    struct Node;
    struct ConnData;
    struct FilterCtx;
    // What a folder node is for. Plain = a folder of the provider (a table's Columns...).
    enum class Flavor : uint8_t { Plain, Databases, Users, Tables, Views, Indexes, Other };

    Node *node(const QModelIndex &index) const;
    QModelIndex indexFor(const Node *n) const;
    Node *rootOf(Node *n) const;
    bool attached(const Node *root) const;
    void renumberRoots();
    std::unique_ptr<Node> makeRoot(const QUuid &id) const;

    void fillRoots();
    void onSourceInserted(int first, int last);
    void onSourceRemoved(int first, int last);
    void onSourceChanged(int first, int last);
    void onSourceReset();
    void flushParked();

    void setChildren(Node *n, std::vector<std::unique_ptr<Node>> kids);
    // Tree v2: what the provider listed under `n`, regrouped into the folders of UX.md.
    std::vector<std::unique_ptr<Node>> connectionFolders(Node *root,
                                                         std::vector<quarry::SchemaNode> &&nodes);
    std::vector<std::unique_ptr<Node>> relationFolders(const QStringList &baseParts,
                                                       std::vector<quarry::SchemaNode> &&nodes);
    std::unique_ptr<Node> makeFolder(Flavor flavor, const QString &name, QStringList parts) const;
    static void fillFolder(Node *folder, std::vector<std::unique_ptr<Node>> kids);
    bool supportsUsers(const Node *root) const;

    // ---- search ----
    bool busy() const;
    void applyFilter(); // the full recompute, between beginResetModel() and endResetModel()
    void commitFilter(const QString &text, int scope);
    bool filterWalk(Node *n, FilterCtx &ctx, bool forced);
    bool matches(const Node *n, const FilterCtx &ctx) const;
    void restoreRows(Node *n);
    bool isShown(const Node *n) const;
    int visibleCount(const Node *parent) const;
    Node *visibleAt(const Node *parent, int row) const;
    void retryFilter();
    void setState(Node *n, LoadState state);
    // fetchMore() only marks the node and queues the work: a view (or QAbstractItemModelTester)
    // may call it from inside a model-change handler, where changing the model again is illegal.
    void defer(Node *n);
    static std::unique_ptr<Node> makeObject(quarry::SchemaNode &&sn);
    void queueDrain();
    void drainDeferred();
    void load(Node *n);
    void connectRoot(Node *root);
    void proceedConnect(Node *root, const QString &password);
    void connectWith(Node *root, const quarry::ConnectionProfile &profile,
                     const quarry::Secrets &secrets);
    void onPreflightReady(int request, const quarry::Secrets &secrets);
    void onPreflightFailed(int request, const quarry::Error &error);
    void onPreflightCancelled(int request);
    Node *rootByPreflight(int request) const;
    void openSession(Node *root, std::unique_ptr<quarry::IConnection> connection);
    void failNode(Node *n, const quarry::Error &error);
    void disposeSession(ConnData &conn);
    void dropRequests(quint64 token);
    void setConnected(Node *root, bool connected);
    void onChildrenReady(quint64 token, quint64 requestId, std::vector<quarry::SchemaNode> nodes);
    void onChildrenFailed(quint64 token, quint64 requestId, const quarry::Error &error);
    void onSessionOpened(quint64 token);
    void onSessionFailed(quint64 token, const quarry::Error &error);
    void advanceReveal();
    Node *findForReveal(Node *n, const QStringList &target, Node *&toLoad) const;
    Node *rootById(const QUuid &id) const;
    Node *rootByToken(quint64 token) const;
    QVariant rootData(const Node *root, int role) const;

    ConnectionListModel *m_list;
    quarry::ProviderRegistry *m_registry;
    SchemaCredentials m_credentials;
    // Only compared and called, owned by the controller that owns this model too.
    ConnectionPreflight *m_preflight = nullptr;
    std::vector<std::unique_ptr<Node>> m_roots;
    // Roots whose row was removed: a rename moves the row (remove + insert), and the open
    // session must survive that. Whatever nobody claimed by the next event-loop turn is closed.
    std::vector<std::unique_ptr<Node>> m_parked;
    // (session token, request id) -> node with a request in flight. Entries of a closed session
    // are erased, so a late answer finds nothing and is ignored.
    QHash<std::pair<quint64, quint64>, Node *> m_requests;
    quint64 m_nextToken = 0;
    bool m_drainQueued = false;
    struct Reveal
    {
        QUuid id;
        QStringList parts;
    };
    std::optional<Reveal> m_reveal;

    struct Filter
    {
        bool active = false;
        bool pending = false;
        bool incomplete = false;
        int count = 0;
        int scope = Everything;
        QString text; // as typed, trimmed
        QString folded; // case and accents removed
        QString wantedText; // what the box says (applied when the tree is quiet)
        int wantedScope = Everything;
    };
    Filter m_filter;
    std::vector<Node *> m_shownRoots; // the roots that pass the filter, in order
    // Polls while a new text waits for the loads to end.
    QTimer *m_filterTimer = nullptr;
};

} // namespace quarry::ui
