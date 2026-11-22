#include "ai_controller.h"

#include <QDir>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace quarry;
using namespace quarry::ui;

namespace {

// Puts an environment variable back when the test ends.
class EnvGuard
{
public:
    EnvGuard(const char *name, const QByteArray &value)
        : m_name(name)
        , m_old(qgetenv(name))
        , m_wasSet(qEnvironmentVariableIsSet(name))
    {
        qputenv(name, value);
    }
    EnvGuard(const EnvGuard &) = delete;
    EnvGuard &operator=(const EnvGuard &) = delete;
    ~EnvGuard()
    {
        if (m_wasSet)
            qputenv(m_name, m_old);
        else
            qunsetenv(m_name);
    }

private:
    const char *m_name;
    QByteArray m_old;
    bool m_wasSet;
};

} // namespace

class TstAiController : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QTemporaryDir m_prefs;
    int m_files = 0;

    QString dbPath() const { return m_dir.filePath(QStringLiteral("quarry.db")); }
    QString fakeClaude() const
    {
        return QDir::toNativeSeparators(QStringLiteral(QUARRY_FAKE_CLAUDE));
    }

    void startConnections(ConnectionController &c)
    {
        c.setDatabasePath(dbPath());
        c.setProviderDirectory(QStringLiteral(QUARRY_PLUGIN_DIR));
        c.start();
        QTRY_VERIFY_WITH_TIMEOUT(!c.model()->loading(), 10000);
    }

    // A SQLite connection in the temporary quarry.db; returns its id. No password: no keychain.
    QString saveConnection(ConnectionController &c, const QString &name, bool production)
    {
        const QString file = m_dir.filePath(QStringLiteral("data-%1.db").arg(++m_files));
        c.prepare({});
        c.form()->setValue(0, file);
        QSignalSpy saved(&c, &ConnectionController::connectionSaved);
        const QVariantMap draft { { QStringLiteral("id"), QString() },
                                  { QStringLiteral("providerId"), QStringLiteral("sqlite") },
                                  { QStringLiteral("name"), name },
                                  { QStringLiteral("colorIndex"), 2 },
                                  { QStringLiteral("production"), production },
                                  { QStringLiteral("passwordMode"), 0 },
                                  { QStringLiteral("file"), file } };
        if (!c.save(draft))
            return {};
        saved.wait(10000);
        return saved.isEmpty() ? QString() : saved.at(0).at(0).toString();
    }

private slots:
    void initTestCase()
    {
        // Never the real preferences or the real quarry.db.
        QCoreApplication::setOrganizationName(QStringLiteral("QuarryTest"));
        QCoreApplication::setApplicationName(QStringLiteral("tst_ai_controller"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_prefs.path());
        QStandardPaths::setTestModeEnabled(true);
    }

    void init()
    {
        QSettings().remove(QStringLiteral("ui/layout/ai"));
        QFile::remove(dbPath());
        QFile::remove(dbPath() + QStringLiteral("-wal"));
        QFile::remove(dbPath() + QStringLiteral("-shm"));
    }

    void nothingIsCreatedUntilUsed();
    void cliPathIsKeptInThePreferences();
    void testCliFindsTheFakeAndReadsItsVersion();
    void testCliSaysWhenTheVersionIsTooOld();
    void testCliFailsInPlainLanguage();
    void aTypedPathThatIsNotAFileIsNotFound();
    void anEmptyPathThatFindsNothingIsNotFound();
    void cancellingATestDropsItsAnswer();
    void settingsDefaultToOff();
    void settingsRoundTripWithTheRealDatabase();
    void turningOnAProductionConnectionNeedsTheConfirmation();
    void anUnknownConnectionIsRefused();
    void removingTheConnectionForgetsItsSettings();
    void optionsMapToTheContextOptions();
};

