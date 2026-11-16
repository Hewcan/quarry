#include "ui/connection_controller.h"
#include "ui/history_model.h"

#include <quarry/query_history_store.h>

#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

#include <memory>

using namespace quarry;
using namespace quarry::ui;

namespace {

constexpr int kWaitMs = 10000;

} // namespace

// HistoryModel (H4) over the real QueryHistoryStore in a temporary quarry.db: paging, the
// filters, stale answers, deleting and clearing, and the entries that arrive while it is listed.
class TstHistoryModel : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    int m_files = 0;

    struct Env
    {
        std::unique_ptr<ConnectionController> controller;
        std::unique_ptr<HistoryModel> history;
        QString a; // two connections that record history
        QString b;
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
        e.a = addConnection(*e.controller, QStringLiteral("alpha"));
        e.b = addConnection(*e.controller, QStringLiteral("beta"));
        e.history = std::make_unique<HistoryModel>();
        e.history->setDatabasePath(e.controller->databasePath());
        e.history->setConnections(e.controller.get());
        e.history->setDebounce(30);
        return e;
    }

    // Stores `n` entries, one second apart, and waits until the store has them.
    static void fill(HistoryModel &h, const QString &connection, int n, const QString &sqlPrefix,
                     bool success = true, int firstSecond = 0)
    {
        QSignalSpy recorded(h.store(), &QueryHistoryStore::recorded);
        const QDateTime base = QDateTime::fromSecsSinceEpoch(1'700'000'000, QTimeZone::utc());
        for (int i = 0; i < n; ++i) {
            HistoryEntry entry;
            entry.connectionId = QUuid::fromString(connection);
            entry.sql = QStringLiteral("%1 %2").arg(sqlPrefix).arg(i);
            entry.executedAt = base.addSecs(firstSecond + i);
            entry.durationMs = 5 + i;
            entry.rows = success ? i : -1;
            entry.success = success;
            if (!success)
                entry.errorMessage = QStringLiteral("It failed.");
            h.record(entry);
        }
        [&] { QTRY_COMPARE_WITH_TIMEOUT(recorded.count(), n, kWaitMs); }();
    }

    static void refreshAndWait(HistoryModel &h)
    {
        QSignalSpy finished(&h, &HistoryModel::searchFinished);
        h.refresh();
        [&] { QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, kWaitMs); }();
    }

    static QString sqlAt(const HistoryModel &h, int row)
    {
        return h.data(h.index(row), HistoryModel::SqlRole).toString();
    }

