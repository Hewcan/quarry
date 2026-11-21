#include "ui/connection_controller.h"

#include <QAbstractItemModelTester>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <algorithm>
#include <memory>
#include <sqlite3.h>

using namespace quarry;
using namespace quarry::ui;

namespace {

bool createDatabase(const QString &file, int tables)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open(file.toUtf8().constData(), &db) != SQLITE_OK)
        return false;
    QByteArray sql = "BEGIN;";
    for (int i = 0; i < tables; ++i)
        sql += "CREATE TABLE t" + QByteArray::number(i) + "(id INTEGER PRIMARY KEY, v TEXT);";
    sql += "CREATE VIEW v1 AS SELECT * FROM t0;";
    if (tables > 0)
        sql += "CREATE INDEX t0_v_idx ON t0(v);";
    sql += "COMMIT;";
    const bool ok = sqlite3_exec(db, sql.constData(), nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(db);
    return ok;
}

} // namespace

class TstSchemaTreeModel : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    int m_files = 0;

    QString newFile(int tables)
    {
        const QString f = m_dir.filePath(QStringLiteral("data-%1.db").arg(++m_files));
        return createDatabase(f, tables) ? f : QString();
    }

    void startController(ConnectionController &c)
    {
        c.setDatabasePath(m_dir.filePath(QStringLiteral("quarry-%1.db").arg(++m_files)));
        c.setProviderDirectory(QStringLiteral(QUARRY_PLUGIN_DIR));
        c.start();
        QTRY_VERIFY_WITH_TIMEOUT(!c.model()->loading(), 10000);
    }

    QString addConnection(ConnectionController &c, const QString &name, const QString &file,
                          const QString &id = {})
    {
        c.prepare(id);
        c.form()->setValue(0, file);
        QSignalSpy saved(&c, &ConnectionController::connectionSaved);
        const bool ok = c.save({ { QStringLiteral("id"), id },
                                 { QStringLiteral("providerId"), QStringLiteral("sqlite") },
                                 { QStringLiteral("name"), name },
                                 { QStringLiteral("colorIndex"), 1 },
                                 { QStringLiteral("production"), false },
                                 { QStringLiteral("passwordMode"), 1 } });
        if (!ok)
            return {};
        saved.wait(10000);
        return saved.isEmpty() ? QString() : saved.at(0).at(0).toString();
    }

    static QString text(const QAbstractItemModel *m, const QModelIndex &i, int role)
    {
        return m->data(i, role).toString();
    }

    // Child of `parent` with this name (waits for it to appear).
    QModelIndex child(SchemaTreeModel *m, const QModelIndex &parent, const QString &name)
    {
        QModelIndex found;
        const auto lookup = [&] {
            const int count = m->rowCount(parent);
            for (int r = 0; r < count; ++r) {
                const QModelIndex i = m->index(r, 0, parent);
                if (text(m, i, SchemaTreeModel::NameRole) == name) {
                    found = i;
                    return true;
                }
            }
            return false;
        };
        [&] { QTRY_VERIFY_WITH_TIMEOUT(lookup(), 10000); }();
        return found;
    }

    // Expands (fetchMore) and waits until the real children are there.
    void expand(SchemaTreeModel *m, const QModelIndex &i)
    {
        QVERIFY(m->canFetchMore(i));
        m->fetchMore(i);
        QTRY_COMPARE_WITH_TIMEOUT(m->data(i, SchemaTreeModel::LoadStateRole).toInt(),
                                  int(SchemaTreeModel::Loaded), 20000);
    }

    // QAbstractItemModelTester follows canFetchMore()/fetchMore() everywhere, so with it attached
    // every node loads by itself. The tests that assert laziness run without it; these two run
    // with it, through every kind of change (roots, loading, error, retry, disconnect).
    static std::unique_ptr<QAbstractItemModelTester> attachTester(SchemaTreeModel *m)
    {
        return std::make_unique<QAbstractItemModelTester>(
            m, QAbstractItemModelTester::FailureReportingMode::Fatal);
    }

    // Saves a profile straight into the store, with the password mode the SQLite form would not
    // allow (the SQLite engine has no secret field). The password itself is never stored.
    QString addProfile(ConnectionController &c, const QString &name, const QString &file,
                       PasswordMode mode)
    {
        ConnectionProfile p;
        p.id = QUuid::createUuid();
        p.name = name;
        p.providerId = QStringLiteral("sqlite");
        p.values = { { QStringLiteral("file"), file } };
        p.passwordMode = mode;
        c.model()->store()->save(p);
        const QString id = p.id.toString(QUuid::WithoutBraces);
        (void)QTest::qWaitFor([&] { return c.model()->indexOf(id) >= 0; }, 10000);
        return id;
    }

    void startFake(ConnectionController &c)
    {
        c.setDatabasePath(m_dir.filePath(QStringLiteral("quarry-%1.db").arg(++m_files)));
        c.setProviderDirectory(QStringLiteral(QUARRY_FAKE_TREE_DIR));
        c.start();
        QTRY_VERIFY_WITH_TIMEOUT(!c.model()->loading(), 10000);
    }

    // A FakeTree connection (tests/app/fake_tree): several databases, a schema, users, indexes.
    QString addFakeTree(ConnectionController &c, const QString &name, const QString &users = {},
                        const QString &database = {})
    {
        ConnectionProfile p;
        p.id = QUuid::createUuid();
        p.name = name;
        p.providerId = QStringLiteral("faketree");
        if (!users.isEmpty())
            p.values = { { QStringLiteral("users"), users } };
        if (!database.isEmpty())
            p.values.insert(QStringLiteral("database"), database);
        p.passwordMode = PasswordMode::None;
        c.model()->store()->save(p);
        const QString id = p.id.toString(QUuid::WithoutBraces);
        (void)QTest::qWaitFor([&] { return c.model()->indexOf(id) >= 0; }, 10000);
        return id;
    }

    // The node at `names` below `from`, waiting for each level; opens (loads) what is not loaded.
    QModelIndex walk(SchemaTreeModel *m, QModelIndex from, const QStringList &names)
    {
        for (const QString &name : names) {
            if (m->canFetchMore(from))
                expand(m, from);
            from = child(m, from, name);
            if (!from.isValid())
                return {};
        }
        return from;
    }

    static QStringList names(const SchemaTreeModel *m, const QModelIndex &parent)
    {
        QStringList out;
        for (int r = 0; r < m->rowCount(parent); ++r)
            out << text(m, m->index(r, 0, parent), SchemaTreeModel::NameRole);
        return out;
    }

    // Opens everything the FakeTree has, so a search has all of it to look at.
    QModelIndex loadAllFake(SchemaTreeModel *m, const QString &id)
    {
        const QModelIndex root = m->indexOfConnection(id);
        expand(m, root);
        walk(m, root, { QStringLiteral("Databases"), QStringLiteral("alpha") });
        walk(m, root,
             { QStringLiteral("Databases"), QStringLiteral("gamma"), QStringLiteral("public") });
        expand(m,
               walk(m, root,
                    { QStringLiteral("Databases"), QStringLiteral("alpha"),
                      QStringLiteral("Indexes") }));
        walk(m, root, { QStringLiteral("Users"), QStringLiteral("admin") });
        return root;
    }

private slots:
    void askEveryTimePromptsOnceThenRemembers();
    void askEveryTimeDismissedLeavesItCollapsed();
    void savedPasswordComesFromTheKeychainBackend();
    void testerSeesRootsAddedRenamedRemovedWhileEverythingLoads();
    void testerSeesLoadingErrorRetryAndDisconnect();
    void initialRoots();
    void expandConnectionShowsLoadingThenRows();
    void walksDatabaseFolderTableColumns();
    void brokenFileShowsErrorRowAndRetryRecovers();
    void failedOpenShowsErrorRow();
    void disconnectResetsTheSubtree();
    void connectionsAddRemoveRenameWhileExpanded();
    void staleReplyAfterDisconnectIsIgnored();
    void removedConnectionWithRequestInFlight();
    void tenThousandTablesDoNotBlockTheEventLoop();
    void aChangedHostKeyFailsTheNodeAndNeverConnects();
    void anUnknownHostKeyAsksAndDismissingLeavesTheNodeCollapsed();
    void trustingAnUnknownHostKeyCarriesOnToTheConnection();
    // object tree v2 (H12 phase A)
    void sqliteListsTablesViewsAndIndexesUnderTheConnection();
    void aServerWithSeveralDatabasesHasDatabasesAndUsers();
    void aDatabaseGroupsItsRelationsIntoFolders();
    void aDatabaseOfSchemasKeepsItsSchemas();
    void failingUsersShowTheErrorInThatFolderOnly();
    void revealCrossesTheFolders();
    void testerWalksTheNewTree();
    // tree search
    void searchKeepsMatchesAndTheirAncestors();
    void searchScopesLookAtTheirOwnKind();
    void searchIgnoresCaseAndAccents();
    void searchOnlyLooksAtWhatIsLoaded();
    void searchWaitsForLoadsToEnd();
    void searchGivesWayToReveal();
    void searchKeysRoundTrip();
    void testerSurvivesSearchOnAndOff();
    void searchingTenThousandTablesIsFast();
    // migrations (H12 phase B)
    void migrationsAreSiblingsOfTablesOnlyWhenConfigured();
    void migrationsSetBeforeConnectingAreThereWhenTheTreeLoads();
    void theReportFillsPendingAppliedAndOrphans();
    void aNewReportUpdatesTheFolderKeepingWhatIsOpen();
    void notReadReadingAndFailedSayTheirWord();
    void migrationsAreOfEachDatabase();
    void aFixedDatabaseShowsTheSettingsWithoutDatabase();
    void migrationScopeSaysWhereTheSettingsOfANodeLive();
    void testerSurvivesPerDatabaseMigrations();
    void testerSurvivesMigrationChanges();
    void searchByMigrations();
    void migrationsDoNotNeedTablesLoaded();
};

void TstSchemaTreeModel::testerSeesRootsAddedRenamedRemovedWhileEverythingLoads()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    const auto tester = attachTester(m);
    startController(c);
    const QString idA = addConnection(c, QStringLiteral("Alpha"), newFile(3));
    addConnection(c, QStringLiteral("Bravo"), newFile(2));
    QTRY_COMPARE_WITH_TIMEOUT(m->connectedIds().size(), 2, 20000); // the tester expanded both
    QTRY_COMPARE_WITH_TIMEOUT(m->data(m->index(0, 0), SchemaTreeModel::LoadStateRole).toInt(),
                              int(SchemaTreeModel::Loaded), 20000);
    // Rename in place, rename that moves the row, add before, delete the connected one.
    const QString fileA = m_dir.filePath(QStringLiteral("data-1.db"));
    addConnection(c, QStringLiteral("Alpine"), fileA, idA);
    addConnection(c, QStringLiteral("Zulu"), fileA, idA);
    QTRY_COMPARE(text(m, m->index(1, 0), SchemaTreeModel::NameRole), QStringLiteral("Zulu"));
    addConnection(c, QStringLiteral("Aardvark"), newFile(1));
    QTRY_COMPARE(m->rowCount(), 3);
    c.remove(idA);
    QTRY_COMPARE(m->rowCount(), 2);
    QTest::qWait(200); // let the tester walk what is left
    QCOMPARE(text(m, m->index(0, 0), SchemaTreeModel::NameRole), QStringLiteral("Aardvark"));
}

void TstSchemaTreeModel::testerSeesLoadingErrorRetryAndDisconnect()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    const auto tester = attachTester(m);
    startController(c);
    const QString broken = m_dir.filePath(QStringLiteral("broken-t.db"));
    QVERIFY(QDir().mkdir(broken));
    const QString idBad = addConnection(c, QStringLiteral("Bad"), broken);
    const QString idGood = addConnection(c, QStringLiteral("Good"), newFile(4));
    QTRY_COMPARE_WITH_TIMEOUT(m->data(m->index(0, 0), SchemaTreeModel::LoadStateRole).toInt(),
                              int(SchemaTreeModel::Failed), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(m->isConnected(idGood), 20000);
    // Retry the broken one after fixing it; disconnect the good one (the tester reconnects it).
    QVERIFY(QDir().rmdir(broken));
    QVERIFY(createDatabase(broken, 1));
    m->retry(m->index(0, 0, m->index(0, 0)));
    QTRY_VERIFY_WITH_TIMEOUT(m->isConnected(idBad), 20000);
    m->disconnectConnection(idGood);
    m->disconnectConnection(idBad);
    QTRY_VERIFY_WITH_TIMEOUT(m->isConnected(idGood), 20000);
    QTest::qWait(200);
}

