#include <quarry/ai/claude_runner.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <vector>

using namespace quarry::ai;
using namespace Qt::StringLiterals;

namespace {

// Sets environment variables for the life of the object and puts the old values back.
class EnvGuard
{
public:
    EnvGuard() = default;
    EnvGuard(const EnvGuard &) = delete;
    EnvGuard &operator=(const EnvGuard &) = delete;
    ~EnvGuard()
    {
        for (const Saved &saved : m_saved) {
            if (saved.wasSet)
                qputenv(saved.name.constData(), saved.value);
            else
                qunsetenv(saved.name.constData());
        }
    }

    void set(const QByteArray &name, const QByteArray &value)
    {
        m_saved.push_back(
            { name, qgetenv(name.constData()), qEnvironmentVariableIsSet(name.constData()) });
        qputenv(name.constData(), value);
    }

private:
    struct Saved
    {
        QByteArray name;
        QByteArray value;
        bool wasSet;
    };
    std::vector<Saved> m_saved;
};

// A home folder, with the variables that name it, that has no `claude` and no usable shell.
struct FakeHome
{
    FakeHome()
    {
        const QByteArray home = dir.path().toUtf8();
        env.set("HOME", home);
        env.set("USERPROFILE", home);
        env.set("LOCALAPPDATA", home + "/local");
        env.set("APPDATA", home + "/roaming");
        env.set("SHELL", {});
    }

    // Where candidatePaths() looks first on this system.
    QString candidate() const
    {
#ifdef Q_OS_WIN
        return dir.path() + u"/.local/bin/claude.exe"_s;
#else
        return dir.path() + u"/.local/bin/claude"_s;
#endif
    }

    QTemporaryDir dir;
    EnvGuard env;
};

bool makeExecutable(const QString &path, const QByteArray &content = "x")
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size())
        return false;
    file.close();
    return file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                               | QFileDevice::ExeOwner);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// The inverse of the escaping of fake_claude: \\ \n \r.
QString unescape(const QByteArray &line)
{
    const QString text = QString::fromUtf8(line);
    QString out;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text.at(i) == u'\\' && i + 1 < text.size()) {
            const QChar next = text.at(++i);
            out += next == u'n' ? QChar(u'\n') : next == u'r' ? QChar(u'\r') : next;
        } else {
            out += text.at(i);
        }
    }
    return out;
}

QStringList decodeArgs(const QByteArray &file)
{
    QList<QByteArray> lines = file.split('\n');
    if (!lines.isEmpty())
        lines.removeLast(); // every argument ends in a line feed
    QStringList out;
    out.reserve(lines.size());
    for (const QByteArray &line : std::as_const(lines))
        out.append(unescape(line));
    return out;
}

CliOutcome outcomeOf(const QSignalSpy &spy, int index = 0)
{
    return spy.at(index).at(1).value<CliOutcome>();
}

CliInfo infoOf(const QSignalSpy &spy)
{
    return spy.at(0).at(1).value<CliInfo>();
}

} // namespace

class TstClaudeRunner : public QObject
{
    Q_OBJECT

public:
    TstClaudeRunner()
        : m_fake(QDir::cleanPath(QStringLiteral(QUARRY_FAKE_CLAUDE)))
    { }

private:
    QString file(const QString &name) const { return m_dir.filePath(name); }
    qint64 ticks() const { return QFileInfo(file(u"ticks"_s)).size(); }

    // One ask that ends by itself; the outcome and the id it returned.
    CliOutcome askOnce(ClaudeRunner &runner, const QByteArray &stdinText = "hola",
                       quint64 *id = nullptr)
    {
        QSignalSpy finished(&runner, &ClaudeRunner::finished);
        const quint64 got = runner.ask(m_fake, stdinText);
        if (id != nullptr)
            *id = got;
        [&] { QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 20000); }();
        if (finished.count() != 1)
            return {};
        return outcomeOf(finished);
    }

    void setMode(const char *mode) { qputenv("FAKE_CLAUDE_MODE", mode); }

    QString m_fake;
    QTemporaryDir m_dir;

