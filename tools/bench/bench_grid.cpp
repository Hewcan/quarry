// bench_grid: the results grid with 1 000 000 rows x 10 columns from a local SQLite file
// (docs/PERFORMANCE.md). It runs the real pipeline: EditorTabsModel -> QueryController -> Session
// on its own thread -> ResultTableModel -> QuarryGrid.qml in a window, and prints JSON metric
// lines:
//   grid_first_batch_ms      execute -> the first rows are painted in the grid (warm session)
//   grid_first_batch_cold_ms the same for the first run of the tab (opens the session, loads the
//   plugin) grid_fetch_all_ms        "Fetch all" until the last row is in the model, the window
//   painting grid_ram_after_fetch_all_mb  private working set of the process after it (Windows; RSS
//   elsewhere) grid_scroll_*            a programmatic scroll over the whole range: frame time =
//   from the
//                            scroll step to the end of the frame (afterFrameEnd of a grab)
// Run it by hand (Qt's bin directory on PATH on Windows), on the native platform for real numbers:
//   build/windows-release/bench_grid.exe [rows]
// The fixture is written by a child process, so its memory does not count in the numbers.
#include "../../tests/app/test_screen.h"
#include "ui/connection_controller.h"
#include "ui/editor_tabs_model.h"
#include "ui/query_controller.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <random>
#include <sqlite3.h>
#include <vector>

#ifdef Q_OS_WIN
// clang-format off
#  define NOMINMAX
#  include <windows.h>
#  include <psapi.h>
// clang-format on
#elif defined(Q_OS_MACOS)
#include <mach/mach.h>
#else
#include <cstdlib>
#include <cstring>
#endif

using namespace quarry::ui;

namespace {

// Deterministic, like bench_core: 3 int64, 2 double, 3 text, 1 ISO timestamp, 1 text with ~10 %
// NULL.
bool generate(const QString &path, int rows)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open(path.toUtf8().constData(), &db) != SQLITE_OK)
        return false;
    const auto ok
        = [&](int rc) { return rc == SQLITE_OK || rc == SQLITE_DONE || rc == SQLITE_ROW; };
    bool good
        = ok(sqlite3_exec(db,
                          "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;"
                          "CREATE TABLE t(i1 INTEGER, i2 INTEGER, i3 INTEGER, d1 REAL, d2 REAL,"
                          " s1 TEXT, s2 TEXT, s3 TEXT, ts TEXT, n TEXT); BEGIN",
                          nullptr, nullptr, nullptr));
    sqlite3_stmt *stmt = nullptr;
    good = good
        && ok(sqlite3_prepare_v2(db, "INSERT INTO t VALUES(?,?,?,?,?,?,?,?,?,?)", -1, &stmt,
                                 nullptr));
    std::mt19937_64 rng(42);
    static const char kAlphabet[]
        = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
    char text[64];
    char stamp[32];
    const auto randomText = [&] {
        const int length = 20 + static_cast<int>(rng() % 31);
        for (int i = 0; i < length; ++i)
            text[i] = kAlphabet[rng() % (sizeof(kAlphabet) - 1)];
        return length;
    };
    for (int row = 0; good && row < rows; ++row) {
        sqlite3_bind_int64(stmt, 1, row);
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(rng() % 1000000));
        sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(rng()));
        sqlite3_bind_double(stmt, 4, static_cast<double>(rng() % 1000000) / 100.0);
        sqlite3_bind_double(stmt, 5, static_cast<double>(rng() % 1000000000) / 7.0);
        for (int c = 6; c <= 8; ++c)
            sqlite3_bind_text(stmt, c, text, randomText(), SQLITE_TRANSIENT);
        const unsigned secs = static_cast<unsigned>(rng() % (std::uint64_t(86400) * 365 * 5));
        std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                      2020 + secs / 31536000U, 1 + (secs / 86400U) % 12, 1 + (secs / 3600U) % 28,
                      (secs / 3600U) % 24, (secs / 60U) % 60, secs % 60);
        sqlite3_bind_text(stmt, 9, stamp, -1, SQLITE_TRANSIENT);
        if (rng() % 10 == 0)
            sqlite3_bind_null(stmt, 10);
        else
            sqlite3_bind_text(stmt, 10, text, randomText(), SQLITE_TRANSIENT);
        good = ok(sqlite3_step(stmt));
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    good = good && ok(sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr));
    sqlite3_close(db);
    return good;
}

// Private working set on Windows (what only this process holds); resident set elsewhere.
double memoryMb()
{
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX2 counters {};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                             sizeof(counters)))
        return static_cast<double>(counters.PrivateWorkingSetSize) / (1024.0 * 1024.0);
    return -1;
