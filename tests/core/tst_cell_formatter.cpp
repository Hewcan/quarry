#include <quarry/cell_formatter.h>

#include <QElapsedTimer>
#include <QtTest>

#include <cmath>
#include <limits>

using namespace quarry;
using namespace Qt::StringLiterals;

namespace {

ColumnBuffer textCol(const QByteArray &utf8, ValueType type = ValueType::Text)
{
    ColumnBuffer c;
    c.appendBytes(utf8, type);
    return c;
}

ColumnBuffer blobCol(qsizetype n)
{
    ColumnBuffer c;
    c.appendBytes(QByteArray(n, '\x01'), ValueType::Bytes);
    return c;
}

QString ell()
{
    return QString(QChar(0x2026));
}

} // namespace

class TstCellFormatter : public QObject
{
    Q_OBJECT

private slots:
    void scalars_data()
    {
        QTest::addColumn<QString>("expectedDisplay");
        QTest::addColumn<double>("dbl");
        // Double edge cases (display == copy).
        QTest::newRow("0.1") << u"0.1"_s << 0.1;
        QTest::newRow("1e20") << u"1e+20"_s << 1e20;
        QTest::newRow("neg") << u"-2.5"_s << -2.5;
        QTest::newRow("nan") << u"nan"_s << std::numeric_limits<double>::quiet_NaN();
        QTest::newRow("inf") << u"inf"_s << std::numeric_limits<double>::infinity();
        QTest::newRow("-inf") << u"-inf"_s << -std::numeric_limits<double>::infinity();
        QTest::newRow("whole") << u"3"_s << 3.0;
        QTest::newRow("big") << u"1234567.5"_s << 1234567.5;
    }

    void scalars()
    {
        QFETCH(QString, expectedDisplay);
        QFETCH(double, dbl);
        ColumnBuffer c;
        c.append(dbl);
        QCOMPARE(CellFormatter::display(c, 0), expectedDisplay);
        QCOMPARE(CellFormatter::copy(c, 0), expectedDisplay);
    }

    void boolAndInt()
    {
        ColumnBuffer b;
        b.appendBool(true);
        b.appendBool(false);
        QCOMPARE(CellFormatter::display(b, 0), u"true"_s);
        QCOMPARE(CellFormatter::display(b, 1), u"false"_s);
        ColumnBuffer i;
        i.append(int64_t { -1234567 });
        i.append(std::numeric_limits<int64_t>::min());
        QCOMPARE(CellFormatter::display(i, 0), u"-1234567"_s);
        QCOMPARE(CellFormatter::copy(i, 1), u"-9223372036854775808"_s);
    }

    void textTypes_data()
    {
        QTest::addColumn<int>("type");
        QTest::newRow("Text") << static_cast<int>(ValueType::Text);
        QTest::newRow("Decimal") << static_cast<int>(ValueType::Decimal);
        QTest::newRow("Date") << static_cast<int>(ValueType::Date);
        QTest::newRow("Time") << static_cast<int>(ValueType::Time);
        QTest::newRow("DateTime") << static_cast<int>(ValueType::DateTime);
        QTest::newRow("DateTimeTz") << static_cast<int>(ValueType::DateTimeTz);
        QTest::newRow("Uuid") << static_cast<int>(ValueType::Uuid);
        QTest::newRow("Json") << static_cast<int>(ValueType::Json);
        QTest::newRow("Other") << static_cast<int>(ValueType::Other);
    }

    void textTypes()
    {
        QFETCH(int, type);
        const ColumnBuffer c = textCol("12.50 \xC3\xA9", static_cast<ValueType>(type));
        QCOMPARE(CellFormatter::display(c, 0), u"12.50 é"_s);
        QCOMPARE(CellFormatter::copy(c, 0), u"12.50 é"_s);
    }

