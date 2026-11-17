#include <quarry/exporter.h>
#include <quarry/provider_registry.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <limits>
#include <sqlite3.h>

using namespace quarry;

namespace {

const QByteArray kUnicode = QStringLiteral(u"ñ \U0001F600 日本").toUtf8();

QByteArray lines(std::initializer_list<QByteArray> rows)
{
    QByteArray out;
    for (const QByteArray &row : rows)
        out += row + '\n';
    return out;
}

ColumnInfo column(const QString &name, ValueType type)
{
    return ColumnInfo { name, type, {}, true, std::nullopt };
}

struct Table
{
    std::vector<ColumnInfo> columns;
    std::shared_ptr<const ResultBatch> batch;
};

// Four rows with every kind of value that needs care in some format.
Table sampleTable()
{
    Table t;
    t.columns = { column(QStringLiteral("id"), ValueType::Int64),
                  column(QStringLiteral("name"), ValueType::Text),
                  column(QStringLiteral("score"), ValueType::Double),
                  column(QStringLiteral("flag"), ValueType::Bool),
                  column(QStringLiteral("blob"), ValueType::Bytes),
                  column(QStringLiteral("note"), ValueType::Text),
                  column(QStringLiteral("amount"), ValueType::Decimal),
                  column(QStringLiteral("doc"), ValueType::Json) };
    ResultBatch b;
    b.rowCount = 4;
    b.columns.resize(8);
    b.columns[0].append(int64_t(1));
    b.columns[0].append(int64_t(-2));
    b.columns[0].append(int64_t(3));
    b.columns[0].appendNull();

    for (const QByteArray &v :
         { QByteArray("plain"), QByteArray("has,comma"), QByteArray("quote\"inside"), kUnicode })
        b.columns[1].appendBytes(v, ValueType::Text);

    b.columns[2].append(1.5);
    b.columns[2].append(-0.25);
    b.columns[2].append(0.1);
    b.columns[2].append(std::numeric_limits<double>::quiet_NaN());

    b.columns[3].appendBool(true);
    b.columns[3].appendBool(false);
    b.columns[3].appendBool(true);
    b.columns[3].appendNull();

    const QByteArray raw("\x00\xff"
                         "a",
                         3);
    b.columns[4].appendBytes(raw, ValueType::Bytes);
    b.columns[4].appendBytes(QByteArrayView(), ValueType::Bytes);
    b.columns[4].appendBytes(QByteArrayView("hi"), ValueType::Bytes);
    b.columns[4].appendNull();

    b.columns[5].appendNull();
    b.columns[5].appendBytes(QByteArrayView("line1\nline2"), ValueType::Text);
    b.columns[5].appendBytes(QByteArrayView("tab\there"), ValueType::Text);
    b.columns[5].appendBytes(QByteArrayView("pipe|here"), ValueType::Text);

    for (const char *v : { "12.50", "abc", "1e3", "007" })
        b.columns[6].appendBytes(QByteArrayView(v), ValueType::Decimal);

    for (const char *v : { "{\"a\": 1}", "not json", "[1, 2]", "  " })
        b.columns[7].appendBytes(QByteArrayView(v), ValueType::Json);

    t.batch = std::make_shared<const ResultBatch>(std::move(b));
    return t;
}

// Watches the four outcome signals of one Exporter.
struct Watch
{
    explicit Watch(Exporter &exporter)
        : finished(&exporter, &Exporter::finished)
        , failed(&exporter, &Exporter::failed)
        , cancelled(&exporter, &Exporter::cancelled)
        , progress(&exporter, &Exporter::progress)
    { }

    int outcomes() const { return finished.count() + failed.count() + cancelled.count(); }

    bool wait(int timeoutMs = 60000)
    {
        QElapsedTimer timer;
        timer.start();
        while (outcomes() == 0 && timer.elapsed() < timeoutMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(2);
        }
        if (failed.count() > 0)
            qWarning() << "export failed:" << error().message << error().detail;
        return outcomes() > 0;
    }

    ExportStats stats() const { return finished.at(0).at(0).value<ExportStats>(); }
    Error error() const { return failed.at(0).at(0).value<Error>(); }

    QSignalSpy finished;
    QSignalSpy failed;
    QSignalSpy cancelled;
    QSignalSpy progress;
};

QByteArray readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

QStringList leftovers(const QString &dir)
{
    return QDir(dir).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
}

ConnectionProfile sqliteProfile()
{
    ConnectionProfile p;
    p.id = QUuid::createUuid();
    p.name = QStringLiteral("test");
    p.providerId = QStringLiteral("sqlite");
    p.values = { { QStringLiteral("file"), QStringLiteral(":memory:") } };
    return p;
}

constexpr auto kHugeSql = "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < "
                          "50000000) SELECT x, 'abcdefghij' || x AS t FROM c";

} // namespace

class TstExporter : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void csvDefault();
    void csvOptions();
    void csvMultiByteDelimiter();
    void tsvEscapes();
    void jsonArray();
    void jsonLines();
    void jsonNumbersAndStrings();
    void jsonColumnType();
    void jsonDecimal();
    void duplicateColumnNames();
    void sqlInsertAnsi();
    void sqlInsertDialectRoundTrip();
    void markdownTable();
    void emptyResult();
    void mixedColumn();
    void multipleBatches();
    void statsAndProgress();
    void fromSqlite();
    void sessionErrorPassesThrough();
    void existingTargetIsReplaced();
    void missingFolder();
    void destinationIsFolder();
    void readOnlyTarget();
    void cancelWithBatches();
    void cancelStreamingKeepsOldFile();
    void destroyWhileRunning();
    void destroyWithoutStart();
    void cancelWhenIdleOrFinished();

