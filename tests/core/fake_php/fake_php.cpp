// A stand-in for `php`, for the MigrationRunner tests: it never touches a database and never runs
// PHP. It logs its arguments (one per line, appended) to $FAKE_PHP_LOG and behaves as
// $FAKE_PHP_MODE says: ok (default), fail, target, nocommand, slow, big, cwd.
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QThread>

#include <chrono>
#include <cstdio>

namespace {

void out(const QByteArray &text)
{
    std::fwrite(text.constData(), 1, static_cast<size_t>(text.size()), stdout);
    std::fflush(stdout);
}

void err(const QByteArray &text)
{
    std::fwrite(text.constData(), 1, static_cast<size_t>(text.size()), stderr);
    std::fflush(stderr);
}

} // namespace

int main(int argc, char *argv[])
{
    QStringList args;
    for (int i = 1; i < argc; ++i)
        args << QString::fromLocal8Bit(argv[i]);

    const QString logPath = qEnvironmentVariable("FAKE_PHP_LOG");
    if (!logPath.isEmpty()) {
        QFile log(logPath);
        if (log.open(QIODevice::WriteOnly | QIODevice::Append)) {
            for (const QString &arg : std::as_const(args))
                log.write(arg.toUtf8() + '\n');
        }
    }

    const QString mode = qEnvironmentVariable("FAKE_PHP_MODE", QStringLiteral("ok"));
    const QString command = args.value(1);

    if (mode == QLatin1String("fail")) {
        err("Migration failed: something went wrong\n");
        return 3;
    }
    if (mode == QLatin1String("nocommand") && command == QLatin1String("db:show")) {
        err("Command \"db:show\" is not defined.\n");
        return 1;
    }
    if (mode == QLatin1String("target") && command == QLatin1String("db:show")) {
        out("PHP Warning:  Module \"x\" is already loaded in Unknown on line 0\n");
        out(qgetenv("FAKE_PHP_TARGET_JSON") + '\n');
        return 0;
    }
    if (mode == QLatin1String("slow")) {
        // $FAKE_PHP_TICKS grows by a byte per second alive: the tests see whether it was killed.
        QFile ticks(qEnvironmentVariable("FAKE_PHP_TICKS"));
        for (int i = 0; i < 30; ++i) {
            out("tick\n");
            if (!ticks.fileName().isEmpty()
                && ticks.open(QIODevice::WriteOnly | QIODevice::Append)) {
                ticks.write("x");
                ticks.close();
            }
            QThread::sleep(std::chrono::seconds(1));
        }
        return 0;
    }
    if (mode == QLatin1String("big")) {
        const QByteArray line(99, 'x');
        for (int i = 0; i < 16000; ++i) // about 1.5 MiB
            out(line + '\n');
        return 0;
    }
    if (mode == QLatin1String("cwd")) {
        out(QDir::toNativeSeparators(QDir::currentPath()).toUtf8() + '\n');
        return 0;
    }

    out("Migrating: 2024_01_01_000000_x\n");
    out("Migrated:  2024_01_01_000000_x (1.00ms)\n");
    err("a warning\n");
    return 0;
}
