#include "command.h"

#include <QCoreApplication>

namespace quarry::ui {

QString displayShortcutText(const QString &portable)
{
    const QKeySequence sequence(portable, QKeySequence::PortableText);
#ifdef Q_OS_MACOS
    return sequence.toString(QKeySequence::NativeText);
#else
    // Qt writes the modifiers with their long, translated names ("Control", "Mayusculas"), which
    // swallow the menu's width; these are the short forms.
    QStringList chords;
    for (int i = 0; i < sequence.count(); ++i) {
        const QKeyCombination combination = sequence[i];
        const Qt::KeyboardModifiers mods = combination.keyboardModifiers();
        QStringList parts;
        if (mods.testFlag(Qt::MetaModifier))
            parts << QCoreApplication::translate("ShortcutText", "Meta");
        if (mods.testFlag(Qt::ControlModifier))
            parts << QCoreApplication::translate("ShortcutText", "Ctrl");
        if (mods.testFlag(Qt::AltModifier))
            parts << QCoreApplication::translate("ShortcutText", "Alt");
        if (mods.testFlag(Qt::ShiftModifier))
            parts << QCoreApplication::translate("ShortcutText", "Shift");
        parts << QKeySequence(combination.key()).toString(QKeySequence::NativeText);
        chords << parts.join(QLatin1Char('+'));
    }
    return chords.join(QLatin1String(", "));
#endif
}

QString Command::shortcutText() const
{
    return displayShortcutText(m_shortcut);
}

QStringList Command::sequences() const
{
    QStringList out;
    for (const QString &s : { m_shortcut, m_alternate })
        if (!s.isEmpty())
            out << s;
    return out;
}

void Command::trigger()
{
    if (m_callback)
        m_callback();
    emit triggered();
}

} // namespace quarry::ui
