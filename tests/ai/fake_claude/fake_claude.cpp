// A stand-in for the `claude` CLI, for the ClaudeRunner tests: it never talks to any service and
// never starts a model. The real `claude` is never run by any test.
//
// It records, when the variable is set: $FAKE_CLAUDE_ARGS the arguments, one per line (a backslash,
// a line feed and a carriage return inside one are written as \\, \n and \r); $FAKE_CLAUDE_CWD the
// working directory on the first line and the number of entries it holds on the second;
// $FAKE_CLAUDE_STDIN the bytes received on stdin, untouched. $FAKE_CLAUDE_MODE says what it does:
//   ok (default)  --version prints "2.0.14 (Claude Code)"; a normal run prints a JSON result whose
//                 text is $FAKE_CLAUDE_RESULT when that is set (a reply with a
//                 <quarry-script>...</quarry-script> block, for the chat tests), else a fixed one
//   old           --version prints "1.0.3 (Claude Code)"; a normal run fails on an unknown option
//   notloggedin / usagelimit   a normal run prints the error result of that kind and exits 1
//   corrupt       prints garbage (also for --version)
//   big           a normal run prints more than 8 MiB
//   slow          a normal run lives 30 s, adding a byte per second to $FAKE_CLAUDE_TICKS
//   exit3         prints on stderr and exits 3 (also for --version)
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QThread>

#include <chrono>
#include <cstdio>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

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

void record(const QString &variable, const QByteArray &bytes)
{
    const QString path = qEnvironmentVariable(qPrintable(variable));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(bytes);
}

QByteArray escaped(const QString &arg)
{
    QString text = arg;
    text.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    text.replace(QLatin1Char('\n'), QLatin1String("\\n"));
    text.replace(QLatin1Char('\r'), QLatin1String("\\r"));
    return text.toUtf8();
}

QByteArray readStdin()
{
    QByteArray all;
    char buffer[65536];
    size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof buffer, stdin)) > 0)
        all.append(buffer, static_cast<qsizetype>(n));
    return all;
}

} // namespace

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    // arguments() reads the command line as UTF-16 on Windows; argv would be in the ANSI code page.
    const QCoreApplication app(argc, argv);
    const QStringList args = QCoreApplication::arguments().mid(1);

    QByteArray argsText;
    for (const QString &arg : args)
        argsText += escaped(arg) + '\n';
    record(QStringLiteral("FAKE_CLAUDE_ARGS"), argsText);

    const QDir cwd = QDir::current();
    record(QStringLiteral("FAKE_CLAUDE_CWD"),
           cwd.absolutePath().toUtf8() + '\n'
               + QByteArray::number(cwd.entryList(QDir::AllEntries | QDir::NoDotAndDotDot
                                                  | QDir::Hidden | QDir::System)
                                        .size())
               + '\n');

    const QString mode = qEnvironmentVariable("FAKE_CLAUDE_MODE", QStringLiteral("ok"));
    const bool version = args.size() == 1 && args.first() == QLatin1String("--version");

    if (mode == QLatin1String("exit3")) {
        err("claude: something went wrong\n");
        return 3;
    }
    if (mode == QLatin1String("corrupt")) {
        if (!version)
            record(QStringLiteral("FAKE_CLAUDE_STDIN"), readStdin());
        out("\x01garbage {not json\n");
        return 0;
    }
    if (version) {
        out(mode == QLatin1String("old") ? "1.0.3 (Claude Code)\n" : "2.0.14 (Claude Code)\n");
        return 0;
    }

    record(QStringLiteral("FAKE_CLAUDE_STDIN"), readStdin());

    if (mode == QLatin1String("old")) {
        err("error: unknown option '--tools'\n");
        return 1;
    }
    if (mode == QLatin1String("notloggedin")) {
        out("{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":true,"
            "\"result\":\"Invalid API key \xC2\xB7 Please run /login\"}\n");
        return 1;
    }
    if (mode == QLatin1String("usagelimit")) {
        out("{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":true,"
            "\"result\":\"Claude AI usage limit reached|1700000000\"}\n");
        return 1;
    }
    if (mode == QLatin1String("big")) {
        // More than the runner's 8 MiB limit: 9 MiB in 1 KiB lines.
        const QByteArray line(1023, 'x');
        for (int i = 0; i < 9 * 1024; ++i)
            out(line + '\n');
        return 0;
    }
    if (mode == QLatin1String("slow")) {
        // $FAKE_CLAUDE_TICKS grows by a byte per second alive: the tests see whether it was killed.
        QFile ticks(qEnvironmentVariable("FAKE_CLAUDE_TICKS"));
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

    // ok with a text of the test: the same result object, the text escaped by Qt's JSON writer.
    if (qEnvironmentVariableIsSet("FAKE_CLAUDE_RESULT")) {
        const QJsonObject result { { QStringLiteral("type"), QStringLiteral("result") },
                                   { QStringLiteral("subtype"), QStringLiteral("success") },
                                   { QStringLiteral("is_error"), false },
                                   { QStringLiteral("result"),
                                     qEnvironmentVariable("FAKE_CLAUDE_RESULT") } };
        out(QJsonDocument(result).toJson(QJsonDocument::Compact) + '\n');
        return 0;
    }
    // ok: the reply has an accent and a line break, to check that text survives the round trip.
    out("{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,"
        "\"result\":\"Hecho: SELECT 1;\\nSecond line\"}\n");
    return 0;
}
