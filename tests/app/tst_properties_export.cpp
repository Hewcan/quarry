#include "ui/properties_export.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

using namespace quarry::ui;
using namespace quarry::ui::properties_export;

namespace {

QVariantMap column(const QString &name, const QString &type, bool nullable, const QString &comment,
                   bool primary = false, bool foreign = false)
{
    return { { QStringLiteral("name"), name },
             { QStringLiteral("type"), type },
             { QStringLiteral("nullable"), nullable },
             { QStringLiteral("description"), comment },
             { QStringLiteral("primaryKey"), primary },
             { QStringLiteral("foreignKeys"),
               foreign ? QVariantList { QStringLiteral("Foreign key to t.id") }
                       : QVariantList() } };
}

QVariantMap sample()
{
    return { { QStringLiteral("name"), QStringLiteral("ge_vehicu") },
             { QStringLiteral("tableType"), QStringLiteral("TABLE") },
             { QStringLiteral("description"), QStringLiteral("Tabla <de> vehiculos") },
             { QStringLiteral("columns"),
               QVariantList { column(QStringLiteral("id"), QStringLiteral("BIGINT"), false,
                                     QStringLiteral("Identificador"), true),
                              column(QStringLiteral("plate"), QStringLiteral("VARCHAR(50)"), true,
                                     QStringLiteral("a, \"b\""), false, false),
                              column(QStringLiteral("owner_id"), QStringLiteral("BIGINT"), false,
                                     QString(), false, true) } } };
}

} // namespace

class TstPropertiesExport : public QObject
{
    Q_OBJECT

private slots:
    void parsesFormats()
    {
        Format f {};
        QVERIFY(parseFormat(QStringLiteral("CSV"), &f));
        QCOMPARE(f, Format::Csv);
        QVERIFY(parseFormat(QStringLiteral("md"), &f));
        QCOMPARE(f, Format::Markdown);
        QVERIFY(!parseFormat(QStringLiteral("xml"), &f));
        QCOMPARE(extensionOf(Format::Html), QStringLiteral("html"));
    }

    void csv()
    {
        const QString out = format(sample(), Format::Csv);
        const QStringList lines = out.split(QStringLiteral("\r\n"));
        QCOMPARE(lines.at(0), QStringLiteral("Field,Type,Comment,Nullability,Key"));
        QCOMPARE(lines.at(1), QStringLiteral("id,BIGINT,Identificador,REQUIRED,PRIMARY"));
        QCOMPARE(lines.at(2), QStringLiteral("plate,VARCHAR(50),\"a, \"\"b\"\"\",NULLABLE,"));
        QCOMPARE(lines.at(3), QStringLiteral("owner_id,BIGINT,,REQUIRED,FOREIGN"));
    }

    void csvNeutralisesFormulas()
    {
        QVariantMap info = sample();
        info[QStringLiteral("columns")] = QVariantList { column(
            QStringLiteral("a"), QStringLiteral("T"), true, QStringLiteral("=1+1")) };
        QVERIFY(format(info, Format::Csv).contains(QStringLiteral(",'=1+1,")));
    }

    void json()
    {
        const QJsonObject o
            = QJsonDocument::fromJson(format(sample(), Format::Json).toUtf8()).object();
        QCOMPARE(o.value(QStringLiteral("table")).toString(), QStringLiteral("ge_vehicu"));
        const QJsonArray cols = o.value(QStringLiteral("columns")).toArray();
        QCOMPARE(cols.size(), 3);
        QVERIFY(cols.at(0).toObject().value(QStringLiteral("primaryKey")).toBool());
        QVERIFY(!cols.at(0).toObject().value(QStringLiteral("nullable")).toBool());
        QVERIFY(cols.at(2).toObject().value(QStringLiteral("foreignKey")).toBool());
    }

    void html()
    {
        const QString out = format(sample(), Format::Html);
        QVERIFY(out.contains(QStringLiteral("<h1>ge_vehicu</h1>")));
        QVERIFY(out.contains(QStringLiteral("<h2>Tabla &lt;de&gt; vehiculos</h2>")));
        QVERIFY(out.contains(QStringLiteral("<tr class=\"prim\">")));
        QVERIFY(out.contains(QStringLiteral("<tr class=\"sec\">")));
        QVERIFY(out.contains(QStringLiteral("<td>a, &quot;b&quot;</td>")));
        QVERIFY(out.contains(QStringLiteral("<td>REQUIRED</td>")));
    }

    void markdown()
    {
        const QString out = format(sample(), Format::Markdown);
        QVERIFY(out.startsWith(QStringLiteral("# ge_vehicu\n")));
        QVERIFY(
            out.contains(QStringLiteral("| id | BIGINT | Identificador | REQUIRED | PRIMARY |")));
    }
};

QTEST_APPLESS_MAIN(TstPropertiesExport)
#include "tst_properties_export.moc"
