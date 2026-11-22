#pragma once

#include <quarry/ai/context_builder.h>

#include <QByteArray>
#include <QString>
#include <QStringView>

#include <cstdint>
#include <optional>
#include <vector>

namespace quarry::ai {

struct ChatTurn
{
    enum class Role : uint8_t { User, Assistant };
    Role role = Role::User;
    QString text;
};

struct PromptInput
{
    QString request; // what the user typed now
    QString scriptName;
    QString script; // the working copy (already masked if the user asked for it)
    SelectedContext context;
    QString errorMessage; // already sanitized with sanitizeErrorMessage(), may be empty
    std::vector<ChatTurn> history; // oldest first, without the current request
    qsizetype maxChars = 60000;
};

// Fixed instructions passed with --system-prompt (no user text in it).
QString systemPrompt();
// Each run is independent: the history is rebuilt here, newest turns first until maxChars.
// Untrusted parts go inside tagged blocks whose closing tags are neutralised.
QByteArray buildPrompt(const PromptInput &input);

struct AiReply
{
    QString message;
    std::optional<QString> proposedScript; // the whole new script, if the AI changed it
};
AiReply parseReply(QStringView text);

} // namespace quarry::ai
