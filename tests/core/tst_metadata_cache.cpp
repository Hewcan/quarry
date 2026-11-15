#include <quarry/metadata_cache.h>
#include <quarry/provider_registry.h>
#include <quarry/session.h>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <sqlite3.h>

using namespace quarry;
using namespace Qt::StringLiterals;

namespace {

// A missing object gives an empty path, so the comparison fails instead of dereferencing nullopt.
ObjectPath resolved(const MetadataCache &cache, const QStringList &parts)
{
    return cache.resolve(parts).value_or(ObjectPath {});
}

bool createDatabase(const QString &file)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open(file.toUtf8().constData(), &db) != SQLITE_OK) {
        return false;
    }
    const char *sql = "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL);"
                      "CREATE TABLE Orders(id INTEGER PRIMARY KEY, user_id INTEGER, total REAL);"
                      "CREATE TABLE items(id INTEGER PRIMARY KEY, sku TEXT);"
                      "CREATE VIEW v_users AS SELECT id, name FROM users;";
    const bool ok = sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(db);
    return ok;
}

ObjectPath table(const QString &name, ObjectKind kind = ObjectKind::Table)
{
    return ObjectPath { { u"main"_s, name }, kind };
}

QStringList names(const std::vector<CachedObject> &objects)
{
    QStringList out;
    for (const CachedObject &object : objects) {
        out << object.name;
    }
    return out;
}

// The registry must outlive the session: the connection's code lives in the plugin.
struct Fixture
{
    ProviderRegistry registry;
    QTemporaryDir dir;
    std::unique_ptr<Session> session;
    MetadataCache cache;

    bool init(bool open = true)
    {
        registry.scan(QStringLiteral(QUARRY_PLUGIN_DIR));
        const QString file = dir.filePath(u"t.db"_s);
        if (!dir.isValid() || !createDatabase(file)) {
            return false;
        }
        ConnectionProfile p;
        p.id = QUuid::createUuid();
        p.name = u"test"_s;
        p.providerId = u"sqlite"_s;
        p.values = { { u"file"_s, file } };
        auto connection = registry.createConnection(p, {});
        if (!connection) {
            return false;
        }
        session = std::make_unique<Session>(std::move(connection).value());
        if (open) {
            session->open();
        }
        cache.attach(session.get());
        return true;
    }
};

} // namespace

class TstMetadataCache : public QObject
{
    Q_OBJECT

private slots:
    void startsEmpty();
    void loadWalksTablesViewsAndSchemas();
    void loadIsIdempotent();
    void resolveIgnoresCaseAndUsesTrailingParts();
    void indexIsSortedAndLowercase();
    void ensureColumnsLoadsOnceAndKeepsColumnNodes();
    void ensureColumnsOfAViewAndOfAnUnknownTable();
    void invalidateEmptiesAndNextLoadStartsOver();
    void lateAnswerAfterInvalidateIsIgnored();
    void lateAnswerAfterAttachIsIgnored();
    void lateColumnsAfterInvalidateAreIgnored();
    void failureEmitsFailedAndLoadRetries();
    void ignoresRequestsOfOtherUsersOfTheSession();
    void sessionDestroyedClearsTheCache();
    void loadWithoutSessionDoesNothing();
};

void TstMetadataCache::startsEmpty()
{
    MetadataCache cache;
    QCOMPARE(cache.state(), MetadataCache::State::Empty);
    QVERIFY(cache.relations().empty());
    QVERIFY(cache.schemas().empty());
    QVERIFY(!cache.resolve({ u"x"_s }));
    QVERIFY(cache.columns(table(u"x"_s)) == nullptr);
}