    void nulls()
    {
        ColumnBuffer typed;
        typed.append(int64_t { 1 });
        typed.appendNull();
        QCOMPARE(CellFormatter::display(typed, 1), u"NULL"_s);
        QCOMPARE(CellFormatter::copy(typed, 1), QString());
        QVERIFY(CellFormatter::isNull(typed, 1));
        QVERIFY(!CellFormatter::isNull(typed, 0));

        ColumnBuffer mixed;
        mixed.append(int64_t { 1 });
        mixed.appendBytes("x", ValueType::Text);
        mixed.appendNull();
        QVERIFY(mixed.isMixed());
        QCOMPARE(CellFormatter::display(mixed, 2), u"NULL"_s);
        QCOMPARE(CellFormatter::copy(mixed, 2), QString());

        ColumnBuffer onlyNull;
        onlyNull.appendNull();
        QCOMPARE(CellFormatter::display(onlyNull, 0), u"NULL"_s);
    }

    void mixed()
    {
        ColumnBuffer c;
        c.append(int64_t { 42 });
        c.appendBytes("hi", ValueType::Text);
        c.appendBytes(QByteArray("\x00\xff", 2), ValueType::Bytes);
        c.append(0.5);
        QVERIFY(c.isMixed());
        QCOMPARE(CellFormatter::display(c, 0), u"42"_s);
        QCOMPARE(CellFormatter::display(c, 1), u"hi"_s);
        QCOMPARE(CellFormatter::display(c, 2), u"<binary 2 B>"_s);
        QCOMPARE(CellFormatter::copy(c, 2), u"0x00ff"_s);
        QCOMPARE(CellFormatter::display(c, 3), u"0.5"_s);
    }

    void byteSizes_data()
    {
        QTest::addColumn<qsizetype>("size");
        QTest::addColumn<QString>("expected");
        QTest::newRow("0") << qsizetype { 0 } << u"<binary 0 B>"_s;
        QTest::newRow("12") << qsizetype { 12 } << u"<binary 12 B>"_s;
        QTest::newRow("1023") << qsizetype { 1023 } << u"<binary 1023 B>"_s;
        QTest::newRow("1024") << qsizetype { 1024 } << u"<binary 1.0 KB>"_s;
        QTest::newRow("2355") << qsizetype { 2355 } << u"<binary 2.3 KB>"_s;
        QTest::newRow("1MB-1") << qsizetype { 1048575 } << u"<binary 1.0 MB>"_s;
        QTest::newRow("1MB") << qsizetype { 1048576 } << u"<binary 1.0 MB>"_s;
        QTest::newRow("5MB") << qsizetype { 5767168 } << u"<binary 5.5 MB>"_s;
    }

    void byteSizes()
    {
        QFETCH(qsizetype, size);
        QFETCH(QString, expected);
        QCOMPARE(CellFormatter::display(blobCol(size), 0), expected);
    }

    void blobHex()
    {
        ColumnBuffer c;
        c.appendBytes(QByteArray("\x0a\xAB\xcd\x00", 4), ValueType::Bytes);
        QCOMPARE(CellFormatter::copy(c, 0), u"0x0aabcd00"_s);
        ColumnBuffer e;
        e.appendBytes(QByteArray(), ValueType::Bytes);
        QCOMPARE(CellFormatter::copy(e, 0), u"0x"_s);
    }

    void emptyText()
    {
        const ColumnBuffer c = textCol("");
        QCOMPARE(CellFormatter::display(c, 0), QString());
        QCOMPARE(CellFormatter::copy(c, 0), QString());
        QVERIFY(!CellFormatter::isNull(c, 0));
    }

    void firstLineCut_data()
    {
        QTest::addColumn<QByteArray>("in");
        QTest::addColumn<QString>("display");
        QTest::addColumn<QString>("copy");
        QTest::newRow("lf") << QByteArray("a\nb") << u"a"_s + ell() << u"a\nb"_s;
        QTest::newRow("crlf") << QByteArray("a\r\nb") << u"a"_s + ell() << u"a\r\nb"_s;
        QTest::newRow("cr") << QByteArray("a\rb") << u"a"_s + ell() << u"a\rb"_s;
        QTest::newRow("leading") << QByteArray("\nb") << ell() << u"\nb"_s;
        QTest::newRow("none") << QByteArray("ab") << u"ab"_s << u"ab"_s;
    }

    void firstLineCut()
    {
        QFETCH(QByteArray, in);
        QFETCH(QString, display);
        QFETCH(QString, copy);
        const ColumnBuffer c = textCol(in);
        QCOMPARE(CellFormatter::display(c, 0), display);
        QCOMPARE(CellFormatter::copy(c, 0), copy);
    }