private:
    bool exportTable(const ExportOptions &options, const Table &table, QByteArray *out)
    {
        Exporter exporter(options);
        Watch watch(exporter);
        exporter.startBatches(table.columns, { table.batch });
        if (!watch.wait() || watch.finished.count() != 1) {
            qWarning() << "export did not finish:" << watch.failed.count()
                       << (watch.failed.count() ? watch.error().message : QString());
            return false;
        }
        *out = readAll(options.filePath);
        return true;
    }

    std::unique_ptr<Session> openSession()
    {
        auto connection = m_registry.createConnection(sqliteProfile(), {});
        if (!connection)
            return nullptr;
        auto session = std::make_unique<Session>(std::move(connection).value());
        QSignalSpy opened(session.get(), &Session::opened);
        session->open();
        if (!opened.wait(15000))
            return nullptr;
        return session;
    }

    ExportOptions optionsFor(ExportFormat format, const QString &name = QStringLiteral("out"))
    {
        ExportOptions o;
        o.format = format;
        o.filePath = m_dir.filePath(name);
        return o;
    }

    ProviderRegistry m_registry;
    QTemporaryDir m_dir;
    const ISqlDialect *m_dialect = nullptr;
};

void TstExporter::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_registry.scan(QStringLiteral(QUARRY_PLUGIN_DIR));
    QString error;
    IDatabaseProvider *provider = m_registry.provider(QStringLiteral("sqlite"), &error);
    QVERIFY2(provider, qPrintable(error));
    m_dialect = &provider->dialect();
}

void TstExporter::csvDefault()
{
    QByteArray got;
    QVERIFY(exportTable(optionsFor(ExportFormat::Csv), sampleTable(), &got));
    const QByteArray expected = lines({
        "id,name,score,flag,blob,note,amount,doc",
        "1,plain,1.5,true,AP9h,,12.50,\"{\"\"a\"\": 1}\"",
        "-2,\"has,comma\",-0.25,false,,\"line1\nline2\",abc,not json",
        "3,\"quote\"\"inside\",0.1,true,aGk=,tab\there,1e3,\"[1, 2]\"",
        "," + kUnicode + ",NaN,,,pipe|here,007,  ",
    });
    QCOMPARE(got, expected);
}

void TstExporter::csvOptions()
{
    ExportOptions o = optionsFor(ExportFormat::Csv);
    o.delimiter = QLatin1Char(';');
    o.header = false;
    o.bom = true;
    QByteArray got;
    QVERIFY(exportTable(o, sampleTable(), &got));
    QVERIFY(got.startsWith("\xEF\xBB\xBF"
                           "1;plain;"));
    QVERIFY(!got.contains("id;name"));
    // The comma is an ordinary character now, the semicolon never appears unquoted in a value.
    QVERIFY(got.contains("-2;has,comma;"));

    ExportOptions q = optionsFor(ExportFormat::Csv);
    q.delimiter = QLatin1Char('|');
    QVERIFY(exportTable(q, sampleTable(), &got));
    QVERIFY(got.contains("\"pipe|here\""));
    QVERIFY(got.startsWith("id|name|"));
    QVERIFY(!got.startsWith("\xEF\xBB\xBF"));
}

void TstExporter::csvMultiByteDelimiter()
{
    ExportOptions o = optionsFor(ExportFormat::Csv);
    o.delimiter = QChar(0x00A7); // section sign, two bytes in UTF-8
    QByteArray got;
    ResultBatch b;
    b.rowCount = 2;
    b.columns.resize(2);
    b.columns[0].appendBytes(QByteArrayView("plain"), ValueType::Text);
    b.columns[0].appendBytes(QByteArrayView("a\xC2\xA7"
                                            "b"),
                             ValueType::Text);
    b.columns[1].append(int64_t(1));
    b.columns[1].append(int64_t(2));
    Table t { { column(QStringLiteral("a"), ValueType::Text),
                column(QStringLiteral("b"), ValueType::Int64) },
              std::make_shared<const ResultBatch>(std::move(b)) };
    QVERIFY(exportTable(o, t, &got));
    QCOMPARE(got,
             QByteArray("a\xC2\xA7"
                        "b\nplain\xC2\xA7"
                        "1\n\"a\xC2\xA7"
                        "b\"\xC2\xA7"
                        "2\n"));
}

void TstExporter::tsvEscapes()
{
    QByteArray got;
    ExportOptions o = optionsFor(ExportFormat::Tsv);
    o.delimiter = QLatin1Char(';'); // ignored
    QVERIFY(exportTable(o, sampleTable(), &got));
    const QByteArray expected = lines({
        "id\tname\tscore\tflag\tblob\tnote\tamount\tdoc",
        "1\tplain\t1.5\ttrue\tAP9h\t\t12.50\t{\"a\": 1}",
        "-2\thas,comma\t-0.25\tfalse\t\tline1\\nline2\tabc\tnot json",
        "3\tquote\"inside\t0.1\ttrue\taGk=\ttab\\there\t1e3\t[1, 2]",
        "\t" + kUnicode + "\tNaN\t\t\tpipe|here\t007\t  ",
    });
    QCOMPARE(got, expected);

    ResultBatch b;
    b.rowCount = 1;
    b.columns.resize(1);
    b.columns[0].appendBytes(QByteArrayView("a\\b\r\nc"), ValueType::Text);
    Table t { { column(QStringLiteral("x"), ValueType::Text) },
              std::make_shared<const ResultBatch>(std::move(b)) };
    ExportOptions n = optionsFor(ExportFormat::Tsv);
    n.header = false;
    QVERIFY(exportTable(n, t, &got));
    QCOMPARE(got, QByteArray("a\\\\b\\r\\nc\n"));
}