void TstMetadataCache::loadWalksTablesViewsAndSchemas()
{
    Fixture f;
    QVERIFY(f.init());
    QSignalSpy changed(&f.cache, &MetadataCache::changed);
    f.cache.load();
    QCOMPARE(f.cache.state(), MetadataCache::State::Loading);
    QTRY_COMPARE_WITH_TIMEOUT(f.cache.state(), MetadataCache::State::Ready, 10000);
    QVERIFY(changed.count() >= 1);

    QStringList relations = names(f.cache.relations());
    std::sort(relations.begin(), relations.end());
    QCOMPARE(relations, (QStringList { u"Orders"_s, u"items"_s, u"users"_s, u"v_users"_s }));
    for (const CachedObject &object : f.cache.relations()) {
        QCOMPARE(object.path.parts.size(), qsizetype(2));
        QCOMPARE(object.path.parts.last(), object.name);
        QCOMPARE(object.path.kind,
                 object.name == u"v_users"_s ? ObjectKind::View : ObjectKind::Table);
    }
    // SQLite has no schemas: the database stands in for one.
    QCOMPARE(names(f.cache.schemas()), QStringList { u"main"_s });
    // Columns are not loaded by load().
    QVERIFY(f.cache.columns(table(u"users"_s)) == nullptr);
}

void TstMetadataCache::loadIsIdempotent()
{
    Fixture f;
    QVERIFY(f.init());
    f.cache.load();
    f.cache.load(); // while loading
    QTRY_COMPARE_WITH_TIMEOUT(f.cache.state(), MetadataCache::State::Ready, 10000);
    const size_t count = f.cache.relations().size();
    QSignalSpy changed(&f.cache, &MetadataCache::changed);
    f.cache.load(); // when ready
    QTest::qWait(100);
    QCOMPARE(f.cache.relations().size(), count);
    QCOMPARE(changed.count(), 0);
}

void TstMetadataCache::resolveIgnoresCaseAndUsesTrailingParts()
{
    Fixture f;
    QVERIFY(f.init());
    f.cache.load();
    QTRY_COMPARE_WITH_TIMEOUT(f.cache.state(), MetadataCache::State::Ready, 10000);

    QCOMPARE(resolved(f.cache, { u"users"_s }).parts, (QStringList { u"main"_s, u"users"_s }));
    QCOMPARE(resolved(f.cache, { u"USERS"_s }).parts, (QStringList { u"main"_s, u"users"_s }));
    QCOMPARE(resolved(f.cache, { u"orders"_s }).parts, (QStringList { u"main"_s, u"Orders"_s }));
    QCOMPARE(resolved(f.cache, { u"Main"_s, u"Users"_s }).parts,
             (QStringList { u"main"_s, u"users"_s }));
    QCOMPARE(resolved(f.cache, { u"v_users"_s }).kind, ObjectKind::View);
    QVERIFY(!f.cache.resolve({ u"other"_s, u"users"_s }));
    QVERIFY(!f.cache.resolve({ u"nope"_s }));
    QVERIFY(!f.cache.resolve({}));
}

void TstMetadataCache::indexIsSortedAndLowercase()
{
    Fixture f;
    QVERIFY(f.init());
    f.cache.load();
    QTRY_COMPARE_WITH_TIMEOUT(f.cache.state(), MetadataCache::State::Ready, 10000);
    const auto &keys = f.cache.relationKeys();
    const auto &order = f.cache.relationOrder();
    QCOMPARE(keys.size(), f.cache.relations().size());
    QCOMPARE(order.size(), keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        QCOMPARE(keys[i], f.cache.relations()[i].name.toLower());
    }
    QVERIFY(std::is_sorted(order.begin(), order.end(), [&](qsizetype a, qsizetype b) {
        return keys[static_cast<size_t>(a)] < keys[static_cast<size_t>(b)];
    }));
}

