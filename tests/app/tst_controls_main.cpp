#include "test_screen.h"
#include "ui/plan_model.h"

#include <QAccessible>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QtQuickTest/quicktest.h>

#include <algorithm>
#include <sqlite3.h>

namespace {

// A small database for the schema tree tests: invoices/items start with the same letter, so
// type-ahead has something to tell apart. Returns false if it could not be written.
bool createSampleDatabase(const QString &file, int extraTables = 0)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open(file.toUtf8().constData(), &db) != SQLITE_OK)
        return false;
    const char *sql = "CREATE TABLE customers(id INTEGER PRIMARY KEY, name TEXT NOT NULL);"
                      "CREATE TABLE invoices(id INTEGER PRIMARY KEY, customer_id INTEGER "
                      "REFERENCES customers(id), total REAL);"
                      "CREATE INDEX invoices_customer ON invoices(customer_id);"
                      "CREATE TABLE items(id INTEGER PRIMARY KEY, invoice_id INTEGER);"
                      "CREATE TABLE products(id INTEGER PRIMARY KEY, title TEXT);"
                      "CREATE VIEW totals AS SELECT customer_id, sum(total) FROM invoices;";
    bool ok = sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    if (ok && extraTables > 0) {
        QByteArray more = "BEGIN;";
        for (int i = 0; i < extraTables; ++i)
            more += "CREATE TABLE big" + QByteArray::number(i) + "(id INTEGER PRIMARY KEY);";
        more += "COMMIT;";
        ok = sqlite3_exec(db, more.constData(), nullptr, nullptr, nullptr) == SQLITE_OK;
    }
    sqlite3_close(db);
    return ok;
}

// A Laravel project for the migrations tests: a database with the `migrations` table (two applied,
// one with its file gone) and the folder with the .php files (the two applied ones, two pending and
// one over the 1 MiB the viewer reads, so reading it fails).
bool createMigrationsFixture(const QString &dbFile, const QString &folder)
{
    if (!QDir().mkpath(folder))
        return false;
    const auto write = [&](const QString &name, const QByteArray &content) {
        QFile f(QDir(folder).filePath(name + QStringLiteral(".php")));
        return f.open(QIODevice::WriteOnly) && f.write(content) == content.size();
    };
    const auto code = [](const QString &table) {
        return QStringLiteral("<?php\n\nuse Illuminate\\Support\\Facades\\Schema;\n\n"
                              "return new class extends Migration {\n"
                              "    public function up(): void\n    {\n"
                              "        Schema::create('%1', function (Blueprint $table) {\n"
                              "            $table->id();\n        });\n    }\n};\n")
            .arg(table)
            .toUtf8();
    };
    bool ok = write(QStringLiteral("2024_01_01_000000_create_users_table"),
                    code(QStringLiteral("users")))
        && write(QStringLiteral("2024_02_01_000000_add_email_to_users"),
                 code(QStringLiteral("users")))
        && write(QStringLiteral("2024_03_01_000000_create_posts_table"),
                 code(QStringLiteral("posts")))
        && write(QStringLiteral("2024_04_01_000000_create_comments_table"),
                 code(QStringLiteral("comments")))
        && write(QStringLiteral("2024_05_01_000000_huge"), QByteArray(1024 * 1024 + 1, 'x'));
    sqlite3 *db = nullptr;
    if (!ok || sqlite3_open(dbFile.toUtf8().constData(), &db) != SQLITE_OK)
        return false;
    const char *sql = "CREATE TABLE migrations(id INTEGER PRIMARY KEY, migration TEXT NOT NULL, "
                      "batch INTEGER NOT NULL);"
                      "CREATE TABLE users(id INTEGER PRIMARY KEY);"
                      "INSERT INTO migrations(migration, batch) VALUES "
                      "('2023_12_31_000000_ghost', 1),"
                      "('2024_01_01_000000_create_users_table', 1),"
                      "('2024_02_01_000000_add_email_to_users', 2);";
    ok = sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(db);
    return ok;
}

