#pragma once

#include <QString>
#include <QStringList>
#include <QStringView>

namespace quarry::ai {

// Database error messages can quote real values ("Key (email)=(ana@x.com) already exists").
// Removes quoted strings, parenthesised values and numbers; keeps the shape of the message.
QString sanitizeErrorMessage(QStringView message);

struct MaskedScript
{
    QString text; // literals replaced by __V1__, __V2__...
    QStringList values; // values[i] is the original literal (with its quotes) of __V<i+1>__
};

// Replaces string literals (also N'', E'', dollar-quoted) and, optionally, numeric literals by
// placeholders. Comments and quoted identifiers are left alone.
MaskedScript maskLiterals(QStringView sql, bool maskNumbers = true);
// Puts the original literals back into a script returned by the AI.
QString unmaskLiterals(QStringView text, const QStringList &values);

} // namespace quarry::ai
