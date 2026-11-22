#include <quarry/ai/ai_prompt.h>
#include <quarry/ai/claude_cli.h>

#include <QtTest>

using namespace quarry::ai;
using namespace Qt::StringLiterals;

Q_DECLARE_METATYPE(quarry::ai::AiErrorKind)

class TstClaudeCli : public QObject
{
    Q_OBJECT

private slots:
    void argumentsAllowNoTool()
    {
        const QStringList args = claudeArguments();
        QVERIFY(args.contains(u"--print"_s));
        const qsizetype tools = args.indexOf(u"--tools"_s);
        QVERIFY(tools >= 0);
        QCOMPARE(args.value(tools + 1), QString()); // the empty list: no tool at all
        const qsizetype denied = args.indexOf(u"--disallowedTools"_s);
        QVERIFY(denied >= 0);
        for (const QString &tool : { u"Bash"_s, u"Read"_s, u"Write"_s, u"Edit"_s, u"WebFetch"_s,
                                     u"WebSearch"_s, u"Task"_s, u"Glob"_s, u"Grep"_s })
            QVERIFY2(args.value(denied + 1).split(u',').contains(tool), qPrintable(tool));
        QVERIFY(args.contains(u"--strict-mcp-config"_s));
        QVERIFY(!args.contains(u"--mcp-config"_s));
        QVERIFY(args.contains(u"--no-session-persistence"_s));
        QVERIFY(args.contains(u"--disable-slash-commands"_s));
        QCOMPARE(args.value(args.indexOf(u"--setting-sources"_s) + 1), QString());
        QVERIFY(args.value(args.indexOf(u"--settings"_s) + 1).contains(u"\"disableAllHooks\":true"));
        QCOMPARE(args.value(args.indexOf(u"--max-turns"_s) + 1), u"1"_s);
        for (const QString &arg : args) {
            QVERIFY(!arg.contains(u"allowedTools", Qt::CaseInsensitive) || arg == u"--disallowedTools");
            QVERIFY(!arg.contains(u"dangerously"));
            QVERIFY(!arg.contains(u"bypassPermissions"));
            QVERIFY(!arg.contains(u"--resume") && !arg.contains(u"--continue"));
            QVERIFY(!arg.contains(u"--add-dir"));
        }
    }

    void userTextNeverInArguments()
    {
        PromptInput input;
        input.request = u"USER_REQUEST_MARKER"_s;
        input.script = u"SCRIPT_MARKER"_s;
        input.context.ddl = u"SCHEMA_MARKER"_s;
        const QByteArray stdinData = buildPrompt(input);
        QVERIFY(stdinData.contains("USER_REQUEST_MARKER"));
        for (const QString &arg : claudeArguments())
            QVERIFY(!arg.contains(u"MARKER"));
    }

    void candidates()
    {
        const QStringList win = candidatePaths(HostOs::Windows, u"C:/Users/u"_s, u"C:/L"_s, u"C:/R"_s);
        QCOMPARE(win.first(), u"C:/Users/u/.local/bin/claude.exe"_s);
        QCOMPARE(win.last(), u"C:/R/npm/claude.cmd"_s); // the shim is the last resort
        QVERIFY(win.indexOf(u"C:/R/npm/claude.exe"_s) < win.indexOf(u"C:/R/npm/claude.cmd"_s));
        QVERIFY(candidatePaths(HostOs::MacOs, u"/Users/u"_s, {}, {}).contains(u"/opt/homebrew/bin/claude"_s));
        QCOMPARE(candidatePaths(HostOs::Linux, u"/home/u"_s, {}, {}).first(), u"/home/u/.local/bin/claude"_s);
        QCOMPARE(loginShellLookupArguments().last(), u"command -v claude"_s);
    }

    void versions()
    {
        QVERIFY(isCompatibleVersion(u"2.0.14 (Claude Code)"));
        QVERIFY(isCompatibleVersion(u"10.1.0"));
        QVERIFY(!isCompatibleVersion(u"1.0.128 (Claude Code)"));
        QVERIFY(!isCompatibleVersion(u"garbage"));
    }

    void outputs_data()
    {
        QTest::addColumn<QByteArray>("out");
        QTest::addColumn<QByteArray>("err");
        QTest::addColumn<int>("exitCode");
        QTest::addColumn<AiErrorKind>("kind");
        QTest::addColumn<QString>("text");
        QTest::newRow("success")
            << QByteArray(R"({"type":"result","subtype":"success","is_error":false,"result":"Hi"})")
            << QByteArray() << 0 << AiErrorKind::None << u"Hi"_s;
        QTest::newRow("noise before json")
            << QByteArray("warning: x\n{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"result\":\"Ok\"}\n")
            << QByteArray() << 0 << AiErrorKind::None << u"Ok"_s;
        QTest::newRow("not logged in")
            << QByteArray(R"({"type":"result","subtype":"success","is_error":true,"result":"Invalid API key · Please run /login"})")
            << QByteArray() << 1 << AiErrorKind::NotLoggedIn << QString();
        QTest::newRow("usage limit")
            << QByteArray(R"({"type":"result","subtype":"success","is_error":true,"result":"Claude AI usage limit reached|1700000000"})")
            << QByteArray() << 1 << AiErrorKind::UsageLimit << QString();
        QTest::newRow("old cli") << QByteArray() << QByteArray("error: unknown option '--tools'")
                                 << 1 << AiErrorKind::IncompatibleVersion << QString();
        QTest::newRow("corrupt") << QByteArray("{\"type\":\"res") << QByteArray() << 0
                                 << AiErrorKind::InvalidOutput << QString();
        QTest::newRow("empty result")
            << QByteArray(R"({"type":"result","subtype":"success","is_error":false,"result":""})")
            << QByteArray() << 0 << AiErrorKind::InvalidOutput << QString();
        QTest::newRow("max turns")
            << QByteArray(R"({"type":"result","subtype":"error_max_turns","is_error":false})")
            << QByteArray() << 1 << AiErrorKind::Failed << QString();
        QTest::newRow("crash") << QByteArray() << QByteArray("Segmentation fault") << 139
                               << AiErrorKind::Failed << QString();
    }

    void outputs()
    {
        QFETCH(QByteArray, out);
        QFETCH(QByteArray, err);
        QFETCH(int, exitCode);
        QFETCH(AiErrorKind, kind);
        QFETCH(QString, text);
        const CliOutcome outcome = parseClaudeOutput(out, err, exitCode);
        QCOMPARE(outcome.error, kind);
        QCOMPARE(outcome.text, text);
        if (kind != AiErrorKind::None)
            QVERIFY(!outcome.detail.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TstClaudeCli)
#include "tst_claude_cli.moc"