private slots:
    void modelContract()
    {
        Env e = start();
        QAbstractItemModelTester tester(e.history.get(),
                                        QAbstractItemModelTester::FailureReportingMode::Fatal);
        fill(*e.history, e.a, 3, QStringLiteral("SELECT"));
        refreshAndWait(*e.history);
        QCOMPARE(e.history->count(), 3);
        // Newest first.
        QCOMPARE(sqlAt(*e.history, 0), QStringLiteral("SELECT 2"));
        QCOMPARE(sqlAt(*e.history, 2), QStringLiteral("SELECT 0"));
        const QModelIndex first = e.history->index(0);
        QCOMPARE(first.data(HistoryModel::ConnectionNameRole).toString(), QStringLiteral("alpha"));
        QCOMPARE(first.data(HistoryModel::PreviewRole).toString(), QStringLiteral("SELECT 2"));
        QVERIFY(first.data(HistoryModel::SuccessRole).toBool());
        QCOMPARE(first.data(HistoryModel::DetailRole).toString(), QStringLiteral("7 ms · 2 rows"));
        QVERIFY(!first.data(HistoryModel::WhenRole).toString().isEmpty());
        QCOMPARE(e.history->get(1).value(QStringLiteral("sql")).toString(),
                 QStringLiteral("SELECT 1"));
        e.history->remove(0);
        QTRY_COMPARE_WITH_TIMEOUT(e.history->count(), 2, kWaitMs);
    }

    // Nothing is read, and no thread is started, before the panel asks.
    void nothingBeforeStart()
    {
        Env e = start();
        QVERIFY(!e.history->started());
        QCOMPARE(e.history->count(), 0);
        QVERIFY(!e.history->loading());
    }

    void pagesAndLoadsMore()
    {
        Env e = start();
        fill(*e.history, e.a, 110, QStringLiteral("SELECT"));
        QSignalSpy said(e.history.get(), &HistoryModel::announcement);
        refreshAndWait(*e.history);
        QCOMPARE(e.history->count(), HistoryModel::kPageSize);
        QVERIFY(e.history->hasMore());
        QCOMPARE(said.last().at(0).toString(), QStringLiteral("100 history entries"));
        QSignalSpy finished(e.history.get(), &HistoryModel::searchFinished);
        e.history->loadMore();
        QVERIFY(e.history->loading());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, kWaitMs);
        QCOMPARE(e.history->count(), 110);
        QVERIFY(!e.history->hasMore());
        QCOMPARE(sqlAt(*e.history, 109), QStringLiteral("SELECT 0")); // the oldest, last
        e.history->loadMore(); // nothing more: no search
        QVERIFY(!e.history->loading());
    }

    void filters()
    {
        Env e = start();
        fill(*e.history, e.a, 3, QStringLiteral("SELECT alpha"), true, 0);
        fill(*e.history, e.b, 2, QStringLiteral("SELECT bravo"), true, 10);
        fill(*e.history, e.a, 2, QStringLiteral("DELETE broken"), false, 20);
        refreshAndWait(*e.history);
        QCOMPARE(e.history->count(), 7);

        QSignalSpy finished(e.history.get(), &HistoryModel::searchFinished);
        e.history->setConnectionFilter(e.b);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, kWaitMs);
        QCOMPARE(e.history->count(), 2);
        e.history->setConnectionFilter({});
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, kWaitMs);

        e.history->setOnlyErrors(true);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, kWaitMs);
        QCOMPARE(e.history->count(), 2);
        QVERIFY(!e.history->data(e.history->index(0), HistoryModel::SuccessRole).toBool());
        QCOMPARE(e.history->data(e.history->index(0), HistoryModel::ErrorMessageRole).toString(),
                 QStringLiteral("It failed."));
        e.history->setOnlyErrors(false);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 4, kWaitMs);

        // Text: debounced, every word, the last as a prefix.
        e.history->setText(QStringLiteral("select brav"));
        QCOMPARE(finished.count(), 4); // not yet: the debounce is running
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 5, kWaitMs);
        QCOMPARE(e.history->count(), 2);
        QVERIFY(sqlAt(*e.history, 0).startsWith(QStringLiteral("SELECT bravo")));
        QSignalSpy said(e.history.get(), &HistoryModel::announcement);
        e.history->setText(QStringLiteral("nothing like this"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 6, kWaitMs);
        QCOMPARE(e.history->count(), 0);
        QCOMPARE(said.last().at(0).toString(), QStringLiteral("No history entries"));
    }

    // Typing quickly searches once: the debounce merges the keystrokes.
    void typingIsDebounced()
    {
        Env e = start();
        fill(*e.history, e.a, 3, QStringLiteral("SELECT alpha"));
        refreshAndWait(*e.history);
        QSignalSpy finished(e.history.get(), &HistoryModel::searchFinished);
        e.history->setText(QStringLiteral("s"));
        e.history->setText(QStringLiteral("se"));
        e.history->setText(QStringLiteral("sel"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, kWaitMs);
        QTest::qWait(150);
        QCOMPARE(finished.count(), 1);
    }

    // An answer to a request that was replaced must not reach the list (requestId).
    void staleAnswersAreDropped()
    {
        Env e = start();
        fill(*e.history, e.a, 4, QStringLiteral("SELECT ok"), true, 0);
        fill(*e.history, e.a, 2, QStringLiteral("SELECT bad"), false, 10);
        refreshAndWait(*e.history);
        QCOMPARE(e.history->count(), 6);

        QSignalSpy finished(e.history.get(), &HistoryModel::searchFinished);
        QSignalSpy reset(e.history.get(), &QAbstractItemModel::modelReset);
        e.history->refresh(); // request N, all entries
        e.history->setOnlyErrors(true); // request N + 1 replaces it before N is answered
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, kWaitMs);
        QTest::qWait(200); // the answer to N, if it were taken, would come now
        QCOMPARE(finished.count(), 1);
        QCOMPARE(reset.count(), 1);
        QCOMPARE(e.history->count(), 2);
        QVERIFY(!e.history->loading());
    }

    void entriesArriveWhileListed()
    {
        Env e = start();
        refreshAndWait(*e.history);
        QCOMPARE(e.history->count(), 0);
        QSignalSpy inserted(e.history.get(), &QAbstractItemModel::rowsInserted);
        fill(*e.history, e.a, 1, QStringLiteral("SELECT first"));
        QTRY_COMPARE_WITH_TIMEOUT(e.history->count(), 1, kWaitMs);
        QCOMPARE(inserted.count(), 1);
        // With "only errors" a success does not belong to the list; a failure does.
        e.history->setOnlyErrors(true);
        QTRY_VERIFY_WITH_TIMEOUT(e.history->count() == 0 && !e.history->loading(), kWaitMs);
        fill(*e.history, e.a, 1, QStringLiteral("SELECT fine"), true, 100);
        QCOMPARE(e.history->count(), 0);
        fill(*e.history, e.a, 1, QStringLiteral("SELECT bad"), false, 200);
        QTRY_COMPARE_WITH_TIMEOUT(e.history->count(), 1, kWaitMs);
    }

    void removeAndClear()
    {
        Env e = start();
        fill(*e.history, e.a, 3, QStringLiteral("SELECT alpha"), true, 0);
        fill(*e.history, e.b, 2, QStringLiteral("SELECT bravo"), true, 10);
        refreshAndWait(*e.history);
        QCOMPARE(e.history->count(), 5);

        QSignalSpy said(e.history.get(), &HistoryModel::announcement);
        e.history->remove(0);
        QTRY_COMPARE_WITH_TIMEOUT(e.history->count(), 4, kWaitMs);
        QCOMPARE(said.last().at(0).toString(), QStringLiteral("History entry deleted"));

        // Clear with a connection filter clears that connection only.
        QSignalSpy finished(e.history.get(), &HistoryModel::searchFinished);
        QSignalSpy cleared(e.history.get(), &HistoryModel::cleared);
        e.history->setConnectionFilter(e.a);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, kWaitMs);
        e.history->clear();
        QTRY_COMPARE_WITH_TIMEOUT(cleared.count(), 1, kWaitMs);
        QCOMPARE(cleared.at(0).at(0).toString(), e.a);
        QCOMPARE(cleared.at(0).at(1).toLongLong(), 3);
        QTRY_COMPARE_WITH_TIMEOUT(e.history->count(), 0, kWaitMs);
        QCOMPARE(said.last().at(0).toString(), QStringLiteral("No history entries"));
        e.history->setConnectionFilter({});
        QTRY_COMPARE_WITH_TIMEOUT(e.history->count(), 1, kWaitMs); // beta's last one is still there

        // No filter: everything.
        e.history->clear();
        QTRY_COMPARE_WITH_TIMEOUT(cleared.count(), 2, kWaitMs);
        QCOMPARE(cleared.at(1).at(0).toString(), QString());
        QTRY_COMPARE_WITH_TIMEOUT(e.history->count(), 0, kWaitMs);
    }

    // The privacy question: clearing one connection, whatever the filter shows.
    void clearConnectionIgnoresTheFilter()
    {
        Env e = start();
        fill(*e.history, e.a, 2, QStringLiteral("SELECT alpha"), true, 0);
        fill(*e.history, e.b, 2, QStringLiteral("SELECT bravo"), true, 10);
        refreshAndWait(*e.history);
        QSignalSpy cleared(e.history.get(), &HistoryModel::cleared);
        e.history->clearConnection(e.b);
        QTRY_COMPARE_WITH_TIMEOUT(cleared.count(), 1, kWaitMs);
        QCOMPARE(cleared.at(0).at(0).toString(), e.b);
        QTRY_COMPARE_WITH_TIMEOUT(e.history->count(), 2, kWaitMs);
        QCOMPARE(e.history->data(e.history->index(0), HistoryModel::ConnectionIdRole).toString(),
                 e.a);
    }

    // Deleting a connection deletes its history (core's cascade); the list follows.
    void deletedConnectionLeavesTheList()
    {
        Env e = start();
        fill(*e.history, e.a, 2, QStringLiteral("SELECT alpha"));
        fill(*e.history, e.b, 2, QStringLiteral("SELECT bravo"), true, 10);
        refreshAndWait(*e.history);
        QCOMPARE(e.history->count(), 4);
        e.controller->remove(e.a);
        QTRY_COMPARE_WITH_TIMEOUT(e.history->count(), 2, kWaitMs);
    }
};

QTEST_MAIN(TstHistoryModel)
#include "tst_history_model.moc"