void TstSchemaTreeModel::askEveryTimePromptsOnceThenRemembers()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    const QString id = addProfile(c, QStringLiteral("Ask"), newFile(1), PasswordMode::AskEveryTime);
    const QModelIndex root = m->index(0, 0);
    QSignalSpy prompt(m, &SchemaTreeModel::passwordRequested);
    m->fetchMore(root);
    QTRY_COMPARE(prompt.count(), 1);
    QCOMPARE(prompt.at(0).at(0).toString(), id);
    QCOMPARE(prompt.at(0).at(1).toString(), QStringLiteral("Ask"));
    QCOMPARE(prompt.at(0).at(2).toBool(), false);
    QCOMPARE(m->data(root, SchemaTreeModel::LoadStateRole).toInt(), int(SchemaTreeModel::Loading));
    QVERIFY(!m->isConnected(id)); // nothing opens before the password is given
    m->providePassword(id, QStringLiteral("hunter2"));
    QTRY_COMPARE_WITH_TIMEOUT(m->data(root, SchemaTreeModel::LoadStateRole).toInt(),
                              int(SchemaTreeModel::Loaded), 10000);
    QVERIFY(m->isConnected(id));
    // Remembered for this run: the next connect does not ask again.
    m->disconnectConnection(id);
    expand(m, root);
    QCOMPARE(prompt.count(), 1);
    QVERIFY(m->isConnected(id));
}

void TstSchemaTreeModel::askEveryTimeDismissedLeavesItCollapsed()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    const QString id = addProfile(c, QStringLiteral("Ask"), newFile(1), PasswordMode::AskEveryTime);
    const QModelIndex root = m->index(0, 0);
    QSignalSpy prompt(m, &SchemaTreeModel::passwordRequested);
    QSignalSpy collapse(m, &SchemaTreeModel::collapseRequested);
    m->fetchMore(root);
    QTRY_COMPARE(prompt.count(), 1);
    m->cancelPassword(id);
    QCOMPARE(collapse.count(), 1);
    QCOMPARE(m->rowCount(root), 0);
    QCOMPARE(m->data(root, SchemaTreeModel::LoadStateRole).toInt(), int(SchemaTreeModel::Idle));
    QVERIFY(m->canFetchMore(root)); // expanding again asks again
    m->fetchMore(root);
    QTRY_COMPARE(prompt.count(), 2);
    m->cancelPassword(id);
    // An answer nobody is waiting for is ignored.
    m->providePassword(id, QStringLiteral("late"));
    QTest::qWait(50);
    QVERIFY(!m->isConnected(id));
}

void TstSchemaTreeModel::savedPasswordComesFromTheKeychainBackend()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    // A fake keychain: no real CredentialStore call is ever made.
    bool fail = false;
    QStringList reads;
    CredentialBackend b;
    b.write = [](QUuid, const QString &) {};
    b.remove = [](QUuid) {};
    b.forget = [](QUuid) {};
    b.read = [&](QUuid id) {
        reads << id.toString(QUuid::WithoutBraces);
        QMetaObject::invokeMethod(
            &c,
            [&c, id, &fail] {
                if (fail)
                    c.onCredentialFailed(
                        id,
                        Error { ErrorKind::Unsupported,
                                QStringLiteral("The system keychain is not available."),
                                QStringLiteral("no backend"),
                                {},
                                {} });
                else
                    c.onPasswordRead(id, QStringLiteral("from-keychain"));
            },
            Qt::QueuedConnection);
    };
    c.setCredentialBackend(b);
    startController(c);
    const QString id = addProfile(c, QStringLiteral("Saved"), newFile(1), PasswordMode::Saved);
    const QModelIndex root = m->index(0, 0);

    fail = true;
    m->fetchMore(root);
    QTRY_COMPARE_WITH_TIMEOUT(m->data(root, SchemaTreeModel::LoadStateRole).toInt(),
                              int(SchemaTreeModel::Failed), 10000);
    QCOMPARE(reads, QStringList { id });
    const QModelIndex err = m->index(0, 0, root);
    QCOMPARE(text(m, err, SchemaTreeModel::KindRole), QStringLiteral("error"));
    QCOMPARE(text(m, err, SchemaTreeModel::NameRole),
             QStringLiteral("The system keychain is not available."));
    QCOMPARE(text(m, err, SchemaTreeModel::DetailRole), QStringLiteral("no backend"));

    fail = false;
    m->retry(err);
    QTRY_COMPARE_WITH_TIMEOUT(m->data(root, SchemaTreeModel::LoadStateRole).toInt(),
                              int(SchemaTreeModel::Loaded), 10000);
    QCOMPARE(reads.size(), 2);
    QVERIFY(m->isConnected(id));
}

void TstSchemaTreeModel::initialRoots()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    QCOMPARE(m->rowCount(), 0);
    QVERIFY(!m->hasChildren());

    addConnection(c, QStringLiteral("Bravo"), newFile(1));
    addConnection(c, QStringLiteral("Alpha"), newFile(1));
    QTRY_COMPARE(m->rowCount(), 2);
    const QModelIndex a = m->index(0, 0);
    const QModelIndex b = m->index(1, 0);
    QCOMPARE(text(m, a, SchemaTreeModel::NameRole), QStringLiteral("Alpha"));
    QCOMPARE(text(m, b, SchemaTreeModel::NameRole), QStringLiteral("Bravo"));
    QCOMPARE(text(m, a, SchemaTreeModel::KindRole), QStringLiteral("connection"));
    QCOMPARE(m->data(a, SchemaTreeModel::PositionRole).toInt(), 1);
    QCOMPARE(m->data(b, SchemaTreeModel::PositionRole).toInt(), 2);
    QCOMPARE(m->data(b, SchemaTreeModel::SetSizeRole).toInt(), 2);
    QCOMPARE(m->data(a, SchemaTreeModel::ColorIndexRole).toInt(), 1);
    QVERIFY(m->hasChildren(a));
    QVERIFY(m->canFetchMore(a));
    QCOMPARE(m->rowCount(a), 0); // nothing loaded, nothing connected
    QVERIFY(m->connectedIds().isEmpty());
    // Inserting a connection before the others renumbers them.
    addConnection(c, QStringLiteral("Aardvark"), newFile(1));
    QTRY_COMPARE(m->rowCount(), 3);
    QCOMPARE(m->data(m->index(2, 0), SchemaTreeModel::PositionRole).toInt(), 3);
    QCOMPARE(m->data(m->index(2, 0), SchemaTreeModel::SetSizeRole).toInt(), 3);
}

void TstSchemaTreeModel::expandConnectionShowsLoadingThenRows()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    addConnection(c, QStringLiteral("One"), newFile(3));
    QTRY_COMPARE(m->rowCount(), 1);
    const QModelIndex root = m->index(0, 0);
    QSignalSpy announced(m, &SchemaTreeModel::announcement);

    // The "Loading…" row exists only for a moment: record what is inserted under the root.
    QStringList inserted;
    connect(m, &QAbstractItemModel::rowsInserted, this,
            [&](const QModelIndex &parent, int first, int) {
                if (parent == root)
                    inserted << text(m, m->index(first, 0, parent), SchemaTreeModel::KindRole)
                            + QLatin1Char(':')
                            + text(m, m->index(first, 0, parent), SchemaTreeModel::NameRole);
            });
    m->fetchMore(root);
    QVERIFY(!m->canFetchMore(root)); // a second expand does not start a second request
    QTRY_VERIFY_WITH_TIMEOUT(!inserted.isEmpty(), 10000);
    QCOMPARE(inserted.first(), QStringLiteral("loading:Loading…")); // one synthetic row first
    QVERIFY(!announced.isEmpty());
    QCOMPARE(announced.at(0).at(0).toString(), QStringLiteral("Loading…"));

    QTRY_COMPARE_WITH_TIMEOUT(m->data(root, SchemaTreeModel::LoadStateRole).toInt(),
                              int(SchemaTreeModel::Loaded), 10000);
    // SQLite's single database "main" is not a row: Tables, Views and Indexes hang from the
    // connection (and no Databases or Users folder).
    QCOMPARE(m->rowCount(root), 3);
    const QModelIndex tables = m->index(0, 0, root);
    QCOMPARE(text(m, tables, SchemaTreeModel::NameRole), QStringLiteral("Tables"));
    QCOMPARE(text(m, tables, SchemaTreeModel::KindRole), QStringLiteral("tablesFolder"));
    QCOMPARE(text(m, m->index(1, 0, root), SchemaTreeModel::NameRole), QStringLiteral("Views"));
    QCOMPARE(text(m, m->index(1, 0, root), SchemaTreeModel::KindRole),
             QStringLiteral("viewsFolder"));
    QCOMPARE(text(m, m->index(2, 0, root), SchemaTreeModel::NameRole), QStringLiteral("Indexes"));
    QCOMPARE(text(m, m->index(2, 0, root), SchemaTreeModel::KindRole),
             QStringLiteral("indexesFolder"));
    for (int r = 0; r < m->rowCount(root); ++r)
        QVERIFY(text(m, m->index(r, 0, root), SchemaTreeModel::KindRole)
                != QStringLiteral("database"));
    QVERIFY(m->data(root, SchemaTreeModel::ConnectedRole).toBool());
    QCOMPARE(m->connectedIds().size(), 1);
    QVERIFY(m->isConnected(text(m, root, SchemaTreeModel::ConnectionIdRole)));
    QCOMPARE(m->connectionOf(tables), root);
}

void TstSchemaTreeModel::walksDatabaseFolderTableColumns()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    addConnection(c, QStringLiteral("Walk"), newFile(3));
    QTRY_COMPARE(m->rowCount(), 1);
    const QModelIndex root = m->index(0, 0);
    expand(m, root);
    QCOMPARE(m->rowCount(root), 3);
    const QModelIndex tables = child(m, root, QStringLiteral("Tables"));
    QCOMPARE(text(m, tables, SchemaTreeModel::KindRole), QStringLiteral("tablesFolder"));
    QCOMPARE(m->data(tables, SchemaTreeModel::SetSizeRole).toInt(), 3);
    QVERIFY(text(m, tables, SchemaTreeModel::DetailRole).isEmpty()); // not counted until loaded
    expand(m, tables);
    QCOMPARE(m->rowCount(tables), 3);
    QCOMPARE(text(m, tables, SchemaTreeModel::DetailRole), QStringLiteral("3"));
    const QModelIndex t0 = child(m, tables, QStringLiteral("t0"));
    QCOMPARE(text(m, t0, SchemaTreeModel::KindRole), QStringLiteral("table"));
    QCOMPARE(m->data(t0, SchemaTreeModel::PositionRole).toInt(), 1);
    QCOMPARE(m->data(t0, SchemaTreeModel::SetSizeRole).toInt(), 3);
    QCOMPARE(m->parent(t0), tables);
    QCOMPARE(m->connectionIdOf(t0), m->connectionIdOf(root));
    // The level is hidden, not the path: opening data and "show in tree" still use {main, table}.
    const QVariantMap object = m->objectOf(t0);
    QVERIFY(object.value(QStringLiteral("openable")).toBool());
    QCOMPARE(object.value(QStringLiteral("parts")).toStringList(),
             (QStringList { QStringLiteral("main"), QStringLiteral("t0") }));
    QSignalSpy revealed(m, &SchemaTreeModel::revealed);
    m->reveal(m->connectionIdOf(root), { QStringLiteral("main"), QStringLiteral("t1") });
    QTRY_COMPARE(revealed.count(), 1);
    QCOMPARE(text(m, revealed.at(0).at(0).value<QModelIndex>(), SchemaTreeModel::NameRole),
             QStringLiteral("t1"));
    QCOMPARE(revealed.at(0).at(1).toList().size(), 2); // the connection and Tables
    expand(m, t0);
    QCOMPARE(m->rowCount(t0), 3); // Columns, Indexes, Foreign keys
    const QModelIndex columns = child(m, t0, QStringLiteral("Columns"));
    expand(m, columns);
    QCOMPARE(m->rowCount(columns), 2); // id, v
    const QModelIndex id = m->index(0, 0, columns);
    QCOMPARE(text(m, id, SchemaTreeModel::KindRole), QStringLiteral("column"));
    QVERIFY(!m->hasChildren(id)); // leaves have no expander
    QVERIFY(!m->canFetchMore(id));
    QVERIFY(!text(m, id, SchemaTreeModel::DetailRole).isEmpty());
}

void TstSchemaTreeModel::brokenFileShowsErrorRowAndRetryRecovers()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    // A directory where the file should be: the open itself fails (until the directory goes).
    const QString file = m_dir.filePath(QStringLiteral("broken.db"));
    QVERIFY(QDir().mkdir(file));
    addConnection(c, QStringLiteral("Broken"), file);
    QTRY_COMPARE(m->rowCount(), 1);
    const QModelIndex root = m->index(0, 0);
    QSignalSpy announced(m, &SchemaTreeModel::announcement);
    m->fetchMore(root);
    QTRY_COMPARE_WITH_TIMEOUT(m->data(root, SchemaTreeModel::LoadStateRole).toInt(),
                              int(SchemaTreeModel::Failed), 10000);
    QCOMPARE(m->rowCount(root), 1);
    const QModelIndex err = m->index(0, 0, root);
    QCOMPARE(text(m, err, SchemaTreeModel::KindRole), QStringLiteral("error"));
    QVERIFY(!text(m, err, SchemaTreeModel::NameRole).isEmpty());
    QVERIFY(!text(m, err, SchemaTreeModel::DetailRole).isEmpty()); // technical text for the details
    QVERIFY(!m->hasChildren(err));
    QVERIFY(!m->data(root, SchemaTreeModel::ConnectedRole).toBool());
    QVERIFY(!m->canFetchMore(root)); // only retry() starts it again
    QCOMPARE(announced.last().at(1).toBool(), true); // errors are assertive

    // Fix the file, then Retry from the error row.
    QVERIFY(QDir().rmdir(file));
    QVERIFY(createDatabase(file, 2));
    m->retry(err);
    QVERIFY(!m->canFetchMore(root));
    QTRY_COMPARE_WITH_TIMEOUT(m->data(root, SchemaTreeModel::LoadStateRole).toInt(),
                              int(SchemaTreeModel::Loaded), 10000);
    QCOMPARE(text(m, m->index(0, 0, root), SchemaTreeModel::NameRole), QStringLiteral("Tables"));
    QVERIFY(m->data(root, SchemaTreeModel::ConnectedRole).toBool());
}

