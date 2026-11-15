// Autocompletion benchmark (ARCHITECTURE.md 6.5): a MetadataCache holding 10 000 tables x 20
// columns, loaded through the real SQLite plugin and Session, then analyzeSqlContext + complete()
// for typical statements. Fixture generation and loading are excluded from the timings.
#include <quarry/completion.h>
#include <quarry/metadata_cache.h>
#include <quarry/provider_registry.h>
#include <quarry/session.h>
#include <quarry/sql_context.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTextStream>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <sqlite3.h>
#include <vector>

namespace {

constexpr int kColumns = 20;
constexpr int kRuns = 300;
constexpr double kBudgetMs = 50.0;

bool generate(const QString &path, int tables)
{
    static const char *const kWords[] = { "users", "orders",   "items",    "accounts", "events",
                                          "logs",  "payments", "products", "sessions", "invoices" };
    sqlite3 *db = nullptr;
    if (sqlite3_open(path.toUtf8().constData(), &db) != SQLITE_OK) {
        return false;
    }
    QByteArray sql = "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; BEGIN;";
    for (int i = 0; i < tables; ++i) {
        sql += "CREATE TABLE " + QByteArray(kWords[i % 10]) + QByteArray::number(i)
            + "(id INTEGER PRIMARY KEY, name TEXT NOT NULL";
        for (int c = 2; c < kColumns; ++c) {
            sql += ", c" + QByteArray::number(c) + " TEXT";
        }
        sql += ");";
        if (i % 7 == 0) {
            sql += "CREATE VIEW v_" + QByteArray(kWords[i % 10]) + QByteArray::number(i)
                + " AS SELECT id FROM " + QByteArray(kWords[i % 10]) + QByteArray::number(i) + ";";
        }
    }
    sql += "COMMIT;";
    const bool ok = sqlite3_exec(db, sql.constData(), nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(db);
    return ok;
}

bool waitFor(const std::function<bool()> &done, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return true;
}

struct Scenario
{
    const char *name;
    QString sql; // the cursor is at the end
};

} // namespace

int run(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const int tables = argc > 1 ? std::max(1, QString::fromLocal8Bit(argv[1]).toInt()) : 10000;

    QTemporaryDir dir;
    if (!dir.isValid()) {
        return 2;
    }
    const QString file = dir.filePath(QStringLiteral("bench.sqlite"));
    if (!generate(file, tables)) {
        std::fprintf(stderr, "fixture generation failed\n");
        return 2;
    }

    quarry::ProviderRegistry registry;
    registry.scan(QStringLiteral(QUARRY_PLUGIN_DIR));
    QString error;
    quarry::IDatabaseProvider *provider = registry.provider(QStringLiteral("sqlite"), &error);
    if (!provider) {
        std::fprintf(stderr, "cannot load the sqlite plugin: %s\n", qPrintable(error));
        return 2;
    }
    quarry::ConnectionProfile profile;
    profile.id = QUuid::createUuid();
    profile.name = QStringLiteral("bench");
    profile.providerId = QStringLiteral("sqlite");
    profile.values = { { QStringLiteral("file"), file } };
    auto connection = registry.createConnection(profile, {});
    if (!connection) {
        std::fprintf(stderr, "cannot create the connection\n");
        return 2;
    }
    quarry::Session session(std::move(connection).value());
    session.open();
    quarry::MetadataCache cache;
    cache.attach(&session);

    QElapsedTimer clock;
    clock.start();
    cache.load();
    if (!waitFor([&] { return cache.state() == quarry::MetadataCache::State::Ready; }, 120000)) {
        std::fprintf(stderr, "load did not finish\n");
        return 2;
    }
    const qint64 loadMs = clock.elapsed();
    clock.start();
    for (const quarry::CachedObject &object : cache.relations()) {
        cache.ensureColumns(object.path);
    }
    const auto allColumns = [&] {
        return std::all_of(
            cache.relations().begin(), cache.relations().end(),
            [&](const quarry::CachedObject &o) { return cache.columns(o.path) != nullptr; });
    };
    if (!waitFor(allColumns, 300000)) {
        std::fprintf(stderr, "columns did not load\n");
        return 2;
    }
    std::fprintf(stderr, "fixture: %zu relations, walk %lld ms, columns %lld ms (excluded)\n",
                 cache.relations().size(), static_cast<long long>(loadMs),
                 static_cast<long long>(clock.elapsed()));

    const quarry::ISqlDialect &dialect = provider->dialect();
    const std::vector<Scenario> scenarios = {
        { "prefix_1_char", QStringLiteral("SELECT * FROM u") },
        { "prefix_2_chars", QStringLiteral("SELECT * FROM us") },
        { "prefix_3_chars", QStringLiteral("SELECT * FROM use") },
        { "prefix_3_digits_tail", QStringLiteral("SELECT * FROM users12") },
        { "substring_only", QStringLiteral("SELECT * FROM ers") },
        { "no_match", QStringLiteral("SELECT * FROM zzz") },
        { "empty_prefix", QStringLiteral("SELECT * FROM ") },
        { "keyword_scope_2_chars", QStringLiteral("se") },
        { "member_alias", QStringLiteral("SELECT x. FROM users10 x") },
        { "member_alias_prefix", QStringLiteral("SELECT x.c1 FROM users10 x") },
        { "member_schema", QStringLiteral("SELECT * FROM main.us") },
        { "column_scope",
          QStringLiteral("SELECT c1 FROM users10 a JOIN orders11 b ON a.id = b.id") },
    };

    QTextStream out(stdout);
    double worstP95 = 0;
    double worstMax = 0;
    for (const Scenario &s : scenarios) {
        // The cursor sits at the end, except where the text continues after it.
        qsizetype cursor = s.sql.size();
        if (s.sql.startsWith(QLatin1String("SELECT x."))) {
            cursor = s.sql.indexOf(QLatin1String(" FROM"));
        } else if (s.sql.startsWith(QLatin1String("SELECT c1 FROM"))) {
            cursor = QStringLiteral("SELECT c1").size();
        }
        std::vector<double> ms;
        ms.reserve(kRuns);
        size_t results = 0;
        for (int run = 0; run < kRuns + 5; ++run) {
            QElapsedTimer timer;
            timer.start();
            const quarry::SqlContext ctx = quarry::analyzeSqlContext(s.sql, cursor);
            const auto list = quarry::CompletionEngine::complete(ctx, cache, dialect);
            const double elapsed = static_cast<double>(timer.nsecsElapsed()) / 1e6;
            results = list.size();
            if (run >= 5) { // warm-up
                ms.push_back(elapsed);
            }
        }
        std::sort(ms.begin(), ms.end());
        const double p50 = ms[ms.size() / 2];
        const double p95 = ms[ms.size() * 95 / 100];
        const double max = ms.back();
        worstP95 = std::max(worstP95, p95);
        worstMax = std::max(worstMax, max);
        out << "bench_completion: " << s.name << " (" << results << " results) p50 " << p50
            << " ms, p95 " << p95 << " ms, max " << max << " ms\n";
    }
    out << "bench_completion: " << cache.relations().size() << " relations x " << kColumns
        << " columns, worst p95 " << worstP95 << " ms, worst max " << worstMax << " ms (budget "
        << kBudgetMs << " ms)\n";
    out << "{\"metric\":\"completion_worst_p95_ms\",\"value\":" << worstP95
        << ",\"unit\":\"ms\"}\n";
    out << "{\"metric\":\"completion_worst_max_ms\",\"value\":" << worstMax
        << ",\"unit\":\"ms\"}\n";
    out.flush();
    return worstMax <= kBudgetMs ? 0 : 1;
}

int main(int argc, char **argv)
{
    try {
        return run(argc, argv);
    } catch (...) {
        std::fprintf(stderr, "bench_completion: unexpected exception\n");
        return 2;
    }
}
