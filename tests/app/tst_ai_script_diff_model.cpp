#include "ai_script_diff.h"

#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QtTest>

using namespace quarry;
using namespace quarry::ui;

namespace {

const QString kOriginal = QStringLiteral("SELECT *\nFROM customers\nWHERE id = 1\nORDER BY name\n");
// Changes line 3 (a change) and appends a DELETE (an addition that changes data).
const QString kProposed = QStringLiteral(
    "SELECT *\nFROM customers\nWHERE id = 2\nORDER BY name\nDELETE FROM invoices;\n");

QString kindAt(const AiScriptDiff &m, int row)
{
    return m.data(m.index(row), AiScriptDiff::KindRole).toString();
}

QString decisionAt(const AiScriptDiff &m, int row)
{
    return m.data(m.index(row), AiScriptDiff::DecisionRole).toString();
}

} // namespace

class TstAiScriptDiffModel : public QObject
{
    Q_OBJECT

private slots:
    void blocksCoverTheWholeScriptInOrder()
    {
        AiScriptDiff m(ai::ScriptDiff::compute(kOriginal, kProposed));
        const QAbstractItemModelTester tester(&m);
        // unchanged (2 lines), changed (line 3), unchanged (line 4), added (the DELETE)
        QCOMPARE(m.count(), 4);
        QCOMPARE(m.changeCount(), 2);
        QCOMPARE(kindAt(m, 0), QStringLiteral("unchanged"));
        QCOMPARE(kindAt(m, 1), QStringLiteral("changed"));
        QCOMPARE(kindAt(m, 2), QStringLiteral("unchanged"));
        QCOMPARE(kindAt(m, 3), QStringLiteral("added"));
        QCOMPARE(m.data(m.index(0), AiScriptDiff::OldLinesRole).toStringList().size(), 2);
        QCOMPARE(m.data(m.index(1), AiScriptDiff::OldLinesRole).toStringList(),
                 QStringList { QStringLiteral("WHERE id = 1") });
        QCOMPARE(m.data(m.index(1), AiScriptDiff::NewLinesRole).toStringList(),
                 QStringList { QStringLiteral("WHERE id = 2") });
        QCOMPARE(m.data(m.index(1), AiScriptDiff::StartLineRole).toInt(), 3);
        QCOMPARE(m.data(m.index(1), AiScriptDiff::ChangeNumberRole).toInt(), 1);
        QCOMPARE(m.data(m.index(3), AiScriptDiff::ChangeNumberRole).toInt(), 2);
        QCOMPARE(m.data(m.index(0), AiScriptDiff::ChangeNumberRole).toInt(), 0);
        QCOMPARE(decisionAt(m, 0), QString()); // an unchanged stretch has no decision
        QCOMPARE(decisionAt(m, 1), QStringLiteral("pending"));
        QCOMPARE(m.original(), kOriginal);
        QCOMPARE(m.firstChange(), 1);
        QCOMPARE(m.nextChange(1, 1), 3);
        QCOMPARE(m.nextChange(3, 1), -1);
        QCOMPARE(m.nextChange(3, -1), 1);
        QCOMPARE(m.nextChange(1, -1), -1);
    }

    void dataChangingBlocksAreMarked()
    {
        AiScriptDiff m(ai::ScriptDiff::compute(kOriginal, kProposed));
        QVERIFY(m.modifiesData());
        QVERIFY(!m.data(m.index(1), AiScriptDiff::ModifiesDataRole).toBool()); // a WHERE value
        QVERIFY(m.data(m.index(3), AiScriptDiff::ModifiesDataRole).toBool()); // the DELETE
        // The words for the screen reader say it too.
        const QString description = m.data(m.index(3), AiScriptDiff::DescriptionRole).toString();
        QVERIFY2(description.contains(QStringLiteral("Change 2 of 2")), qPrintable(description));
        QVERIFY2(description.contains(QStringLiteral("adds 1 line")), qPrintable(description));
        QVERIFY2(description.contains(QStringLiteral("pending")), qPrintable(description));
        QVERIFY2(description.contains(QStringLiteral("changes data")), qPrintable(description));
        const QString change = m.data(m.index(1), AiScriptDiff::DescriptionRole).toString();
        QVERIFY2(change.contains(QStringLiteral("replaces 1 line with 1 line")),
                 qPrintable(change));
        QVERIFY(!change.contains(QStringLiteral("changes data")));

        AiScriptDiff plain(ai::ScriptDiff::compute(kOriginal, QStringLiteral("SELECT 1\n")));
        QVERIFY(!plain.modifiesData());
    }