void TstSchemaTreeModel::failedOpenShowsErrorRow()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    // A directory is not a database file: the open itself fails.
    addConnection(c, QStringLiteral("Dir"), m_dir.path());
    QTRY_COMPARE(m->rowCount(), 1);
    const QModelIndex root = m->index(0, 0);
    m->fetchMore(root);
    QTRY_COMPARE_WITH_TIMEOUT(m->data(root, SchemaTreeModel::LoadStateRole).toInt(),
                              int(SchemaTreeModel::Failed), 10000);
    const QModelIndex err = m->index(0, 0, root);
    QCOMPARE(text(m, err, SchemaTreeModel::KindRole), QStringLiteral("error"));
    // The open error, not the generic "The connection is not open." of the queued request.
    QVERIFY2(text(m, err, SchemaTreeModel::NameRole)
                 != QStringLiteral("The connection is not open."),
             qPrintable(text(m, err, SchemaTreeModel::NameRole)));
    QVERIFY(!text(m, err, SchemaTreeModel::DetailRole).isEmpty());
    QTest::qWait(100); // the generic answer arrives later and must not replace it
    QCOMPARE(m->rowCount(root), 1);
    QCOMPARE(m->data(root, SchemaTreeModel::LoadStateRole).toInt(), int(SchemaTreeModel::Failed));
}

void TstSchemaTreeModel::disconnectResetsTheSubtree()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    addConnection(c, QStringLiteral("Disc"), newFile(2));
    QTRY_COMPARE(m->rowCount(), 1);
    const QModelIndex root = m->index(0, 0);
    expand(m, root);
    expand(m, m->index(0, 0, root));
    QSignalSpy collapse(m, &SchemaTreeModel::collapseRequested);
    QSignalSpy connectedSpy(m, &SchemaTreeModel::connectedChanged);

    m->disconnectConnection(text(m, root, SchemaTreeModel::ConnectionIdRole));
    QCOMPARE(collapse.count(), 1);
    QCOMPARE(collapse.at(0).at(0).toModelIndex(), root);
    QCOMPARE(m->rowCount(root), 0);
    QVERIFY(m->hasChildren(root)); // the expander stays
    QVERIFY(m->canFetchMore(root));
    QVERIFY(!m->data(root, SchemaTreeModel::ConnectedRole).toBool());
    QVERIFY(m->connectedIds().isEmpty());
    QVERIFY(connectedSpy.count() >= 1);

    // And it connects again.
    expand(m, root);
    QCOMPARE(m->rowCount(root), 3);
    QVERIFY(m->data(root, SchemaTreeModel::ConnectedRole).toBool());
}

void TstSchemaTreeModel::connectionsAddRemoveRenameWhileExpanded()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    const QString idA = addConnection(c, QStringLiteral("Alpha"), newFile(2));
    addConnection(c, QStringLiteral("Bravo"), newFile(2));
    QTRY_COMPARE(m->rowCount(), 2);
    expand(m, m->index(0, 0));
    QVERIFY(m->isConnected(idA));

    // Rename without moving: same row, still connected, children kept.
    addConnection(c, QStringLiteral("Alpine"), m_dir.filePath(QStringLiteral("data-1.db")), idA);
    QTRY_COMPARE(text(m, m->index(0, 0), SchemaTreeModel::NameRole), QStringLiteral("Alpine"));
    QVERIFY(m->isConnected(idA));
    QCOMPARE(m->rowCount(m->index(0, 0)), 3);

    // Rename that moves the row (Alpha -> Zulu): the open session and its subtree follow it.
    addConnection(c, QStringLiteral("Zulu"), m_dir.filePath(QStringLiteral("data-1.db")), idA);
    QTRY_COMPARE(text(m, m->index(1, 0), SchemaTreeModel::NameRole), QStringLiteral("Zulu"));
    QTest::qWait(50); // the parked node would be closed here if it had not been reclaimed
    QVERIFY(m->isConnected(idA));
    QCOMPARE(m->rowCount(m->index(1, 0)), 3);
    QCOMPARE(m->data(m->index(1, 0), SchemaTreeModel::PositionRole).toInt(), 2);
    QCOMPARE(m->data(m->index(0, 0), SchemaTreeModel::PositionRole).toInt(), 1);

    // Delete the connected one: its session is closed and the row is gone.
    c.remove(idA);
    QTRY_COMPARE(m->rowCount(), 1);
    QTRY_VERIFY(m->connectedIds().isEmpty());
    QCOMPARE(text(m, m->index(0, 0), SchemaTreeModel::NameRole), QStringLiteral("Bravo"));
    QCOMPARE(m->data(m->index(0, 0), SchemaTreeModel::SetSizeRole).toInt(), 1);
}

void TstSchemaTreeModel::staleReplyAfterDisconnectIsIgnored()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    addConnection(c, QStringLiteral("Stale"), newFile(5));
    QTRY_COMPARE(m->rowCount(), 1);
    const QModelIndex root = m->index(0, 0);
    const QString id = text(m, root, SchemaTreeModel::ConnectionIdRole);

    // Disconnect while the connection request is in flight.
    m->fetchMore(root);
    m->disconnectConnection(id);
    QTest::qWait(300);
    QCOMPARE(m->rowCount(root), 0);
    QCOMPARE(m->data(root, SchemaTreeModel::LoadStateRole).toInt(), int(SchemaTreeModel::Idle));
    QVERIFY(!m->isConnected(id));

    // Disconnect while a deeper request is in flight.
    expand(m, root);
    const QModelIndex tables = child(m, root, QStringLiteral("Tables"));
    m->fetchMore(tables);
    m->disconnectConnection(id);
    QTest::qWait(300);
    QCOMPARE(m->rowCount(root), 0);
    QVERIFY(!m->isConnected(id));
    // A fresh connection is not confused by the old session's answers.
    expand(m, root);
    QCOMPARE(m->rowCount(root), 3);
}

void TstSchemaTreeModel::removedConnectionWithRequestInFlight()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    const QString id = addConnection(c, QStringLiteral("Gone"), newFile(5));
    addConnection(c, QStringLiteral("Stays"), newFile(1));
    QTRY_COMPARE(m->rowCount(), 2);
    m->fetchMore(m->index(0, 0));
    c.remove(id);
    QTRY_COMPARE(m->rowCount(), 1);
    QTest::qWait(300); // the answer for the removed node arrives and is dropped
    QCOMPARE(text(m, m->index(0, 0), SchemaTreeModel::NameRole), QStringLiteral("Stays"));
    QCOMPARE(m->rowCount(m->index(0, 0)), 0);
}