void TstExporter::jsonLines()
{
    QByteArray got;
    QVERIFY(exportTable(optionsFor(ExportFormat::JsonLines), sampleTable(), &got));
    const QByteArray expected = lines({
        "{\"id\":1,\"name\":\"plain\",\"score\":1.5,\"flag\":true,\"blob\":\"AP9h\",\"note\":null,"
        "\"amount\":12.50,\"doc\":{\"a\":1}}",
        "{\"id\":-2,\"name\":\"has,comma\",\"score\":-0.25,\"flag\":false,\"blob\":\"\","
        "\"note\":\"line1\\nline2\",\"amount\":\"abc\",\"doc\":\"not json\"}",
        "{\"id\":3,\"name\":\"quote\\\"inside\",\"score\":0.1,\"flag\":true,\"blob\":\"aGk=\","
        "\"note\":\"tab\\there\",\"amount\":1e3,\"doc\":[1,2]}",
        "{\"id\":null,\"name\":\"" + kUnicode
            + "\",\"score\":null,\"flag\":null,\"blob\":null,\"note\":\"pipe|here\","
              "\"amount\":\"007\",\"doc\":\"  \"}",
    });
    QCOMPARE(got, expected);
    for (const QByteArray &line : got.split('\n')) {
        if (line.isEmpty())
            continue;
        QJsonParseError error;
        QVERIFY2(QJsonDocument::fromJson(line, &error).isObject(), qPrintable(error.errorString()));
    }
}

void TstExporter::jsonArray()
{
    QByteArray got;
    QVERIFY(exportTable(optionsFor(ExportFormat::Json), sampleTable(), &got));
    QVERIFY(got.startsWith("[\n{\"id\":1,"));
    QVERIFY(got.endsWith("}\n]\n"));
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(got, &error);
    QVERIFY2(doc.isArray(), qPrintable(error.errorString()));
    const QJsonArray rows = doc.array();
    QCOMPARE(rows.size(), 4);
    const QJsonObject r0 = rows.at(0).toObject();
    QCOMPARE(r0.value(QLatin1String("id")).toInt(), 1);
    QCOMPARE(r0.value(QLatin1String("score")).toDouble(), 1.5);
    QCOMPARE(r0.value(QLatin1String("flag")).toBool(), true);
    QVERIFY(r0.value(QLatin1String("note")).isNull());
    QCOMPARE(r0.value(QLatin1String("blob")).toString(), QStringLiteral("AP9h"));
    QCOMPARE(r0.value(QLatin1String("doc")).toObject().value(QLatin1String("a")).toInt(), 1);
    const QJsonObject r1 = rows.at(1).toObject();
    QCOMPARE(r1.value(QLatin1String("note")).toString(), QStringLiteral("line1\nline2"));
    QCOMPARE(r1.value(QLatin1String("amount")).toString(), QStringLiteral("abc"));
    const QJsonObject r3 = rows.at(3).toObject();
    QCOMPARE(r3.value(QLatin1String("name")).toString(), QString::fromUtf8(kUnicode));
    QVERIFY(r3.value(QLatin1String("score")).isNull()); // NaN
    QVERIFY(r3.value(QLatin1String("id")).isNull());
}

void TstExporter::jsonNumbersAndStrings()
{
    ResultBatch b;
    b.rowCount = 5;
    b.columns.resize(3);
    for (const int64_t v :
         { std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max(), int64_t(0),
           int64_t(42), int64_t(-7) })
        b.columns[0].append(v);
    for (const double v : { 0.1 + 0.2, -0.0, std::numeric_limits<double>::infinity(),
                            -std::numeric_limits<double>::infinity(), 1e20 })
        b.columns[1].append(v);
    for (const char *v :
         { "quote\" back\\slash", "ctl\x01\x1f\b\f\r", "\xC3\xB1\xE6\x97\xA5", "slash/", "" })
        b.columns[2].appendBytes(QByteArrayView(v), ValueType::Text);
    Table t { { column(QStringLiteral("i"), ValueType::Int64),
                column(QStringLiteral("d"), ValueType::Double),
                column(QStringLiteral("s"), ValueType::Text) },
              std::make_shared<const ResultBatch>(std::move(b)) };
    QByteArray got;
    QVERIFY(exportTable(optionsFor(ExportFormat::JsonLines), t, &got));
    const QList<QByteArray> rows = got.split('\n');
    QCOMPARE(rows.at(0),
             QByteArray("{\"i\":-9223372036854775808,\"d\":0.30000000000000004,"
                        "\"s\":\"quote\\\" back\\\\slash\"}"));
    QCOMPARE(rows.at(1),
             QByteArray("{\"i\":9223372036854775807,\"d\":-0,"
                        "\"s\":\"ctl\\u0001\\u001f\\b\\f\\r\"}"));
    QCOMPARE(rows.at(2), QByteArray("{\"i\":0,\"d\":null,\"s\":\"\xC3\xB1\xE6\x97\xA5\"}"));
    QCOMPARE(rows.at(3), QByteArray("{\"i\":42,\"d\":null,\"s\":\"slash/\"}"));
    QVERIFY(rows.at(4).startsWith("{\"i\":-7,\"d\":1e"));
    for (const QByteArray &line : rows) {
        if (!line.isEmpty())
            QVERIFY(QJsonDocument::fromJson(line).isObject());
    }
}

