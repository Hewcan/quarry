#include <quarry/ai/ai_prompt.h>
#include <quarry/ai/claude_cli.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace quarry::ai {

using namespace Qt::StringLiterals;

// shortcut: flags checked against the Claude Code docs, not against `claude --help` on a real
// install (the agent could not run it); verify before enabling the feature (ROADMAP H13).
QStringList claudeArguments()
{
    return {
        u"--print"_s,
        u"--output-format"_s, u"json"_s,
        u"--input-format"_s, u"text"_s,
        u"--no-session-persistence"_s,
        u"--max-turns"_s, u"1"_s,
        u"--tools"_s, QString(),
        u"--disallowedTools"_s, deniedTools().join(u','),
        u"--strict-mcp-config"_s,
        u"--setting-sources"_s, QString(),
        u"--settings"_s, uR"({"disableAllHooks":true})"_s,
        u"--disable-slash-commands"_s,
        u"--system-prompt"_s, systemPrompt(),
    };
}

QStringList deniedTools()
{
    return { u"Bash"_s,      u"BashOutput"_s, u"KillShell"_s, u"Read"_s,  u"Write"_s,
             u"Edit"_s,      u"MultiEdit"_s,  u"NotebookEdit"_s, u"Glob"_s, u"Grep"_s,
             u"LS"_s,        u"WebFetch"_s,   u"WebSearch"_s,  u"Task"_s,  u"TodoWrite"_s,
             u"SlashCommand"_s, u"Skill"_s,   u"ExitPlanMode"_s };
}

QStringList candidatePaths(HostOs os, const QString &home, const QString &localAppData,
                           const QString &appData)
{
    QStringList out;
    switch (os) {
    case HostOs::Windows:
        // Native installer first; the npm .cmd shim goes through cmd.exe, which re-parses its
        // arguments, so it is the last resort.
        out << home + u"/.local/bin/claude.exe"_s << home + u"/.claude/local/claude.exe"_s;
        if (!localAppData.isEmpty())
            out << localAppData + u"/Programs/claude/claude.exe"_s;
        if (!appData.isEmpty())
            out << appData + u"/npm/claude.exe"_s << appData + u"/npm/claude.cmd"_s;
        break;
    case HostOs::MacOs:
        out << home + u"/.local/bin/claude"_s << home + u"/.claude/local/claude"_s
            << u"/opt/homebrew/bin/claude"_s << u"/usr/local/bin/claude"_s;
        break;
    case HostOs::Linux:
        out << home + u"/.local/bin/claude"_s << home + u"/.claude/local/claude"_s
            << u"/usr/local/bin/claude"_s << u"/usr/bin/claude"_s
            << home + u"/.npm-global/bin/claude"_s;
        break;
    }
    return out;
}

QStringList loginShellLookupArguments()
{
    return { u"-l"_s, u"-c"_s, u"command -v claude"_s };
}

bool isCompatibleVersion(QStringView versionOutput)
{
    static const QRegularExpression kVersion(uR"((\d+)\.(\d+)\.(\d+))"_s);
    const auto match = kVersion.matchView(versionOutput);
    if (!match.hasMatch())
        return false;
    for (int i = 0; i < 3; ++i) {
        const int part = match.capturedView(i + 1).toInt();
        if (part != kMinimumVersion[i])
            return part > kMinimumVersion[i];
    }
    return true;
}

QString loginCommandHint()
{
    return u"claude /login"_s;
}

namespace {

AiErrorKind classify(const QString &text)
{
    const QString lower = text.toLower();
    if (lower.contains(u"usage limit") || lower.contains(u"rate limit")
        || lower.contains(u"limit reached") || lower.contains(u"quota"))
        return AiErrorKind::UsageLimit;
    if (lower.contains(u"not logged in") || lower.contains(u"/login")
        || lower.contains(u"invalid api key") || lower.contains(u"authentication")
        || lower.contains(u"oauth"))
        return AiErrorKind::NotLoggedIn;
    if (lower.contains(u"unknown option") || lower.contains(u"unrecognized option")
        || lower.contains(u"unknown argument"))
        return AiErrorKind::IncompatibleVersion;
    return AiErrorKind::None;
}

} // namespace

CliOutcome parseClaudeOutput(const QByteArray &stdoutData, const QByteArray &stderrData,
                             int exitCode)
{
    CliOutcome out;
    const QString err = QString::fromUtf8(stderrData).trimmed();
    const QByteArray trimmed = stdoutData.trimmed();
    // The JSON result is the last line; anything printed before it is ignored.
    const qsizetype lastLine = trimmed.lastIndexOf('\n');
    const QByteArray json = lastLine < 0 ? trimmed : trimmed.mid(lastLine + 1);

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error == QJsonParseError::NoError && doc.isObject()
        && doc.object().value(u"type").toString() == u"result") {
        const QJsonObject result = doc.object();
        const QString text = result.value(u"result").toString();
        const bool isError = result.value(u"is_error").toBool()
            || result.value(u"subtype").toString() != u"success";
        if (!isError && exitCode == 0) {
            if (text.trimmed().isEmpty()) {
                out.error = AiErrorKind::InvalidOutput;
                out.detail = QString::fromUtf8(json);
                return out;
            }
            out.text = text;
            return out;
        }
        out.detail = text.isEmpty() ? (err.isEmpty() ? QString::fromUtf8(json) : err) : text;
        const AiErrorKind kind = classify(out.detail + u'\n' + err);
        out.error = kind == AiErrorKind::None ? AiErrorKind::Failed : kind;
        return out;
    }

    out.detail = (QString::fromUtf8(trimmed) + u'\n' + err).trimmed();
    const AiErrorKind kind = classify(out.detail);
    if (kind != AiErrorKind::None)
        out.error = kind;
    else
        out.error = exitCode == 0 ? AiErrorKind::InvalidOutput : AiErrorKind::Failed;
    return out;
}

} // namespace quarry::ai
