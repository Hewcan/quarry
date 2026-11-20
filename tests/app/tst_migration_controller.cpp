#include "migration_controller.h"
#include "migration_list_model.h"

#include <QAbstractItemModelTester>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace quarry;
using namespace quarry::ui;

class TstMigrationController : public QObject
{
    Q_OBJECT

private slots:
    void modelRolesAndTester();
    void kindsStartWithNone();
    void validateWithoutDisk();
    void checkFolder();
    void showUnconfigured();
};

void TstMigrationController::modelRolesAndTester()
{
    MigrationListModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    model.setEntries({ { QStringLiteral("2014_a"), MigrationState::Applied, 2 },
                       { QStringLiteral("2015_b"), MigrationState::Pending, std::nullopt },
                       { QStringLiteral("2016_c"), MigrationState::Orphan, 1 } });
    QCOMPARE(model.rowCount(), 3);
    const QModelIndex applied = model.index(0, 0);
    QCOMPARE(applied.data(MigrationListModel::StateRole).toString(), QStringLiteral("applied"));
    QVERIFY(applied.data(MigrationListModel::StateTextRole).toString().contains(u'2'));
    QCOMPARE(applied.data(MigrationListModel::BatchRole).toInt(), 2);
    QCOMPARE(model.index(1, 0).data(MigrationListModel::BatchRole).toInt(), -1);
    QCOMPARE(model.index(2, 0).data(MigrationListModel::StateRole).toString(),
             QStringLiteral("orphan"));
    // The state is always written, never only colored.
    for (int r = 0; r < 3; ++r)
        QVERIFY(!model.index(r, 0).data(MigrationListModel::StateTextRole).toString().isEmpty());
    model.clear();
    QCOMPARE(model.rowCount(), 0);
}

void TstMigrationController::kindsStartWithNone()
{
    MigrationController c;
    const QVariantList kinds = c.kinds();
    QVERIFY(kinds.size() >= 2);
    QCOMPARE(kinds.at(0).toMap().value(QStringLiteral("id")).toString(), QString());
    QCOMPARE(kinds.at(1).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("laravel"));
    QCOMPARE(kinds.at(1).toMap().value(QStringLiteral("defaultTable")).toString(),
             QStringLiteral("migrations"));
}

void TstMigrationController::validateWithoutDisk()
{
    MigrationController c;
    QCOMPARE(c.validate({}, {}, {}), QString());
    QVERIFY(!c.validate(QStringLiteral("laravel"), {}, {}).isEmpty());
    QVERIFY(!c.validate(QStringLiteral("laravel"), QStringLiteral("rel/path"), {}).isEmpty());
    const QString laravel = QStringLiteral("laravel");
    QVERIFY(!c.validate(laravel, QDir::tempPath(), QStringLiteral("a;b")).isEmpty());
    QCOMPARE(c.validate(QStringLiteral("laravel"), QDir::tempPath(), {}), QString());
}

void TstMigrationController::checkFolder()
{
    QTemporaryDir folder;
    MigrationController c; // the folder check never opens quarry.db
    c.checkFolder(QStringLiteral("laravel"), folder.path(), {});
    QCOMPARE(c.folderState(), QStringLiteral("checking"));
    QTRY_COMPARE(c.folderState(), QStringLiteral("ok"));
    c.checkFolder(QStringLiteral("laravel"),
                  QDir::tempPath() + QStringLiteral("/quarry-missing-migrations-4242"), {});
    QTRY_COMPARE(c.folderState(), QStringLiteral("failed"));
    QVERIFY(!c.folderMessage().isEmpty());
    c.resetFolderCheck();
    QCOMPARE(c.folderState(), QStringLiteral("idle"));
}

void TstMigrationController::showUnconfigured()
{
    MigrationController c;
    c.show(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QCOMPARE(c.state(), QStringLiteral("unconfigured"));
    QVERIFY(!c.message().isEmpty());
}

QTEST_GUILESS_MAIN(TstMigrationController)
#include "tst_migration_controller.moc"