void TstExporter::jsonColumnType()
{
    ResultBatch b;
    const QList<QByteArray> docs = { "{\"a\" : [1, 2 ,\n {\"b\":null}] , \"s\":\"x\\\"y\"}",
                                     "true",
                                     "\"str\"",
                                     "-1.5e+3",
                                     "{\"a\":}",
                                     "[1,",
                                     "",
                                     "{\"a\":1} trailing",
                                     "\"bad \\q escape\"",
                                     "\"raw\ttab\"",
                                     "[]" };
    b.rowCount = docs.size();
    b.columns.resize(1);
    for (const QByteArray &d : docs)
        b.columns[0].appendBytes(d, ValueType::Json);
    Table t { { column(QStringLiteral("doc"), ValueType::Json) },
              std::make_shared<const ResultBatch>(std::move(b)) };
    QByteArray got;
    QVERIFY(exportTable(optionsFor(ExportFormat::JsonLines), t, &got));
    const QList<QByteArray> rows = got.split('\n');
    QCOMPARE(rows.at(0), QByteArray("{\"doc\":{\"a\":[1,2,{\"b\":null}],\"s\":\"x\\\"y\"}}"));
    QCOMPARE(rows.at(1), QByteArray("{\"doc\":true}"));
    QCOMPARE(rows.at(2), QByteArray("{\"doc\":\"str\"}"));
    QCOMPARE(rows.at(3), QByteArray("{\"doc\":-1.5e+3}"));
    QCOMPARE(rows.at(4), QByteArray("{\"doc\":\"{\\\"a\\\":}\"}"));
    QCOMPARE(rows.at(5), QByteArray("{\"doc\":\"[1,\"}"));
    QCOMPARE(rows.at(6), QByteArray("{\"doc\":\"\"}"));
    QCOMPARE(rows.at(7), QByteArray("{\"doc\":\"{\\\"a\\\":1} trailing\"}"));
    QCOMPARE(rows.at(8), QByteArray("{\"doc\":\"\\\"bad \\\\q escape\\\"\"}"));
    QCOMPARE(rows.at(9), QByteArray("{\"doc\":\"\\\"raw\\ttab\\\"\"}"));
    QCOMPARE(rows.at(10), QByteArray("{\"doc\":[]}"));
}

void TstExporter::jsonDecimal()
{
    ResultBatch b;
    const QList<QByteArray> values = { "1",  "-0.5", "1E+5", "0",  "01", "+1",
                                       "1.", ".5",   "NaN",  "1e", "-",  "12345678901234567890.5" };
    b.rowCount = values.size();
    b.columns.resize(1);
    for (const QByteArray &v : values)
        b.columns[0].appendBytes(v, ValueType::Decimal);
    Table t { { column(QStringLiteral("n"), ValueType::Decimal) },
              std::make_shared<const ResultBatch>(std::move(b)) };
    QByteArray got;
    QVERIFY(exportTable(optionsFor(ExportFormat::JsonLines), t, &got));
    const QList<QByteArray> rows = got.split('\n');
    const QList<QByteArray> expected
        = { "1",      "-0.5",   "1E+5",    "0",      "\"01\"", "\"+1\"",
            "\"1.\"", "\".5\"", "\"NaN\"", "\"1e\"", "\"-\"",  "12345678901234567890.5" };
    for (int i = 0; i < expected.size(); ++i)
        QCOMPARE(rows.at(i), "{\"n\":" + expected.at(i) + "}");
}

void TstExporter::duplicateColumnNames()
{
    ResultBatch b;
    b.rowCount = 1;
    b.columns.resize(5);
    for (int c = 0; c < 5; ++c)
        b.columns[static_cast<size_t>(c)].append(int64_t(c));
    Table t { { column(QStringLiteral("a"), ValueType::Int64),
                column(QStringLiteral("a"), ValueType::Int64),
                column(QStringLiteral("a"), ValueType::Int64),
                column(QStringLiteral("a_2"), ValueType::Int64),
                column(QStringLiteral("b"), ValueType::Int64) },
              std::make_shared<const ResultBatch>(std::move(b)) };
    QByteArray got;
    QVERIFY(exportTable(optionsFor(ExportFormat::JsonLines), t, &got));
    QCOMPARE(got, QByteArray("{\"a\":0,\"a_2\":1,\"a_3\":2,\"a_2_2\":3,\"b\":4}\n"));
    const QJsonObject o = QJsonDocument::fromJson(got).object();
    QCOMPARE(o.size(), 5);
    // CSV keeps the names as they are.
    QVERIFY(exportTable(optionsFor(ExportFormat::Csv), t, &got));
    QVERIFY(got.startsWith("a,a,a,a_2,b\n"));
}

void TstExporter::sqlInsertAnsi()
{
    ExportOptions o = optionsFor(ExportFormat::SqlInsert);
    QByteArray got;
    QVERIFY(exportTable(o, sampleTable(), &got));
    const QByteArray prefix = "INSERT INTO \"export_table\" (\"id\", \"name\", \"score\", "
                              "\"flag\", \"blob\", \"note\", \"amount\", \"doc\") VALUES ";
    const QByteArray expected = lines({
        prefix + "(1, 'plain', 1.5, TRUE, X'00ff61', NULL, 12.50, '{\"a\": 1}');",
        prefix + "(-2, 'has,comma', -0.25, FALSE, X'', 'line1\nline2', 'abc', 'not json');",
        prefix + "(3, 'quote\"inside', 0.1, TRUE, X'6869', 'tab\there', 1e3, '[1, 2]');",
        prefix + "(NULL, '" + kUnicode + "', NULL, NULL, NULL, 'pipe|here', '007', '  ');",
    });
    QCOMPARE(got, expected);

    ResultBatch b;
    b.rowCount = 1;
    b.columns.resize(1);
    b.columns[0].appendBytes(QByteArrayView("it's"), ValueType::Text);
    Table t { { column(QStringLiteral("we\"ird"), ValueType::Text) },
              std::make_shared<const ResultBatch>(std::move(b)) };
    o.tableName = QStringLiteral("my\"t");
    QVERIFY(exportTable(o, t, &got));
    QCOMPARE(got, QByteArray("INSERT INTO \"my\"\"t\" (\"we\"\"ird\") VALUES ('it''s');\n"));
}

