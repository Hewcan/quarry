#include <quarry/column_buffer.h>

#include <QtTest>

#include <limits>

using namespace quarry;

namespace quarry {
struct ColumnBufferTestAccess
{
    static size_t nulls(const ColumnBuffer &c) { return c.m_nulls.capacity(); }
    static size_t fixed(const ColumnBuffer &c) { return c.m_fixed.capacity(); }
    static size_t ends(const ColumnBuffer &c) { return c.m_ends.capacity(); }
    static size_t tags(const ColumnBuffer &c) { return c.m_tags.capacity(); }
    static qsizetype arena(const ColumnBuffer &c) { return c.m_arena.capacity(); }
};
} // namespace quarry
using Access = ColumnBufferTestAccess;

class TstColumnBuffer : public QObject
{
    Q_OBJECT

private slots:
    void typedInt64()
    {
        ColumnBuffer c;
        c.reserve(4);
        c.append(int64_t(1));
        c.append(std::numeric_limits<int64_t>::min());
        c.appendNull();
        c.append(int64_t(-3));
        QCOMPARE(c.size(), 4);
        QVERIFY(!c.isMixed());
        QCOMPARE(c.int64At(0), 1);
        QCOMPARE(c.int64At(1), std::numeric_limits<int64_t>::min());
        QVERIFY(c.isNull(2));
        QVERIFY(!c.isNull(3));
        QCOMPARE(c.int64At(3), -3);
        QCOMPARE(c.typeAt(0), ValueType::Int64);
        QCOMPARE(c.typeAt(2), ValueType::Null);
    }

    void typedDouble()
    {
        ColumnBuffer c;
        c.append(1.5);
        c.append(-0.25);
        QVERIFY(!c.isMixed());
        QCOMPARE(c.doubleAt(0), 1.5);
        QCOMPARE(c.doubleAt(1), -0.25);
        QCOMPARE(c.typeAt(1), ValueType::Double);
    }

    void typedBytesAndEmptyString()
    {
        ColumnBuffer c;
        QVERIFY(c.appendBytes("hello", ValueType::Text));
        c.appendBytes("", ValueType::Text);
        c.appendNull();
        c.appendBytes("wörld", ValueType::Text);
        QVERIFY(!c.isMixed());
        QCOMPARE(c.bytesAt(0), QByteArrayView("hello"));
        QCOMPARE(c.bytesAt(1).size(), 0);
        QVERIFY(!c.isNull(1));
        QVERIFY(c.isNull(2));
        QCOMPARE(c.bytesAt(3), QByteArrayView("wörld"));
        QCOMPARE(c.typeAt(3), ValueType::Text);
    }

    void boolColumn()
    {
        ColumnBuffer c;
        c.appendBool(true);
        c.appendBool(false);
        QVERIFY(!c.isMixed());
        QCOMPARE(c.typeAt(0), ValueType::Bool);
        QCOMPARE(c.int64At(0), 1);
        QCOMPARE(c.int64At(1), 0);
    }

    void leadingNullsThenTyped()
    {
        ColumnBuffer c;
        c.appendNull();
        c.appendNull();
        c.append(int64_t(9));
        QVERIFY(!c.isMixed());
        QVERIFY(c.isNull(0) && c.isNull(1));
        QCOMPARE(c.int64At(2), 9);
        QCOMPARE(c.typeAt(2), ValueType::Int64);
    }

    void nullBitmapAcrossWords()
    {
        ColumnBuffer c;
        for (int i = 0; i < 200; ++i) {
            if (i % 7 == 0)
                c.appendNull();
            else
                c.append(int64_t(i));
        }
        for (int i = 0; i < 200; ++i) {
            QCOMPARE(c.isNull(i), i % 7 == 0);
            if (i % 7 != 0)
                QCOMPARE(c.int64At(i), i);
        }
    }

    void mixedPromotion()
    {
        ColumnBuffer c;
        c.append(int64_t(10));
        c.append(int64_t(20));
        QVERIFY(!c.isMixed());
        c.appendBytes("text", ValueType::Text);
        QVERIFY(c.isMixed());
        c.appendBytes(QByteArray("\x00\x01\xff", 3), ValueType::Bytes);
        c.appendNull();
        c.append(2.5);

        QCOMPARE(c.size(), 6);
        QCOMPARE(c.typeAt(0), ValueType::Int64);
        QCOMPARE(c.int64At(0), 10);
        QCOMPARE(c.typeAt(1), ValueType::Int64);
        QCOMPARE(c.int64At(1), 20);
        QCOMPARE(c.typeAt(2), ValueType::Text);
        QCOMPARE(c.bytesAt(2), QByteArrayView("text"));
        QCOMPARE(c.typeAt(3), ValueType::Bytes);
        QCOMPARE(c.bytesAt(3), QByteArrayView(QByteArray("\x00\x01\xff", 3)));
        QCOMPARE(c.typeAt(4), ValueType::Null);
        QVERIFY(c.isNull(4));
        QCOMPARE(c.typeAt(5), ValueType::Double);
        QCOMPARE(c.doubleAt(5), 2.5);
    }