void TstAiController::nothingIsCreatedUntilUsed()
{
    ConnectionController connections;
    connections.setDatabasePath(dbPath());
    AiController c;
    c.setConnections(&connections);
    QVERIFY(c.runnerIfCreated() == nullptr);
    QVERIFY(c.serviceIfCreated() == nullptr);
    QCOMPARE(c.cliState(), QStringLiteral("unknown"));
    QVERIFY(!c.settingsReady());

    c.settingsOf(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QVERIFY(c.serviceIfCreated() != nullptr);
    QVERIFY(c.runnerIfCreated() == nullptr); // reading settings never starts anything

    c.setCliPath(fakeClaude());
    QVERIFY(c.runnerIfCreated() == nullptr); // nor does setting the path
    c.testCli();
    QVERIFY(c.runnerIfCreated() != nullptr);
    c.cancelCli();
}

void TstAiController::cliPathIsKeptInThePreferences()
{
    {
        AiController c;
        QCOMPARE(c.cliPath(), QString());
        QSignalSpy changed(&c, &AiController::cliPathChanged);
        c.setCliPath(QStringLiteral("  /opt/claude/bin/claude  "));
        QCOMPARE(c.cliPath(), QStringLiteral("/opt/claude/bin/claude"));
        QCOMPARE(changed.size(), 1);
        c.setCliPath(QStringLiteral("/opt/claude/bin/claude"));
        QCOMPARE(changed.size(), 1); // same value: nothing
    }
    // The UI preferences, next to LayoutStore's keys: not quarry.db.
    QCOMPARE(QSettings().value(QStringLiteral("ui/layout/ai/cliPath")).toString(),
             QStringLiteral("/opt/claude/bin/claude"));
    AiController again;
    QCOMPARE(again.cliPath(), QStringLiteral("/opt/claude/bin/claude"));
    again.setCliPath({});
    QVERIFY(!QSettings().contains(QStringLiteral("ui/layout/ai/cliPath")));
}

void TstAiController::testCliFindsTheFakeAndReadsItsVersion()
{
    const EnvGuard mode("FAKE_CLAUDE_MODE", "ok");
    AiController c;
    c.setCliPath(fakeClaude());
    QSignalSpy changed(&c, &AiController::cliChanged);
    c.testCli();
    QCOMPARE(c.cliState(), QStringLiteral("checking"));
    QVERIFY(!c.cliMessage().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(c.cliState(), QStringLiteral("ok"), 15000);
    QVERIFY(c.cliVersion().startsWith(QStringLiteral("2.0.14")));
    QCOMPARE(QDir::fromNativeSeparators(c.cliLocation()), QDir::fromNativeSeparators(fakeClaude()));
    QVERIFY(c.cliMessage().contains(QStringLiteral("2.0.14")));
    QVERIFY(c.cliMessage().contains(c.cliLocation()));
    QVERIFY(c.cliDetail().isEmpty());
    QVERIFY(changed.size() >= 2);
}

void TstAiController::testCliSaysWhenTheVersionIsTooOld()
{
    const EnvGuard mode("FAKE_CLAUDE_MODE", "old");
    AiController c;
    c.setCliPath(fakeClaude());
    c.testCli();
    QTRY_COMPARE_WITH_TIMEOUT(c.cliState(), QStringLiteral("incompatible"), 15000);
    QVERIFY(c.cliMessage().contains(QStringLiteral("1.0.3")));
    QVERIFY(c.cliMessage().contains(QStringLiteral("update Claude Code")));
}

void TstAiController::testCliFailsInPlainLanguage()
{
    for (const char *mode : { "corrupt", "exit3" }) {
        const EnvGuard guard("FAKE_CLAUDE_MODE", mode);
        AiController c;
        c.setCliPath(fakeClaude());
        c.testCli();
        QTRY_VERIFY_WITH_TIMEOUT(c.cliState() != QStringLiteral("checking"), 15000);
        QVERIFY2(c.cliState() == QStringLiteral("failed")
                     || c.cliState() == QStringLiteral("incompatible"),
                 mode);
        QVERIFY(!c.cliMessage().isEmpty());
        QVERIFY2(!c.cliDetail().isEmpty(), mode); // the raw text, to copy
    }
}

void TstAiController::aTypedPathThatIsNotAFileIsNotFound()
{
    AiController c;
    c.setCliPath(m_dir.filePath(QStringLiteral("no-such-claude")));
    c.testCli();
    // At once, and without starting anything: the runner would have searched the usual places.
    QCOMPARE(c.cliState(), QStringLiteral("notFound"));
    QVERIFY(c.cliMessage().contains(QStringLiteral("no-such-claude")));
    QVERIFY(c.runnerIfCreated() == nullptr);
}

void TstAiController::anEmptyPathThatFindsNothingIsNotFound()
{
    // A home folder without Claude Code and no login shell: the search ends empty-handed. The real
    // `claude` of the machine is never started.
    QTemporaryDir home;
    const QByteArray h = home.path().toUtf8();
    const EnvGuard a("HOME", h);
    const EnvGuard b("USERPROFILE", h);
    const EnvGuard l("LOCALAPPDATA", h + "/local");
    const EnvGuard p("APPDATA", h + "/roaming");
    const EnvGuard s("SHELL", {});
    AiController c;
    c.testCli();
    QTRY_COMPARE_WITH_TIMEOUT(c.cliState(), QStringLiteral("notFound"), 15000);
    QVERIFY(c.cliMessage().contains(QStringLiteral("Install Claude Code first")));
    QVERIFY(
        !c.cliMessage().contains(QStringLiteral("http"))); // no URL is documented: none is invented
}

void TstAiController::cancellingATestDropsItsAnswer()
{
    const EnvGuard mode("FAKE_CLAUDE_MODE", "ok");
    AiController c;
    c.setCliPath(fakeClaude());
    c.testCli();
    QCOMPARE(c.cliState(), QStringLiteral("checking"));
    c.cancelCli();
    QCOMPARE(c.cliState(), QStringLiteral("unknown"));
    QTest::qWait(300); // whatever was on its way
    QCOMPARE(c.cliState(), QStringLiteral("unknown"));
}

void TstAiController::settingsDefaultToOff()
{
    ConnectionController connections;
    connections.setDatabasePath(dbPath());
    AiController c;
    c.setConnections(&connections);
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QVariantMap s = c.settingsOf(id);
    QCOMPARE(s.value(QStringLiteral("enabled")).toBool(), false);
    QCOMPARE(s.value(QStringLiteral("sendComments")).toBool(), false);
    QCOMPARE(s.value(QStringLiteral("sendDefaults")).toBool(), false);
    QCOMPARE(s.value(QStringLiteral("sendChecks")).toBool(), false);
    QCOMPARE(s.value(QStringLiteral("sendEnumValues")).toBool(), false);
    QCOMPARE(s.value(QStringLiteral("sendDefinitions")).toBool(), false);
    QCOMPARE(s.value(QStringLiteral("maskLiterals")).toBool(), true);
    QCOMPARE(s.value(QStringLiteral("maskNumbers")).toBool(), false);
    QCOMPARE(s.value(QStringLiteral("maxTables")).toInt(), 40);
    QVERIFY(s.value(QStringLiteral("excludedSchemas")).toStringList().isEmpty());
    QVERIFY(!c.isEnabled(id));
    QTRY_VERIFY_WITH_TIMEOUT(c.settingsReady(), 10000); // the lazy read of quarry.db ended
}

void TstAiController::settingsRoundTripWithTheRealDatabase()
{
    ConnectionController connections;
    startConnections(connections);
    const QString id = saveConnection(connections, QStringLiteral("Dev"), false);
    QVERIFY(!id.isEmpty());

    AiController c;
    c.setConnections(&connections);
    QVERIFY(!c.isEnabled(id));
    QTRY_VERIFY_WITH_TIMEOUT(c.settingsReady(), 10000);

    QSignalSpy changed(&c, &AiController::settingsChanged);
    const QVariantMap in { { QStringLiteral("enabled"), true },
                           { QStringLiteral("excludedSchemas"),
                             QStringLiteral("audit, internal\nlogs") },
                           { QStringLiteral("excludedTables"),
                             QStringList { QStringLiteral("users"), QStringLiteral("USERS"),
                                           QStringLiteral(" billing.cards ") } },
                           { QStringLiteral("sendComments"), true },
                           { QStringLiteral("maskNumbers"), true },
                           { QStringLiteral("maxTables"), 500 } };
    QVERIFY(c.saveSettings(id, in));
    QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 10000);
    QCOMPARE(changed.last().at(0).toString(), id);
    QVERIFY(c.isEnabled(id));
    const QVariantMap stored = c.settingsOf(id);
    QCOMPARE(stored.value(QStringLiteral("excludedSchemas")).toStringList(),
             (QStringList { QStringLiteral("audit"), QStringLiteral("internal"),
                            QStringLiteral("logs") }));
    QCOMPARE(stored.value(QStringLiteral("excludedTables")).toStringList(),
             (QStringList { QStringLiteral("users"),
                            QStringLiteral("billing.cards") })); // de-duplicated
    QCOMPARE(stored.value(QStringLiteral("maxTables")).toInt(), 200); // clamped in core
    QCOMPARE(stored.value(QStringLiteral("sendComments")).toBool(), true);
    QCOMPARE(stored.value(QStringLiteral("sendDefaults")).toBool(), false);
    QCOMPARE(stored.value(QStringLiteral("maskLiterals")).toBool(), true);
    QCOMPARE(stored.value(QStringLiteral("maskNumbers")).toBool(), true);

    // A second controller reads it from quarry.db (a different object, the same file).
    AiController other;
    other.setConnections(&connections);
    other.ensureSettings();
    QTRY_VERIFY_WITH_TIMEOUT(other.settingsReady(), 10000);
    QVERIFY(other.isEnabled(id));
    QCOMPARE(other.settingsOf(id).value(QStringLiteral("maxTables")).toInt(), 200);

    // Back to the defaults: the row is deleted and the connection reads as off.
    QVERIFY(c.saveSettings(id, { { QStringLiteral("enabled"), false } }));
    QTRY_VERIFY_WITH_TIMEOUT(!c.isEnabled(id), 10000);
    QCOMPARE(c.settingsOf(id).value(QStringLiteral("maxTables")).toInt(), 40);
}

void TstAiController::turningOnAProductionConnectionNeedsTheConfirmation()
{
    ConnectionController connections;
    startConnections(connections);
    const QString id = saveConnection(connections, QStringLiteral("Live"), true);
    QVERIFY(!id.isEmpty());

    AiController c;
    c.setConnections(&connections);
    QSignalSpy failed(&c, &AiController::saveFailed);
    QSignalSpy changed(&c, &AiController::settingsChanged);

    // Without the confirmation: refused in the controller, nothing is stored.
    QVERIFY(!c.saveSettings(id, { { QStringLiteral("enabled"), true } }));
    QCOMPARE(failed.size(), 1);
    QVERIFY(!c.lastError().isEmpty());
    QVERIFY(c.lastError().contains(QStringLiteral("production")));
    QVERIFY(!c.isEnabled(id));
    QVERIFY(!c.saveSettings(
        id,
        { { QStringLiteral("enabled"), true }, { QStringLiteral("confirmedProduction"), false } }));
    QCOMPARE(failed.size(), 2);
    QTest::qWait(200);
    QVERIFY(!c.isEnabled(id));

    // Turning it OFF never needs it.
    QVERIFY(c.saveSettings(
        id, { { QStringLiteral("enabled"), false }, { QStringLiteral("sendChecks"), true } }));
    QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 10000);

    // With it: stored. Then a change that keeps it on does not ask again.
    QVERIFY(c.saveSettings(
        id,
        { { QStringLiteral("enabled"), true }, { QStringLiteral("confirmedProduction"), true } }));
    QTRY_VERIFY_WITH_TIMEOUT(c.isEnabled(id), 10000);
    QVERIFY(c.saveSettings(
        id, { { QStringLiteral("enabled"), true }, { QStringLiteral("maxTables"), 20 } }));
    QTRY_COMPARE_WITH_TIMEOUT(c.settingsOf(id).value(QStringLiteral("maxTables")).toInt(), 20,
                              10000);
    // The confirmation is not stored anywhere.
    QVERIFY(!c.settingsOf(id).contains(QStringLiteral("confirmedProduction")));
}