void TstSchemaTreeModel::tenThousandTablesDoNotBlockTheEventLoop()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    addConnection(c, QStringLiteral("Big"), newFile(10'000));
    QTRY_COMPARE(m->rowCount(), 1);
    const QModelIndex root = m->index(0, 0);
    expand(m, root);
    const QModelIndex tables = child(m, root, QStringLiteral("Tables"));

    qint64 maxGap = 0;
    QElapsedTimer sinceTick;
    QTimer ticker;
    ticker.setInterval(5);
    connect(&ticker, &QTimer::timeout, this,
            [&] { maxGap = std::max(maxGap, sinceTick.restart()); });
    sinceTick.start();
    ticker.start();
    // A machine that stalls this long while idle (macOS CI runners) cannot measure a 50 ms budget.
    QTest::qWait(500);
    if (maxGap > 25)
        QSKIP(qPrintable(
            QStringLiteral("idle event-loop gap %1 ms: too noisy to measure").arg(maxGap)));
    maxGap = 0;
    QElapsedTimer clock;
    clock.start();
    m->fetchMore(tables);
    QTRY_COMPARE_WITH_TIMEOUT(m->data(tables, SchemaTreeModel::LoadStateRole).toInt(),
                              int(SchemaTreeModel::Loaded), 30000);
    const qint64 elapsed = clock.elapsed();
    ticker.stop();
    qInfo() << "expanding 10000 tables:" << elapsed << "ms, max event-loop gap" << maxGap << "ms";
    QCOMPARE(m->rowCount(tables), 10'000);
    QCOMPARE(m->data(m->index(9'999, 0, tables), SchemaTreeModel::SetSizeRole).toInt(), 10'000);
    QVERIFY2(maxGap <= 50, qPrintable(QString::number(maxGap)));
}

namespace {

// A host key checker without a server; the SSH secret is never needed (agent).
struct FakeChecker
{
    explicit FakeChecker(ConnectionPreflight &preflight, HostKeyStatus found)
        : p(preflight)
        , status(found)
    {
        HostKeyBackend h;
        h.check = [this](const SshTunnelConfig &) {
            ++checks;
            QMetaObject::invokeMethod(
                &p,
                [this] {
                    HostKeyInfo info;
                    info.status = status;
                    info.keyType = QStringLiteral("ssh-ed25519");
                    info.fingerprint = QStringLiteral("SHA256:received");
                    info.knownFingerprint = QStringLiteral("SHA256:known");
                    p.onHostKeyChecked(info);
                },
                Qt::QueuedConnection);
        };
        h.cancel = [] {};
        h.accept = [this](const HostKeyInfo &info, bool) { p.onHostKeyAccepted(info); };
        p.setHostKeyBackend(h);
    }

    ConnectionPreflight &p;
    HostKeyStatus status;
    int checks = 0;
};

} // namespace

// The SQLite engine has no tunnel; the profile asks for one anyway so the tree takes the SSH
// path. When the fingerprint passes, the registry is what refuses (Unsupported).
static QString addTunnelled(ConnectionController &c, const QString &file)
{
    ConnectionProfile p;
    p.id = QUuid::createUuid();
    p.name = QStringLiteral("Tunnelled");
    p.providerId = QStringLiteral("sqlite");
    p.values = { { QStringLiteral("file"), file },
                 { QStringLiteral("ssh.enabled"), true },
                 { QStringLiteral("ssh.host"), QStringLiteral("bastion") },
                 { QStringLiteral("ssh.user"), QStringLiteral("deploy") },
                 { QStringLiteral("ssh.auth"), QStringLiteral("agent") } };
    c.model()->store()->save(p);
    const QString id = p.id.toString(QUuid::WithoutBraces);
    (void)QTest::qWaitFor([&] { return c.model()->indexOf(id) >= 0; }, 10000);
    return id;
}

void TstSchemaTreeModel::aChangedHostKeyFailsTheNodeAndNeverConnects()
{
    ConnectionController c;
    FakeChecker checker(*c.preflight(), HostKeyStatus::Mismatch);
    SchemaTreeModel *m = c.tree();
    startController(c);
    const QString id = addTunnelled(c, newFile(1));
    QVERIFY(!id.isEmpty());
    const QModelIndex root = m->indexOfConnection(id);
    QVERIFY(m->canFetchMore(root));
    m->fetchMore(root);
    QTRY_COMPARE_WITH_TIMEOUT(m->data(root, SchemaTreeModel::LoadStateRole).toInt(),
                              int(SchemaTreeModel::Failed), 10000);
    const QModelIndex row = m->index(0, 0, root);
    QCOMPARE(text(m, row, SchemaTreeModel::KindRole), QStringLiteral("error"));
    QVERIFY(
        text(m, row, SchemaTreeModel::NameRole).contains(QStringLiteral("host key has changed")));
    QVERIFY(!m->isConnected(id));
    QCOMPARE(checker.checks, 1);
}

void TstSchemaTreeModel::anUnknownHostKeyAsksAndDismissingLeavesTheNodeCollapsed()
{
    ConnectionController c;
    FakeChecker checker(*c.preflight(), HostKeyStatus::Unknown);
    SchemaTreeModel *m = c.tree();
    startController(c);
    const QString id = addTunnelled(c, newFile(1));
    const QModelIndex root = m->indexOfConnection(id);
    QSignalSpy asked(c.preflight(), &ConnectionPreflight::hostKeyRequested);
    QSignalSpy collapse(m, &SchemaTreeModel::collapseRequested);
    m->fetchMore(root);
    QTRY_COMPARE_WITH_TIMEOUT(asked.count(), 1, 10000);
    QCOMPARE(m->data(root, SchemaTreeModel::LoadStateRole).toInt(), int(SchemaTreeModel::Loading));

    c.preflight()->cancelHostKey();
    QCOMPARE(collapse.count(), 1);
    QCOMPARE(m->data(root, SchemaTreeModel::LoadStateRole).toInt(), int(SchemaTreeModel::Idle));
    QVERIFY(m->canFetchMore(root)); // the user can try again
}

void TstSchemaTreeModel::trustingAnUnknownHostKeyCarriesOnToTheConnection()
{
    ConnectionController c;
    FakeChecker checker(*c.preflight(), HostKeyStatus::Unknown);
    SchemaTreeModel *m = c.tree();
    startController(c);
    const QString id = addTunnelled(c, newFile(1));
    const QModelIndex root = m->indexOfConnection(id);
    QSignalSpy asked(c.preflight(), &ConnectionPreflight::hostKeyRequested);
    m->fetchMore(root);
    QTRY_COMPARE_WITH_TIMEOUT(asked.count(), 1, 10000);
    c.preflight()->trustHostKey(false);
    // The registry refuses a tunnel for an engine without one: the node failed there, not before.
    QTRY_COMPARE_WITH_TIMEOUT(m->data(root, SchemaTreeModel::LoadStateRole).toInt(),
                              int(SchemaTreeModel::Failed), 10000);
    QVERIFY(!text(m, m->index(0, 0, root), SchemaTreeModel::NameRole)
                 .contains(QStringLiteral("host key")));
}

// ---- object tree v2 -----------------------------------------------------------------------------

void TstSchemaTreeModel::sqliteListsTablesViewsAndIndexesUnderTheConnection()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    addConnection(c, QStringLiteral("Lite"), newFile(2));
    QTRY_COMPARE(m->rowCount(), 1);
    const QModelIndex root = m->index(0, 0);
    expand(m, root);
    QCOMPARE(names(m, root),
             (QStringList { QStringLiteral("Tables"), QStringLiteral("Views"),
                            QStringLiteral("Indexes") }));
    const QModelIndex indexes = m->index(2, 0, root);
    // Indexes are only asked for when the folder opens.
    QCOMPARE(m->data(indexes, SchemaTreeModel::LoadStateRole).toInt(), int(SchemaTreeModel::Idle));
    QVERIFY(m->canFetchMore(indexes));
    QVERIFY(text(m, indexes, SchemaTreeModel::DetailRole).isEmpty());
    expand(m, indexes);
    QCOMPARE(m->rowCount(indexes), 1);
    const QModelIndex idx = m->index(0, 0, indexes);
    QCOMPARE(text(m, idx, SchemaTreeModel::NameRole), QStringLiteral("t0_v_idx"));
    QCOMPARE(text(m, idx, SchemaTreeModel::KindRole), QStringLiteral("index"));
    QVERIFY(text(m, idx, SchemaTreeModel::DetailRole).contains(QStringLiteral("t0")));
    QVERIFY(!m->hasChildren(idx));
    QCOMPARE(text(m, indexes, SchemaTreeModel::DetailRole), QStringLiteral("1"));
    QVERIFY(!m->objectOf(idx).value(QStringLiteral("openable")).toBool());
    QVERIFY(!m->objectOf(indexes).value(QStringLiteral("openable")).toBool());

    const QModelIndex views = m->index(1, 0, root);
    expand(m, views);
    QCOMPARE(names(m, views), QStringList { QStringLiteral("v1") });
    QCOMPARE(text(m, views, SchemaTreeModel::DetailRole), QStringLiteral("1"));
    QCOMPARE(text(m, m->index(0, 0, views), SchemaTreeModel::KindRole), QStringLiteral("view"));
    QVERIFY(m->objectOf(m->index(0, 0, views)).value(QStringLiteral("openable")).toBool());
}

void TstSchemaTreeModel::aServerWithSeveralDatabasesHasDatabasesAndUsers()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startFake(c);
    const QString id = addFakeTree(c, QStringLiteral("Fake"));
    const QModelIndex root = m->indexOfConnection(id);
    expand(m, root);
    QCOMPARE(names(m, root),
             (QStringList { QStringLiteral("Databases"), QStringLiteral("Users") }));
    const QModelIndex databases = m->index(0, 0, root);
    QCOMPARE(text(m, databases, SchemaTreeModel::KindRole), QStringLiteral("databasesFolder"));
    QCOMPARE(text(m, databases, SchemaTreeModel::DetailRole), QStringLiteral("3"));
    // Already there: no request to open it.
    QCOMPARE(m->data(databases, SchemaTreeModel::LoadStateRole).toInt(),
             int(SchemaTreeModel::Loaded));
    QVERIFY(!m->canFetchMore(databases));
    QCOMPARE(
        names(m, databases),
        (QStringList { QStringLiteral("alpha"), QStringLiteral("beta"), QStringLiteral("gamma") }));
    const QModelIndex alpha = m->index(0, 0, databases);
    QCOMPARE(text(m, alpha, SchemaTreeModel::KindRole), QStringLiteral("database"));
    QCOMPARE(m->data(alpha, SchemaTreeModel::PositionRole).toInt(), 1);
    QCOMPARE(m->data(alpha, SchemaTreeModel::SetSizeRole).toInt(), 3);
    QCOMPARE(m->connectionIdOf(alpha), id);

    // Users: asked for when the folder opens.
    const QModelIndex users = m->index(1, 0, root);
    QCOMPARE(text(m, users, SchemaTreeModel::KindRole), QStringLiteral("usersFolder"));
    QVERIFY(m->canFetchMore(users));
    QVERIFY(text(m, users, SchemaTreeModel::DetailRole).isEmpty());
    expand(m, users);
    QCOMPARE(names(m, users), (QStringList { QStringLiteral("admin"), QStringLiteral("reader") }));
    const QModelIndex admin = m->index(0, 0, users);
    QCOMPARE(text(m, admin, SchemaTreeModel::KindRole), QStringLiteral("user"));
    QCOMPARE(text(m, admin, SchemaTreeModel::DetailRole), QStringLiteral("superuser"));
    QVERIFY(!m->hasChildren(admin));
    QVERIFY(!m->canFetchMore(admin));
    QCOMPARE(text(m, users, SchemaTreeModel::DetailRole), QStringLiteral("2"));
    QVERIFY(!m->objectOf(admin).value(QStringLiteral("openable")).toBool());
}

void TstSchemaTreeModel::aDatabaseGroupsItsRelationsIntoFolders()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startFake(c);
    const QString id = addFakeTree(c, QStringLiteral("Fake"));
    const QModelIndex root = m->indexOfConnection(id);
    const QModelIndex alpha
        = walk(m, root, { QStringLiteral("Databases"), QStringLiteral("alpha") });
    QVERIFY(alpha.isValid());
    expand(m, alpha);
    QCOMPARE(names(m, alpha),
             (QStringList { QStringLiteral("Tables"), QStringLiteral("Views"),
                            QStringLiteral("Indexes"), QStringLiteral("Other") }));
    const QModelIndex tables = m->index(0, 0, alpha);
    const QModelIndex views = m->index(1, 0, alpha);
    const QModelIndex indexes = m->index(2, 0, alpha);
    const QModelIndex other = m->index(3, 0, alpha);
    // Tables, Views and Other are grouped in memory: counted and loaded at once.
    QCOMPARE(text(m, tables, SchemaTreeModel::DetailRole), QStringLiteral("2"));
    QCOMPARE(names(m, tables),
             (QStringList { QStringLiteral("customers"), QStringLiteral("orders") }));
    QCOMPARE(names(m, views),
             (QStringList { QStringLiteral("recent_orders"), QStringLiteral("totals") }));
    QCOMPARE(text(m, m->index(1, 0, views), SchemaTreeModel::KindRole),
             QStringLiteral("materializedView"));
    QCOMPARE(names(m, other), QStringList { QStringLiteral("seq_orders") });
    QCOMPARE(text(m, other, SchemaTreeModel::KindRole), QStringLiteral("otherFolder"));
    // The paths of tables and views did not change: open data, reveal and export still use them.
    const QVariantMap orders = m->objectOf(m->index(1, 0, tables));
    QVERIFY(orders.value(QStringLiteral("openable")).toBool());
    QCOMPARE(orders.value(QStringLiteral("parts")).toStringList(),
             (QStringList { QStringLiteral("alpha"), QStringLiteral("orders") }));
    QVERIFY(!m->objectOf(tables).value(QStringLiteral("openable")).toBool());
    QVERIFY(!m->objectOf(m->index(0, 0, other)).value(QStringLiteral("openable")).toBool());
    QCOMPARE(m->data(m->index(1, 0, tables), SchemaTreeModel::PositionRole).toInt(), 2);
    QCOMPARE(m->data(m->index(1, 0, tables), SchemaTreeModel::SetSizeRole).toInt(), 2);

    // Indexes: the provider answers for the whole database, only when the folder opens.
    QCOMPARE(m->data(indexes, SchemaTreeModel::LoadStateRole).toInt(), int(SchemaTreeModel::Idle));
    expand(m, indexes);
    QCOMPARE(
        names(m, indexes),
        (QStringList { QStringLiteral("customers_name_idx"), QStringLiteral("orders_date_idx") }));
    QCOMPARE(text(m, indexes, SchemaTreeModel::DetailRole), QStringLiteral("2"));
    QCOMPARE(text(m, m->index(1, 0, indexes), SchemaTreeModel::DetailRole),
             QStringLiteral("orders (placed_on) unique"));

    // A database with nothing in it still has its folders, each saying "(empty)".
    const QModelIndex beta = walk(m, root, { QStringLiteral("Databases"), QStringLiteral("beta") });
    expand(m, beta);
    QCOMPARE(names(m, beta),
             (QStringList { QStringLiteral("Tables"), QStringLiteral("Views"),
                            QStringLiteral("Indexes") }));
    QCOMPARE(text(m, m->index(0, 0, beta), SchemaTreeModel::DetailRole), QStringLiteral("0"));
    QCOMPARE(text(m, m->index(0, 0, m->index(0, 0, beta)), SchemaTreeModel::KindRole),
             QStringLiteral("empty"));
}

void TstSchemaTreeModel::aDatabaseOfSchemasKeepsItsSchemas()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startFake(c);
    const QString id = addFakeTree(c, QStringLiteral("Fake"));
    const QModelIndex root = m->indexOfConnection(id);
    const QModelIndex gamma
        = walk(m, root, { QStringLiteral("Databases"), QStringLiteral("gamma") });
    expand(m, gamma);
    // No folders above the schema: they are inside it.
    QCOMPARE(names(m, gamma), QStringList { QStringLiteral("public") });
    const QModelIndex schema = m->index(0, 0, gamma);
    QCOMPARE(text(m, schema, SchemaTreeModel::KindRole), QStringLiteral("schema"));
    expand(m, schema);
    QCOMPARE(names(m, schema),
             (QStringList { QStringLiteral("Tables"), QStringLiteral("Views"),
                            QStringLiteral("Indexes") }));
    const QModelIndex accounts = m->index(0, 0, m->index(0, 0, schema));
    QCOMPARE(m->objectOf(accounts).value(QStringLiteral("parts")).toStringList(),
             (QStringList { QStringLiteral("gamma"), QStringLiteral("public"),
                            QStringLiteral("accounts") }));
    const QModelIndex indexes = m->index(2, 0, schema);
    expand(m, indexes);
    QCOMPARE(names(m, indexes), QStringList { QStringLiteral("accounts_pkey") });
}

void TstSchemaTreeModel::failingUsersShowTheErrorInThatFolderOnly()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startFake(c);
    const QString id = addFakeTree(c, QStringLiteral("Fake"), QStringLiteral("fail"));
    const QModelIndex root = m->indexOfConnection(id);
    expand(m, root);
    const QModelIndex users = m->index(1, 0, root);
    m->fetchMore(users);
    QTRY_COMPARE_WITH_TIMEOUT(m->data(users, SchemaTreeModel::LoadStateRole).toInt(),
                              int(SchemaTreeModel::Failed), 10000);
    const QModelIndex err = m->index(0, 0, users);
    QCOMPARE(text(m, err, SchemaTreeModel::KindRole), QStringLiteral("error"));
    QCOMPARE(text(m, err, SchemaTreeModel::NameRole),
             QStringLiteral("Not allowed to list the users."));
    QCOMPARE(text(m, err, SchemaTreeModel::DetailRole),
             QStringLiteral("permission denied for table users"));
    // The rest of the tree is untouched, and the connection stays connected.
    QVERIFY(m->isConnected(id));
    QCOMPARE(m->data(root, SchemaTreeModel::LoadStateRole).toInt(), int(SchemaTreeModel::Loaded));
    QCOMPARE(m->rowCount(m->index(0, 0, root)), 3);
    // Retry asks again (and fails again here).
    QSignalSpy invalidated(m, &SchemaTreeModel::metadataInvalidated);
    m->retry(err);
    QCOMPARE(invalidated.count(), 1);
    QCOMPARE(m->data(users, SchemaTreeModel::LoadStateRole).toInt(), int(SchemaTreeModel::Loading));
    QTRY_COMPARE_WITH_TIMEOUT(m->data(users, SchemaTreeModel::LoadStateRole).toInt(),
                              int(SchemaTreeModel::Failed), 10000);
    QCOMPARE(text(m, m->index(0, 0, users), SchemaTreeModel::KindRole), QStringLiteral("error"));
}

