#include "ui/migration_name.h"

#include <QtTest>

using quarry::ui::shortMigrationName;

class TstMigrationName : public QObject
{
    Q_OBJECT

private slots:
    void dropsTheDatePrefix()
    {
        QCOMPARE(shortMigrationName(QStringLiteral("2024_01_01_000000_create_users_table")),
                 QStringLiteral("000000_create_users_table"));
        QCOMPARE(shortMigrationName(QStringLiteral("2023_12_31_123456_ghost")),
                 QStringLiteral("123456_ghost"));
    }

    void keepsWhatDoesNotStartWithADate()
    {
        QCOMPARE(shortMigrationName(QStringLiteral("create_users_table")),
                 QStringLiteral("create_users_table"));
        QCOMPARE(shortMigrationName(QStringLiteral("24_01_01_000000_short_year")),
                 QStringLiteral("24_01_01_000000_short_year"));
        QCOMPARE(shortMigrationName(QStringLiteral("x2024_01_01_000000_late_date")),
                 QStringLiteral("x2024_01_01_000000_late_date"));
        QCOMPARE(shortMigrationName(QString()), QString());
    }

    void neverReturnsAnEmptyName()
    {
        QCOMPARE(shortMigrationName(QStringLiteral("2024_01_01_")), QStringLiteral("2024_01_01_"));
    }
};

QTEST_APPLESS_MAIN(TstMigrationName)
#include "tst_migration_name.moc"
