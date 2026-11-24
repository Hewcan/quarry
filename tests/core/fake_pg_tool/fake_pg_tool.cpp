// A stand-in for pg_dump, psql and pg_restore, for the DumpRunner tests: it never touches a
// database. It appends its arguments (one per line) to $FAKE_PG_LOG, then "PGPASSFILE:" plus the
// content of that file and "PGPASSWORD:" plus that variable, and behaves as $FAKE_PG_MODE says:
// ok (default; writes "-- fake dump" to --file=), authfail, mismatch, slow.
#include <QByteArray>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QThread>

#include <chrono>
#include <cstdio>

namespace {

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

    if (const QString logPath = qEnvironmentVariable("FAKE_PG_LOG"); !logPath.isEmpty()) {
        QFile log(logPath);
        if (log.open(QIODevice::WriteOnly | QIODevice::Append)) {
            for (const QString &arg : std::as_const(args))
                log.write(arg.toUtf8() + '\n');
            QByteArray pass;
            QFile passFile(qEnvironmentVariable("PGPASSFILE"));
            if (passFile.open(QIODevice::ReadOnly))
                pass = passFile.readAll().trimmed();
            log.write("PGPASSFILE:" + pass + '\n');
            log.write("PGPASSFILEPATH:" + qgetenv("PGPASSFILE") + '\n');
            log.write("PGPASSWORD:" + qgetenv("PGPASSWORD") + '\n');
        }
    }

    QString output;
    for (const QString &arg : std::as_const(args)) {
        if (arg.startsWith(QLatin1String("--file=")))
            output = arg.mid(7);
    }

    const QString mode = qEnvironmentVariable("FAKE_PG_MODE", QStringLiteral("ok"));
    if (mode == QLatin1String("authfail")) {
        err("pg_dump: error: connection to server at \"localhost\" failed: FATAL:  password "
            "authentication failed for user \"app\"\n");
        return 1;
    }
    if (mode == QLatin1String("mismatch")) {
        err("pg_dump: error: aborting because of server version mismatch\n");
        return 1;
    }
    err("pg_dump: dumping contents of table \"public.items\"\n");
    if (!output.isEmpty() && !args.contains(QLatin1String("--no-psqlrc"))) {
        QFile file(output);
        if (file.open(QIODevice::WriteOnly))
            file.write("-- fake dump\n");
    }
    if (mode == QLatin1String("slow"))
        QThread::sleep(std::chrono::seconds(30));
    return 0;
}