// What a screen reader bridge gets from an item: the platform-neutral accessibility interface.
class AccessibilityProbe : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    // The system clipboard, for the grid's copy tests (they put the user's text back).
    Q_INVOKABLE QString clipboardText() const { return QGuiApplication::clipboard()->text(); }
    Q_INVOKABLE void setClipboardText(const QString &text) const
    {
        QGuiApplication::clipboard()->setText(text);
    }

    // The environment of this process, for the fake `php` of the migration tests (FAKE_PHP_MODE,
    // FAKE_PHP_LOG, FAKE_PHP_TARGET_JSON): a process started afterwards inherits it. Empty = unset.
    Q_INVOKABLE void setEnvironment(const QString &name, const QString &value) const
    {
        if (value.isEmpty())
            qunsetenv(name.toUtf8().constData());
        else
            qputenv(name.toUtf8().constData(), value.toUtf8());
    }

    // Plain files for the export tests: read what was written, make an existing file, lock it.
    Q_INVOKABLE QString readFile(const QString &path) const
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    }
    // void, not bool: moc emits a std::move of a bool result that clang-tidy rejects. The tests
    // check the effect with readFile() instead.
    Q_INVOKABLE void writeFile(const QString &path, const QString &text) const
    {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly))
            f.write(text.toUtf8());
    }
    Q_INVOKABLE void setReadOnly(const QString &path, bool readOnly) const
    {
        QFile::setPermissions(path,
                              readOnly ? QFileDevice::ReadOwner
                                       : (QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    }

    // Plans for the Plan view tests (PlanView.qml): "costs" (analyzed, with times, warnings and a
    // loop), "many" (5000 steps), "estimated" (costs only), "sqlite" (no cost, a hidden root),
    // "truncated", "failed" (the engine's text and no tree) and "running". The model lives as long
    // as the probe.
    Q_INVOKABLE QVariant makePlan(const QString &kind)
    {
        auto *model = new quarry::ui::PlanModel(this);
        fillPlan(*model, kind);
        // A QVariant: moc would std::move() a bare pointer result, which clang-tidy rejects.
        return QVariant::fromValue<QObject *>(model);
    }
    // The same on a model that exists already (a plan that arrives while the view waits).
    Q_INVOKABLE void refill(QObject *model, const QString &kind)
    {
        if (auto *plan = qobject_cast<quarry::ui::PlanModel *>(model))
            fillPlan(*plan, kind);
    }

    static void fillPlan(quarry::ui::PlanModel &model, const QString &kind)
    {
        using quarry::ExplainNode;
        using quarry::ExplainPlan;
        using quarry::ExplainProperty;
        const auto step = [](const QString &label, double cost, double timeMs) {
            ExplainNode n;
            n.label = label;
            n.totalCost = cost;
            n.estimatedRows = 1000;
            if (timeMs >= 0) {
                n.actualRows = 1200;
                n.actualTimeMs = timeMs;
            }
            return n;
        };
        if (kind == QLatin1String("running")) {
            model.begin(false);
        } else if (kind == QLatin1String("failed")) {
            model.begin(false);
            model.setFailure(QStringLiteral("The plan could not be read."),
                             QStringLiteral("unexpected token"),
                             QStringLiteral("QUERY PLAN\nSeq Scan on t\n  Filter: (a > 1)"));
        } else if (kind == QLatin1String("many")) {
            // The 5000 steps the parsers allow: 50 top-level steps with 99 children each.
            ExplainPlan p;
            p.engine = QStringLiteral("PostgreSQL");
            p.rawText = QStringLiteral("many");
            for (int i = 0; i < 50; ++i) {
                ExplainNode top = step(QStringLiteral("Top %1").arg(i), 1000, -1);
                for (int k = 0; k < 99; ++k)
                    top.children.push_back(
                        step(QStringLiteral("Child %1.%2").arg(i).arg(k), 5, -1));
                p.root.children.push_back(top);
            }
            model.setPlan(p);
        } else if (kind == QLatin1String("sqlite")) {
            ExplainPlan p;
            p.engine = QStringLiteral("SQLite");
            ExplainNode a;
            a.label = QStringLiteral("SEARCH orders USING INDEX orders_customer (customer_id=?)");
            ExplainNode b;
            b.label = QStringLiteral("SCAN items");
            ExplainNode c;
            c.label = QStringLiteral("USE TEMP B-TREE FOR ORDER BY");
            b.children.push_back(c);
            p.root.children.push_back(a);
            p.root.children.push_back(b);
            p.rawText = QStringLiteral("3\t0\tSEARCH orders\n7\t0\tSCAN items");
            model.setPlan(p);
        } else {
            const bool analyzed = kind == QLatin1String("costs");
            ExplainPlan p;
            p.engine = analyzed ? QStringLiteral("PostgreSQL") : QStringLiteral("MySQL");
            p.analyzed = analyzed;
            p.truncated = kind == QLatin1String("truncated");
            p.root = step(QStringLiteral("Hash Join"), 100, analyzed ? 50 : -1);
            ExplainNode scan = step(QStringLiteral("Seq Scan on orders"), 60, analyzed ? 30 : -1);
            scan.details.push_back(
                ExplainProperty { QStringLiteral("Filter"), QStringLiteral("(total > 100)") });
            scan.details.push_back(
                ExplainProperty { QStringLiteral("Relation"), QStringLiteral("orders") });
            scan.warnings << QStringLiteral("Rows estimate off by 10x");
            ExplainNode hash = step(QStringLiteral("Hash"), 30, analyzed ? 10 : -1);
            ExplainNode inner
                = step(QStringLiteral("Index Scan on customers"), 20, analyzed ? 8 : -1);
            inner.loops = 3;
            hash.children.push_back(inner);
            p.root.children.push_back(scan);
            p.root.children.push_back(hash);
            p.rawText = QStringLiteral("Hash Join  (cost=100)\n  -> Seq Scan on orders\n  -> Hash");
            if (analyzed) {
                p.planningTimeMs = 0.3;
                p.executionTimeMs = 50;
            }
            model.begin(analyzed);
            model.setPlan(p);
        }
    }

    Q_INVOKABLE QVariantMap inspect(QObject *object) const
    {
        QAccessible::setActive(true);
        // Back to inactive on return: an active bridge slows every later model change in the run.
        struct Off
        {
            ~Off() { QAccessible::setActive(false); }
        } off;
        QVariantMap out;
        QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(object);
        if (iface == nullptr)
            return out;
        const QAccessible::State st = iface->state();
        out.insert(QStringLiteral("role"), static_cast<int>(iface->role()));
        out.insert(QStringLiteral("name"), iface->text(QAccessible::Name));
        out.insert(QStringLiteral("description"), iface->text(QAccessible::Description));
        out.insert(QStringLiteral("expandable"), bool(st.expandable));
        out.insert(QStringLiteral("expanded"), bool(st.expanded));
        out.insert(QStringLiteral("collapsed"), bool(st.collapsed));
        out.insert(QStringLiteral("selected"), bool(st.selected));
        out.insert(QStringLiteral("focusable"), bool(st.focusable));
        out.insert(QStringLiteral("disabled"), bool(st.disabled));
        out.insert(QStringLiteral("focused"), bool(st.focused));
        out.insert(QStringLiteral("childCount"), iface->childCount());
        QStringList children;
        for (int i = 0; i < std::min(iface->childCount(), 40); ++i)
            if (QAccessibleInterface *c = iface->child(i))
                children << QStringLiteral("%1:%2:%3:%4x%5:%6")
                                .arg(static_cast<int>(c->role()))
                                .arg(c->text(QAccessible::Name))
                                .arg(c->childCount())
                                .arg(c->rect().width())
                                .arg(c->rect().height())
                                .arg(c->object() ? QString::fromLatin1(
                                                       c->object()->metaObject()->className())
                                                 : QString());
        out.insert(QStringLiteral("children"), children);
        if (QAccessibleInterface *p = iface->parent())
            out.insert(QStringLiteral("parentRole"), static_cast<int>(p->role()));
        return out;
    }
};

} // namespace