void TstSchemaTreeModel::revealCrossesTheFolders()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startFake(c);
    const QString id = addFakeTree(c, QStringLiteral("Fake"));
    const QModelIndex root = m->indexOfConnection(id);
    expand(m, root);
    QSignalSpy revealed(m, &SchemaTreeModel::revealed);
    m->reveal(id, { QStringLiteral("alpha"), QStringLiteral("orders") });
    QTRY_COMPARE_WITH_TIMEOUT(revealed.count(), 1, 10000);
    const QModelIndex orders = revealed.at(0).at(0).toModelIndex();
    QCOMPARE(text(m, orders, SchemaTreeModel::NameRole), QStringLiteral("orders"));
    const QVariantList chain = revealed.at(0).at(1).toList();
    QCOMPARE(chain.size(), 4); // connection, Databases, alpha, Tables
    QCOMPARE(text(m, chain.at(1).toModelIndex(), SchemaTreeModel::NameRole),
             QStringLiteral("Databases"));
    QCOMPARE(text(m, chain.at(3).toModelIndex(), SchemaTreeModel::NameRole),
             QStringLiteral("Tables"));
    // A materialized view is in the Views folder; a table of a schema goes through the schema.
    m->reveal(id, { QStringLiteral("alpha"), QStringLiteral("totals") });
    QTRY_COMPARE_WITH_TIMEOUT(revealed.count(), 2, 10000);
    const QModelIndex totals = revealed.at(1).at(0).toModelIndex();
    QCOMPARE(text(m, totals, SchemaTreeModel::KindRole), QStringLiteral("materializedView"));
    QCOMPARE(text(m, m->parent(totals), SchemaTreeModel::NameRole), QStringLiteral("Views"));
    m->reveal(id,
              { QStringLiteral("gamma"), QStringLiteral("public"), QStringLiteral("accounts") });
    QTRY_COMPARE_WITH_TIMEOUT(revealed.count(), 3, 10000);
    QCOMPARE(revealed.at(2).at(1).toList().size(),
             5); // connection, Databases, gamma, public, Tables
    // Looking for a table never opened Users or Indexes.
    QCOMPARE(m->data(m->index(1, 0, root), SchemaTreeModel::LoadStateRole).toInt(),
             int(SchemaTreeModel::Idle));
    const QModelIndex alpha = m->parent(m->parent(orders));
    QCOMPARE(m->data(m->index(2, 0, alpha), SchemaTreeModel::LoadStateRole).toInt(),
             int(SchemaTreeModel::Idle));
    // No such table: said, not hung.
    QSignalSpy said(m, &SchemaTreeModel::announcement);
    m->reveal(id, { QStringLiteral("alpha"), QStringLiteral("nope") });
    QTRY_VERIFY_WITH_TIMEOUT(!said.isEmpty(), 10000);
    QCOMPARE(said.last().at(0).toString(), QStringLiteral("nope was not found in the tree"));
}

void TstSchemaTreeModel::testerWalksTheNewTree()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    const auto tester = attachTester(m);
    startFake(c);
    const QString id = addFakeTree(c, QStringLiteral("Fake"));
    addFakeTree(c, QStringLiteral("Broken users"), QStringLiteral("fail"));
    // The tester opens every node it sees: connections, Users, every database, every Indexes.
    QTRY_COMPARE_WITH_TIMEOUT(m->connectedIds().size(), 2, 20000);
    QTest::qWait(500);
    const QModelIndex root = m->indexOfConnection(id);
    QVERIFY(walk(m, root,
                 { QStringLiteral("Databases"), QStringLiteral("alpha"), QStringLiteral("Indexes"),
                   QStringLiteral("orders_date_idx") })
                .isValid());
    m->disconnectConnection(id);
    QTest::qWait(200);
}

// ---- tree search --------------------------------------------------------------------------------

void TstSchemaTreeModel::searchKeepsMatchesAndTheirAncestors()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startFake(c);
    const QString id = addFakeTree(c, QStringLiteral("Fake"));
    const QModelIndex root = loadAllFake(m, id);
    QSignalSpy reset(m, &QAbstractItemModel::modelReset);

    m->setFilter(QStringLiteral("order"), SchemaTreeModel::Everything);
    QVERIFY(m->filterActive());
    QCOMPARE(reset.count(), 1);
    // orders, recent_orders, seq_orders and orders_date_idx.
    QCOMPARE(m->filterCount(), 4);
    QVERIFY(m->filterIncomplete()); // beta was never opened
    QCOMPARE(m->rowCount(), 1);
    QCOMPARE(m->rowCount(root), 1); // only Databases: Users has no match
    const QModelIndex databases = m->index(0, 0, root);
    QCOMPARE(names(m, databases), QStringList { QStringLiteral("alpha") });
    const QModelIndex alpha = m->index(0, 0, databases);
    QCOMPARE(names(m, alpha),
             (QStringList { QStringLiteral("Tables"), QStringLiteral("Views"),
                            QStringLiteral("Indexes"), QStringLiteral("Other") }));
    QCOMPARE(names(m, m->index(0, 0, alpha)), QStringList { QStringLiteral("orders") });
    QCOMPARE(names(m, m->index(1, 0, alpha)), QStringList { QStringLiteral("recent_orders") });
    const QModelIndex orders = m->index(0, 0, m->index(0, 0, alpha));
    // Position and size are among what is shown, so the screen reader says "1 of 1".
    QCOMPARE(m->data(orders, SchemaTreeModel::PositionRole).toInt(), 1);
    QCOMPARE(m->data(orders, SchemaTreeModel::SetSizeRole).toInt(), 1);
    QCOMPARE(m->data(m->index(1, 0, alpha), SchemaTreeModel::PositionRole).toInt(), 2);
    QVERIFY(m->objectOf(orders).value(QStringLiteral("openable")).toBool());
    QCOMPARE(m->connectionIdOf(orders), id);
    // The branches the view must open: connection, Databases, alpha and its four folders.
    QCOMPARE(m->filterExpansion().size(), 7);
    QCOMPARE(text(m, m->firstMatch(), SchemaTreeModel::NameRole), QStringLiteral("orders"));

    // Typing on narrows it; a text that matches nothing leaves an empty tree.
    m->setFilter(QStringLiteral("orders_d"), SchemaTreeModel::Everything);
    QCOMPARE(m->filterCount(), 1);
    QCOMPARE(m->rowCount(m->index(0, 0, m->index(0, 0, root))), 1);
    m->setFilter(QStringLiteral("zzzz"), SchemaTreeModel::Everything);
    QCOMPARE(m->filterCount(), 0);
    QCOMPARE(m->rowCount(), 0);

    // Clearing brings back every node as it was: the search destroyed nothing.
    m->setFilter({}, SchemaTreeModel::Everything);
    QVERIFY(!m->filterActive());
    QCOMPARE(m->filterCount(), 0);
    QCOMPARE(m->rowCount(), 1);
    QCOMPARE(names(m, root),
             (QStringList { QStringLiteral("Databases"), QStringLiteral("Users") }));
    const QModelIndex databases2 = m->index(0, 0, root);
    QCOMPARE(
        names(m, databases2),
        (QStringList { QStringLiteral("alpha"), QStringLiteral("beta"), QStringLiteral("gamma") }));
    QCOMPARE(m->data(m->index(2, 0, databases2), SchemaTreeModel::PositionRole).toInt(), 3);
    QCOMPARE(m->data(m->index(2, 0, databases2), SchemaTreeModel::SetSizeRole).toInt(), 3);
    // alpha is still loaded, and so are the users.
    QCOMPARE(m->data(m->index(0, 0, databases2), SchemaTreeModel::LoadStateRole).toInt(),
             int(SchemaTreeModel::Loaded));
    QCOMPARE(m->rowCount(m->index(1, 0, root)), 2);
}

void TstSchemaTreeModel::searchScopesLookAtTheirOwnKind()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startFake(c);
    const QString id = addFakeTree(c, QStringLiteral("Fake server"));
    const QModelIndex root = loadAllFake(m, id);

    m->setFilter(QStringLiteral("order"), SchemaTreeModel::Tables); // tables and indexes
    QCOMPARE(m->filterCount(), 2); // orders and orders_date_idx, not the view
    m->setFilter(QStringLiteral("order"), SchemaTreeModel::Views);
    QCOMPARE(m->filterCount(), 1); // recent_orders; totals is a view without the word
    m->setFilter(QStringLiteral("TOT"), SchemaTreeModel::Views);
    QCOMPARE(m->filterCount(), 1);
    m->setFilter(QStringLiteral("min"), SchemaTreeModel::Users);
    QCOMPARE(m->filterCount(), 1);
    QVERIFY(!m->filterIncomplete()); // Users was loaded: nothing more to find there
    QCOMPARE(names(m, root), QStringList { QStringLiteral("Users") });
    QCOMPARE(names(m, m->index(0, 0, root)), QStringList { QStringLiteral("admin") });
    m->setFilter(QStringLiteral("alp"), SchemaTreeModel::Databases);
    QCOMPARE(m->filterCount(), 1);
    QCOMPARE(names(m, m->index(0, 0, root)), QStringList { QStringLiteral("alpha") });
    // The names of tables never match a search for databases.
    m->setFilter(QStringLiteral("orders"), SchemaTreeModel::Databases);
    QCOMPARE(m->filterCount(), 0);

    // Connections: by name only, and a match shows its whole branch (collapsed by the view).
    m->setFilter(QStringLiteral("server"), SchemaTreeModel::Connections);
    QCOMPARE(m->filterCount(), 1);
    QCOMPARE(m->rowCount(), 1);
    QCOMPARE(m->rowCount(root), 2);
    QCOMPARE(m->rowCount(m->index(0, 0, root)), 3);
    QVERIFY(m->filterExpansion().isEmpty()); // "without expanding"
    // A database is not a connection.
    m->setFilter(QStringLiteral("alpha"), SchemaTreeModel::Connections);
    QCOMPARE(m->filterCount(), 0);
    QCOMPARE(m->rowCount(), 0);

    // Migrations: not in the tree yet. Nothing matches, and that is not an error.
    m->setFilter(QStringLiteral("a"), SchemaTreeModel::Migrations);
    QVERIFY(m->filterActive());
    QCOMPARE(m->filterCount(), 0);
    QCOMPARE(m->rowCount(), 0);
    m->clearFilter();
    QVERIFY(!m->filterActive());
    QCOMPARE(m->rowCount(), 1);
}

void TstSchemaTreeModel::searchIgnoresCaseAndAccents()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    const QString file = m_dir.filePath(QStringLiteral("accents.db"));
    sqlite3 *db = nullptr;
    QVERIFY(sqlite3_open(file.toUtf8().constData(), &db) == SQLITE_OK);
    QVERIFY(sqlite3_exec(
                db, "CREATE TABLE \"caf\xC3\xA9\"(id INTEGER); CREATE TABLE Plain(id INTEGER);",
                nullptr, nullptr, nullptr)
            == SQLITE_OK);
    sqlite3_close(db);
    addConnection(c, QStringLiteral("Crème"), file);
    QTRY_COMPARE(m->rowCount(), 1);
    const QModelIndex root = m->index(0, 0);
    expand(m, root);
    expand(m, m->index(0, 0, root));
    m->setFilter(QStringLiteral("CAFE"), SchemaTreeModel::Tables); // accent and case folded
    QCOMPARE(m->filterCount(), 1);
    m->setFilter(QStringLiteral("café"), SchemaTreeModel::Tables); // and the other way round
    QCOMPARE(m->filterCount(), 1);
    m->setFilter(QStringLiteral("plain"), SchemaTreeModel::Tables);
    QCOMPARE(m->filterCount(), 1);
    m->setFilter(QStringLiteral("creme"), SchemaTreeModel::Connections);
    QCOMPARE(m->filterCount(), 1);
    // The file of the connection is searched too: that is what it is known by.
    m->setFilter(QStringLiteral("accents.db"), SchemaTreeModel::Connections);
    QCOMPARE(m->filterCount(), 1);
    m->setFilter(QStringLiteral("  plain   "), SchemaTreeModel::Tables); // spaces are not text
    QCOMPARE(m->filterCount(), 1);
    m->setFilter(QStringLiteral("   "), SchemaTreeModel::Tables); // only spaces: no search
    QVERIFY(!m->filterActive());
}

