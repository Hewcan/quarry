#include "ui/connection_controller.h"
#include "ui/saved_queries_model.h"

#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace quarry;
using namespace quarry::ui;

namespace {

constexpr int kWaitMs = 10000;

} // namespace

// SavedQueriesModel (H4) over the real SavedQueryStore in a temporary quarry.db: the two-level
// list, expanding without a reset, and every change going through the store.
class TstSavedQueriesModel : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    int m_files = 0;

    struct Env
    {
        std::unique_ptr<ConnectionController> controller;
        std::unique_ptr<SavedQueriesModel> model;
        QString connection;
    };

    QString addConnection(ConnectionController &c, const QString &name)
    {
        const QString file = m_dir.filePath(QStringLiteral("data-%1.db").arg(++m_files));
        c.prepare(QString());
        c.form()->setValue(0, file);
        QSignalSpy saved(&c, &ConnectionController::connectionSaved);
        const bool ok = c.save({ { QStringLiteral("id"), QString() },
                                 { QStringLiteral("providerId"), QStringLiteral("sqlite") },
                                 { QStringLiteral("name"), name },
                                 { QStringLiteral("passwordMode"), 1 } });
        if (!ok || !saved.wait(kWaitMs))
            return {};
        return saved.at(0).at(0).toString();
    }

    Env start()
    {
        Env e;
        e.controller = std::make_unique<ConnectionController>();
        e.controller->setDatabasePath(
            m_dir.filePath(QStringLiteral("quarry-%1.db").arg(++m_files)));
        e.controller->setProviderDirectory(QStringLiteral(QUARRY_PLUGIN_DIR));
        e.controller->start();
        [&] { QTRY_VERIFY_WITH_TIMEOUT(!e.controller->model()->loading(), kWaitMs); }();
        e.connection = addConnection(*e.controller, QStringLiteral("alpha"));
        e.model = std::make_unique<SavedQueriesModel>();
        e.model->setDatabasePath(e.controller->databasePath());
        e.model->setConnections(e.controller.get());
        return e;
    }

    static void load(SavedQueriesModel &m)
    {
        m.start();
        [&] { QTRY_VERIFY_WITH_TIMEOUT(!m.loading(), kWaitMs); }();
    }

    // Runs `action`, which changes the store, and waits for the list to be rebuilt.
    template<typename F> static void change(SavedQueriesModel &m, F &&action)
    {
        QSignalSpy changed(&m, &SavedQueriesModel::changed);
        action();
        [&] { QTRY_VERIFY_WITH_TIMEOUT(changed.count() >= 1, kWaitMs); }();
    }

    static QString name(const SavedQueriesModel &m, int row)
    {
        return m.data(m.index(row), SavedQueriesModel::NameRole).toString();
    }
    static QString kind(const SavedQueriesModel &m, int row)
    {
        return m.data(m.index(row), SavedQueriesModel::KindRole).toString();
    }
    static QString id(const SavedQueriesModel &m, int row)
    {
        return m.data(m.index(row), SavedQueriesModel::IdRole).toString();
    }
    static QStringList names(const SavedQueriesModel &m)
    {
        QStringList out;
        for (int i = 0; i < m.count(); ++i)
            out << QStringLiteral("%1%2").arg(
                QString(m.data(m.index(i), SavedQueriesModel::DepthRole).toInt() * qsizetype(2),
                        QLatin1Char(' ')),
                name(m, i));
        return out;
    }

    // A folder "Reports" with two queries and one query at the top level.
    static void fillSample(SavedQueriesModel &m, const QString &connection)
    {
        change(m, [&] { QVERIFY(m.newFolder(QStringLiteral("Reports"))); });
        const QString folder = id(m, 0);
        change(m, [&] {
            m.saveQuery(QStringLiteral("Monthly"), QStringLiteral("SELECT 1"), connection, folder);
        });
        change(m, [&] {
            m.saveQuery(QStringLiteral("Daily"), QStringLiteral("SELECT 2"), {}, folder);
        });
        change(m,
               [&] { m.saveQuery(QStringLiteral("Scratch"), QStringLiteral("SELECT 3"), {}, {}); });
    }