void TstExporter::sqlInsertDialectRoundTrip()
{
    ResultBatch b;
    b.rowCount = 7;
    b.columns.resize(3);
    for (int i = 1; i <= 7; ++i) {
        b.columns[0].append(int64_t(i));
        b.columns[1].appendBytes(QByteArray("it's #") + QByteArray::number(i), ValueType::Text);
        if (i == 3)
            b.columns[2].appendBytes(QByteArray("\x00\x01\xfe", 3), ValueType::Bytes);
        else
            b.columns[2].appendNull();
    }
    Table t { { column(QStringLiteral("id"), ValueType::Int64),
                column(QStringLiteral("na me"), ValueType::Text),
                column(QStringLiteral("data"), ValueType::Bytes) },
              std::make_shared<const ResultBatch>(std::move(b)) };
    ExportOptions o = optionsFor(ExportFormat::SqlInsert);
    o.dialect = m_dialect;
    o.tableName = QStringLiteral("my \"t\"");
    o.rowsPerInsert = 3;
    QByteArray got;
    QVERIFY(exportTable(o, t, &got));
    QCOMPARE(got.count(";\n"), 3); // 3 + 3 + 1 rows
    QCOMPARE(got.count("INSERT INTO"), 3);
    const QString table = m_dialect->quoteIdentifier(o.tableName);
    QVERIFY(got.startsWith("INSERT INTO " + table.toUtf8() + " ("));

    sqlite3 *db = nullptr;
    QVERIFY(sqlite3_open(":memory:", &db) == SQLITE_OK);
    const QByteArray create = "CREATE TABLE " + table.toUtf8() + " (id INTEGER, "
        + m_dialect->quoteIdentifier(QStringLiteral("na me")).toUtf8() + " TEXT, data BLOB);";
    QVERIFY(sqlite3_exec(db, create.constData(), nullptr, nullptr, nullptr) == SQLITE_OK);
    char *message = nullptr;
    QVERIFY2(sqlite3_exec(db, got.constData(), nullptr, nullptr, &message) == SQLITE_OK,
             message ? message : "");
    sqlite3_stmt *stmt = nullptr;
    const QByteArray query = "SELECT count(*), sum(id), (SELECT hex(data) FROM " + table.toUtf8()
        + " WHERE id = 3), (SELECT " + m_dialect->quoteIdentifier(QStringLiteral("na me")).toUtf8()
        + " FROM " + table.toUtf8() + " WHERE id = 5) FROM " + table.toUtf8();
    QVERIFY(sqlite3_prepare_v2(db, query.constData(), -1, &stmt, nullptr) == SQLITE_OK);
    QVERIFY(sqlite3_step(stmt) == SQLITE_ROW);
    QCOMPARE(sqlite3_column_int(stmt, 0), 7);
    QCOMPARE(sqlite3_column_int(stmt, 1), 28);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2))),
             QByteArray("0001FE"));
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3))),
             QByteArray("it's #5"));
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

void TstExporter::markdownTable()
{
    QByteArray got;
    QVERIFY(exportTable(optionsFor(ExportFormat::Markdown), sampleTable(), &got));
    const QByteArray expected = lines({
        "| id | name | score | flag | blob | note | amount | doc |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
        "| 1 | plain | 1.5 | true | AP9h |  | 12.50 | {\"a\": 1} |",
        "| -2 | has,comma | -0.25 | false |  | line1<br>line2 | abc | not json |",
        "| 3 | quote\"inside | 0.1 | true | aGk= | tab\there | 1e3 | [1, 2] |",
        "|  | " + kUnicode + " | NaN |  |  | pipe\\|here | 007 |    |",
    });
    QCOMPARE(got, expected);

    ResultBatch b;
    b.rowCount = 1;
    b.columns.resize(1);
    b.columns[0].appendBytes(QByteArrayView("a\r\nb\rc|d"), ValueType::Text);
    Table t { { column(QStringLiteral("x|y"), ValueType::Text) },
              std::make_shared<const ResultBatch>(std::move(b)) };
    QVERIFY(exportTable(optionsFor(ExportFormat::Markdown), t, &got));
    QCOMPARE(got, lines({ "| x\\|y |", "| --- |", "| a<br>b<br>c\\|d |" }));
    ExportOptions n = optionsFor(ExportFormat::Markdown);
    n.header = false;
    QVERIFY(exportTable(n, t, &got));
    QCOMPARE(got, lines({ "|  |", "| --- |", "| a<br>b<br>c\\|d |" }));
}