void TstSchemaTreeModel::searchOnlyLooksAtWhatIsLoaded()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startFake(c);
    const QString id = addFakeTree(c, QStringLiteral("Fake"));
    addFakeTree(c, QStringLiteral("Other"));
    QTRY_COMPARE(m->rowCount(), 2);
    // Nothing is connected: a search opens nothing and says that more could be found.
    QSignalSpy passwords(m, &SchemaTreeModel::passwordRequested);
    m->setFilter(QStringLiteral("orders"), SchemaTreeModel::Everything);
    QVERIFY(m->filterActive());
    QCOMPARE(m->filterCount(), 0);
    QVERIFY(m->filterIncomplete());
    QCOMPARE(m->rowCount(), 0);
    QTest::qWait(100);
    QVERIFY(m->connectedIds().isEmpty());
    QCOMPARE(passwords.count(), 0);
    // A connection that matches by name is shown, expandable (that is how the user searches
    // more), and expanding it loads it without dropping the search.
    m->setFilter(QStringLiteral("fake"), SchemaTreeModel::Everything);
    QCOMPARE(m->filterCount(), 1);
    const QModelIndex fake = m->indexOfConnection(id);
    QVERIFY(fake.isValid());
    QVERIFY(m->hasChildren(fake));
    QVERIFY(m->canFetchMore(fake));
    QCOMPARE(m->rowCount(fake), 0);
    m->fetchMore(fake);
    QTRY_VERIFY_WITH_TIMEOUT(m->isConnected(id), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(
        m->data(m->indexOfConnection(id), SchemaTreeModel::LoadStateRole).toInt(),
        int(SchemaTreeModel::Loaded), 10000);
    QVERIFY(m->filterActive());
    // "Loading…" never shows through a search, and the rows are of what matched ("Fake").
    QCOMPARE(m->rowCount(m->indexOfConnection(id)), 0);
    QCOMPARE(m->filterCount(), 1);
    m->setFilter(QStringLiteral("alp"), SchemaTreeModel::Everything);
    QCOMPARE(m->filterCount(), 1); // alpha, now that its connection is open
    QVERIFY(m->filterIncomplete()); // the other connection is not
}

void TstSchemaTreeModel::searchWaitsForLoadsToEnd()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startFake(c);
    const QString id = addFakeTree(c, QStringLiteral("Fake"));
    const QModelIndex root = m->indexOfConnection(id);
    m->fetchMore(root); // a request is on its way
    m->setFilter(QStringLiteral("alp"), SchemaTreeModel::Everything);
    QVERIFY(m->filterPending());
    QVERIFY(!m->filterActive()); // the tree is not touched while it loads
    QTRY_VERIFY_WITH_TIMEOUT(m->filterActive(), 10000);
    QVERIFY(!m->filterPending());
    QVERIFY(m->isConnected(id));
    // The text the box says last is the one that counts.
    m->clearFilter();
    m->fetchMore(m->index(1, 0, m->indexOfConnection(id)));
    m->setFilter(QStringLiteral("a"), SchemaTreeModel::Everything);
    m->setFilter(QStringLiteral("admin"), SchemaTreeModel::Users);
    QVERIFY(m->filterPending());
    QTRY_VERIFY_WITH_TIMEOUT(!m->filterPending(), 10000);
    QVERIFY(m->filterActive());
    QCOMPARE(m->filterCount(), 1);
}

void TstSchemaTreeModel::searchGivesWayToReveal()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startFake(c);
    const QString id = addFakeTree(c, QStringLiteral("Fake"));
    loadAllFake(m, id);
    m->setFilter(QStringLiteral("admin"), SchemaTreeModel::Users);
    QVERIFY(m->filterActive());
    QSignalSpy cleared(m, &SchemaTreeModel::filterCleared);
    QSignalSpy revealed(m, &SchemaTreeModel::revealed);
    // The table is hidden by the search: it is dropped first.
    m->reveal(id, { QStringLiteral("alpha"), QStringLiteral("customers") });
    QCOMPARE(cleared.count(), 1);
    QVERIFY(!m->filterActive());
    QTRY_COMPARE_WITH_TIMEOUT(revealed.count(), 1, 10000);
    QCOMPARE(text(m, revealed.at(0).at(0).toModelIndex(), SchemaTreeModel::NameRole),
             QStringLiteral("customers"));
}

void TstSchemaTreeModel::searchKeysRoundTrip()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startFake(c);
    const QString id = addFakeTree(c, QStringLiteral("Fake"));
    const QModelIndex root = loadAllFake(m, id);
    const QModelIndex orders = walk(m, root,
                                    { QStringLiteral("Databases"), QStringLiteral("alpha"),
                                      QStringLiteral("Tables"), QStringLiteral("orders") });
    QStringList chain;
    for (const QVariant &a : m->ancestorsOf(orders))
        chain << m->keyOf(a.toModelIndex());
    chain << m->keyOf(orders);
    QCOMPARE(chain.size(), 5);
    QVERIFY(m->keyOf(QModelIndex()).isEmpty());
    // A key finds the node again after the tree was reset by a search and brought back.
    m->setFilter(QStringLiteral("customers"), SchemaTreeModel::Everything);
    QVERIFY(m->indexesOfKeys(chain).size() < 5); // orders is hidden now
    m->clearFilter();
    const QVariantList found = m->indexesOfKeys(chain);
    QCOMPARE(found.size(), 5);
    QCOMPARE(text(m, found.last().toModelIndex(), SchemaTreeModel::NameRole),
             QStringLiteral("orders"));
    QCOMPARE(m->keyOf(found.last().toModelIndex()), chain.last());
    // Only the nodes asked for come back.
    QCOMPARE(m->indexesOfKeys({ chain.first() }).size(), 1);
}

void TstSchemaTreeModel::testerSurvivesSearchOnAndOff()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    const auto tester = attachTester(m);
    startFake(c);
    addFakeTree(c, QStringLiteral("Fake"));
    addFakeTree(c, QStringLiteral("Zed"), QStringLiteral("fail"));
    QTRY_COMPARE_WITH_TIMEOUT(m->connectedIds().size(), 2, 20000);
    QTest::qWait(600); // the tester has opened everything
    for (const QString &typed : { QStringLiteral("a"), QStringLiteral("order"),
                                  QStringLiteral("zz"), QStringLiteral("e"), QString() }) {
        m->setFilter(typed, SchemaTreeModel::Everything);
        QTest::qWait(300); // what the tester loads under the search resets it again
    }
    m->setFilter(QStringLiteral("fake"), SchemaTreeModel::Connections);
    QTest::qWait(100);
    m->setFilter(QString(), SchemaTreeModel::Connections);
    // A connection removed while the search is on.
    m->setFilter(QStringLiteral("e"), SchemaTreeModel::Everything);
    QTest::qWait(300);
    QVERIFY(m->rowCount() > 0);
    c.remove(m->connectionIdOf(m->index(0, 0)));
    QTest::qWait(300);
    m->setFilter(QString(), SchemaTreeModel::Everything);
    QTest::qWait(100);
}

void TstSchemaTreeModel::searchingTenThousandTablesIsFast()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    addConnection(c, QStringLiteral("Big"), newFile(10'000));
    QTRY_COMPARE(m->rowCount(), 1);
    const QModelIndex root = m->index(0, 0);
    expand(m, root);
    expand(m, child(m, root, QStringLiteral("Tables")));
    QElapsedTimer clock;
    clock.start();
    m->setFilter(QStringLiteral("t99"), SchemaTreeModel::Tables);
    const qint64 filtered = clock.elapsed();
    QCOMPARE(m->filterCount(), 111); // t99, t990..t999, t9900..t9999
    clock.restart();
    m->setFilter(QStringLiteral("t999"), SchemaTreeModel::Everything);
    const qint64 narrowed = clock.elapsed();
    QCOMPARE(m->filterCount(), 11); // t999, t9990..t9999
    clock.restart();
    m->setFilter({}, SchemaTreeModel::Everything);
    const qint64 cleared = clock.elapsed();
    qInfo() << "searching 10000 tables:" << filtered << "ms, narrowing" << narrowed
            << "ms, clearing" << cleared << "ms";
    QVERIFY2(filtered < 250 && narrowed < 250 && cleared < 250, "the search is O(n) and cheap");
    QCOMPARE(m->rowCount(child(m, root, QStringLiteral("Tables"))), 10'000);
}

// ---- migrations (H12 phase B) -------------------------------------------------------------------

namespace {

MigrationFeed notRead()
{
    MigrationFeed f;
    f.state = MigrationFeed::State::NotRead;
    return f;
}

MigrationFeed ready(std::vector<MigrationEntry> entries, bool tableMissing = false)
{
    MigrationFeed f;
    f.state = MigrationFeed::State::Ready;
    f.report.tableMissing = tableMissing;
    for (const MigrationEntry &e : entries) {
        if (e.state == MigrationState::Pending)
            ++f.report.pending;
        else if (e.state == MigrationState::Applied)
            ++f.report.applied;
        else
            ++f.report.orphans;
        if (e.state != MigrationState::Orphan)
            ++f.report.files;
    }
    f.report.entries = std::move(entries);
    return f;
}

const QString kCreateUsers = QStringLiteral("2024_01_01_000000_create_users_table");
const QString kAddEmail = QStringLiteral("2024_02_01_000000_add_email_to_users");
const QString kCreatePosts = QStringLiteral("2024_03_01_000000_create_posts_table");
const QString kGhost = QStringLiteral("2023_12_31_000000_ghost");

std::vector<MigrationEntry> sample()
{
    return { { kGhost, MigrationState::Orphan, 1 },
             { kCreateUsers, MigrationState::Applied, 1 },
             { kAddEmail, MigrationState::Applied, 2 },
             { kCreatePosts, MigrationState::Pending, std::nullopt } };
}

} // namespace

void TstSchemaTreeModel::migrationsAreSiblingsOfTablesOnlyWhenConfigured()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    const QString id = addConnection(c, QStringLiteral("Lite"), newFile(2));
    QTRY_COMPARE(m->rowCount(), 1);
    const QModelIndex root = m->index(0, 0);
    expand(m, root);
    const QModelIndex tables = child(m, root, QStringLiteral("Tables"));
    expand(m, tables);
    QCOMPARE(names(m, tables), (QStringList { QStringLiteral("t0"), QStringLiteral("t1") }));
    const QStringList plain { QStringLiteral("Tables"), QStringLiteral("Views"),
                              QStringLiteral("Indexes") };
    QCOMPARE(names(m, root), plain);

    // Configured now: Migrations is the next folder after Tables, at the same depth; the count of
    // Tables and its tables are untouched.
    m->setMigrations(id, QString(), notRead());
    QCOMPARE(names(m, root),
             (QStringList { QStringLiteral("Tables"), QStringLiteral("Migrations"),
                            QStringLiteral("Views"), QStringLiteral("Indexes") }));
    QCOMPARE(names(m, tables), (QStringList { QStringLiteral("t0"), QStringLiteral("t1") }));
    QCOMPARE(text(m, tables, SchemaTreeModel::DetailRole), QStringLiteral("2"));
    const QModelIndex migrations = m->index(1, 0, root);
    QCOMPARE(m->parent(migrations), m->parent(tables));
    QCOMPARE(text(m, migrations, SchemaTreeModel::KindRole), QStringLiteral("migrationsFolder"));
    QCOMPARE(m->data(migrations, SchemaTreeModel::LoadStateRole).toInt(),
             int(SchemaTreeModel::Loaded));
    QVERIFY(!m->canFetchMore(migrations)); // nothing is asked of the database
    QVERIFY(m->hasChildren(migrations));
    QCOMPARE(m->connectionIdOf(migrations), id);
    QVERIFY(!m->objectOf(migrations).value(QStringLiteral("openable")).toBool());
    QCOMPARE(m->data(m->index(2, 0, root), SchemaTreeModel::PositionRole).toInt(), 3);
    QCOMPARE(m->data(m->index(0, 0, root), SchemaTreeModel::SetSizeRole).toInt(), 4);
    // The tables are where they were: their paths did not change.
    QCOMPARE(m->objectOf(m->index(0, 0, tables)).value(QStringLiteral("parts")).toStringList(),
             (QStringList { QStringLiteral("main"), QStringLiteral("t0") }));

    // SQLite: the connection node is where its one database's settings live (database "").
    const QVariantMap scope = m->migrationScopeOf(root);
    QVERIFY(scope.value(QStringLiteral("valid")).toBool());
    QCOMPARE(scope.value(QStringLiteral("connectionId")).toString(), id);
    QCOMPARE(scope.value(QStringLiteral("database")).toString(), QString());

    // No project any more: the folder goes.
    m->setMigrations(id, QString(), {});
    QCOMPARE(names(m, root), plain);
    QCOMPARE(m->data(m->index(1, 0, root), SchemaTreeModel::PositionRole).toInt(), 2);
    QCOMPARE(m->data(m->index(1, 0, root), SchemaTreeModel::SetSizeRole).toInt(), 3);
}

void TstSchemaTreeModel::migrationsSetBeforeConnectingAreThereWhenTheTreeLoads()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    const QString id = addConnection(c, QStringLiteral("Empty lite"), newFile(0));
    QTRY_COMPARE(m->rowCount(), 1);
    // The settings arrive before the connection is open (the usual order at start-up).
    m->setMigrations(id, QString(), notRead());
    const QModelIndex root = m->index(0, 0);
    expand(m, root);
    QCOMPARE(names(m, root),
             (QStringList { QStringLiteral("Tables"), QStringLiteral("Migrations"),
                            QStringLiteral("Views"), QStringLiteral("Indexes") }));
    // An empty Tables folder says so, and Migrations is not in it.
    const QModelIndex tables = child(m, root, QStringLiteral("Tables"));
    expand(m, tables);
    QCOMPARE(text(m, m->index(0, 0, tables), SchemaTreeModel::KindRole), QStringLiteral("empty"));
    QCOMPARE(m->rowCount(tables), 1);
    QCOMPARE(text(m, tables, SchemaTreeModel::DetailRole), QStringLiteral("0"));
}