#elif defined(Q_OS_MACOS)
    mach_task_basic_info info {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count)
        != KERN_SUCCESS)
        return -1;
    return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
#else
    FILE *file = std::fopen("/proc/self/status", "r");
    if (!file)
        return -1;
    char line[256];
    double kb = -1;
    while (std::fgets(line, sizeof(line), file)) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            kb = std::atof(line + 6);
            break;
        }
    }
    std::fclose(file);
    return kb / 1024.0;
#endif
}

double percentile(std::vector<double> &v, double q)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[std::min(v.size() - 1, static_cast<std::size_t>(q * v.size()))];
}

void metric(const char *name, double value, const char *unit)
{
    std::printf("{\"metric\":\"%s\",\"value\":%.2f,\"unit\":\"%s\"}\n", name, value, unit);
    std::fflush(stdout);
}

void reportFrames(const char *prefix, std::vector<double> &frames)
{
    const std::string p = prefix;
    metric((p + "_p50_ms").c_str(), percentile(frames, 0.5), "ms");
    metric((p + "_p95_ms").c_str(), percentile(frames, 0.95), "ms");
    metric((p + "_max_ms").c_str(), frames.empty() ? 0.0 : frames.back(), "ms");
    int over = 0;
    for (const double f : frames)
        over += f > 33.0 ? 1 : 0;
    metric((p + "_frames_over_33ms").c_str(), over, "frames");
}

} // namespace