    void acceptRejectAndTheResult()
    {
        AiScriptDiff m(ai::ScriptDiff::compute(kOriginal, kProposed));
        const QAbstractItemModelTester tester(&m);
        QSignalSpy decisions(&m, &AiScriptDiff::decisionsChanged);
        QVERIFY(m.hasPending());
        QCOMPARE(m.pendingCount(), 2);
        // Nothing decided: the script is the original, untouched.
        QCOMPARE(m.result(), kOriginal);

        m.accept(1);
        QCOMPARE(decisionAt(m, 1), QStringLiteral("accepted"));
        QCOMPARE(decisionAt(m, 3), QStringLiteral("pending"));
        QCOMPARE(m.pendingCount(), 1);
        QVERIFY(m.hasPending());
        QCOMPARE(decisions.size(), 1);
        QCOMPARE(m.result(),
                 QStringLiteral("SELECT *\nFROM customers\nWHERE id = 2\nORDER BY name\n"));

        m.reject(3);
        QCOMPARE(decisionAt(m, 3), QStringLiteral("rejected"));
        QVERIFY(!m.hasPending());
        // The rejected DELETE is not in the result.
        QVERIFY(!m.result().contains(QStringLiteral("DELETE")));
        QVERIFY(m.original() == kOriginal); // the original never changes
        QCOMPARE(decisions.size(), 2);

        // An unchanged row or a row out of range does nothing.
        m.accept(0);
        m.reject(99);
        m.accept(-1);
        QCOMPARE(decisions.size(), 2);
        QCOMPARE(decisionAt(m, 0), QString());
    }

    void acceptAllRejectAllAndUndo()
    {
        AiScriptDiff m(ai::ScriptDiff::compute(kOriginal, kProposed));
        const QAbstractItemModelTester tester(&m);
        QVERIFY(!m.canUndo());
        m.acceptAll();
        QCOMPARE(m.pendingCount(), 0);
        QVERIFY(m.canUndo());
        QCOMPARE(m.result(), kProposed);
        m.undo(); // one step back for the whole "Accept all"
        QCOMPARE(m.pendingCount(), 2);
        QVERIFY(!m.canUndo());
        QCOMPARE(m.result(), kOriginal);

        m.accept(1);
        m.rejectAll();
        QCOMPARE(decisionAt(m, 1), QStringLiteral("rejected"));
        QCOMPARE(m.result(), kOriginal);
        m.undo(); // back to "only block 1 accepted"
        QCOMPARE(decisionAt(m, 1), QStringLiteral("accepted"));
        QCOMPARE(decisionAt(m, 3), QStringLiteral("pending"));
        m.undo();
        QCOMPARE(m.pendingCount(), 2);
        m.undo(); // nothing left: no change, no crash
        QCOMPARE(m.pendingCount(), 2);
    }

    void aScriptWithoutChangesHasNoBlocksToDecide()
    {
        AiScriptDiff same(ai::ScriptDiff::compute(kOriginal, kOriginal));
        const QAbstractItemModelTester tester(&same);
        QCOMPARE(same.changeCount(), 0);
        QVERIFY(!same.hasPending());
        QCOMPARE(same.firstChange(), -1);
        same.acceptAll(); // nothing to do
        QVERIFY(!same.canUndo());
        QCOMPARE(same.result(), kOriginal);

        AiScriptDiff empty(ai::ScriptDiff::compute(QString(), QStringLiteral("SELECT 1\n")));
        const QAbstractItemModelTester tester2(&empty);
        QCOMPARE(empty.count(), 1);
        QCOMPARE(kindAt(empty, 0), QStringLiteral("added"));
        empty.accept(0);
        QCOMPARE(empty.result(),
                 QStringLiteral("SELECT 1")); // the empty original had no final newline
    }

    void aRemovalIsABlockOfItsOwn()
    {
        AiScriptDiff m(ai::ScriptDiff::compute(
            kOriginal, QStringLiteral("SELECT *\nFROM customers\nORDER BY name\n")));
        const QAbstractItemModelTester tester(&m);
        int removed = -1;
        for (int i = 0; i < m.count(); ++i) {
            if (kindAt(m, i) == QLatin1String("removed"))
                removed = i;
        }
        QVERIFY(removed >= 0);
        QVERIFY(m.data(m.index(removed), AiScriptDiff::DescriptionRole)
                    .toString()
                    .contains(QStringLiteral("removes 1 line")));
        m.accept(removed);
        QCOMPARE(m.result(), QStringLiteral("SELECT *\nFROM customers\nORDER BY name\n"));
    }

    void aStretchWithoutChangesKeepsItsLines()
    {
        QStringList lines;
        for (int i = 1; i <= 30; ++i)
            lines << QStringLiteral("SELECT %1;").arg(i);
        const QString text = lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
        AiScriptDiff m(ai::ScriptDiff::compute(text, text + QStringLiteral("SELECT 31;\n")));
        QCOMPARE(m.count(), 2);
        QCOMPARE(m.data(m.index(0), AiScriptDiff::OldLinesRole).toStringList().size(), 30);
        QCOMPARE(m.data(m.index(0), AiScriptDiff::StartLineRole).toInt(), 1);
    }
};

QTEST_GUILESS_MAIN(TstAiScriptDiffModel)
#include "tst_ai_script_diff_model.moc"