void TstAiController::anUnknownConnectionIsRefused()
{
    ConnectionController connections;
    startConnections(connections);
    AiController c;
    c.setConnections(&connections);
    QSignalSpy failed(&c, &AiController::saveFailed);
    QVERIFY(!c.saveSettings(QStringLiteral("not-an-id"), { { QStringLiteral("enabled"), true } }));
    QVERIFY(!c.saveSettings(QUuid::createUuid().toString(QUuid::WithoutBraces),
                            { { QStringLiteral("enabled"), true } }));
    QCOMPARE(failed.size(), 2);
    QVERIFY(!c.lastError().isEmpty());
}

void TstAiController::removingTheConnectionForgetsItsSettings()
{
    ConnectionController connections;
    startConnections(connections);
    const QString id = saveConnection(connections, QStringLiteral("Temp"), false);
    QVERIFY(!id.isEmpty());
    AiController c;
    c.setConnections(&connections);
    QVERIFY(c.saveSettings(id, { { QStringLiteral("enabled"), true } }));
    QTRY_VERIFY_WITH_TIMEOUT(c.isEnabled(id), 10000);

    QSignalSpy removed(&connections, &ConnectionController::connectionRemoved);
    connections.remove(id);
    QTRY_VERIFY_WITH_TIMEOUT(!removed.isEmpty(), 10000);
    QVERIFY(!c.isEnabled(id)); // the cascade of core dropped the row; the index follows
}

void TstAiController::optionsMapToTheContextOptions()
{
    AiConnectionSettings s;
    s.excludedSchemas = { QStringLiteral("audit") };
    s.excludedTables = { QStringLiteral("a.b") };
    s.sendComments = true;
    s.sendChecks = true;
    s.maxTables = 12;
    const ai::ContextOptions o = AiController::optionsFrom(s);
    QCOMPARE(o.excludedSchemas, s.excludedSchemas);
    QCOMPARE(o.excludedTables, s.excludedTables);
    QVERIFY(o.sendComments);
    QVERIFY(!o.sendDefaults);
    QVERIFY(o.sendChecks);
    QVERIFY(!o.sendEnumValues);
    QVERIFY(!o.sendDefinitions);
    QCOMPARE(o.maxTables, 12);
}

QTEST_MAIN(TstAiController)
#include "tst_ai_controller.moc"
