// Export benchmark: streams N rows (default 10 000 000) of a generated SQLite result into a CSV
// file through Exporter::start and reports speed and process memory. Budget (ROADMAP H5): memory
// grows by no more than 50 MB between 10 % and the end of the export, whatever N is.
#include <quarry/exporter.h>
#include <quarry/provider_registry.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef Q_OS_WIN
// clang-format off
#  define NOMINMAX
#  include <windows.h>
#  include <psapi.h>
// clang-format on
#elif defined(Q_OS_MACOS)
#include <sys/resource.h>
#endif

namespace {

constexpr double kMb = 1024.0 * 1024.0;

#if defined(Q_OS_WIN)
double currentMb()
{
    PROCESS_MEMORY_COUNTERS counters {};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return -1;
    return static_cast<double>(counters.WorkingSetSize) / kMb;
}
double peakMb()
{
    PROCESS_MEMORY_COUNTERS counters {};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return -1;
    return static_cast<double>(counters.PeakWorkingSetSize) / kMb;
}
#elif defined(Q_OS_MACOS)
// shortcut: macOS has only the peak (ru_maxrss) here, so the "current" value is the peak so far;
// the 10 %..end delta then only shows growth of the peak. Use task_info for an exact reading.
double peakMb()
{
    rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<double>(usage.ru_maxrss) / kMb; // bytes on macOS
}
double currentMb()
{
    return peakMb();
}
#else
double statusKb(const char *key)
{
    FILE *file = std::fopen("/proc/self/status", "r");
    if (!file)
        return -1;
    char line[256];
    double kb = -1;
    const size_t length = std::strlen(key);
    while (std::fgets(line, sizeof(line), file)) {
        if (std::strncmp(line, key, length) == 0) {
            kb = std::atof(line + length);
            break;
        }
    }
    std::fclose(file);
    return kb;
}
double currentMb()
{
    return statusKb("VmRSS:") / 1024.0;
}
double peakMb()
{
    return statusKb("VmHWM:") / 1024.0;
}
#endif

} // namespace

int run(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const qint64 rows
        = argc > 1 ? std::max<qint64>(1, QString::fromLocal8Bit(argv[1]).toLongLong()) : 10'000'000;

    QTemporaryDir dir;
    if (!dir.isValid())
        return 2;
    quarry::ProviderRegistry registry;
    registry.scan(QStringLiteral(QUARRY_PLUGIN_DIR));
    quarry::ConnectionProfile profile;
    profile.id = QUuid::createUuid();
    profile.name = QStringLiteral("bench");
    profile.providerId = QStringLiteral("sqlite");
    profile.values = { { QStringLiteral("file"), QStringLiteral(":memory:") } };
    auto connection = registry.createConnection(profile, {});
    if (!connection) {
        std::fprintf(stderr, "cannot create the sqlite connection: %s\n",
                     qPrintable(connection.error().message));
        return 2;
    }
    auto session = std::make_unique<quarry::Session>(std::move(connection).value());
    {
        QEventLoop wait;
        QObject::connect(session.get(), &quarry::Session::opened, &wait, &QEventLoop::quit);
        QObject::connect(session.get(), &quarry::Session::failed, &wait, &QEventLoop::quit);
        QTimer::singleShot(30000, &wait, &QEventLoop::quit);
        session->open();
        wait.exec();
    }

    quarry::ExportOptions options;
    options.format = quarry::ExportFormat::Csv;
    options.filePath = dir.filePath(QStringLiteral("bench.csv"));
    quarry::Exporter exporter(options);

    const double startMb = currentMb();
    double at10Mb = -1;
    double peakAfter10Mb = -1;
    double endMb = -1;
    const qint64 tenPercent = std::max<qint64>(1, rows / 10);
    bool ok = false;
    qint64 exported = 0;
    QEventLoop loop;
    QObject::connect(&exporter, &quarry::Exporter::progress, &loop, [&](qint64 done) {
        if (at10Mb < 0 && done >= tenPercent)
            at10Mb = currentMb();
        if (at10Mb >= 0)
            peakAfter10Mb = std::max(peakAfter10Mb, currentMb());
    });
    QObject::connect(&exporter, &quarry::Exporter::finished, &loop,
                     [&](const quarry::ExportStats &stats) {
                         ok = true;
                         exported = stats.rows;
                         endMb = currentMb();
                         loop.quit();
                     });
    QObject::connect(&exporter, &quarry::Exporter::failed, &loop, [&](const quarry::Error &error) {
        std::fprintf(stderr, "export failed: %s (%s)\n", qPrintable(error.message),
                     qPrintable(error.detail));
        loop.quit();
    });
    QObject::connect(&exporter, &quarry::Exporter::cancelled, &loop, &QEventLoop::quit);

    const QString sql
        = QStringLiteral("WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < "
                         "%1) SELECT x AS id, x * 7 AS n, x * 0.37 AS price, 'name-' || x AS name, "
                         "'some longer text value ' || (x % 1000) AS note, "
                         "CASE WHEN x % 10 = 0 THEN NULL ELSE 'k' || (x % 97) END AS tag FROM c")
              .arg(rows);
    QElapsedTimer clock;
    clock.start();
    exporter.start(std::move(session), quarry::Statement { sql, {}, 5000 });
    loop.exec();
    const double seconds = static_cast<double>(clock.elapsed()) / 1000.0;
    if (!ok)
        return 1;
    if (peakAfter10Mb < 0)
        peakAfter10Mb = endMb;
    const qint64 fileBytes = QFileInfo(options.filePath).size();

    QTextStream out(stdout);
    out.setRealNumberNotation(QTextStream::FixedNotation);
    out.setRealNumberPrecision(2);
    out << "{\"metric\":\"export_csv\",\"rows\":" << exported << ",\"seconds\":" << seconds
        << ",\"rows_per_s\":" << static_cast<qint64>(static_cast<double>(exported) / seconds)
        << ",\"file_mb\":" << static_cast<double>(fileBytes) / kMb
        << ",\"start_mem_mb\":" << startMb << ",\"mem_at_10pct_mb\":" << at10Mb
        << ",\"mem_at_end_mb\":" << endMb << ",\"delta_10pct_to_end_mb\":" << endMb - at10Mb
        << ",\"delta_10pct_to_peak_mb\":" << peakAfter10Mb - at10Mb
        << ",\"peak_mem_mb\":" << peakMb() << "}\n";
    return 0;
}

int main(int argc, char **argv)
{
    try {
        return run(argc, argv);
    } catch (...) {
        std::fprintf(stderr, "bench_export: unexpected exception\n");
        return 2;
    }
}