    void charLimit()
    {
        const QByteArray exact(CellFormatter::kDisplayChars, 'a');
        QCOMPARE(CellFormatter::display(textCol(exact), 0), QString::fromLatin1(exact));

        const QByteArray over(CellFormatter::kDisplayChars + 1, 'a');
        const QString shown = CellFormatter::display(textCol(over), 0);
        QCOMPARE(shown.size(), CellFormatter::kDisplayChars + 1);
        QVERIFY(shown.endsWith(QChar(0x2026)));
        QCOMPARE(CellFormatter::copy(textCol(over), 0).size(), CellFormatter::kDisplayChars + 1);

        // Multi-byte text longer than the decode window still yields kDisplayChars units.
        QByteArray wide;
        for (int i = 0; i < 3000; ++i) {
            wide += "\xC3\xA9";
        }
        const QString w = CellFormatter::display(textCol(wide), 0);
        QCOMPARE(w.size(), CellFormatter::kDisplayChars + 1);
        QVERIFY(w.endsWith(QChar(0x2026)));
        QCOMPARE(CellFormatter::copy(textCol(wide), 0).size(), 3000);
    }

    void surrogateAtCut()
    {
        // 499 ASCII + U+1F600 (high at index 499, low at 500): cut must drop the whole pair.
        QByteArray in(CellFormatter::kDisplayChars - 1, 'a');
        in += "\xF0\x9F\x98\x80";
        in += "tail";
        const QString s = CellFormatter::display(textCol(in), 0);
        QCOMPARE(s.size(), CellFormatter::kDisplayChars);
        QVERIFY(s.endsWith(QChar(0x2026)));
        QVERIFY(!s.at(s.size() - 2).isHighSurrogate());

        // Pair fully inside the limit is kept.
        QByteArray in2(CellFormatter::kDisplayChars - 2, 'a');
        in2 += "\xF0\x9F\x98\x80";
        in2 += "tail";
        const QString s2 = CellFormatter::display(textCol(in2), 0);
        QVERIFY(s2.contains(QString::fromUcs4(U"\U0001F600")));
        QVERIFY(s2.endsWith(QChar(0x2026)));
    }

    void hugeText()
    {
        const ColumnBuffer c = textCol(QByteArray(qsizetype { 10485760 }, 'x'));
        QCOMPARE(CellFormatter::display(c, 0).size(), CellFormatter::kDisplayChars + 1);
    }

    void perf()
    {
        constexpr int kCols = 10;
        constexpr int kRows = 100000;
        std::vector<ColumnBuffer> cols(kCols);
        for (int r = 0; r < kRows; ++r) {
            cols[0].append(int64_t { r });
            cols[1].append(r * 0.25);
            cols[2].appendBytes("some short text value", ValueType::Text);
            cols[3].appendBool(r % 2 == 0);
            cols[4].appendBytes("2026-11-14T10:20:30Z", ValueType::DateTime);
            cols[5].appendBytes(QByteArray(64, '\x07'), ValueType::Bytes);
            cols[6].appendBytes("line one\nline two", ValueType::Text);
            if (r % 3 == 0) {
                cols[7].appendNull();
            } else {
                cols[7].append(int64_t { r });
            }
            cols[8].appendBytes("{\"a\": 1}", ValueType::Json);
            // mixed
            if (r % 2 == 0) {
                cols[9].append(int64_t { r });
            } else {
                cols[9].appendBytes("x", ValueType::Text);
            }
        }
        QElapsedTimer timer;
        timer.start();
        qsizetype total = 0;
        for (int rep = 0; rep < 10; ++rep) {
            for (const ColumnBuffer &c : cols) {
                for (qsizetype r = 0; r < kRows; ++r) {
                    total += CellFormatter::display(c, r).size();
                }
            }
        }
        const qint64 ms = timer.elapsed();
        qInfo() << "1000000 display() calls:" << ms << "ms";
        QVERIFY(total > 0);
#ifdef NDEBUG
        QVERIFY2(ms < 2000, "perf budget (release)");
#endif
    }
};

QTEST_GUILESS_MAIN(TstCellFormatter)
#include "tst_cell_formatter.moc"