    void mixedPromotionFromBytesWithNulls()
    {
        ColumnBuffer c;
        c.appendNull();
        c.appendBytes("a", ValueType::Text);
        c.append(int64_t(5));
        QVERIFY(c.isMixed());
        QCOMPARE(c.typeAt(0), ValueType::Null);
        QCOMPARE(c.typeAt(1), ValueType::Text);
        QCOMPARE(c.bytesAt(1), QByteArrayView("a"));
        QCOMPARE(c.typeAt(2), ValueType::Int64);
        QCOMPARE(c.int64At(2), 5);
    }

    void bytesViewIsStableWithoutAppends()
    {
        ColumnBuffer c;
        c.reserve(1000);
        for (int i = 0; i < 1000; ++i)
            c.appendBytes(QByteArray::number(i), ValueType::Text);
        const QByteArrayView v = c.bytesAt(500);
        const char *p = v.data();
        QCOMPARE(c.bytesAt(500).data(), p); // same arena memory, no copy
        QCOMPARE(v, QByteArrayView("500"));
        QCOMPARE(c.bytesAt(999), QByteArrayView("999"));
    }

    void shrinkToFitTypedKeepsValuesAndDropsCapacity()
    {
        ColumnBuffer c;
        c.reserve(10000);
        for (int i = 0; i < 100; ++i) {
            if (i % 10 == 0)
                c.appendNull();
            else
                c.append(int64_t(i));
        }
        QVERIFY(Access::fixed(c) >= 10000);
        c.shrinkToFit();
        QCOMPARE(Access::fixed(c), size_t(100));
        QCOMPARE(Access::nulls(c), size_t(2));
        QCOMPARE(Access::ends(c), size_t(0));
        for (int i = 0; i < 100; ++i) {
            QCOMPARE(c.isNull(i), i % 10 == 0);
            if (i % 10 != 0)
                QCOMPARE(c.int64At(i), i);
        }
    }

    void shrinkToFitBytes()
    {
        ColumnBuffer c;
        c.reserve(5000);
        for (int i = 0; i < 50; ++i)
            c.appendBytes(QByteArray::number(i).repeated(20), ValueType::Text);
        c.appendNull();
        c.shrinkToFit();
        QCOMPARE(Access::ends(c), size_t(51));
        QCOMPARE(Access::arena(c), qsizetype(10 * 20 + 40 * 40)); // 1- and 2-digit ids x20
        for (int i = 0; i < 50; ++i)
            QCOMPARE(c.bytesAt(i), QByteArrayView(QByteArray::number(i).repeated(20)));
        QVERIFY(c.isNull(50));
    }

    void shrinkToFitMixedAndNullOnly()
    {
        ColumnBuffer m;
        m.reserve(1000);
        m.append(int64_t(7));
        m.appendBytes("abc", ValueType::Text);
        m.appendNull();
        m.append(1.25);
        m.shrinkToFit();
        QCOMPARE(Access::tags(m), size_t(4));
        QCOMPARE(Access::fixed(m), size_t(4));
        QCOMPARE(Access::ends(m), size_t(4));
        QCOMPARE(m.int64At(0), 7);
        QCOMPARE(m.bytesAt(1), QByteArrayView("abc"));
        QVERIFY(m.isNull(2));
        QCOMPARE(m.doubleAt(3), 1.25);
        QCOMPARE(m.typeAt(3), ValueType::Double);

        ColumnBuffer n;
        n.reserve(1000);
        n.appendNull();
        n.appendNull();
        n.shrinkToFit();
        QCOMPARE(n.size(), 2);
        QVERIFY(n.isNull(0) && n.isNull(1));
        QCOMPARE(n.typeAt(1), ValueType::Null);
        n.appendNull(); // still appendable after shrinking
        QCOMPARE(n.size(), 3);
    }
};

QTEST_GUILESS_MAIN(TstColumnBuffer)
#include "tst_column_buffer.moc"