void TstSchemaTreeModel::migrationsDoNotNeedTablesLoaded()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    const QString id = addConnection(c, QStringLiteral("Lite"), newFile(2));
    QTRY_COMPARE(m->rowCount(), 1);
    const QModelIndex root = m->index(0, 0);
    expand(m, root);
    const QModelIndex tables = child(m, root, QStringLiteral("Tables"));
    // Tables was never expanded: the folder is created next to it anyway and does not load it.
    QVERIFY(m->canFetchMore(tables));
    m->setMigrations(id, QString(), ready(sample()));
    QCOMPARE(m->data(m->index(1, 0, root), SchemaTreeModel::NameRole).toString(),
             QStringLiteral("Migrations"));
    QVERIFY(m->canFetchMore(tables));
    expand(m, tables);
    QCOMPARE(names(m, tables), (QStringList { QStringLiteral("t0"), QStringLiteral("t1") }));
    QCOMPARE(text(m, tables, SchemaTreeModel::DetailRole), QStringLiteral("2"));
    // reveal still finds a table: Migrations is not on its way.
    QSignalSpy revealed(m, &SchemaTreeModel::revealed);
    m->reveal(id, { QStringLiteral("main"), QStringLiteral("t1") });
    QTRY_COMPARE(revealed.size(), 1);
}

void TstSchemaTreeModel::theReportFillsPendingAppliedAndOrphans()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    const QString id = addConnection(c, QStringLiteral("Lite"), newFile(1));
    QTRY_COMPARE(m->rowCount(), 1);
    m->setMigrations(id, QString(), ready(sample()));
    const QModelIndex root = m->index(0, 0);
    expand(m, root);
    const QModelIndex migrations = child(m, root, QStringLiteral("Migrations"));
    QCOMPARE(names(m, migrations),
             (QStringList { QStringLiteral("Pending"), QStringLiteral("Applied"),
                            QStringLiteral("Orphans") }));
    QCOMPARE(text(m, migrations, SchemaTreeModel::DetailRole), QStringLiteral("4"));
    const QModelIndex pending = m->index(0, 0, migrations);
    const QModelIndex applied = m->index(1, 0, migrations);
    const QModelIndex orphans = m->index(2, 0, migrations);
    QCOMPARE(text(m, pending, SchemaTreeModel::KindRole), QStringLiteral("pendingFolder"));
    QCOMPARE(text(m, applied, SchemaTreeModel::KindRole), QStringLiteral("appliedFolder"));
    QCOMPARE(text(m, orphans, SchemaTreeModel::KindRole), QStringLiteral("orphansFolder"));
    QCOMPARE(text(m, pending, SchemaTreeModel::DetailRole), QStringLiteral("1"));
    QCOMPARE(text(m, applied, SchemaTreeModel::DetailRole), QStringLiteral("2"));
    QCOMPARE(text(m, orphans, SchemaTreeModel::DetailRole), QStringLiteral("1"));

    // Chronological, as the report; the batch is the secondary text of the applied ones.
    QCOMPARE(names(m, pending), QStringList { kCreatePosts });
    QCOMPARE(names(m, applied), (QStringList { kCreateUsers, kAddEmail }));
    QCOMPARE(names(m, orphans), QStringList { kGhost });
    const QModelIndex users = m->index(0, 0, applied);
    QCOMPARE(text(m, users, SchemaTreeModel::KindRole), QStringLiteral("migration"));
    QCOMPARE(text(m, users, SchemaTreeModel::DetailRole), QStringLiteral("batch 1"));
    QCOMPARE(text(m, users, SchemaTreeModel::MigrationStateRole), QStringLiteral("applied"));
    QCOMPARE(m->data(users, SchemaTreeModel::BatchRole).toInt(), 1);
    QCOMPARE(m->data(m->index(1, 0, applied), SchemaTreeModel::BatchRole).toInt(), 2);
    const QModelIndex posts = m->index(0, 0, pending);
    QCOMPARE(text(m, posts, SchemaTreeModel::MigrationStateRole), QStringLiteral("pending"));
    QVERIFY(text(m, posts, SchemaTreeModel::DetailRole).isEmpty());
    QCOMPARE(m->data(posts, SchemaTreeModel::BatchRole).toInt(), -1);
    // The row shows the name without the date prefix; the name itself stays whole (it opens the
    // tab and is what the tooltip says). Everything that is not a migration shows its name.
    QCOMPARE(text(m, users, SchemaTreeModel::DisplayNameRole),
             QStringLiteral("000000_create_users_table"));
    QCOMPARE(text(m, users, SchemaTreeModel::NameRole), kCreateUsers);
    QCOMPARE(text(m, posts, SchemaTreeModel::DisplayNameRole),
             QStringLiteral("000000_create_posts_table"));
    QCOMPARE(text(m, applied, SchemaTreeModel::DisplayNameRole),
             text(m, applied, SchemaTreeModel::NameRole));
    const QModelIndex ghost = m->index(0, 0, orphans);
    QCOMPARE(text(m, ghost, SchemaTreeModel::DisplayNameRole), QStringLiteral("000000_ghost"));
    QCOMPARE(text(m, ghost, SchemaTreeModel::MigrationStateRole), QStringLiteral("orphan"));
    QCOMPARE(m->data(ghost, SchemaTreeModel::PositionRole).toInt(), 1);
    QCOMPARE(m->data(m->index(1, 0, applied), SchemaTreeModel::SetSizeRole).toInt(), 2);

    // A leaf opens as a migration, never as data.
    QVERIFY(!m->hasChildren(users));
    QVERIFY(!m->canFetchMore(users));
    QVERIFY(!m->objectOf(users).value(QStringLiteral("openable")).toBool());
    const QVariantMap open = m->migrationOf(users);
    QVERIFY(open.value(QStringLiteral("valid")).toBool());
    QCOMPARE(open.value(QStringLiteral("connectionId")).toString(), id);
    QCOMPARE(open.value(QStringLiteral("name")).toString(), kCreateUsers);
    QCOMPARE(open.value(QStringLiteral("state")).toString(), QStringLiteral("applied"));
    QCOMPARE(open.value(QStringLiteral("batch")).toInt(), 1);
    QCOMPARE(m->migrationOf(posts).value(QStringLiteral("state")).toString(),
             QStringLiteral("pending"));
    QVERIFY(!m->migrationOf(applied).value(QStringLiteral("valid")).toBool());
    QVERIFY(!m->migrationOf(child(m, root, QStringLiteral("Tables")))
                 .value(QStringLiteral("valid"))
                 .toBool());
    QVERIFY(!m->migrationOf({}).value(QStringLiteral("valid")).toBool());
}

void TstSchemaTreeModel::aNewReportUpdatesTheFolderKeepingWhatIsOpen()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    const QString id = addConnection(c, QStringLiteral("Lite"), newFile(1));
    QTRY_COMPARE(m->rowCount(), 1);
    m->setMigrations(id, QString(), ready(sample()));
    const QModelIndex root = m->index(0, 0);
    expand(m, root);
    const QModelIndex migrations = child(m, root, QStringLiteral("Migrations"));
    // What the view keeps while it is open: persistent indexes of the folders (and of a leaf).
    const QPersistentModelIndex folder(migrations);
    const QPersistentModelIndex applied(m->index(1, 0, migrations));
    const QPersistentModelIndex orphans(m->index(2, 0, migrations));
    const QPersistentModelIndex users(m->index(0, 0, applied));
    QSignalSpy reset(m, &QAbstractItemModel::modelReset);

    // The pending one was applied (batch 3) and the others did not change: same folders.
    auto entries = sample();
    entries[3] = { kCreatePosts, MigrationState::Applied, 3 };
    m->setMigrations(id, QString(), ready(entries));
    QVERIFY(folder.isValid() && applied.isValid() && orphans.isValid());
    QCOMPARE(names(m, QModelIndex(applied)),
             (QStringList { kCreateUsers, kAddEmail, kCreatePosts }));
    QCOMPARE(text(m, QModelIndex(applied), SchemaTreeModel::DetailRole), QStringLiteral("3"));
    QCOMPARE(text(m, QModelIndex(folder), SchemaTreeModel::DetailRole), QStringLiteral("4"));
    const QModelIndex pending = m->index(0, 0, migrations);
    QCOMPARE(text(m, pending, SchemaTreeModel::DetailRole), QStringLiteral("0"));
    // No pending migration: the folder says so instead of being empty.
    QCOMPARE(text(m, m->index(0, 0, pending), SchemaTreeModel::KindRole), QStringLiteral("empty"));
    QCOMPARE(text(m, m->index(0, 0, pending), SchemaTreeModel::NameRole),
             QStringLiteral("Nothing pending"));
    // The same report again changes nothing, not even the leaves.
    const QPersistentModelIndex last(m->index(2, 0, QModelIndex(applied)));
    m->setMigrations(id, QString(), ready(entries));
    QVERIFY(last.isValid());
    QVERIFY(users.isValid());

    // The orphan went: the Orphans folder with it.
    entries.erase(entries.begin());
    m->setMigrations(id, QString(), ready(entries));
    QCOMPARE(names(m, QModelIndex(folder)),
             (QStringList { QStringLiteral("Pending"), QStringLiteral("Applied") }));
    QCOMPARE(reset.size(), 0); // never a reset of the tree
    // The table of migrations does not exist: everything pending, and Applied says why it is empty.
    m->setMigrations(id, QString(),
                     ready({ { kCreateUsers, MigrationState::Pending, std::nullopt },
                             { kAddEmail, MigrationState::Pending, std::nullopt } },
                           true));
    const QModelIndex pending2 = m->index(0, 0, QModelIndex(folder));
    const QModelIndex applied2 = m->index(1, 0, QModelIndex(folder));
    QCOMPARE(names(m, pending2), (QStringList { kCreateUsers, kAddEmail }));
    QCOMPARE(text(m, applied2, SchemaTreeModel::DetailRole), QStringLiteral("0"));
    QCOMPARE(names(m, applied2),
             QStringList { QStringLiteral("Migrations table not found: everything is pending") });
    QCOMPARE(text(m, m->index(0, 0, applied2), SchemaTreeModel::KindRole), QStringLiteral("empty"));
    QCOMPARE(reset.size(), 0);
}

void TstSchemaTreeModel::notReadReadingAndFailedSayTheirWord()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    const QString id = addConnection(c, QStringLiteral("Lite"), newFile(1));
    QTRY_COMPARE(m->rowCount(), 1);
    m->setMigrations(id, QString(), notRead());
    const QModelIndex root = m->index(0, 0);
    expand(m, root);
    const QModelIndex migrations = child(m, root, QStringLiteral("Migrations"));
    QVERIFY(text(m, m->index(0, 0, migrations), SchemaTreeModel::NameRole)
                .startsWith(QStringLiteral("Not read yet")));
    QCOMPARE(text(m, m->index(0, 0, migrations), SchemaTreeModel::KindRole),
             QStringLiteral("empty"));
    QVERIFY(text(m, migrations, SchemaTreeModel::DetailRole).isEmpty());

    MigrationFeed reading;
    reading.state = MigrationFeed::State::Reading;
    m->setMigrations(id, QString(), reading);
    QCOMPARE(names(m, migrations), QStringList { QStringLiteral("Reading…") });

    MigrationFeed failed;
    failed.state = MigrationFeed::State::Failed;
    failed.message = QStringLiteral("The table cannot be read.");
    m->setMigrations(id, QString(), failed);
    QCOMPARE(names(m, migrations), QStringList { QStringLiteral("The table cannot be read.") });

    // And back to a report: the folder is filled in place.
    m->setMigrations(id, QString(), ready(sample()));
    QCOMPARE(m->rowCount(migrations), 3);
    QCOMPARE(text(m, migrations, SchemaTreeModel::DetailRole), QStringLiteral("4"));
}