// Keeps the tests away from the real user settings and makes Tab reach every control.
class Setup : public QObject
{
    Q_OBJECT

public slots:
    void applicationAvailable()
    {
        quarry::testing::ScreenPlacer::install();
        QCoreApplication::setOrganizationName(QStringLiteral("QuarryTest"));
        QCoreApplication::setApplicationName(QStringLiteral("tst_controls"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir.path());
        // Qt's own file dialog (the fallback offscreen) is built on the Basic style.
        qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
        QStandardPaths::setTestModeEnabled(true); // never the user's real data directory
        QGuiApplication::styleHints()->setTabFocusBehavior(Qt::TabFocusAllControls);
        // The tests read English strings whatever the language of the machine running them.
        QSettings().setValue(QStringLiteral("ui/language"), QStringLiteral("English"));
    }

    // QML reads these: a temporary directory for the connection databases and test files, and
    // the folder of the real SQLite plugin (never the keychain: no test saves a password).
    void qmlEngineAvailable(QQmlEngine *engine)
    {
        engine->rootContext()->setContextProperty(QStringLiteral("testTempDir"), m_work.path());
        engine->rootContext()->setContextProperty(QStringLiteral("testPluginDir"),
                                                  QStringLiteral(QUARRY_PLUGIN_DIR));
        const QString sample = m_work.filePath(QStringLiteral("sample.db"));
        if (!QFileInfo::exists(sample))
            createSampleDatabase(sample);
        engine->rootContext()->setContextProperty(QStringLiteral("testSampleDb"), sample);
        const QString big = m_work.filePath(QStringLiteral("big.db")); // 10 000 tables
        if (!QFileInfo::exists(big))
            createSampleDatabase(big, 10'000);
        engine->rootContext()->setContextProperty(QStringLiteral("testBigDb"), big);
        const QString migrationsDb = m_work.filePath(QStringLiteral("laravel.db"));
        const QString migrationsDir = m_work.filePath(QStringLiteral("laravel-migrations"));
        if (!QFileInfo::exists(migrationsDb))
            createMigrationsFixture(migrationsDb, migrationsDir);
        engine->rootContext()->setContextProperty(QStringLiteral("testMigrationsDb"), migrationsDb);
        // The project root of those migrations (it holds `artisan`) and the fake `php` that runs
        // instead of PHP: it only logs its arguments. No test runs a real PHP or migration.
        const QString projectRoot = m_work.filePath(QStringLiteral("laravel-project"));
        if (QDir().mkpath(projectRoot)) {
            QFile artisan(projectRoot + QStringLiteral("/artisan"));
            if (!artisan.exists() && artisan.open(QIODevice::WriteOnly))
                artisan.write("<?php\n");
        }
        engine->rootContext()->setContextProperty(QStringLiteral("testMigrationsRoot"),
                                                  projectRoot);
        engine->rootContext()->setContextProperty(QStringLiteral("testFakePhp"),
                                                  QStringLiteral(QUARRY_FAKE_PHP));
        engine->rootContext()->setContextProperty(QStringLiteral("testMigrationsDir"),
                                                  migrationsDir);
        // The stand-in for `claude` of tests/ai (FAKE_CLAUDE_MODE picks what it does). The real
        // Claude Code is never run by a test.
        engine->rootContext()->setContextProperty(QStringLiteral("testFakeClaude"),
                                                  QStringLiteral(QUARRY_FAKE_CLAUDE));
        // The frame budget (PERFORMANCE.md) is for optimized builds; a debug build is several
        // times slower in the QML engine and the TreeView, so it only has to stay responsive.
        // Shared CI runners add noise the budgets do not account for: PERFORMANCE.md allows +25 %
        // in the CI (GitHub sets CI=true). A median of 53 ms against 50 failed the Windows runner.
        [[maybe_unused]] const bool onCi = qEnvironmentVariableIsSet("CI");
#ifdef QT_NO_DEBUG
        const int kMaxGapMs = onCi ? 62 : 50;
#else
        constexpr int kMaxGapMs = 5000;
#endif
        engine->rootContext()->setContextProperty(QStringLiteral("testMaxGapMs"), kMaxGapMs);
        // Typing in a 10 000 line file, key press to painted frame (PERFORMANCE.md: p95 16 ms).
#ifdef QT_NO_DEBUG
        const int kMaxKeystrokeMs = onCi ? 20 : 16;
#else
        constexpr int kMaxKeystrokeMs = 1000;
#endif
        engine->rootContext()->setContextProperty(QStringLiteral("testMaxKeystrokeMs"),
                                                  kMaxKeystrokeMs);
        engine->rootContext()->setContextProperty(QStringLiteral("a11yProbe"),
                                                  new AccessibilityProbe(engine));
    }

private:
    QTemporaryDir m_dir;
    QTemporaryDir m_work;
};

QUICK_TEST_MAIN_WITH_SETUP(tst_controls, Setup)

#include "tst_controls_main.moc"