void TstExporter::emptyResult()
{
    Table t;
    t.columns = { column(QStringLiteral("a"), ValueType::Int64),
                  column(QStringLiteral("b"), ValueType::Text) };
    t.batch = std::make_shared<const ResultBatch>(ResultBatch { 0, std::vector<ColumnBuffer>(2) });
    QByteArray got;
    QVERIFY(exportTable(optionsFor(ExportFormat::Csv), t, &got));
    QCOMPARE(got, QByteArray("a,b\n"));
    ExportOptions n = optionsFor(ExportFormat::Csv);
    n.header = false;
    QVERIFY(exportTable(n, t, &got));
    QVERIFY(QFileInfo::exists(n.filePath));
    QCOMPARE(got, QByteArray());
    QVERIFY(exportTable(optionsFor(ExportFormat::Tsv), t, &got));
    QCOMPARE(got, QByteArray("a\tb\n"));
    QVERIFY(exportTable(optionsFor(ExportFormat::Json), t, &got));
    QCOMPARE(got, QByteArray("[]\n"));
    QVERIFY(exportTable(optionsFor(ExportFormat::JsonLines), t, &got));
    QCOMPARE(got, QByteArray());
    QVERIFY(exportTable(optionsFor(ExportFormat::SqlInsert), t, &got));
    QCOMPARE(got, QByteArray());
    QVERIFY(exportTable(optionsFor(ExportFormat::Markdown), t, &got));
    QCOMPARE(got, lines({ "| a | b |", "| --- | --- |" }));

    // No batches at all behaves the same.
    Exporter exporter(optionsFor(ExportFormat::Json));
    Watch watch(exporter);
    exporter.startBatches(t.columns, {});
    QVERIFY(watch.wait());
    QCOMPARE(watch.finished.count(), 1);
    QCOMPARE(watch.stats().rows, qint64(0));
    QCOMPARE(readAll(m_dir.filePath(QStringLiteral("out"))), QByteArray("[]\n"));
}

void TstExporter::mixedColumn()
{
    ResultBatch b;
    b.rowCount = 5;
    b.columns.resize(1);
    b.columns[0].append(int64_t(7));
    b.columns[0].appendBytes(QByteArrayView("text"), ValueType::Text);
    b.columns[0].append(2.5);
    b.columns[0].appendNull();
    b.columns[0].appendBool(true);
    QVERIFY(b.columns[0].isMixed());
    Table t { { column(QStringLiteral("m"), ValueType::Other) },
              std::make_shared<const ResultBatch>(std::move(b)) };
    QByteArray got;
    QVERIFY(exportTable(optionsFor(ExportFormat::JsonLines), t, &got));
    QCOMPARE(
        got,
        lines({ "{\"m\":7}", "{\"m\":\"text\"}", "{\"m\":2.5}", "{\"m\":null}", "{\"m\":true}" }));
    ExportOptions o = optionsFor(ExportFormat::Csv);
    o.header = false;
    QVERIFY(exportTable(o, t, &got));
    QCOMPARE(got, lines({ "7", "text", "2.5", "", "true" }));
    o = optionsFor(ExportFormat::SqlInsert);
    QVERIFY(exportTable(o, t, &got));
    QVERIFY(got.contains("(7);"));
    QVERIFY(got.contains("('text');"));
    QVERIFY(got.contains("(NULL);"));
    QVERIFY(got.contains("(TRUE);"));
}

void TstExporter::multipleBatches()
{
    const std::vector<ColumnInfo> columns = { column(QStringLiteral("n"), ValueType::Int64) };
    std::vector<std::shared_ptr<const ResultBatch>> batches;
    int next = 0;
    for (const int size : { 3, 0, 4, 2 }) {
        ResultBatch b;
        b.rowCount = size;
        b.columns.resize(1);
        for (int i = 0; i < size; ++i)
            b.columns[0].append(int64_t(next++));
        batches.push_back(std::make_shared<const ResultBatch>(std::move(b)));
    }
    batches.push_back(nullptr);

    ExportOptions o = optionsFor(ExportFormat::Json);
    Exporter exporter(o);
    Watch watch(exporter);
    exporter.startBatches(columns, batches);
    QVERIFY(watch.wait());
    QCOMPARE(watch.finished.count(), 1);
    const QJsonArray rows = QJsonDocument::fromJson(readAll(o.filePath)).array();
    QCOMPARE(rows.size(), 9);
    for (int i = 0; i < 9; ++i)
        QCOMPARE(rows.at(i).toObject().value(QLatin1String("n")).toInt(), i);

    // INSERT statements continue across batches: 9 rows, 4 per statement.
    ExportOptions s = optionsFor(ExportFormat::SqlInsert, QStringLiteral("multi.sql"));
    s.rowsPerInsert = 4;
    Exporter sqlExporter(s);
    Watch sqlWatch(sqlExporter);
    sqlExporter.startBatches(columns, batches);
    QVERIFY(sqlWatch.wait());
    const QByteArray sql = readAll(s.filePath);
    QCOMPARE(sql.count("INSERT INTO"), 3);
    QVERIFY(sql.contains("(0), (1), (2), (3);\n"));
    QVERIFY(sql.endsWith("(8);\n"));
}

