#include <quarry/ai/script_diff.h>

#include <QtTest>

using namespace quarry::ai;
using namespace Qt::StringLiterals;

namespace {
const QString kOriginal = u"select 1;\nselect 2;\nselect 3;\n"_s;
const QString kProposed = u"select 1;\nselect 20;\nselect 3;\ndelete from t where id = 1;\n"_s;
} // namespace

class TstScriptDiff : public QObject
{
    Q_OBJECT

private slots:
    void hunks()
    {
        const ScriptDiff diff = ScriptDiff::compute(kOriginal, kProposed);
        QCOMPARE(diff.hunks().size(), size_t(2));
        const DiffHunk &change = diff.hunks()[0];
        QCOMPARE(change.originalLine, qsizetype(1));
        QCOMPARE(change.removed, QStringList { u"select 2;"_s });
        QCOMPARE(change.added, QStringList { u"select 20;"_s });
        QVERIFY(!change.modifiesData);
        const DiffHunk &insert = diff.hunks()[1];
        QCOMPARE(insert.originalLine, qsizetype(3));
        QVERIFY(insert.removed.isEmpty());
        QVERIFY(insert.modifiesData); // the DELETE is flagged before anyone runs it
        QVERIFY(diff.hasPending());
    }

    void originalUntilAccepted()
    {
        ScriptDiff diff = ScriptDiff::compute(kOriginal, kProposed);
        QCOMPARE(diff.result(), kOriginal);
        QCOMPARE(diff.original(), kOriginal);
    }

    void acceptAllAndRejectAll()
    {
        ScriptDiff diff = ScriptDiff::compute(kOriginal, kProposed);
        diff.acceptAll();
        QCOMPARE(diff.result(), kProposed);
        QVERIFY(!diff.hasPending());
        diff.rejectAll();
        QCOMPARE(diff.result(), kOriginal);
        QVERIFY(diff.undo());
        QCOMPARE(diff.result(), kProposed);
        QVERIFY(diff.undo());
        QCOMPARE(diff.result(), kOriginal);
        QVERIFY(diff.hasPending());
        QVERIFY(!diff.undo());
    }

    void perHunkWithUndo()
    {
        ScriptDiff diff = ScriptDiff::compute(kOriginal, kProposed);
        diff.accept(0);
        diff.reject(1);
        QCOMPARE(diff.result(), u"select 1;\nselect 20;\nselect 3;\n"_s);
        QVERIFY(diff.undo());
        QCOMPARE(diff.hunks()[1].decision, DiffHunk::Decision::Pending);
        diff.accept(1);
        QCOMPARE(diff.result(), kProposed);
        diff.accept(99); // out of range: ignored
        QCOMPARE(diff.result(), kProposed);
    }

    void edgeCases()
    {
        QVERIFY(ScriptDiff::compute(kOriginal, kOriginal).hunks().empty());
        ScriptDiff fromEmpty = ScriptDiff::compute(QString(), u"select 1;"_s);
        QCOMPARE(fromEmpty.hunks().size(), size_t(1));
        fromEmpty.acceptAll();
        QCOMPARE(fromEmpty.result(), u"select 1;"_s);
        ScriptDiff toEmpty = ScriptDiff::compute(u"a\nb"_s, QString());
        toEmpty.acceptAll();
        QCOMPARE(toEmpty.result(), QString());
    }

    void modifiesDataIgnoresComments()
    {
        QVERIFY(!ScriptDiff::modifiesData({ u"select updated_at from t -- update later"_s }));
        QVERIFY(ScriptDiff::modifiesData({ u"  DROP TABLE x;"_s }));
        QVERIFY(ScriptDiff::modifiesData({ u"Update t set a = 1"_s }));
    }
};

QTEST_GUILESS_MAIN(TstScriptDiff)
#include "tst_script_diff.moc"