// Two databases of a server connection without a fixed database: each one has its own feed.
void TstSchemaTreeModel::migrationsAreOfEachDatabase()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startFake(c);
    const QString id = addFakeTree(c, QStringLiteral("Fake"));
    m->setMigrations(id, QStringLiteral("alpha"), notRead());
    m->setMigrations(id, QStringLiteral("beta"), ready(sample()));
    // The connection has no database of its own: a feed without database shows nowhere.
    m->setMigrations(id, QString(), ready(sample()));
    const QModelIndex root = m->indexOfConnection(id);
    const QModelIndex alpha
        = walk(m, root, { QStringLiteral("Databases"), QStringLiteral("alpha") });
    expand(m, alpha);
    const QModelIndex alphaTables = m->index(0, 0, alpha);
    QCOMPARE(names(m, alpha).mid(0, 2),
             (QStringList { QStringLiteral("Tables"), QStringLiteral("Migrations") }));
    QCOMPARE(names(m, alphaTables),
             (QStringList { QStringLiteral("customers"), QStringLiteral("orders") }));
    QCOMPARE(text(m, alphaTables, SchemaTreeModel::DetailRole), QStringLiteral("2"));
    const QModelIndex alphaMigrations = m->index(1, 0, alpha);
    QCOMPARE(m->rowCount(alphaMigrations), 1); // "Not read yet..."
    QCOMPARE(text(m, alphaMigrations, SchemaTreeModel::DetailRole), QString());

    const QModelIndex beta = walk(m, root, { QStringLiteral("Databases"), QStringLiteral("beta") });
    expand(m, beta);
    QCOMPARE(names(m, beta).mid(0, 2),
             (QStringList { QStringLiteral("Tables"), QStringLiteral("Migrations") }));
    const QModelIndex betaMigrations = m->index(1, 0, beta);
    QCOMPARE(text(m, betaMigrations, SchemaTreeModel::DetailRole), QStringLiteral("4"));
    QCOMPARE(names(m, betaMigrations),
             (QStringList { QStringLiteral("Pending"), QStringLiteral("Applied"),
                            QStringLiteral("Orphans") }));
    QCOMPARE(text(m, m->index(0, 0, betaMigrations), SchemaTreeModel::DetailRole),
             QStringLiteral("1"));
    // A leaf knows its database, which is the key the controller reads the file with.
    const QModelIndex leaf = m->index(0, 0, m->index(0, 0, betaMigrations));
    QCOMPARE(m->migrationOf(leaf).value(QStringLiteral("name")).toString(), kCreatePosts);
    QCOMPARE(m->migrationOf(leaf).value(QStringLiteral("database")).toString(),
             QStringLiteral("beta"));

    // gamma has none: its schema's Tables folder is not a host without a feed.
    const QModelIndex gamma
        = walk(m, root,
               { QStringLiteral("Databases"), QStringLiteral("gamma"), QStringLiteral("public") });
    expand(m, gamma);
    QVERIFY(!names(m, gamma).contains(QStringLiteral("Migrations")));

    // A new report for alpha changes only alpha's folder.
    auto entries = sample();
    entries.pop_back();
    m->setMigrations(id, QStringLiteral("alpha"), ready(entries));
    QCOMPARE(text(m, m->index(1, 0, alpha), SchemaTreeModel::DetailRole), QStringLiteral("3"));
    QCOMPARE(text(m, betaMigrations, SchemaTreeModel::DetailRole), QStringLiteral("4"));
    QCOMPARE(text(m, m->index(0, 0, m->index(1, 0, alpha)), SchemaTreeModel::DetailRole),
             QStringLiteral("0"));

    // Removing the settings of alpha leaves beta's folder alone.
    m->setMigrations(id, QStringLiteral("alpha"), {});
    QVERIFY(!names(m, alpha).contains(QStringLiteral("Migrations")));
    QCOMPARE(names(m, beta).mid(0, 2),
             (QStringList { QStringLiteral("Tables"), QStringLiteral("Migrations") }));

    // Closing the connection and opening it again builds them again, in the same places.
    m->disconnectConnection(id);
    const QModelIndex again = walk(m, m->indexOfConnection(id),
                                   { QStringLiteral("Databases"), QStringLiteral("beta") });
    expand(m, again);
    QCOMPARE(names(m, again).value(1), QStringLiteral("Migrations"));
    QCOMPARE(text(m, m->index(1, 0, again), SchemaTreeModel::DetailRole), QStringLiteral("4"));
}

// A connection with a fixed database: its own database shows the feed stored without database.
void TstSchemaTreeModel::aFixedDatabaseShowsTheSettingsWithoutDatabase()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startFake(c);
    const QString id = addFakeTree(c, QStringLiteral("Fixed"), {}, QStringLiteral("alpha"));
    m->setMigrations(id, QString(), ready(sample()));
    const QModelIndex root = m->indexOfConnection(id);
    const QModelIndex alpha
        = walk(m, root, { QStringLiteral("Databases"), QStringLiteral("alpha") });
    expand(m, alpha);
    QCOMPARE(names(m, alpha).mid(0, 2),
             (QStringList { QStringLiteral("Tables"), QStringLiteral("Migrations") }));
    const QModelIndex leaf = m->index(0, 0, m->index(0, 0, m->index(1, 0, alpha)));
    QCOMPARE(m->migrationOf(leaf).value(QStringLiteral("database")).toString(), QString());
    // Not the other databases: those need a feed of their own.
    const QModelIndex beta = walk(m, root, { QStringLiteral("Databases"), QStringLiteral("beta") });
    expand(m, beta);
    QVERIFY(!names(m, beta).contains(QStringLiteral("Migrations")));
    m->setMigrations(id, QStringLiteral("beta"), notRead());
    QCOMPARE(names(m, beta).value(1), QStringLiteral("Migrations"));
    // Dropping the one of the connection leaves beta's.
    m->setMigrations(id, QString(), {});
    QVERIFY(!names(m, alpha).contains(QStringLiteral("Migrations")));
    QCOMPARE(names(m, beta).value(1), QStringLiteral("Migrations"));
}

// The tester opens everything it sees; the feeds of different databases come and go meanwhile.
void TstSchemaTreeModel::testerSurvivesPerDatabaseMigrations()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    const auto tester = attachTester(m);
    startFake(c);
    const QString id = addFakeTree(c, QStringLiteral("Fake"), {}, QStringLiteral("alpha"));
    m->setMigrations(id, QString(), ready(sample()));
    m->setMigrations(id, QStringLiteral("beta"), notRead());
    m->setMigrations(id, QStringLiteral("gamma"), ready(sample()));
    QTest::qWait(300);
    auto entries = sample();
    entries.erase(entries.begin());
    m->setMigrations(id, QStringLiteral("beta"), ready(entries));
    m->setMigrations(id, QString(), ready({}, true));
    m->setMigrations(id, QStringLiteral("gamma"), {});
    m->setMigrations(id, QString(), {});
    m->setMigrations(id, QStringLiteral("beta"), {});
    m->setMigrations(id, QStringLiteral("alpha"), ready(sample()));
    m->setFilter(QStringLiteral("users"), SchemaTreeModel::Everything);
    QTRY_VERIFY(m->filterActive());
    m->setMigrations(id, QStringLiteral("alpha"), ready(entries));
    m->setMigrations(id, QStringLiteral("alpha"), {});
    m->setFilter({}, SchemaTreeModel::Everything);
    m->disconnectConnection(id);
    QTest::qWait(200);
}

void TstSchemaTreeModel::migrationScopeSaysWhereTheSettingsOfANodeLive()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startFake(c);
    // No fixed database: the connection node has no scope, each database has its own.
    const QString open = addFakeTree(c, QStringLiteral("Open"));
    const QModelIndex openRoot = m->indexOfConnection(open);
    QVERIFY(!m->migrationScopeOf(openRoot).value(QStringLiteral("valid")).toBool());
    const QModelIndex alpha
        = walk(m, openRoot, { QStringLiteral("Databases"), QStringLiteral("alpha") });
    const QVariantMap scope = m->migrationScopeOf(alpha);
    QVERIFY(scope.value(QStringLiteral("valid")).toBool());
    QCOMPARE(scope.value(QStringLiteral("connectionId")).toString(), open);
    QCOMPARE(scope.value(QStringLiteral("database")).toString(), QStringLiteral("alpha"));
    QCOMPARE(scope.value(QStringLiteral("base")).toString(), QStringLiteral("alpha"));
    QVERIFY(!m->migrationScopeOf(child(m, openRoot, QStringLiteral("Databases")))
                 .value(QStringLiteral("valid"))
                 .toBool());
    QVERIFY(!m->migrationScopeOf({}).value(QStringLiteral("valid")).toBool());

    // A fixed database: the connection node and its own database are scope "".
    const QString fixed = addFakeTree(c, QStringLiteral("Fixed"), {}, QStringLiteral("alpha"));
    const QModelIndex fixedRoot = m->indexOfConnection(fixed);
    const QVariantMap own = m->migrationScopeOf(fixedRoot);
    QVERIFY(own.value(QStringLiteral("valid")).toBool());
    QCOMPARE(own.value(QStringLiteral("database")).toString(), QString());
    const QModelIndex fixedAlpha
        = walk(m, fixedRoot, { QStringLiteral("Databases"), QStringLiteral("alpha") });
    QCOMPARE(m->migrationScopeOf(fixedAlpha).value(QStringLiteral("database")).toString(),
             QString());
    const QModelIndex fixedBeta
        = walk(m, fixedRoot, { QStringLiteral("Databases"), QStringLiteral("beta") });
    QCOMPARE(m->migrationScopeOf(fixedBeta).value(QStringLiteral("database")).toString(),
             QStringLiteral("beta"));
}

void TstSchemaTreeModel::testerSurvivesMigrationChanges()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    const auto tester = attachTester(m);
    startController(c);
    const QString id = addConnection(c, QStringLiteral("Lite"), newFile(3));
    QTRY_COMPARE_WITH_TIMEOUT(m->connectedIds().size(), 1, 20000);
    QTest::qWait(300); // the tester opens everything it sees
    m->setMigrations(id, QString(), notRead());
    m->setMigrations(id, QString(), ready(sample()));
    auto entries = sample();
    entries[3] = { kCreatePosts, MigrationState::Applied, 3 };
    m->setMigrations(id, QString(), ready(entries));
    entries.erase(entries.begin());
    m->setMigrations(id, QString(), ready(entries));
    m->setMigrations(id, QString(), ready({}, true));
    m->setMigrations(id, QString(), notRead());
    m->setMigrations(id, QString(), {});
    m->setMigrations(id, QString(), ready(sample()));
    // With a search on, the same changes go through a reset of the filtered tree.
    m->setFilter(QStringLiteral("users"), SchemaTreeModel::Everything);
    QTRY_VERIFY(m->filterActive());
    m->setMigrations(id, QString(), ready(entries));
    m->setMigrations(id, QString(), {});
    m->setMigrations(id, QString(), ready(sample()));
    m->setFilter({}, SchemaTreeModel::Everything);
    m->disconnectConnection(id);
    QTest::qWait(200);
}

void TstSchemaTreeModel::searchByMigrations()
{
    ConnectionController c;
    SchemaTreeModel *m = c.tree();
    startController(c);
    const QString id = addConnection(c, QStringLiteral("Lite"), newFile(2));
    const QString plain = addConnection(c, QStringLiteral("Plain"), newFile(2));
    QTRY_COMPARE(m->rowCount(), 2);
    m->setMigrations(id, QString(), ready(sample()));
    const QModelIndex root = m->indexOfConnection(id);
    // Not opened yet: the migrations may be in there, so the results are incomplete.
    m->setFilter(QStringLiteral("create"), SchemaTreeModel::Migrations);
    QCOMPARE(m->filterCount(), 0);
    QVERIFY(m->filterIncomplete());
    m->setFilter({}, SchemaTreeModel::Migrations);
    expand(m, root);

    m->setFilter(QStringLiteral("create"), SchemaTreeModel::Migrations);
    QVERIFY(m->filterActive());
    QCOMPARE(m->filterCount(), 2); // create_users_table, create_posts_table
    QVERIFY(!m->filterIncomplete()); // Migrations is next to Tables, not in it; the other has none
    QCOMPARE(m->rowCount(), 1); // only the connection that has them
    // Connection > Migrations > Pending|Applied > leaf; nothing else is shown (Tables is not a
    // parent of it).
    const QModelIndex fRoot = m->index(0, 0);
    QCOMPARE(m->rowCount(fRoot), 1);
    const QModelIndex fMigrations = m->index(0, 0, fRoot);
    QCOMPARE(text(m, fMigrations, SchemaTreeModel::NameRole), QStringLiteral("Migrations"));
    QCOMPARE(names(m, fMigrations),
             (QStringList { QStringLiteral("Pending"), QStringLiteral("Applied") }));
    QCOMPARE(names(m, m->index(0, 0, fMigrations)), QStringList { kCreatePosts });
    QCOMPARE(names(m, m->index(1, 0, fMigrations)), QStringList { kCreateUsers });
    QVERIFY(m->firstMatch().isValid());
    QCOMPARE(text(m, m->firstMatch(), SchemaTreeModel::NameRole), kCreatePosts);
    QVERIFY(m->filterExpansion().size() >= 3);

    // The tables are not migrations, and migrations are not tables.
    m->setFilter(QStringLiteral("users"), SchemaTreeModel::Tables);
    QCOMPARE(m->filterCount(), 0);
    m->setFilter(QStringLiteral("t0"), SchemaTreeModel::Migrations);
    QCOMPARE(m->filterCount(), 0);
    // A new report under the search is shown at once.
    m->setFilter(QStringLiteral("users"), SchemaTreeModel::Migrations);
    QCOMPARE(m->filterCount(), 2); // create_users_table, add_email_to_users
    auto entries = sample();
    entries.push_back(
        { QStringLiteral("2024_04_01_000000_more_users"), MigrationState::Pending, std::nullopt });
    QSignalSpy changed(m, &SchemaTreeModel::filterChanged);
    m->setMigrations(id, QString(), ready(entries));
    QVERIFY(!changed.isEmpty());
    QCOMPARE(m->filterCount(), 3);
    m->setFilter({}, SchemaTreeModel::Migrations);
    QVERIFY(!m->filterActive());
    QVERIFY(m->indexOfConnection(plain).isValid());
}

QTEST_MAIN(TstSchemaTreeModel)
#include "tst_schema_tree_model.moc"