void TstMetadataCache::ensureColumnsLoadsOnceAndKeepsColumnNodes()
{
    Fixture f;
    QVERIFY(f.init());
    f.cache.load();
    QTRY_COMPARE_WITH_TIMEOUT(f.cache.state(), MetadataCache::State::Ready, 10000);

    QSignalSpy changed(&f.cache, &MetadataCache::changed);
    const ObjectPath users = resolved(f.cache, { u"users"_s });
    f.cache.ensureColumns(users);
    f.cache.ensureColumns(users); // once per table
    QVERIFY(f.cache.columns(users) == nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(changed.count(), 1, 10000);
    const std::vector<CachedObject> *columns = f.cache.columns(users);
    QVERIFY(columns != nullptr);
    QCOMPARE(names(*columns), (QStringList { u"id"_s, u"name"_s }));
    for (const CachedObject &column : *columns) {
        QCOMPARE(column.path.kind, ObjectKind::Column);
    }
    QCOMPARE((*columns)[0].detail, u"INTEGER PK"_s);
    QCOMPARE((*columns)[1].detail, u"TEXT NOT NULL"_s);

    QTest::qWait(100);
    QCOMPARE(changed.count(), 1);
    f.cache.ensureColumns(users); // already loaded: nothing happens
    QTest::qWait(100);
    QCOMPARE(changed.count(), 1);
    // Other tables stay unloaded.
    QVERIFY(f.cache.columns(resolved(f.cache, { u"items"_s })) == nullptr);
}

void TstMetadataCache::ensureColumnsOfAViewAndOfAnUnknownTable()
{
    Fixture f;
    QVERIFY(f.init());
    f.cache.load();
    QTRY_COMPARE_WITH_TIMEOUT(f.cache.state(), MetadataCache::State::Ready, 10000);

    const ObjectPath view = resolved(f.cache, { u"v_users"_s });
    f.cache.ensureColumns(view);
    QTRY_VERIFY_WITH_TIMEOUT(f.cache.columns(view) != nullptr, 10000);
    QCOMPARE(names(*f.cache.columns(view)), (QStringList { u"id"_s, u"name"_s }));

    // A table that does not exist answers with no columns: loaded, but empty.
    const ObjectPath ghost = table(u"ghost"_s);
    f.cache.ensureColumns(ghost);
    QTRY_VERIFY_WITH_TIMEOUT(f.cache.columns(ghost) != nullptr, 10000);
    QVERIFY(f.cache.columns(ghost)->empty());
}

void TstMetadataCache::invalidateEmptiesAndNextLoadStartsOver()
{
    Fixture f;
    QVERIFY(f.init());
    f.cache.load();
    QTRY_COMPARE_WITH_TIMEOUT(f.cache.state(), MetadataCache::State::Ready, 10000);
    const ObjectPath users = resolved(f.cache, { u"users"_s });
    f.cache.ensureColumns(users);
    QTRY_VERIFY_WITH_TIMEOUT(f.cache.columns(users) != nullptr, 10000);

    QSignalSpy changed(&f.cache, &MetadataCache::changed);
    f.cache.invalidate();
    QCOMPARE(changed.count(), 1);
    QCOMPARE(f.cache.state(), MetadataCache::State::Empty);
    QVERIFY(f.cache.relations().empty());
    QVERIFY(f.cache.schemas().empty());
    QVERIFY(f.cache.columns(users) == nullptr);
    QVERIFY(!f.cache.resolve({ u"users"_s }));

    f.cache.invalidate(); // nothing to drop: silent
    QCOMPARE(changed.count(), 1);

    f.cache.load();
    QTRY_COMPARE_WITH_TIMEOUT(f.cache.state(), MetadataCache::State::Ready, 10000);
    QCOMPARE(f.cache.relations().size(), size_t(4));
    // Columns are requested again after an invalidation.
    f.cache.ensureColumns(users);
    QTRY_VERIFY_WITH_TIMEOUT(f.cache.columns(users) != nullptr, 10000);
}

void TstMetadataCache::lateAnswerAfterInvalidateIsIgnored()
{
    Fixture f;
    QVERIFY(f.init());
    f.cache.load();
    f.cache.invalidate(); // the root answer is already on its way
    QSignalSpy changed(&f.cache, &MetadataCache::changed);
    QTest::qWait(300);
    QCOMPARE(f.cache.state(), MetadataCache::State::Empty);
    QVERIFY(f.cache.relations().empty());
    QCOMPARE(changed.count(), 0);

    // A new walk is not confused by the stale one.
    f.cache.load();
    QTRY_COMPARE_WITH_TIMEOUT(f.cache.state(), MetadataCache::State::Ready, 10000);
    QCOMPARE(f.cache.relations().size(), size_t(4)); // not doubled
    QCOMPARE(f.cache.schemas().size(), size_t(1));
}

void TstMetadataCache::lateAnswerAfterAttachIsIgnored()
{
    Fixture f;
    QVERIFY(f.init());
    f.cache.load();
    f.cache.attach(nullptr);
    QTest::qWait(300);
    QCOMPARE(f.cache.state(), MetadataCache::State::Empty);
    QVERIFY(f.cache.relations().empty());

    f.cache.attach(f.session.get());
    f.cache.load();
    QTRY_COMPARE_WITH_TIMEOUT(f.cache.state(), MetadataCache::State::Ready, 10000);
    QCOMPARE(f.cache.relations().size(), size_t(4));
}

void TstMetadataCache::lateColumnsAfterInvalidateAreIgnored()
{
    Fixture f;
    QVERIFY(f.init());
    f.cache.load();
    QTRY_COMPARE_WITH_TIMEOUT(f.cache.state(), MetadataCache::State::Ready, 10000);
    const ObjectPath users = resolved(f.cache, { u"users"_s });
    f.cache.ensureColumns(users);
    f.cache.invalidate();
    QSignalSpy changed(&f.cache, &MetadataCache::changed);
    QTest::qWait(300);
    QCOMPARE(changed.count(), 0);
    QVERIFY(f.cache.columns(users) == nullptr);
}

void TstMetadataCache::failureEmitsFailedAndLoadRetries()
{
    Fixture f;
    QVERIFY(f.init(false)); // never opened: children() fails with "The connection is not open."
    QSignalSpy failed(&f.cache, &MetadataCache::failed);
    f.cache.load();
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
    QCOMPARE(f.cache.state(), MetadataCache::State::Failed);
    QVERIFY(f.cache.relations().empty());
    const auto error = failed.at(0).at(0).value<Error>();
    QVERIFY(!error.message.isEmpty());

    f.session->open();
    f.cache.load(); // retries
    QCOMPARE(f.cache.state(), MetadataCache::State::Loading);
    QTRY_COMPARE_WITH_TIMEOUT(f.cache.state(), MetadataCache::State::Ready, 10000);
    QCOMPARE(f.cache.relations().size(), size_t(4));
    QCOMPARE(failed.count(), 1);
}

void TstMetadataCache::ignoresRequestsOfOtherUsersOfTheSession()
{
    Fixture f;
    QVERIFY(f.init());
    f.cache.load();
    // The schema tree uses the same session; its answers are none of the cache's business.
    f.session->children(ObjectPath { {}, ObjectKind::Database });
    f.session->children(table(u"users"_s));
    QTRY_COMPARE_WITH_TIMEOUT(f.cache.state(), MetadataCache::State::Ready, 10000);
    QTest::qWait(100);
    QCOMPARE(f.cache.relations().size(), size_t(4));
    QCOMPARE(f.cache.schemas().size(), size_t(1));
}

void TstMetadataCache::sessionDestroyedClearsTheCache()
{
    Fixture f;
    QVERIFY(f.init());
    f.cache.load();
    QTRY_COMPARE_WITH_TIMEOUT(f.cache.state(), MetadataCache::State::Ready, 10000);
    f.session.reset();
    QCOMPARE(f.cache.state(), MetadataCache::State::Empty);
    QVERIFY(f.cache.relations().empty());
    f.cache.load(); // no session: nothing happens, no crash
    f.cache.ensureColumns(table(u"users"_s));
    QCOMPARE(f.cache.state(), MetadataCache::State::Empty);
}

void TstMetadataCache::loadWithoutSessionDoesNothing()
{
    MetadataCache cache;
    cache.load();
    cache.ensureColumns(table(u"users"_s));
    QCOMPARE(cache.state(), MetadataCache::State::Empty);
}

QTEST_GUILESS_MAIN(TstMetadataCache)
#include "tst_metadata_cache.moc"
