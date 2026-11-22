#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <cstdint>

namespace quarry::ai {

enum class AiErrorKind : uint8_t {
    None,
    CliNotFound,
    NotLoggedIn,
    IncompatibleVersion,
    UsageLimit,
    Timeout,
    Cancelled,
    InvalidOutput,
    Failed,
};

struct CliOutcome
{
    AiErrorKind error = AiErrorKind::None;
    QString text; // the reply on success
    QString detail; // raw text from the CLI on failure, for the "Details" of the error
};

enum class HostOs : uint8_t { Windows, MacOs, Linux };

// Fixed argument list for one non-interactive, isolated run of `claude`. It never contains text
// from the user, the script or the schema: all of that goes through stdin. No tools, no MCP, no
// hooks, no settings files, no slash commands, no session saved to disk. With every tool off the
// run is a single turn (no --max-turns).
QStringList claudeArguments();
// The built-in tools denied again by name, in case --tools "" is ignored by a future CLI.
QStringList deniedTools();
// Usual install locations, best first; native executables before npm shims on Windows.
// home/localAppData/appData are passed in so tests do not depend on the machine.
QStringList candidatePaths(HostOs os, const QString &home, const QString &localAppData,
                           const QString &appData);
// Arguments for the one-off login-shell lookup when no candidate exists (Unix only).
QStringList loginShellLookupArguments();

// "2.0.14 (Claude Code)" -> true if at least kMinimumVersion.
inline constexpr int kMinimumVersion[3] = { 2, 0, 0 };
bool isCompatibleVersion(QStringView versionOutput);
QString loginCommandHint(); // what to tell the user when there is no session

// Classifies one run of `claude -p --output-format json`.
CliOutcome parseClaudeOutput(const QByteArray &stdoutData, const QByteArray &stderrData,
                             int exitCode);

} // namespace quarry::ai