private slots:
    void modelContract()
    {
        Env e = start();
        QAbstractItemModelTester tester(e.model.get(),
                                        QAbstractItemModelTester::FailureReportingMode::Fatal);
        load(*e.model);
        QCOMPARE(e.model->count(), 0);
        fillSample(*e.model, e.connection);
        // The folder opened itself when its first query was saved.
        QCOMPARE(names(*e.model),
                 (QStringList { QStringLiteral("Reports"), QStringLiteral("  Monthly"),
                                QStringLiteral("  Daily"), QStringLiteral("Scratch") }));
        e.model->setExpanded(0, false);
        QCOMPARE(names(*e.model),
                 (QStringList { QStringLiteral("Reports"), QStringLiteral("Scratch") }));
        e.model->setExpanded(0, true);
        QCOMPARE(e.model->count(), 4);
        QCOMPARE(e.model->queryCount(), 3);
        QCOMPARE(e.model->folderCount(), 1);
    }

    // Nothing is read before start().
    void lazy()
    {
        Env e = start();
        QVERIFY(!e.model->started());
        QVERIFY(!e.model->loading());
        e.model->start();
        QVERIFY(e.model->loading());
        QTRY_VERIFY_WITH_TIMEOUT(!e.model->loading(), kWaitMs);
    }

    void rolesAndPositions()
    {
        Env e = start();
        load(*e.model);
        fillSample(*e.model, e.connection);
        const SavedQueriesModel &m = *e.model;
        QCOMPARE(kind(m, 0), QStringLiteral("folder"));
        QVERIFY(m.data(m.index(0), SavedQueriesModel::ExpandedRole).toBool());
        QCOMPARE(m.data(m.index(0), SavedQueriesModel::ChildCountRole).toInt(), 2);
        QCOMPARE(m.data(m.index(0), SavedQueriesModel::PositionRole).toInt(), 1);
        QCOMPARE(m.data(m.index(0), SavedQueriesModel::SiblingCountRole).toInt(),
                 2); // folder + Scratch
        QCOMPARE(kind(m, 1), QStringLiteral("query"));
        QCOMPARE(m.data(m.index(1), SavedQueriesModel::DepthRole).toInt(), 1);
        QCOMPARE(m.data(m.index(1), SavedQueriesModel::ConnectionNameRole).toString(),
                 QStringLiteral("alpha"));
        QCOMPARE(m.data(m.index(1), SavedQueriesModel::ConnectionIdRole).toString(), e.connection);
        QCOMPARE(m.data(m.index(1), SavedQueriesModel::FolderIdRole).toString(), id(m, 0));
        QCOMPARE(m.data(m.index(2), SavedQueriesModel::PositionRole).toInt(), 2);
        QCOMPARE(m.data(m.index(2), SavedQueriesModel::SiblingCountRole).toInt(), 2);
        QCOMPARE(m.data(m.index(2), SavedQueriesModel::ConnectionNameRole).toString(), QString());
        QCOMPARE(m.data(m.index(3), SavedQueriesModel::DepthRole).toInt(), 0);
        QCOMPARE(m.data(m.index(3), SavedQueriesModel::SqlRole).toString(),
                 QStringLiteral("SELECT 3"));
        QCOMPARE(m.get(3).value(QStringLiteral("name")).toString(), QStringLiteral("Scratch"));
        QCOMPARE(m.indexOfId(id(m, 2)), 2);
        QCOMPARE(m.indexOfId(QStringLiteral("nope")), -1);
        QCOMPARE(m.folders().size(), 1);
        QCOMPARE(m.queriesIn(id(m, 0)), 2);
    }

    // Collapsing and expanding insert and remove rows, they never reset the model.
    void expandDoesNotReset()
    {
        Env e = start();
        load(*e.model);
        fillSample(*e.model, e.connection);
        QSignalSpy reset(e.model.get(), &QAbstractItemModel::modelReset);
        QSignalSpy removed(e.model.get(), &QAbstractItemModel::rowsRemoved);
        QSignalSpy inserted(e.model.get(), &QAbstractItemModel::rowsInserted);
        e.model->setExpanded(0, false);
        e.model->setExpanded(0, false); // already collapsed: nothing
        QCOMPARE(removed.count(), 1);
        e.model->setExpanded(0, true);
        QCOMPARE(inserted.count(), 1);
        e.model->setExpanded(3, true); // not a folder
        e.model->setExpanded(99, true);
        QCOMPARE(reset.count(), 0);
        QCOMPARE(e.model->count(), 4);
    }

    void persistsAcrossModels()
    {
        Env e = start();
        load(*e.model);
        fillSample(*e.model, e.connection);
        SavedQueriesModel second;
        second.setDatabasePath(e.controller->databasePath());
        load(second);
        // A model that was not told to expand shows the folder collapsed.
        QCOMPARE(names(second),
                 (QStringList { QStringLiteral("Reports"), QStringLiteral("Scratch") }));
        second.setExpanded(0, true);
        QCOMPARE(name(second, 1), QStringLiteral("Monthly"));
    }

    void renameAndMove()
    {
        Env e = start();
        load(*e.model);
        fillSample(*e.model, e.connection);
        SavedQueriesModel &m = *e.model;
        QSignalSpy said(&m, &SavedQueriesModel::announcement);
        const QString scratch = id(m, 3);

        QVERIFY(!m.renameQuery(scratch, QStringLiteral("   "))); // empty names are refused
        change(m, [&] { QVERIFY(m.renameQuery(scratch, QStringLiteral("Notes"))); });
        QCOMPARE(said.last().at(0).toString(), QStringLiteral("Renamed to “Notes”"));
        QCOMPARE(name(m, 3), QStringLiteral("Notes"));

        // Into the folder, which opens so the query is seen; positions follow.
        m.setExpanded(0, false);
        change(m, [&] { m.moveQuery(scratch, id(m, 0)); });
        QCOMPARE(said.last().at(0).toString(), QStringLiteral("“Notes” moved to folder Reports"));
        QCOMPARE(names(m),
                 (QStringList { QStringLiteral("Reports"), QStringLiteral("  Monthly"),
                                QStringLiteral("  Daily"), QStringLiteral("  Notes") }));
        // And back to the top level.
        change(m, [&] { m.moveQuery(scratch, {}); });
        QCOMPARE(said.last().at(0).toString(), QStringLiteral("“Notes” moved to the top level"));
        QCOMPARE(m.get(m.count() - 1).value(QStringLiteral("name")).toString(),
                 QStringLiteral("Notes"));
        QCOMPARE(m.get(m.count() - 1).value(QStringLiteral("depth")).toInt(), 0);

        change(m, [&] { QVERIFY(m.renameFolder(id(m, 0), QStringLiteral("Reporting"))); });
        QCOMPARE(name(m, 0), QStringLiteral("Reporting"));
        QCOMPARE(said.last().at(0).toString(), QStringLiteral("Folder renamed to “Reporting”"));
    }

    void removeQueryAndFolder()
    {
        Env e = start();
        load(*e.model);
        fillSample(*e.model, e.connection);
        SavedQueriesModel &m = *e.model;
        QSignalSpy said(&m, &SavedQueriesModel::announcement);

        change(m, [&] { m.removeQuery(id(m, 2)); }); // Daily
        QCOMPARE(said.last().at(0).toString(), QStringLiteral("Saved query deleted"));
        QCOMPARE(m.queryCount(), 2);

        // The folder goes, its query stays, at the top level.
        change(m, [&] { m.removeFolder(id(m, 0)); });
        QCOMPARE(said.last().at(0).toString(),
                 QStringLiteral("Folder deleted. 1 query moved to the top level"));
        QCOMPARE(m.folderCount(), 0);
        QCOMPARE(names(m), (QStringList { QStringLiteral("Monthly"), QStringLiteral("Scratch") }));
        QCOMPARE(m.data(m.index(0), SavedQueriesModel::FolderIdRole).toString(), QString());
    }

    // The same query list on a second load: what the store keeps is what the list shows.
    void deletedConnectionClearsTheAssociation()
    {
        Env e = start();
        load(*e.model);
        fillSample(*e.model, e.connection);
        QCOMPARE(e.model->data(e.model->index(1), SavedQueriesModel::ConnectionIdRole).toString(),
                 e.connection);
        change(*e.model, [&] { e.controller->remove(e.connection); });
        QCOMPARE(e.model->queryCount(), 3); // never deleted with the connection
        QCOMPARE(e.model->data(e.model->index(1), SavedQueriesModel::ConnectionIdRole).toString(),
                 QString());
    }

    void failureIsReported()
    {
        Env e = start();
        load(*e.model);
        QSignalSpy failed(e.model.get(), &SavedQueriesModel::failed);
        QSignalSpy said(e.model.get(), &SavedQueriesModel::announcement);
        // A connection that matches nothing is a constraint error in the store.
        QVERIFY(e.model->saveQuery(QStringLiteral("Orphan"), QStringLiteral("SELECT 1"),
                                   QUuid::createUuid().toString(QUuid::WithoutBraces), {}));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, kWaitMs);
        QVERIFY(!failed.at(0).at(0).toString().isEmpty());
        QVERIFY(said.last().at(1).toBool()); // assertive
        QCOMPARE(e.model->queryCount(), 0);
        QVERIFY(!e.model->saveQuery(QStringLiteral(" "), QStringLiteral("SELECT 1"), {}, {}));
    }
};

QTEST_MAIN(TstSavedQueriesModel)
#include "tst_saved_queries_model.moc"
