#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QtTest>

using namespace Qt::StringLiterals;

// The AI module must not be able to reach result types or query execution (ARCHITECTURE §6.13).
// CMake already refuses any link but Qt6::Core (quarry_assert_ai_isolated); this test checks the
// sources so that a header-only include of core or the provider API cannot slip in either.
class TstAiArchitecture : public QObject
{
    Q_OBJECT

private:
    static QStringList sources()
    {
        QStringList out;
        QDirIterator it(QStringLiteral(QUARRY_AI_SOURCE_DIR), { u"*.h"_s, u"*.cpp"_s },
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
            out.append(it.next());
        return out;
    }

    static QString read(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(file.readAll());
    }

private slots:
    void onlyAiAndQtCoreIncludes()
    {
        const QStringList files = sources();
        QVERIFY(files.size() >= 10);
        // \x23 is '#' and \x22 is '"': spelled out so moc's preprocessor does not trip on them.
        static const QRegularExpression kInclude(
            u"^\\s*\\x23\\s*include\\s*[<\\x22]([^>\\x22]+)[>\\x22]"_s,
            QRegularExpression::MultilineOption);
        static const QRegularExpression kAllowedQt(
            uR"(^Q(Byte|String|Hash|Set|Cryptographic|Regular|Json|List|Char|Dir|File|Process|Timer|Object|Elapsed|Pointer|Deadline|Standard|Operating)\w*$)"_s);
        for (const QString &path : files) {
            auto it = kInclude.globalMatch(read(path));
            while (it.hasNext()) {
                const QString header = it.next().captured(1);
                if (header.startsWith(u"quarry/")) {
                    QVERIFY2(header.startsWith(u"quarry/ai/"),
                             qPrintable(path + u" includes " + header));
                } else if (header.startsWith(u'Q')) {
                    QVERIFY2(kAllowedQt.match(header).hasMatch(),
                             qPrintable(path + u" includes " + header));
                } else {
                    QVERIFY2(!header.contains(u'/') && !header.contains(u'.'),
                             qPrintable(path + u" includes " + header)); // only the std library
                }
            }
        }
    }

    void noExecutionOrResultTypes()
    {
        static const QRegularExpression kForbidden(
            uR"(\b(Session|IConnection|ResultBatch|ColumnBuffer|ResultTableModel|QueryController|SqlSafety|Statement|ExecutionStats)\b|\bexecute\s*\()"_s);
        for (const QString &path : sources()) {
            const auto match = kForbidden.match(read(path));
            QVERIFY2(!match.hasMatch(), qPrintable(path + u" mentions " + match.captured()));
        }
    }

    void linksOnlyQtCore()
    {
        const QString cmake = read(QStringLiteral(QUARRY_AI_SOURCE_DIR "/CMakeLists.txt"));
        static const QRegularExpression kLink(uR"(target_link_libraries\(quarry_ai([^)]*)\))"_s);
        auto it = kLink.globalMatch(cmake);
        int count = 0;
        while (it.hasNext()) {
            ++count;
            QCOMPARE(it.next().captured(1).simplified(), u"PUBLIC Qt6::Core"_s);
        }
        QCOMPARE(count, 1);
    }
};

QTEST_GUILESS_MAIN(TstAiArchitecture)
#include "tst_ai_architecture.moc"