void TstExporter::statsAndProgress()
{
    const std::vector<ColumnInfo> columns = { column(QStringLiteral("n"), ValueType::Int64) };
    std::vector<std::shared_ptr<const ResultBatch>> batches;
    for (int k = 0; k < 20; ++k) {
        ResultBatch b;
        b.rowCount = 1000;
        b.columns.resize(1);
        for (int i = 0; i < 1000; ++i)
            b.columns[0].append(int64_t(k) * 1000 + i);
        batches.push_back(std::make_shared<const ResultBatch>(std::move(b)));
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ExportOptions o = optionsFor(ExportFormat::Csv);
    o.filePath = dir.filePath(QStringLiteral("stats.csv"));
    Exporter exporter(o);
    Watch watch(exporter);
    exporter.startBatches(columns, batches);
    QVERIFY(watch.wait());
    QCOMPARE(watch.finished.count(), 1);
    const ExportStats stats = watch.stats();
    QCOMPARE(stats.rows, qint64(20000));
    QCOMPARE(stats.filePath, o.filePath);
    QCOMPARE(stats.bytes, QFileInfo(o.filePath).size());
    QVERIFY(stats.elapsedMs >= 0);
    QVERIFY(watch.progress.count() >= 1);
    QCOMPARE(watch.progress.last().at(0).toLongLong(), qint64(20000));
    // Throttled: nowhere near one signal per batch plus the final one.
    QVERIFY(watch.progress.count() <= 21);
    QCOMPARE(leftovers(dir.path()), QStringList { QStringLiteral("stats.csv") });
}

void TstExporter::fromSqlite()
{
    auto session = openSession();
    QVERIFY(session);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ExportOptions o = optionsFor(ExportFormat::Csv);
    o.filePath = dir.filePath(QStringLiteral("rows.csv"));
    Exporter exporter(o);
    Watch watch(exporter);
    exporter.start(std::move(session),
                   Statement { QStringLiteral("WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT "
                                              "x+1 FROM c WHERE x < 12345) SELECT x, 'r' || x AS "
                                              "label, x * 0.5 AS half, NULL AS nada, x'00ff' AS "
                                              "bytes FROM c"),
                               {},
                               500 }); // the Exporter overrides the fetch size
    QVERIFY(watch.wait());
    QCOMPARE(watch.finished.count(), 1);
    QCOMPARE(watch.stats().rows, qint64(12345));
    const QByteArray got = readAll(o.filePath);
    const QList<QByteArray> rows = got.split('\n');
    QCOMPARE(rows.size(), 12345 + 2); // header, rows, empty tail
    QCOMPARE(rows.at(0), QByteArray("x,label,half,nada,bytes"));
    QCOMPARE(rows.at(1), QByteArray("1,r1,0.5,,AP8="));
    QCOMPARE(rows.at(12345), QByteArray("12345,r12345,6172.5,,AP8="));
    QCOMPARE(leftovers(dir.path()), QStringList { QStringLiteral("rows.csv") });
}

void TstExporter::sessionErrorPassesThrough()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile old(dir.filePath(QStringLiteral("keep.csv")));
    QVERIFY(old.open(QIODevice::WriteOnly));
    old.write("OLD");
    old.close();
    auto session = openSession();
    QVERIFY(session);
    ExportOptions o = optionsFor(ExportFormat::Csv);
    o.filePath = dir.filePath(QStringLiteral("keep.csv"));
    Exporter exporter(o);
    Watch watch(exporter);
    exporter.start(std::move(session), Statement { QStringLiteral("SELEC 1"), {}, 500 });
    QVERIFY(watch.wait());
    QCOMPARE(watch.failed.count(), 1);
    QCOMPARE(watch.error().kind, ErrorKind::Syntax);
    QCOMPARE(readAll(o.filePath), QByteArray("OLD"));
    QCOMPARE(leftovers(dir.path()), QStringList { QStringLiteral("keep.csv") });
}

void TstExporter::existingTargetIsReplaced()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ExportOptions o = optionsFor(ExportFormat::Csv);
    o.filePath = dir.filePath(QStringLiteral("replace.csv"));
    QFile old(o.filePath);
    QVERIFY(old.open(QIODevice::WriteOnly));
    old.write("OLD CONTENT THAT IS LONGER THAN THE NEW ONE ........................");
    old.close();
    ResultBatch b;
    b.rowCount = 1;
    b.columns.resize(1);
    b.columns[0].append(int64_t(1));
    Table t { { column(QStringLiteral("a"), ValueType::Int64) },
              std::make_shared<const ResultBatch>(std::move(b)) };
    QByteArray got;
    QVERIFY(exportTable(o, t, &got));
    QCOMPARE(got, QByteArray("a\n1\n"));
    QCOMPARE(leftovers(dir.path()), QStringList { QStringLiteral("replace.csv") });
}

void TstExporter::missingFolder()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ExportOptions o = optionsFor(ExportFormat::Csv);
    o.filePath = dir.filePath(QStringLiteral("nope/deeper/out.csv"));
    Exporter exporter(o);
    Watch watch(exporter);
    exporter.startBatches({ column(QStringLiteral("a"), ValueType::Int64) }, {});
    QVERIFY(watch.wait());
    QCOMPARE(watch.failed.count(), 1);
    const Error error = watch.error();
    QCOMPARE(error.kind, ErrorKind::Internal);
    QVERIFY(!error.message.isEmpty());
    QVERIFY(!error.detail.isEmpty());
    QVERIFY(!QDir(dir.filePath(QStringLiteral("nope"))).exists());
    QVERIFY(leftovers(dir.path()).isEmpty());
}

void TstExporter::destinationIsFolder()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("target")));
    ExportOptions o = optionsFor(ExportFormat::Csv);
    o.filePath = dir.filePath(QStringLiteral("target"));
    Exporter exporter(o);
    Watch watch(exporter);
    exporter.startBatches({ column(QStringLiteral("a"), ValueType::Int64) }, {});
    QVERIFY(watch.wait());
    QCOMPARE(watch.failed.count(), 1);
    QVERIFY(QFileInfo(o.filePath).isDir());
}

