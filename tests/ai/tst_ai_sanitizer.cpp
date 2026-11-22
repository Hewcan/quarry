#include <quarry/ai/ai_sanitizer.h>

#include <QtTest>

using namespace quarry::ai;
using namespace Qt::StringLiterals;

class TstAiSanitizer : public QObject
{
    Q_OBJECT

private slots:
    void errorMessages_data()
    {
        QTest::addColumn<QString>("message");
        QTest::addColumn<QString>("expected");
        QTest::newRow("postgres unique")
            << u"duplicate key value violates unique constraint \"users_email_key\" DETAIL: Key (email)=(ana@example.com) already exists."_s
            << u"duplicate key value violates unique constraint \"users_email_key\" DETAIL: Key (email)=(?) already exists."_s;
        QTest::newRow("postgres row")
            << u"Failing row contains (17, Ana, f(x))."_s << u"Failing row contains (?)."_s;
        QTest::newRow("mysql duplicate")
            << u"Duplicate entry 'ana@example.com' for key 'users.email'"_s
            << u"Duplicate entry '?' for key '?'"_s;
        QTest::newRow("sqlserver")
            << u"The duplicate key value is (ana, 42)."_s << u"The duplicate key value is (?)."_s;
        QTest::newRow("numbers") << u"value 123456.78 out of range for column amount2"_s
                                 << u"value ? out of range for column amount2"_s;
        QTest::newRow("plain") << u"relation \"users\" does not exist"_s
                               << u"relation \"users\" does not exist"_s;
    }

    void errorMessages()
    {
        QFETCH(QString, message);
        QFETCH(QString, expected);
        QCOMPARE(sanitizeErrorMessage(message), expected);
    }

    void masksLiterals()
    {
        const QString sql = u"-- keep 'this' comment\nSELECT \"o'k\", t.col1 FROM t WHERE a = 'it''s' "
                            "AND b = N'Ana' AND c = 42 AND d = $$x$$ AND e = $1 AND f = $tag$y$tag$;"_s;
        const MaskedScript masked = maskLiterals(sql);
        QCOMPARE(masked.text,
                 u"-- keep 'this' comment\nSELECT \"o'k\", t.col1 FROM t WHERE a = __V1__ "
                 "AND b = __V2__ AND c = __V3__ AND d = __V4__ AND e = $1 AND f = __V5__;"_s);
        QCOMPARE(masked.values, (QStringList { u"'it''s'"_s, u"N'Ana'"_s, u"42"_s, u"$$x$$"_s,
                                               u"$tag$y$tag$"_s }));
        QCOMPARE(unmaskLiterals(masked.text, masked.values), sql);
    }

    void numbersOptional()
    {
        const MaskedScript masked = maskLiterals(u"SELECT * FROM t LIMIT 10 OFFSET 'x'"_s, false);
        QCOMPARE(masked.text, u"SELECT * FROM t LIMIT 10 OFFSET __V1__"_s);
    }

    void unmaskKeepsUnknownPlaceholders()
    {
        QCOMPARE(unmaskLiterals(u"a = __V1__ or b = __V9__"_s, { u"'x'"_s }),
                 u"a = 'x' or b = __V9__"_s);
    }
};

QTEST_GUILESS_MAIN(TstAiSanitizer)
#include "tst_ai_sanitizer.moc"
