#include "ui/value_viewer.h"

#include <QtTest>

using namespace quarry;
using namespace quarry::ui;

// What the record panel's viewers show (H11 phase 3): which viewer a value gets, JSON and XML
// re-indented (keys keep their order), the hex dump of a BLOB with its cap and size, and the cap on
// text. Pure formatting: no UI.
class TstValueViewer : public QObject
{
    Q_OBJECT

private slots:
    void detectByTypeThenByContent()
    {
        const auto kind = [](ValueType type, const QString &native, const QString &text) {
            return ValueViewer::detect(type, native, text);
        };
        QCOMPARE(kind(ValueType::Json, {}, QStringLiteral("{}")), QStringLiteral("json"));
        QCOMPARE(kind(ValueType::Other, QStringLiteral("jsonb"), QStringLiteral("1")),
                 QStringLiteral("json"));
        QCOMPARE(kind(ValueType::Bytes, {}, {}), QStringLiteral("blob"));
        QCOMPARE(kind(ValueType::Other, QStringLiteral("xml"), QStringLiteral("<a/>")),
                 QStringLiteral("xml"));
        // Text: by its content.
        QCOMPARE(kind(ValueType::Text, QStringLiteral("TEXT"), QStringLiteral(" {\"a\": 1}")),
                 QStringLiteral("json"));
        QCOMPARE(kind(ValueType::Text, {}, QStringLiteral("[1, 2]")), QStringLiteral("json"));
        QCOMPARE(kind(ValueType::Text, {}, QStringLiteral("<a><b/></a>")), QStringLiteral("xml"));
        // Looks like JSON / XML but is not, and is short: nothing special.
        QCOMPARE(kind(ValueType::Text, {}, QStringLiteral("{oops")), QString());
        QCOMPARE(kind(ValueType::Text, {}, QStringLiteral("<a><b></a>")), QString());
        QCOMPARE(kind(ValueType::Text, {}, QStringLiteral("hello")), QString());
        QCOMPARE(kind(ValueType::Int64, {}, QStringLiteral("42")), QString());
        // Long text, or with a line break: the grid cuts it, so it has a viewer.
        QCOMPARE(kind(ValueType::Text, {}, QString(ValueViewer::kLongTextChars, QLatin1Char('x'))),
                 QStringLiteral("text"));
        QCOMPARE(kind(ValueType::Text, {}, QStringLiteral("one\ntwo")), QStringLiteral("text"));
        // Invalid but long: text.
        QCOMPARE(kind(ValueType::Text, {}, QStringLiteral("{") + QString(300, QLatin1Char('x'))),
                 QStringLiteral("text"));
    }

    void jsonIsIndentedInTheOrderItCameIn()
    {
        const auto f = ValueViewer::formatJson(
            QStringLiteral("{\"b\":1,\"a\":[1,2,{}],\"c\":{\"d\":null}}"));
        QVERIFY(f.ok);
        QVERIFY(f.notice.isEmpty());
        QCOMPARE(f.text,
                 QStringLiteral("{\n"
                                "  \"b\": 1,\n"
                                "  \"a\": [\n"
                                "    1,\n"
                                "    2,\n"
                                "    {}\n"
                                "  ],\n"
                                "  \"c\": {\n"
                                "    \"d\": null\n"
                                "  }\n"
                                "}"));
        // Braces, commas and escaped quotes inside a string are left alone.
        const auto g = ValueViewer::formatJson(QStringLiteral("{\"k\":\"a,{b}\\\"c:\"}"));
        QVERIFY(g.ok);
        QCOMPARE(g.text, QStringLiteral("{\n  \"k\": \"a,{b}\\\"c:\"\n}"));
        // An empty array stays on one line.
        QCOMPARE(ValueViewer::formatJson(QStringLiteral("[ ]")).text, QStringLiteral("[]"));
    }

    void invalidJsonIsShownAsItIs()
    {
        const auto f = ValueViewer::formatJson(QStringLiteral("{\"a\": }"));
        QVERIFY(!f.ok);
        QCOMPARE(f.text, QStringLiteral("{\"a\": }"));
        QVERIFY2(f.notice.contains(QLatin1String("not valid JSON")), qPrintable(f.notice));
    }