void TstExporter::readOnlyTarget()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ExportOptions o = optionsFor(ExportFormat::Csv);
    o.filePath = dir.filePath(QStringLiteral("locked.csv"));
    QFile locked(o.filePath);
    QVERIFY(locked.open(QIODevice::WriteOnly));
    locked.write("OLD");
    locked.close();
    QVERIFY(locked.setPermissions(QFileDevice::ReadOwner));
    Exporter exporter(o);
    Watch watch(exporter);
    exporter.startBatches({ column(QStringLiteral("a"), ValueType::Int64) }, {});
    QVERIFY(watch.wait());
    locked.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (watch.finished.count() == 1)
        QSKIP("This platform or user may replace read-only files (e.g. running as root).");
    QCOMPARE(watch.failed.count(), 1);
    QVERIFY(!watch.error().message.isEmpty());
    QCOMPARE(watch.error().kind, ErrorKind::Permission);
    QCOMPARE(readAll(o.filePath), QByteArray("OLD"));
    QCOMPARE(leftovers(dir.path()), QStringList { QStringLiteral("locked.csv") });
}

void TstExporter::cancelWithBatches()
{
    const std::vector<ColumnInfo> columns = { column(QStringLiteral("n"), ValueType::Int64) };
    std::vector<std::shared_ptr<const ResultBatch>> batches;
    for (int k = 0; k < 200; ++k) {
        ResultBatch b;
        b.rowCount = 1000;
        b.columns.resize(1);
        for (int i = 0; i < 1000; ++i)
            b.columns[0].append(int64_t(i));
        batches.push_back(std::make_shared<const ResultBatch>(std::move(b)));
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ExportOptions o = optionsFor(ExportFormat::Csv);
    o.filePath = dir.filePath(QStringLiteral("c.csv"));
    Exporter exporter(o);
    Watch watch(exporter);
    exporter.startBatches(columns, batches);
    exporter.cancel();
    exporter.cancel(); // second call is harmless
    QVERIFY(watch.wait());
    QCOMPARE(watch.cancelled.count(), 1);
    QCOMPARE(watch.finished.count(), 0);
    QCOMPARE(watch.failed.count(), 0);
    QVERIFY(leftovers(dir.path()).isEmpty());
}

void TstExporter::cancelStreamingKeepsOldFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ExportOptions o = optionsFor(ExportFormat::Csv);
    o.filePath = dir.filePath(QStringLiteral("big.csv"));
    QFile old(o.filePath);
    QVERIFY(old.open(QIODevice::WriteOnly));
    old.write("PREVIOUS");
    old.close();

    auto session = openSession();
    QVERIFY(session);
    Exporter exporter(o);
    Watch watch(exporter);
    exporter.start(std::move(session), Statement { QString::fromLatin1(kHugeSql), {}, 500 });
    QTRY_VERIFY_WITH_TIMEOUT(watch.progress.count() >= 1, 60000);
    // The temporary sibling exists while it runs and the target is still the old one.
    QCOMPARE(readAll(o.filePath), QByteArray("PREVIOUS"));
    QCOMPARE(leftovers(dir.path()).size(), 2);
    QElapsedTimer timer;
    timer.start();
    exporter.cancel();
    QVERIFY(watch.wait(30000));
    QCOMPARE(watch.cancelled.count(), 1);
    QCOMPARE(watch.finished.count(), 0);
    QVERIFY2(timer.elapsed() < 10000, "cancel took too long");
    QCOMPARE(readAll(o.filePath), QByteArray("PREVIOUS"));
    QCOMPARE(leftovers(dir.path()), QStringList { QStringLiteral("big.csv") });
}

void TstExporter::destroyWhileRunning()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ExportOptions o = optionsFor(ExportFormat::Csv);
    o.filePath = dir.filePath(QStringLiteral("gone.csv"));
    auto session = openSession();
    QVERIFY(session);
    auto exporter = std::make_unique<Exporter>(o);
    QSignalSpy progress(exporter.get(), &Exporter::progress);
    exporter->start(std::move(session), Statement { QString::fromLatin1(kHugeSql), {}, 500 });
    QTRY_VERIFY_WITH_TIMEOUT(progress.count() >= 1, 60000);
    QElapsedTimer timer;
    timer.start();
    exporter.reset();
    QVERIFY2(timer.elapsed() < 10000, "destructor took too long");
    QVERIFY(leftovers(dir.path()).isEmpty());
}

void TstExporter::destroyWithoutStart()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ExportOptions o = optionsFor(ExportFormat::Csv);
    o.filePath = dir.filePath(QStringLiteral("never.csv"));
    {
        Exporter exporter(o);
    }
    {
        Exporter exporter(o);
        exporter.cancel(); // idle: no signal, no crash
    }
    QVERIFY(leftovers(dir.path()).isEmpty());
}

void TstExporter::cancelWhenIdleOrFinished()
{
    Exporter idle(optionsFor(ExportFormat::Csv));
    Watch idleWatch(idle);
    idle.cancel();
    QTest::qWait(100);
    QCOMPARE(idleWatch.outcomes(), 0);

    Table t = sampleTable();
    Exporter done(optionsFor(ExportFormat::Csv, QStringLiteral("done.csv")));
    Watch doneWatch(done);
    done.startBatches(t.columns, { t.batch });
    QVERIFY(doneWatch.wait());
    QCOMPARE(doneWatch.finished.count(), 1);
    done.cancel();
    QTest::qWait(100);
    QCOMPARE(doneWatch.outcomes(), 1);
    QVERIFY(QFileInfo::exists(m_dir.filePath(QStringLiteral("done.csv"))));
}

QTEST_MAIN(TstExporter)
#include "tst_exporter.moc"