private slots:
    void init()
    {
        for (const char *name : { "FAKE_CLAUDE_MODE", "QUARRY_AI_TIMEOUT_MS" })
            qunsetenv(name);
        for (const QString &name : { u"args"_s, u"cwd"_s, u"stdin"_s, u"ticks"_s })
            QFile::remove(file(name));
        qputenv("FAKE_CLAUDE_ARGS", file(u"args"_s).toUtf8());
        qputenv("FAKE_CLAUDE_CWD", file(u"cwd"_s).toUtf8());
        qputenv("FAKE_CLAUDE_STDIN", file(u"stdin"_s).toUtf8());
        qputenv("FAKE_CLAUDE_TICKS", file(u"ticks"_s).toUtf8());
    }

    void cleanup()
    {
        for (const char *name : { "FAKE_CLAUDE_MODE", "QUARRY_AI_TIMEOUT_MS", "FAKE_CLAUDE_ARGS",
                                  "FAKE_CLAUDE_CWD", "FAKE_CLAUDE_STDIN", "FAKE_CLAUDE_TICKS" })
            qunsetenv(name);
    }

    // ---- locate ----

    void locateOwnPath()
    {
        FakeHome home;
        ClaudeRunner runner;
        QSignalSpy located(&runner, &ClaudeRunner::located);
        QSignalSpy notFound(&runner, &ClaudeRunner::locateFailed);
        const quint64 id = runner.locate(m_fake);
        QVERIFY(id > 0);
        QCOMPARE(located.count(), 0); // never inside the call
        QTRY_COMPARE(located.count(), 1);
        QCOMPARE(located.at(0).at(0).toULongLong(), id);
        QCOMPARE(located.at(0).at(1).toString(), m_fake);
        QCOMPARE(notFound.count(), 0);
    }

    void locateInvalidOwnPathFindsNothing()
    {
        FakeHome home;
        ClaudeRunner runner;
        QSignalSpy located(&runner, &ClaudeRunner::located);
        QSignalSpy notFound(&runner, &ClaudeRunner::locateFailed);
        runner.locate(home.dir.path() + u"/missing/claude"_s);
        QCOMPARE(notFound.count(), 0);
        QTRY_COMPARE(notFound.count(), 1);
        QVERIFY(!notFound.at(0).at(1).toString().isEmpty());
        QCOMPARE(located.count(), 0);
    }

    void locateOwnPathThatIsNotExecutableIsIgnored()
    {
#ifdef Q_OS_WIN
        QSKIP("Windows decides what is executable by the extension.");
#else
        FakeHome home;
        const QString plain = home.dir.path() + u"/plain"_s;
        QVERIFY(makeExecutable(plain));
        QVERIFY(QFile::setPermissions(plain, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        ClaudeRunner runner;
        QSignalSpy notFound(&runner, &ClaudeRunner::locateFailed);
        runner.locate(plain);
        QTRY_COMPARE(notFound.count(), 1);
#endif
    }

    void locateFindsACandidateUnderHome()
    {
        FakeHome home;
        QVERIFY(makeExecutable(home.candidate()));
        ClaudeRunner runner;
        QSignalSpy located(&runner, &ClaudeRunner::located);
        runner.locate({});
        QTRY_COMPARE(located.count(), 1);
        QCOMPARE(located.at(0).at(1).toString(), QDir::cleanPath(home.candidate()));
    }

    void locateOwnPathWinsOverCandidates()
    {
        FakeHome home;
        QVERIFY(makeExecutable(home.candidate()));
        ClaudeRunner runner;
        QSignalSpy located(&runner, &ClaudeRunner::located);
        runner.locate(m_fake);
        QTRY_COMPARE(located.count(), 1);
        QCOMPARE(located.at(0).at(1).toString(), m_fake);
    }

    void locateCachesUntilForget()
    {
        FakeHome home;
        QVERIFY(makeExecutable(home.candidate()));
        ClaudeRunner runner;
        QSignalSpy located(&runner, &ClaudeRunner::located);
        QSignalSpy notFound(&runner, &ClaudeRunner::locateFailed);
        runner.locate({});
        QTRY_COMPARE(located.count(), 1);

        QVERIFY(QFile::remove(home.candidate()));
        runner.locate({}); // the answer is remembered
        QTRY_COMPARE(located.count(), 2);
        QCOMPARE(located.at(1).at(1).toString(), located.at(0).at(1).toString());
        QCOMPARE(notFound.count(), 0);

        runner.locate({}, true); // forget: looks again and finds nothing now
        QCOMPARE(notFound.count(), 0);
        QTRY_COMPARE(notFound.count(), 1);
        QCOMPARE(located.count(), 2);
    }

    void locateAsksTheLoginShellWhenNothingElseExists()
    {
#ifdef Q_OS_WIN
        QSKIP("The login-shell lookup is Unix only.");
#else
        FakeHome home;
        const QString target = home.dir.path() + u"/elsewhere/claude"_s;
        QVERIFY(makeExecutable(target));
        // A shell that ignores -l -c and prints where `command -v claude` would answer.
        const QString shell = home.dir.path() + u"/fakeshell"_s;
        QVERIFY(makeExecutable(shell, "#!/bin/sh\necho " + target.toUtf8() + "\n"));
        home.env.set("SHELL", shell.toUtf8());

        ClaudeRunner runner;
        QSignalSpy located(&runner, &ClaudeRunner::located);
        runner.locate({});
        QTRY_COMPARE_WITH_TIMEOUT(located.count(), 1, 10000);
        QCOMPARE(located.at(0).at(1).toString(), QDir::cleanPath(target));
#endif
    }

    // ---- check ----

    void checkReadsTheVersion()
    {
        ClaudeRunner runner;
        QSignalSpy checked(&runner, &ClaudeRunner::checked);
        const quint64 id = runner.check(m_fake);
        QVERIFY(id > 0);
        QTRY_COMPARE_WITH_TIMEOUT(checked.count(), 1, 20000);
        QCOMPARE(checked.at(0).at(0).toULongLong(), id);
        const CliInfo info = infoOf(checked);
        QCOMPARE(info.path, m_fake);
        QCOMPARE(info.version, u"2.0.14 (Claude Code)"_s);
        QVERIFY(info.compatible);
        QCOMPARE(info.error, AiErrorKind::None);
        QCOMPARE(decodeArgs(readFile(file(u"args"_s))), QStringList { u"--version"_s });
        QVERIFY(!runner.isRunning(id));
    }

    void checkOldVersionIsIncompatible()
    {
        setMode("old");
        ClaudeRunner runner;
        QSignalSpy checked(&runner, &ClaudeRunner::checked);
        runner.check(m_fake);
        QTRY_COMPARE_WITH_TIMEOUT(checked.count(), 1, 20000);
        const CliInfo info = infoOf(checked);
        QCOMPARE(info.version, u"1.0.3 (Claude Code)"_s);
        QVERIFY(!info.compatible);
        QCOMPARE(info.error, AiErrorKind::IncompatibleVersion);
    }

    void checkFileThatCannotStart()
    {
        FakeHome home;
        ClaudeRunner runner;
        QSignalSpy checked(&runner, &ClaudeRunner::checked);
        runner.check(home.dir.path() + u"/missing/claude"_s);
        QTRY_COMPARE_WITH_TIMEOUT(checked.count(), 1, 20000);
        const CliInfo info = infoOf(checked);
        QCOMPARE(info.error, AiErrorKind::CliNotFound);
        QVERIFY(!info.compatible);
        QVERIFY(info.version.isEmpty());
    }

    void checkGarbageIsAFailure()
    {
        setMode("corrupt");
        ClaudeRunner runner;
        QSignalSpy checked(&runner, &ClaudeRunner::checked);
        runner.check(m_fake);
        QTRY_COMPARE_WITH_TIMEOUT(checked.count(), 1, 20000);
        const CliInfo info = infoOf(checked);
        QCOMPARE(info.error, AiErrorKind::Failed);
        QVERIFY(!info.compatible);
        QVERIFY(info.version.isEmpty());
        QVERIFY(!info.detail.isEmpty());
        QVERIFY(info.detail.size() <= 500);
    }

    void checkFailingExitIsAFailure()
    {
        setMode("exit3");
        ClaudeRunner runner;
        QSignalSpy checked(&runner, &ClaudeRunner::checked);
        runner.check(m_fake);
        QTRY_COMPARE_WITH_TIMEOUT(checked.count(), 1, 20000);
        const CliInfo info = infoOf(checked);
        QCOMPARE(info.error, AiErrorKind::Failed);
        QVERIFY(info.detail.contains(u"went wrong"_s));
    }

    // ---- ask ----

    void askUsesExactlyTheFixedArgumentsAndNoUserText()
    {
        const QByteArray text = u"MARCADOR_USUARIO ñ"_s.toUtf8();
        ClaudeRunner runner;
        QSignalSpy started(&runner, &ClaudeRunner::started);
        QSignalSpy finished(&runner, &ClaudeRunner::finished);
        const quint64 id = runner.ask(m_fake, text);
        QVERIFY(id > 0);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 20000);
        QCOMPARE(started.count(), 1);
        QCOMPARE(started.at(0).at(0).toULongLong(), id);
        QCOMPARE(finished.at(0).at(0).toULongLong(), id);

        const QStringList args = decodeArgs(readFile(file(u"args"_s)));
        QCOMPARE(args, claudeArguments());
        for (const QString &arg : args)
            QVERIFY(!arg.contains(u"MARCADOR_USUARIO"_s));
        QVERIFY(!runner.isRunning(id));
    }

    void askSendsStdinIntactAndReadsTheReply()
    {
        const QByteArray text
            = u"Línea 1: ¿qué tablas hay?\nLínea ñ é € 日本\r\n\r\nfin sin salto"_s.toUtf8();
        ClaudeRunner runner;
        const CliOutcome outcome = askOnce(runner, text);
        QCOMPARE(readFile(file(u"stdin"_s)), text);
        QCOMPARE(outcome.error, AiErrorKind::None);
        QCOMPARE(outcome.text, u"Hecho: SELECT 1;\nSecond line"_s);
    }

    void askSendsLargeStdinIntact()
    {
        QByteArray text;
        constexpr qsizetype kTarget = qsizetype(1500) * 1024;
        for (int i = 0; text.size() < kTarget; ++i)
            text += "línea número " + QByteArray::number(i) + " ñ\n";
        QVERIFY(text.size() <= ClaudeRunner::kMaxStdinBytes);
        ClaudeRunner runner;
        const CliOutcome outcome = askOnce(runner, text);
        QCOMPARE(outcome.error, AiErrorKind::None);
        QVERIFY(readFile(file(u"stdin"_s)) == text);
    }

    void askRunsInAnEmptyTemporaryFolderThatIsRemoved()
    {
        ClaudeRunner runner;
        askOnce(runner);
        const QList<QByteArray> first = readFile(file(u"cwd"_s)).split('\n');
        QCOMPARE(first.size(), 3); // path, entries, and the empty piece after the last line feed
        const QString dir1 = QString::fromUtf8(first.at(0));
        QCOMPARE(first.at(1), QByteArray("0")); // empty
        QVERIFY(!dir1.isEmpty());
        QVERIFY(dir1 != QDir::currentPath());
        QVERIFY(QFileInfo(dir1).fileName().startsWith(u"quarry-ai-"_s));
        QVERIFY(!QDir(dir1).exists()); // removed once it finished

        askOnce(runner);
        const QString dir2 = QString::fromUtf8(readFile(file(u"cwd"_s)).split('\n').at(0));
        QVERIFY(!dir2.isEmpty());
        QVERIFY(dir1 != dir2);
        QVERIFY(!QDir(dir2).exists());
    }

    void askNotLoggedIn()
    {
        setMode("notloggedin");
        ClaudeRunner runner;
        const CliOutcome outcome = askOnce(runner);
        QCOMPARE(outcome.error, AiErrorKind::NotLoggedIn);
        QVERIFY(!outcome.detail.isEmpty());
    }

    void askUsageLimit()
    {
        setMode("usagelimit");
        ClaudeRunner runner;
        QCOMPARE(askOnce(runner).error, AiErrorKind::UsageLimit);
    }

    void askOldCli()
    {
        setMode("old");
        ClaudeRunner runner;
        QCOMPARE(askOnce(runner).error, AiErrorKind::IncompatibleVersion);
    }

    void askCorruptOutput()
    {
        setMode("corrupt");
        ClaudeRunner runner;
        QCOMPARE(askOnce(runner).error, AiErrorKind::InvalidOutput);
    }

    void askFailingExit()
    {
        setMode("exit3");
        ClaudeRunner runner;
        const CliOutcome outcome = askOnce(runner);
        QCOMPARE(outcome.error, AiErrorKind::Failed);
        QVERIFY(outcome.detail.contains(u"went wrong"_s));
    }

    void askTooMuchOutputIsInvalidAndNotDumped()
    {
        setMode("big");
        ClaudeRunner runner;
        const CliOutcome outcome = askOnce(runner);
        QCOMPARE(outcome.error, AiErrorKind::InvalidOutput);
        QVERIFY(outcome.text.isEmpty());
        QVERIFY(!outcome.detail.contains(u"xxxxxxxx"_s));
        QVERIFY(outcome.detail.size() < 1000);
    }

    void askTimeoutKillsTheProcess()
    {
        setMode("slow");
        qputenv("QUARRY_AI_TIMEOUT_MS", "1500");
        ClaudeRunner runner;
        QSignalSpy finished(&runner, &ClaudeRunner::finished);
        const quint64 id = runner.ask(m_fake, "hola");
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 20000);
        QCOMPARE(outcomeOf(finished).error, AiErrorKind::Timeout);
        QVERIFY(!runner.isRunning(id));
        const qint64 atTimeout = ticks();
        QVERIFY(atTimeout >= 1); // it did start
        QTest::qWait(1500);
        QCOMPARE(ticks(), atTimeout); // and it is dead: no more ticks
        QCOMPARE(finished.count(), 1);
    }

    void askCancelKillsTheProcessAndSignalsOnce()
    {
        setMode("slow");
        ClaudeRunner runner;
        QSignalSpy started(&runner, &ClaudeRunner::started);
        QSignalSpy finished(&runner, &ClaudeRunner::finished);
        const quint64 id = runner.ask(m_fake, "hola");
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 20000);
        QTRY_VERIFY_WITH_TIMEOUT(ticks() >= 1, 20000);
        QVERIFY(runner.isRunning(id));

        runner.cancel(id);
        QVERIFY(!runner.isRunning(id));
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(0).toULongLong(), id);
        QCOMPARE(outcomeOf(finished).error, AiErrorKind::Cancelled);

        const qint64 atCancel = ticks();
        QTest::qWait(1500);
        QCOMPARE(ticks(), atCancel);
        QCOMPARE(finished.count(), 1); // no second signal
        runner.cancel(id); // unknown now: nothing happens
        runner.cancel(9999);
        QTest::qWait(50);
        QCOMPARE(finished.count(), 1);
    }

    void destroyingTheRunnerKillsWhatIsLeft()
    {
        setMode("slow");
        auto runner = std::make_unique<ClaudeRunner>();
        QSignalSpy started(runner.get(), &ClaudeRunner::started);
        runner->ask(m_fake, "hola");
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 20000);
        QTRY_VERIFY_WITH_TIMEOUT(ticks() >= 1, 20000);
        runner.reset();
        const qint64 atDelete = ticks();
        QTest::qWait(1500);
        QCOMPARE(ticks(), atDelete);
    }

    void askRefusesAnEmptyPath()
    {
        ClaudeRunner runner;
        QSignalSpy finished(&runner, &ClaudeRunner::finished);
        QCOMPARE(runner.ask(QString(), "hola"), quint64(0));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(0).toULongLong(), quint64(0));
        QCOMPARE(outcomeOf(finished).error, AiErrorKind::Failed);
        QVERIFY(!QFileInfo::exists(file(u"args"_s))); // nothing was started
    }

    void askRefusesTooMuchStdin()
    {
        ClaudeRunner runner;
        QSignalSpy finished(&runner, &ClaudeRunner::finished);
        const QByteArray big(ClaudeRunner::kMaxStdinBytes + 1, 'a');
        QCOMPARE(runner.ask(m_fake, big), quint64(0));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(outcomeOf(finished).error, AiErrorKind::Failed);
        QVERIFY(!QFileInfo::exists(file(u"args"_s)));
    }

    void askFileThatCannotStart()
    {
        FakeHome home;
        ClaudeRunner runner;
        QSignalSpy started(&runner, &ClaudeRunner::started);
        QSignalSpy finished(&runner, &ClaudeRunner::finished);
        const quint64 id = runner.ask(home.dir.path() + u"/missing/claude"_s, "hola");
        QVERIFY(id > 0);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 20000);
        QCOMPARE(outcomeOf(finished).error, AiErrorKind::CliNotFound);
        QCOMPARE(started.count(), 0);
        QVERIFY(!runner.isRunning(id));
    }

    void twoAsksGetDifferentIdsAndEachOneEnds()
    {
        ClaudeRunner runner;
        QSignalSpy finished(&runner, &ClaudeRunner::finished);
        const quint64 a = runner.ask(m_fake, "uno");
        const quint64 b = runner.ask(m_fake, "dos");
        QVERIFY(a > 0 && b > 0);
        QVERIFY(a != b);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 30000);
        QSet<quint64> ids;
        for (int i = 0; i < 2; ++i) {
            ids.insert(finished.at(i).at(0).toULongLong());
            QCOMPARE(outcomeOf(finished, i).error, AiErrorKind::None);
        }
        QCOMPARE(ids, QSet<quint64>({ a, b }));
        QVERIFY(!runner.isRunning(a) && !runner.isRunning(b));

        quint64 c = 0;
        askOnce(runner, "tres", &c);
        QVERIFY(c != a && c != b);
    }
};

QTEST_GUILESS_MAIN(TstClaudeRunner)
#include "tst_claude_runner.moc"