    void xmlIsIndentedOrShownAsItIs()
    {
        const auto f = ValueViewer::formatXml(QStringLiteral("<a><b>x</b><c/></a>"));
        QVERIFY(f.ok);
        QVERIFY2(f.text.contains(QLatin1String("\n  <b>x</b>")), qPrintable(f.text));
        QVERIFY2(f.text.contains(QLatin1String("\n  <c/>")), qPrintable(f.text));
        const auto bad = ValueViewer::formatXml(QStringLiteral("<a><b></a>"));
        QVERIFY(!bad.ok);
        QCOMPARE(bad.text, QStringLiteral("<a><b></a>"));
        QVERIFY2(bad.notice.contains(QLatin1String("not well-formed XML")), qPrintable(bad.notice));
    }

    void textIsCutAtTheLimitWithANotice()
    {
        const QString big(ValueViewer::kMaxChars + 10, QLatin1Char('x'));
        const auto f = ValueViewer::format(QStringLiteral("text"), big);
        QCOMPARE(f.text.size(), ValueViewer::kMaxChars);
        QVERIFY2(f.notice.contains(QLatin1String("10 more characters")), qPrintable(f.notice));
        const auto small = ValueViewer::format(QStringLiteral("text"), QStringLiteral("abc"));
        QCOMPARE(small.text, QStringLiteral("abc"));
        QVERIFY(small.notice.isEmpty());
        // Too big to format: shown as it is, cut, with the reason.
        const auto json
            = ValueViewer::formatJson(QString(ValueViewer::kMaxChars + 1, QLatin1Char('[')));
        QVERIFY(!json.ok);
        QVERIFY2(json.notice.contains(QLatin1String("Too large")), qPrintable(json.notice));
    }

    void hexDumpHasOffsetsBytesAndAscii()
    {
        const auto f = ValueViewer::hexDump(QByteArray("ABCDEFGHIJKLMNOPQRST"), 20);
        QVERIFY(f.notice.isEmpty());
        const QStringList lines = f.text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines.at(0),
                 QStringLiteral("00000000  41 42 43 44 45 46 47 48  49 4a 4b 4c 4d 4e 4f 50  "
                                "|ABCDEFGHIJKLMNOP|"));
        QCOMPARE(lines.at(1),
                 QStringLiteral("00000010  51 52 53 54 ") + QString(12 * 3 + 1, QLatin1Char(' '))
                     + QStringLiteral(" |QRST|"));
        // Not printable: a dot.
        const auto g = ValueViewer::hexDump(QByteArray("\x00\x1f\x7f\xff", 4), 4);
        QVERIFY2(g.text.contains(QLatin1String("|....|")), qPrintable(g.text));
        QVERIFY(g.text.contains(QLatin1String("00 1f 7f ff")));
    }

    void hexDumpShowsOnlyTheFirst64KiBAndSaysSo()
    {
        const QByteArray bytes(70000, 'a');
        const auto f = ValueViewer::hexDump(bytes, 70000);
        QCOMPARE(f.text.count(QLatin1Char('\n')), 4096); // 65 536 / 16
        QVERIFY2(f.notice.contains(QLatin1String("4464 more bytes not shown")),
                 qPrintable(f.notice));
        // Fewer bytes were read than the value has (the model reads only the first 64 KiB).
        const auto g = ValueViewer::hexDump(QByteArray(ValueViewer::kMaxBlobBytes, 'a'), 5000000);
        QVERIFY2(g.notice.contains(QLatin1String("4934464 more bytes")), qPrintable(g.notice));
        // Empty: no lines, no notice.
        const auto e = ValueViewer::hexDump({}, 0);
        QVERIFY(e.text.isEmpty());
        QVERIFY(e.notice.isEmpty());
    }

    void sizeIsSaidInWords()
    {
        QCOMPARE(ValueViewer::sizeText(0), QStringLiteral("0 bytes"));
        QCOMPARE(ValueViewer::sizeText(1), QStringLiteral("1 byte"));
        QCOMPARE(ValueViewer::sizeText(512), QStringLiteral("512 bytes"));
        QCOMPARE(ValueViewer::sizeText(2048), QStringLiteral("2048 bytes (2.0 KiB)"));
        QCOMPARE(ValueViewer::sizeText(qint64(5) * 1024 * 1024),
                 QStringLiteral("5242880 bytes (5.0 MiB)"));
    }
};

QTEST_MAIN(TstValueViewer)
#include "tst_value_viewer.moc"