static int run(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    quarry::testing::ScreenPlacer::install();
    QCoreApplication::setOrganizationName(QStringLiteral("QuarryBench"));
    QCoreApplication::setApplicationName(QStringLiteral("bench_grid"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir dir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    const int rows = argc > 1 ? std::max(1, QString::fromLocal8Bit(argv[1]).toInt()) : 1'000'000;

    const QString fixture = dir.filePath(QStringLiteral("bench.sqlite"));
    QElapsedTimer clock;
    clock.start();
    if (QProcess::execute(QCoreApplication::applicationFilePath(),
                          { QStringLiteral("--make-fixture"), fixture, QString::number(rows) })
        != 0) {
        std::fprintf(stderr, "fixture generation failed\n");
        return 1;
    }
    std::fprintf(stderr, "fixture: %d rows in %lld ms (excluded)\n", rows,
                 static_cast<long long>(clock.elapsed()));

    // Declared in this order so the QueryController dies before the plugins' registry.
    ConnectionController controller;
    controller.setDatabasePath(dir.filePath(QStringLiteral("quarry.db")));
    controller.setProviderDirectory(QStringLiteral(QUARRY_PLUGIN_DIR));
    controller.start();
    QSignalSpy saved(&controller, &ConnectionController::connectionSaved);
    controller.openSqliteFile(QUrl::fromLocalFile(fixture));
    if (!saved.wait(10000)) {
        std::fprintf(stderr, "could not save the bench connection\n");
        return 1;
    }
    EditorTabsModel tabs;
    tabs.setDatabasePath(dir.filePath(QStringLiteral("tabs.db")));
    tabs.setConnections(&controller);
    tabs.setSaveDelay(600000);
    tabs.start();
    QSignalSpy loaded(&tabs, &EditorTabsModel::loadedChanged);
    if (!tabs.loaded() && !loaded.wait(10000)) {
        std::fprintf(stderr, "tabs did not load\n");
        return 1;
    }
    QueryController queries;
    queries.setTabs(&tabs);
    const int row = tabs.openTab(saved.at(0).at(0).toString());
    const QString sql = QStringLiteral("SELECT * FROM t");
    tabs.setText(row, sql);
    QueryState *state = queries.stateOf(tabs.idOf(row));
    ResultTableModel *results = state->results();

    QQmlEngine engine;
    QQmlComponent component(
        &engine, QUrl::fromLocalFile(QStringLiteral(QUARRY_APP_QML_DIR "/QuarryGrid.qml")));
    QQuickWindow window;
    window.resize(1200, 800);
    QObject *object = component.createWithInitialProperties(
        { { QStringLiteral("results"), QVariant::fromValue(results) } });
    auto *grid = qobject_cast<QQuickItem *>(object);
    if (grid == nullptr) {
        std::fprintf(stderr, "QuarryGrid.qml: %s\n", qPrintable(component.errorString()));
        return 1;
    }
    grid->setParentItem(window.contentItem());
    grid->setSize(window.size());
    window.show();
    if (!QTest::qWaitForWindowExposed(&window)) {
        std::fprintf(stderr, "window not exposed\n");
        return 1;
    }
    auto *table = qvariant_cast<QQuickItem *>(grid->property("tableView"));
    QElapsedTimer timer;
    // Emitted on the render thread (direct connection): only timestamps and a counter here.
    std::atomic<qint64> lastFrameEnd { 0 };
    std::atomic<int> frames { 0 };
    QObject::connect(
        &window, &QQuickWindow::afterFrameEnd, &window,
        [&] {
            lastFrameEnd = timer.nsecsElapsed();
            ++frames;
        },
        Qt::DirectConnection);
    QTest::qWait(300); // the first empty frames

    // ---- execute -> first rows painted (cold, then warm) --------------------------------------
    // Painted = the rows are in the model, the first cell exists, and a frame that holds it was
    // rendered (a grab, which does not wait for vsync).
    const auto firstCellExists = [&] {
        QQuickItem *cell = nullptr;
        QMetaObject::invokeMethod(table, "itemAtCell", Q_RETURN_ARG(QQuickItem *, cell),
                                  Q_ARG(QPoint, QPoint(0, 0)));
        return results->rows() > 0 && cell != nullptr;
    };
    for (const char *name : { "grid_first_batch_cold_ms", "grid_first_batch_ms" }) {
        timer.start();
        tabs.run(row, false, 0);
        while (!firstCellExists() && timer.elapsed() < 20000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        window.grabWindow();
        metric(name, static_cast<double>(timer.nsecsElapsed()) / 1e6, "ms");
        if (!QTest::qWaitFor([&] { return state->state() == QLatin1String("done"); }, 20000)) {
            std::fprintf(stderr, "the query did not finish\n");
            return 1;
        }
        QTest::qWait(200);
    }
    metric("grid_first_batch_rows", results->rows(), "rows");

    // ---- Fetch all -----------------------------------------------------------------------------
    // BENCH_HIDE_GRID=1: Fetch all with the grid hidden, to tell the model from the view.
    if (qEnvironmentVariableIsSet("BENCH_HIDE_GRID"))
        grid->setVisible(false);
    const double baseline = memoryMb();
    timer.start();
    results->fetchAll();
    // A real event loop (not QTest::qWait, whose sleeps are 15 ms steps on Windows and would be
    // counted once per fetch round trip).
    QEventLoop loop;
    QTimer poll;
    poll.setTimerType(Qt::PreciseTimer);
    poll.setInterval(1);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if ((!results->moreAvailable() && !results->fetchingAll() && results->rows() >= rows)
            || timer.elapsed() > 120000)
            loop.quit();
    });
    poll.start();
    loop.exec();
    poll.stop();
    const double fetchMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    metric("grid_fetch_all_ms", fetchMs, "ms");
    metric("grid_rows", results->rows(), "rows");
    if (results->rows() != rows) {
        std::fprintf(stderr, "expected %d rows, got %d\n", rows, results->rows());
        return 1;
    }
    QTest::qWait(200);
    metric("grid_ram_before_mb", baseline, "MB");
    metric("grid_ram_after_fetch_all_mb", memoryMb(), "MB");

    // ---- scroll over the whole range ---------------------------------------------------------
    const auto step = [&](double y) {
        const qint64 start = timer.nsecsElapsed();
        table->setProperty("contentY", y);
        QCoreApplication::processEvents();
        window.grabWindow(); // polish, sync and render of the frame, without waiting for vsync
        const qint64 end = std::max(lastFrameEnd.load(), start);
        return static_cast<double>(end > start ? end - start : timer.nsecsElapsed() - start) / 1e6;
    };
    timer.start();
    lastFrameEnd = 0;
    frames = 0;
    const double contentHeight = table->property("contentHeight").toDouble();
    const double span = std::max(0.0, contentHeight - table->height());
    // The whole range in 800 jumps (about 1 250 rows per frame, like dragging the scroll bar).
    std::vector<double> jumps;
    jumps.reserve(801);
    constexpr int kJumps = 800;
    for (int i = 0; i <= kJumps; ++i)
        jumps.push_back(step(span * i / kJumps));
    reportFrames("grid_scroll", jumps);
    // Smooth: the wheel, 3 rows per frame, in the middle of the data.
    std::vector<double> smooth;
    smooth.reserve(600);
    const double rowHeight = grid->property("rowHeight").toDouble();
    for (int i = 0; i < 600; ++i)
        smooth.push_back(step(span / 2 + i * rowHeight * 3));
    reportFrames("grid_scroll_wheel", smooth);
    metric("grid_scroll_frames_signalled", frames.load(), "frames");
    metric("grid_content_height_px", contentHeight, "px");
    return 0;
}

int main(int argc, char *argv[])
{
    // The fixture child: plain SQLite, no GUI.
    if (argc == 4 && QByteArrayView(argv[1]) == "--make-fixture")
        return generate(QString::fromLocal8Bit(argv[2]), QByteArray(argv[3]).toInt()) ? 0 : 1;
    try {
        return run(argc, argv);
    } catch (...) {
        return 1;
    }
}
