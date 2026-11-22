#include "ai_controller.h"

#include <quarry/ai/claude_cli.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <sqlite3.h>

using namespace quarry;
using namespace quarry::ui;

// The chat of a query tab (H13 phase C): every request runs the fake `claude` of tests/ai, never
// the real one, against a real SQLite connection. FAKE_CLAUDE_MODE picks what the fake does,
// FAKE_CLAUDE_RESULT the text of its reply and FAKE_CLAUDE_STDIN where it records what it received.

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

const QString kRowSentinel = QStringLiteral("SENTINEL_ROW_VALUE_42");

QString slurp(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

int countRows(const QString &file, const char *table)
{
    sqlite3 *db = nullptr;
    int rows = -1;
    if (sqlite3_open(file.toUtf8().constData(), &db) == SQLITE_OK) {
        sqlite3_stmt *st = nullptr;
        const QByteArray sql = QByteArray("SELECT count(*) FROM ") + table;
        if (sqlite3_prepare_v2(db, sql.constData(), -1, &st, nullptr) == SQLITE_OK
            && sqlite3_step(st) == SQLITE_ROW)
            rows = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return rows;
}

} // namespace

class TstAiChatController : public QObject
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
    QString stdinFile() const { return m_dir.filePath(QStringLiteral("claude-stdin.txt")); }
    QString ticksFile() const { return m_dir.filePath(QStringLiteral("claude-ticks.txt")); }

    // Connections, the tabs, the queries and the AI controller, in the order the app makes them
    // (the connections are destroyed last: the sessions live in their plugins).
    struct Env
    {
        ConnectionController connections;
        EditorTabsModel tabs;
        QueryController queries;
        AiController ai;
        QString dataFile;
        QString connectionId;
        QString tabId;
        int row = -1;
    };

    // A SQLite connection with a few tables and rows (one holds a marker that must never be sent).
    std::unique_ptr<Env> makeEnv(bool enable = true, const QString &script = QString())
    {
        auto env = std::make_unique<Env>();
        env->connections.setDatabasePath(dbPath());
        env->connections.setProviderDirectory(QStringLiteral(QUARRY_PLUGIN_DIR));
        env->connections.start();
        if (!QTest::qWaitFor([&] { return !env->connections.model()->loading(); }, 10000))
            return nullptr;

        env->dataFile = m_dir.filePath(QStringLiteral("data-%1.db").arg(++m_files));
        sqlite3 *db = nullptr;
        if (sqlite3_open(env->dataFile.toUtf8().constData(), &db) != SQLITE_OK)
            return nullptr;
        const QByteArray sql = "CREATE TABLE customers(id INTEGER PRIMARY KEY, name TEXT NOT NULL, "
                               "email TEXT);"
                               "CREATE TABLE invoices(id INTEGER PRIMARY KEY, customer_id INTEGER "
                               "NOT NULL REFERENCES customers(id), total REAL);"
                               "INSERT INTO customers(name, email) VALUES ('"
            + kRowSentinel.toUtf8()
            + "', 'x@y.z'), ('Bea', 'bea@y.z');"
              "INSERT INTO invoices(customer_id, total) VALUES (1, 99.5);";
        const int rc = sqlite3_exec(db, sql.constData(), nullptr, nullptr, nullptr);
        sqlite3_close(db);
        if (rc != SQLITE_OK)
            return nullptr;

        env->connections.prepare({});
        env->connections.form()->setValue(0, env->dataFile);
        QSignalSpy saved(&env->connections, &ConnectionController::connectionSaved);
        const QVariantMap draft { { QStringLiteral("id"), QString() },
                                  { QStringLiteral("providerId"), QStringLiteral("sqlite") },
                                  { QStringLiteral("name"), QStringLiteral("shop") },
                                  { QStringLiteral("colorIndex"), 2 },
                                  { QStringLiteral("production"), false },
                                  { QStringLiteral("passwordMode"), 0 },
                                  { QStringLiteral("file"), env->dataFile } };
        if (!env->connections.save(draft) || (saved.isEmpty() && !saved.wait(10000)))
            return nullptr;
        env->connectionId = saved.at(0).at(0).toString();

        env->tabs.setDatabasePath(dbPath());
        env->tabs.setConnections(&env->connections);
        env->tabs.start();
        if (!QTest::qWaitFor([&] { return env->tabs.loaded(); }, 10000))
            return nullptr;
        env->queries.setTabs(&env->tabs);
        env->ai.setConnections(&env->connections);
        env->ai.setTabs(&env->tabs);
        env->ai.setQueries(&env->queries);
        env->ai.setCliPath(fakeClaude());
        env->ai.setLogDirectory(m_dir.filePath(QStringLiteral("logs")));

        if (enable) {
            if (!env->ai.saveSettings(env->connectionId, { { QStringLiteral("enabled"), true } }))
                return nullptr;
            if (!QTest::qWaitFor(
                    [&] { return env->ai.isEnabled(env->connectionId) && env->ai.settingsReady(); },
                    10000))
                return nullptr;
        }
        env->row = env->tabs.openTab(env->connectionId);
        env->tabId = env->tabs.idOf(env->row);
        env->tabs.setText(env->row, script);
        return env;
    }

    // Waits for the chat to leave preparing / thinking.
    static bool settles(AiChatController *chat, int ms = 20000)
    {
        return QTest::qWaitFor([&] { return !chat->busy(); }, ms);
    }

private slots:
    void initTestCase()
    {
        // Never the real preferences or the real quarry.db.
        QCoreApplication::setOrganizationName(QStringLiteral("QuarryTest"));
        QCoreApplication::setApplicationName(QStringLiteral("tst_ai_chat_controller"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_prefs.path());
        QStandardPaths::setTestModeEnabled(true);
    }

    void init()
    {
        QSettings().remove(QStringLiteral("ui/layout/ai"));
        for (const QString &suffix : { QString(), QStringLiteral("-wal"), QStringLiteral("-shm") })
            QFile::remove(dbPath() + suffix);
        QFile::remove(stdinFile());
        QFile::remove(ticksFile());
        QDir(m_dir.filePath(QStringLiteral("logs"))).removeRecursively();
    }

    void nothingExistsUntilThePanelOpens();
    void aReplyWithoutAScript();
    void aReplyWithAScriptMakesTheDiffAndTheLiteralsGoAndComeBack();
    void theHistoryTravelsAndIsLimited();
    void theErrorOfTheLastRunIsSanitized();
    void theContextHasStructureAndNoRows();
    void cancellingWhileClaudeIsThinking();
    void everyErrorIsSaidInPlainLanguage();
    void aMissingCliSaysSoAndStartsNothing();
    void withTheAiOffNothingStarts();
    void aTabWithoutAConnectionCannotAsk();
    void closingTheTabCancelsTheRequest();
    void oneRequestAtATime();
    void editingTheScriptMeanwhileWarnsAndAppliesNothing();
    void nothingRunsAnySql();
    void theLocalLogIsOffUnlessAskedAndRotates();
    void clearingTheChatForgetsEverything();
};

void TstAiChatController::nothingExistsUntilThePanelOpens()
{
    auto env = makeEnv();
    QVERIFY(env);
    QCOMPARE(env->ai.chatCount(), 0);
    QVERIFY(env->ai.existingChat(env->tabId) == nullptr);
    QVERIFY(env->ai.runnerIfCreated() == nullptr);
    QVERIFY(env->ai.chatFor(QStringLiteral("not-a-tab")) == nullptr);
    AiChatController *chat = env->ai.chatFor(env->tabId);
    QVERIFY(chat != nullptr);
    QCOMPARE(env->ai.chatCount(), 1);
    QCOMPARE(env->ai.chatFor(env->tabId), chat); // one chat per tab
    QCOMPARE(env->ai.existingChat(env->tabId), chat);
    QCOMPARE(chat->state(), QStringLiteral("idle"));
    QVERIFY(chat->canSend());
    // Opening the panel starts no process and no adapter: only a request does.
    QVERIFY(env->ai.runnerIfCreated() == nullptr);
    QVERIFY(env->ai.adapterIfCreated() == nullptr);
    QVERIFY(!QFileInfo::exists(stdinFile()));
}

void TstAiChatController::aReplyWithoutAScript()
{
    const EnvGuard mode("FAKE_CLAUDE_MODE", "ok");
    const EnvGuard result("FAKE_CLAUDE_RESULT", "Join customers to invoices on customer_id.");
    const EnvGuard in("FAKE_CLAUDE_STDIN", stdinFile().toUtf8());
    auto env = makeEnv(true, QStringLiteral("SELECT * FROM customers;"));
    QVERIFY(env);
    AiChatController *chat = env->ai.chatFor(env->tabId);
    QSignalSpy states(chat, &AiChatController::stateChanged);
    QSignalSpy announced(chat, &AiChatController::announcement);

    QVERIFY(chat->send(QStringLiteral("How do I list invoices per customer?")));
    QVERIFY(chat->busy());
    QCOMPARE(chat->state(), QStringLiteral("preparing"));
    QVERIFY(!chat->canSend());
    QVERIFY(!chat->send(QStringLiteral("again"))); // one at a time
    QTRY_COMPARE_WITH_TIMEOUT(chat->state(), QStringLiteral("done"), 20000);

    QCOMPARE(chat->messages()->count(), 2);
    QCOMPARE(chat->messages()->get(0).value(QStringLiteral("role")).toString(),
             QStringLiteral("user"));
    QCOMPARE(chat->messages()->get(0).value(QStringLiteral("author")).toString(),
             QStringLiteral("You"));
    QCOMPARE(chat->messages()->get(1).value(QStringLiteral("author")).toString(),
             QStringLiteral("Claude"));
    QCOMPARE(chat->messages()->get(1).value(QStringLiteral("text")).toString(),
             QStringLiteral("Join customers to invoices on customer_id."));
    QVERIFY(chat->diff() == nullptr);
    QVERIFY(chat->errorKind().isEmpty());
    QVERIFY(chat->canSend());
    QCOMPARE(chat->stateText(), QStringLiteral("Done."));
    // The states went by in order, and the screen reader was told.
    QVERIFY(states.size() >= 3); // preparing, thinking, done
    QVERIFY(!announced.isEmpty());
    bool thinking = false;
    for (const QList<QVariant> &a : std::as_const(announced))
        thinking = thinking || a.at(0).toString() == QStringLiteral("Claude is thinking…");
    QVERIFY(thinking);

    // What reached Claude: the request, the structure of the table the script names and the script.
    const QString sent = slurp(stdinFile());
    QVERIFY(sent.contains(QStringLiteral("How do I list invoices per customer?")));
    QVERIFY(sent.contains(QStringLiteral("customers")));
    QVERIFY(sent.contains(QStringLiteral("SELECT * FROM customers;")));
    QVERIFY(!sent.contains(kRowSentinel));
}

void TstAiChatController::aReplyWithAScriptMakesTheDiffAndTheLiteralsGoAndComeBack()
{
    const QString script = QStringLiteral(
        "SELECT * FROM customers\nWHERE name = 'Ana Lopez'\n  AND email = 'secret@tok.en';\n");
    // Claude only ever saw __V1__ and __V2__; it adds an ORDER BY.
    const EnvGuard mode("FAKE_CLAUDE_MODE", "ok");
    const EnvGuard result(
        "FAKE_CLAUDE_RESULT",
        "Added an order.\n<quarry-script>\nSELECT * FROM customers\nWHERE name = __V1__\n  AND "
        "email = __V2__\nORDER BY id;\n</quarry-script>");
    const EnvGuard in("FAKE_CLAUDE_STDIN", stdinFile().toUtf8());
    auto env = makeEnv(true, script);
    QVERIFY(env);
    AiChatController *chat = env->ai.chatFor(env->tabId);
    QSignalSpy ready(chat, &AiChatController::proposalReady);

    QVERIFY(chat->send(QStringLiteral("order the rows")));
    QTRY_COMPARE_WITH_TIMEOUT(chat->state(), QStringLiteral("done"), 20000);

    // The literals of the script never went out; the placeholders did.
    const QString sent = slurp(stdinFile());
    QVERIFY(!sent.isEmpty());
    QVERIFY(!sent.contains(QStringLiteral("Ana Lopez")));
    QVERIFY(!sent.contains(QStringLiteral("secret@tok.en")));
    QVERIFY(sent.contains(QStringLiteral("__V1__")));
    QVERIFY(sent.contains(QStringLiteral("__V2__")));
    // The request itself is the user's own text and goes as typed.
    QVERIFY(sent.contains(QStringLiteral("order the rows")));

    // The proposal came back with the literals in place, as a diff against the script that was
    // sent.
    QCOMPARE(chat->messages()->get(1).value(QStringLiteral("text")).toString(),
             QStringLiteral("Added an order."));
    QVERIFY(chat->diff() != nullptr);
    QVERIFY(!chat->diffStale());
    QCOMPARE(ready.size(), 1);
    QCOMPARE(ready.at(0).at(0).toInt(), 1);
    AiScriptDiff *diff = chat->diff();
    QCOMPARE(diff->changeCount(), 1);
    QCOMPARE(diff->original(), script);
    int change = -1;
    for (int i = 0; i < diff->count(); ++i) {
        if (diff->data(diff->index(i), AiScriptDiff::KindRole).toString()
            != QLatin1String("unchanged"))
            change = i;
    }
    QVERIFY(change >= 0);
    QCOMPARE(diff->data(diff->index(change), AiScriptDiff::KindRole).toString(),
             QStringLiteral("changed"));
    QCOMPARE(diff->data(diff->index(change), AiScriptDiff::OldLinesRole).toStringList(),
             QStringList { QStringLiteral("  AND email = 'secret@tok.en';") });
    QCOMPARE(diff->data(diff->index(change), AiScriptDiff::NewLinesRole).toStringList(),
             (QStringList { QStringLiteral("  AND email = 'secret@tok.en'"),
                            QStringLiteral("ORDER BY id;") }));
    QVERIFY(diff->hasPending());
    // Nothing was applied: the tab still has the script, byte for byte.
    QCOMPARE(env->tabs.textOf(env->row), script);
    diff->acceptAll();
    QVERIFY(diff->result().contains(QStringLiteral("name = 'Ana Lopez'")));
    QVERIFY(diff->result().contains(QStringLiteral("ORDER BY id;")));
    QCOMPARE(env->tabs.textOf(env->row), script); // still: Apply is the view's decision

    chat->discardDiff();
    QVERIFY(chat->diff() == nullptr);
    QCOMPARE(env->tabs.textOf(env->row), script);
}

void TstAiChatController::theHistoryTravelsAndIsLimited()
{
    const EnvGuard mode("FAKE_CLAUDE_MODE", "ok");
    const EnvGuard result("FAKE_CLAUDE_RESULT", "Noted.");
    const EnvGuard in("FAKE_CLAUDE_STDIN", stdinFile().toUtf8());
    auto env = makeEnv(true, QStringLiteral("SELECT 1;"));
    QVERIFY(env);
    AiChatController *chat = env->ai.chatFor(env->tabId);

    QVERIFY(chat->send(QStringLiteral("FIRSTQUESTION about the customers")));
    QVERIFY(settles(chat));
    QCOMPARE(chat->contexts()->get(0).value(QStringLiteral("historyCount")).toInt(), 0);
    QVERIFY(chat->send(QStringLiteral("SECONDQUESTION")));
    QVERIFY(settles(chat));
    // The second request carries the first exchange.
    QCOMPARE(chat->contexts()->get(1).value(QStringLiteral("historyCount")).toInt(), 2);
    const QString second = slurp(stdinFile());
    QVERIFY(second.contains(QStringLiteral("FIRSTQUESTION")));
    QVERIFY(second.contains(QStringLiteral("SECONDQUESTION")));
    QVERIFY(second.contains(QStringLiteral("Noted.")));

    // Long messages push the oldest ones out of the prompt (60 000 characters), and it says so.
    const QString filler(25000, QLatin1Char('x'));
    for (int i = 3; i <= 6; ++i) {
        QVERIFY(chat->send(QStringLiteral("MARK%1 ").arg(i) + filler));
        QVERIFY(settles(chat));
    }
    const QString last = slurp(stdinFile());
    QVERIFY(last.contains(QStringLiteral("MARK6"))); // the request itself
    QVERIFY(last.contains(QStringLiteral("MARK5")));
    // The budget is what is left of 60 000 after the script, the structure and the request itself:
    // the older turns that do not fit are left out, and the prompt says how many.
    QVERIFY(!last.contains(QStringLiteral("MARK4")));
    QVERIFY(!last.contains(QStringLiteral("MARK3")));
    QVERIFY(last.contains(QStringLiteral("earlier messages omitted")));
    QVERIFY(!last.contains(QStringLiteral("FIRSTQUESTION")));
    QVERIFY(last.size() < 100000);
    QCOMPARE(chat->messages()->count(), 12); // the conversation on screen keeps everything
    QCOMPARE(chat->contexts()->count(), 6);
}

void TstAiChatController::theErrorOfTheLastRunIsSanitized()
{
    const EnvGuard mode("FAKE_CLAUDE_MODE", "ok");
    const EnvGuard result("FAKE_CLAUDE_RESULT", "The key already exists.");
    const EnvGuard in("FAKE_CLAUDE_STDIN", stdinFile().toUtf8());
    auto env = makeEnv(true, QStringLiteral("INSERT INTO customers(email) VALUES ('ana@x.com');"));
    QVERIFY(env);
    QueryState *state = env->queries.stateOf(env->tabId);
    QVERIFY(state != nullptr);
    state->messages()->append({ QStringLiteral("error"), QStringLiteral("Error"),
                                QStringLiteral("The value already exists."),
                                QStringLiteral("UNIQUE constraint failed: Key (email)=(ana@x.com) "
                                               "already exists, value is (4242)"),
                                1, 1 });
    state->finish(QueryState::State::Failed);

    AiChatController *chat = env->ai.chatFor(env->tabId);
    QVERIFY(chat->send(QStringLiteral("why did it fail?")));
    QVERIFY(settles(chat));
    const QString sent = slurp(stdinFile());
    QVERIFY(sent.contains(QStringLiteral("already exists")));
    // The value the database quoted is gone from the error (the script still has its own, masked).
    QVERIFY(!sent.contains(QStringLiteral("ana@x.com")));
    QVERIFY(!sent.contains(QStringLiteral("4242")));
    const QString errorText
        = chat->contexts()->get(0).value(QStringLiteral("errorText")).toString();
    QVERIFY(errorText.contains(QStringLiteral("already exists")));
    QVERIFY(!errorText.contains(QStringLiteral("ana@x.com")));

    // A run that did not fail adds nothing.
    state->finish(QueryState::State::Done);
    QVERIFY(chat->send(QStringLiteral("and now?")));
    QVERIFY(settles(chat));
    QVERIFY(chat->contexts()->get(1).value(QStringLiteral("errorText")).toString().isEmpty());
}

void TstAiChatController::theContextHasStructureAndNoRows()
{
    const EnvGuard mode("FAKE_CLAUDE_MODE", "ok");
    const EnvGuard result("FAKE_CLAUDE_RESULT", "ok");
    const EnvGuard in("FAKE_CLAUDE_STDIN", stdinFile().toUtf8());
    auto env = makeEnv(true, QStringLiteral("SELECT * FROM invoices;"));
    QVERIFY(env);
    AiChatController *chat = env->ai.chatFor(env->tabId);
    QVERIFY(chat->send(QStringLiteral("which customer owns each invoice?")));
    QVERIFY(settles(chat));
    QCOMPARE(chat->contexts()->count(), 1);
    const QVariantMap entry = chat->contexts()->get(0);
    const QStringList tables = entry.value(QStringLiteral("tables")).toStringList();
    QVERIFY2(tables.join(QLatin1Char(',')).contains(QStringLiteral("invoices")),
             qPrintable(tables.join(QLatin1Char(','))));
    // One hop away through the foreign key: customers, with both sides of the key described.
    QVERIFY(tables.join(QLatin1Char(',')).contains(QStringLiteral("customers")));
    const QString ddl = entry.value(QStringLiteral("ddl")).toString();
    QVERIFY(ddl.contains(QStringLiteral("customer_id")));
    QVERIFY(ddl.contains(QStringLiteral("customers")));
    QVERIFY(ddl.contains(QStringLiteral("name")));
    // The fixed sentence is part of what is copied and logged, and no row value is anywhere.
    const QString text = chat->contextText(0);
    QVERIFY(text.contains(QStringLiteral("Not sent: rows, results or connection details.")));
    QVERIFY(text.contains(QStringLiteral("Tables (")));
    for (const QString &haystack : { text, ddl, slurp(stdinFile()) }) {
        QVERIFY(!haystack.contains(kRowSentinel));
        QVERIFY(!haystack.contains(QStringLiteral("bea@y.z")));
        QVERIFY(!haystack.contains(QStringLiteral("99.5")));
    }
    // Nor the file of the connection.
    QVERIFY(!slurp(stdinFile()).contains(env->dataFile));
}

void TstAiChatController::cancellingWhileClaudeIsThinking()
{
    const EnvGuard mode("FAKE_CLAUDE_MODE", "slow");
    const EnvGuard ticks("FAKE_CLAUDE_TICKS", ticksFile().toUtf8());
    auto env = makeEnv(true, QStringLiteral("SELECT 1;"));
    QVERIFY(env);
    AiChatController *chat = env->ai.chatFor(env->tabId);
    QVERIFY(chat->send(QStringLiteral("take your time")));
    QTRY_COMPARE_WITH_TIMEOUT(chat->state(), QStringLiteral("thinking"), 20000);
    QCOMPARE(chat->stateText(), QStringLiteral("Claude is thinking…"));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(ticksFile()).size() >= 1, 20000); // it is really running

    chat->cancel();
    QCOMPARE(chat->state(), QStringLiteral("cancelled"));
    QVERIFY(!chat->busy());
    QVERIFY(chat->errorKind().isEmpty()); // not an error
    QVERIFY(chat->errorMessage().isEmpty());
    QCOMPARE(chat->messages()->count(), 1); // only what the user asked
    QVERIFY(chat->canSend());

    // The process is gone: the file stops growing.
    QTest::qWait(300);
    const qint64 size = QFileInfo(ticksFile()).size();
    QTest::qWait(2500);
    QCOMPARE(QFileInfo(ticksFile()).size(), size);
    QCOMPARE(chat->state(), QStringLiteral("cancelled"));

    // And it can ask again.
    const EnvGuard fast("FAKE_CLAUDE_MODE", "ok");
    QVERIFY(chat->send(QStringLiteral("again")));
    QVERIFY(settles(chat));
    QCOMPARE(chat->state(), QStringLiteral("done"));
}

void TstAiChatController::everyErrorIsSaidInPlainLanguage()
{
    struct Case
    {
        const char *mode;
        const char *kind;
    };
    for (const Case &c : { Case { "notloggedin", "notLoggedIn" },
                           Case { "usagelimit", "usageLimit" }, Case { "corrupt", "invalidOutput" },
                           Case { "exit3", "failed" }, Case { "old", "incompatibleVersion" } }) {
        const EnvGuard mode("FAKE_CLAUDE_MODE", c.mode);
        auto env = makeEnv(true, QStringLiteral("SELECT 1;"));
        QVERIFY(env);
        AiChatController *chat = env->ai.chatFor(env->tabId);
        QSignalSpy errors(chat, &AiChatController::errorChanged);
        QVERIFY2(chat->send(QStringLiteral("hello")), c.mode);
        QVERIFY2(settles(chat), c.mode);
        QCOMPARE(chat->state(), QStringLiteral("error"));
        QCOMPARE(chat->errorKind(), QString::fromLatin1(c.kind));
        QVERIFY2(!chat->errorMessage().isEmpty(), c.mode);
        QCOMPARE(chat->stateText(), chat->errorMessage());
        QVERIFY(!errors.isEmpty());
        // No reply message, and the user's question stays.
        QCOMPARE(chat->messages()->count(), 1);
        QVERIFY(chat->diff() == nullptr);
        QVERIFY(chat->canSend()); // the user can try again
        if (QByteArray(c.mode) == "notloggedin") {
            QCOMPARE(chat->errorHint(), ai::loginCommandHint());
            QVERIFY(chat->errorMessage().contains(QStringLiteral("not signed in")));
        } else {
            QVERIFY(chat->errorHint().isEmpty());
        }
        if (QByteArray(c.mode) == "usagelimit") {
            QVERIFY(chat->errorMessage().contains(QStringLiteral("usage limit")));
            QVERIFY(chat->errorMessage().contains(QStringLiteral("subscription or API key")));
        }
        QCOMPARE(chat->errorAction(),
                 QByteArray(c.mode) == "old" ? QStringLiteral("settings") : QString());
        // The next request clears the error card.
        const EnvGuard ok("FAKE_CLAUDE_MODE", "ok");
        QVERIFY(chat->send(QStringLiteral("again")));
        QVERIFY(chat->errorKind().isEmpty());
        QVERIFY(settles(chat));
        QCOMPARE(chat->state(), QStringLiteral("done"));
    }
}

void TstAiChatController::aMissingCliSaysSoAndStartsNothing()
{
    auto env = makeEnv(true, QStringLiteral("SELECT 1;"));
    QVERIFY(env);
    env->ai.setCliPath(m_dir.filePath(QStringLiteral("no-such-claude")));
    AiChatController *chat = env->ai.chatFor(env->tabId);
    QCOMPARE(chat->unavailableKind(), QStringLiteral("needsCli"));
    QCOMPARE(chat->state(), QStringLiteral("needsCli"));
    QVERIFY(!chat->unavailableText().isEmpty());
    QVERIFY(!chat->canSend());
    QVERIFY(!chat->send(QStringLiteral("hello")));
    QCOMPARE(chat->state(), QStringLiteral("needsCli"));
    QCOMPARE(chat->messages()->count(), 0);
    QVERIFY(env->ai.runnerIfCreated() == nullptr); // nothing was started
    QVERIFY(env->ai.adapterIfCreated() == nullptr);
    // Fixing the path brings the chat back.
    env->ai.setCliPath(fakeClaude());
    QTRY_COMPARE(chat->state(), QStringLiteral("idle"));
    QVERIFY(chat->canSend());
}

void TstAiChatController::withTheAiOffNothingStarts()
{
    const EnvGuard in("FAKE_CLAUDE_STDIN", stdinFile().toUtf8());
    auto env = makeEnv(false, QStringLiteral("SELECT 1;")); // the default: off
    QVERIFY(env);
    AiChatController *chat = env->ai.chatFor(env->tabId);
    QTRY_COMPARE_WITH_TIMEOUT(chat->state(), QStringLiteral("off"), 10000);
    QCOMPARE(chat->unavailableKind(), QStringLiteral("off"));
    QVERIFY(!chat->unavailableText().isEmpty());
    QVERIFY(!chat->canSend());
    QVERIFY(!chat->send(QStringLiteral("hello")));
    QCOMPARE(chat->state(), QStringLiteral("off"));
    QCOMPARE(chat->messages()->count(), 0);
    QVERIFY(env->ai.runnerIfCreated() == nullptr);
    QVERIFY(env->ai.adapterIfCreated() == nullptr);
    QVERIFY(!QFileInfo::exists(stdinFile()));

    // Turning it on in the connection's settings frees the chat.
    QVERIFY(env->ai.saveSettings(env->connectionId, { { QStringLiteral("enabled"), true } }));
    QTRY_COMPARE_WITH_TIMEOUT(chat->state(), QStringLiteral("idle"), 10000);
    QVERIFY(chat->canSend());
}

void TstAiChatController::aTabWithoutAConnectionCannotAsk()
{
    auto env = makeEnv(true);
    QVERIFY(env);
    const int row = env->tabs.openTab({});
    env->tabs.setConnection(row, QString());
    AiChatController *chat = env->ai.chatFor(env->tabs.idOf(row));
    QVERIFY(chat != nullptr);
    QCOMPARE(chat->unavailableKind(), QStringLiteral("noConnection"));
    QVERIFY(!chat->send(QStringLiteral("hello")));
    QCOMPARE(chat->state(), QStringLiteral("noConnection"));
    QVERIFY(env->ai.runnerIfCreated() == nullptr);
}

void TstAiChatController::closingTheTabCancelsTheRequest()
{
    const EnvGuard mode("FAKE_CLAUDE_MODE", "slow");
    const EnvGuard ticks("FAKE_CLAUDE_TICKS", ticksFile().toUtf8());
    auto env = makeEnv(true, QStringLiteral("SELECT 1;"));
    QVERIFY(env);
    AiChatController *chat = env->ai.chatFor(env->tabId);
    QVERIFY(chat->send(QStringLiteral("slow one")));
    QTRY_COMPARE_WITH_TIMEOUT(chat->state(), QStringLiteral("thinking"), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(ticksFile()).size() >= 1, 20000);

    // Closing the tab of the editor takes the chat with it, and the process.
    const int other = env->tabs.openTab(env->connectionId); // so the closed one is not the last
    Q_UNUSED(other)
    QSignalSpy gone(&env->ai, &AiController::chatsChanged);
    env->tabs.closeTab(env->row);
    QCOMPARE(env->ai.chatCount(), 0);
    QVERIFY(env->ai.existingChat(env->tabId) == nullptr);
    QVERIFY(!gone.isEmpty());
    QTest::qWait(300);
    const qint64 size = QFileInfo(ticksFile()).size();
    QTest::qWait(2500);
    QCOMPARE(QFileInfo(ticksFile()).size(), size);

    // forgetChat does the same by hand, and an unknown id is nothing.
    AiChatController *second = env->ai.chatFor(env->tabs.idOf(env->tabs.currentIndex()));
    QVERIFY(second != nullptr);
    env->ai.forgetChat(env->tabs.idOf(env->tabs.currentIndex()));
    QCOMPARE(env->ai.chatCount(), 0);
    env->ai.forgetChat(QStringLiteral("nothing"));
}

void TstAiChatController::oneRequestAtATime()
{
    const EnvGuard mode("FAKE_CLAUDE_MODE", "slow");
    const EnvGuard ticks("FAKE_CLAUDE_TICKS", ticksFile().toUtf8());
    auto env = makeEnv(true, QStringLiteral("SELECT 1;"));
    QVERIFY(env);
    AiChatController *chat = env->ai.chatFor(env->tabId);
    QVERIFY(chat->send(QStringLiteral("first")));
    QVERIFY(!chat->send(QStringLiteral("second")));
    QCOMPARE(chat->messages()->count(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(chat->state(), QStringLiteral("thinking"), 20000);
    QVERIFY(!chat->send(QStringLiteral("third")));
    QCOMPARE(chat->messages()->count(), 1);
    chat->cancel();
    QCOMPARE(chat->state(), QStringLiteral("cancelled"));
    // An empty text is not a request either.
    QVERIFY(!chat->send(QStringLiteral("   \n ")));
    QCOMPARE(chat->messages()->count(), 1);
}

void TstAiChatController::editingTheScriptMeanwhileWarnsAndAppliesNothing()
{
    const QString script = QStringLiteral("SELECT 1;\n");
    const EnvGuard mode("FAKE_CLAUDE_MODE", "ok");
    const EnvGuard result("FAKE_CLAUDE_RESULT",
                          "Done.\n<quarry-script>\nSELECT 1;\nSELECT 2;\n</quarry-script>");
    auto env = makeEnv(true, script);
    QVERIFY(env);
    AiChatController *chat = env->ai.chatFor(env->tabId);
    QVERIFY(chat->send(QStringLiteral("add a second select")));
    // The user types while Claude works.
    const QString edited = QStringLiteral("SELECT 1;\n-- my note\n");
    env->tabs.setText(env->row, edited);
    QVERIFY(settles(chat));
    QCOMPARE(chat->state(), QStringLiteral("done"));
    QVERIFY(chat->diff() != nullptr);
    QVERIFY(chat->diffStale()); // "The script changed while Claude was working"
    QCOMPARE(chat->diff()->original(), script); // made for what was sent
    QCOMPARE(env->tabs.textOf(env->row), edited); // and nothing was applied

    // Redo it for the current script...
    QSignalSpy diffs(chat, &AiChatController::diffChanged);
    chat->redoDiff();
    QCOMPARE(diffs.size(), 1);
    QVERIFY(chat->diff() != nullptr);
    QVERIFY(!chat->diffStale());
    QCOMPARE(chat->diff()->original(), edited);
    QCOMPARE(env->tabs.textOf(env->row), edited);
    // ...or drop it.
    chat->discardDiff();
    QVERIFY(chat->diff() == nullptr);
    QCOMPARE(env->tabs.textOf(env->row), edited);
    chat->redoDiff(); // nothing to redo: nothing happens
    QVERIFY(chat->diff() == nullptr);
}

void TstAiChatController::nothingRunsAnySql()
{
    // Claude proposes a DELETE and the user accepts the block: it is only text. The rows are still
    // there, no session ran anything for the tab and the editor text is the user's until Apply.
    const QString script = QStringLiteral("SELECT * FROM customers;\n");
    const EnvGuard mode("FAKE_CLAUDE_MODE", "ok");
    const EnvGuard result("FAKE_CLAUDE_RESULT",
                          "Cleaned up.\n<quarry-script>\nSELECT * FROM customers;\nDELETE FROM "
                          "customers;\nDROP TABLE invoices;\n</quarry-script>");
    auto env = makeEnv(true, script);
    QVERIFY(env);
    const int before = countRows(env->dataFile, "customers");
    QCOMPARE(before, 2);
    AiChatController *chat = env->ai.chatFor(env->tabId);
    QVERIFY(chat->send(QStringLiteral("delete everything")));
    QVERIFY(settles(chat));
    QVERIFY(chat->diff() != nullptr);
    QVERIFY(chat->diff()->modifiesData()); // flagged before anything could be run
    chat->diff()->acceptAll();
    QVERIFY(chat->diff()->result().contains(QStringLiteral("DELETE FROM customers;")));

    QCOMPARE(countRows(env->dataFile, "customers"), before);
    QCOMPARE(countRows(env->dataFile, "invoices"), 1);
    QCOMPARE(env->tabs.textOf(env->row), script);
    QCOMPARE(env->queries.sessionCount(), 0); // no statement session was ever opened for a run
    QVERIFY(!env->queries.hasSession(env->tabId));
    QVERIFY(env->queries.stateOf(env->tabId)->state() == QLatin1String("idle"));
    QCOMPARE(env->queries.stateOf(env->tabId)->messages()->count(), 0);
}

void TstAiChatController::theLocalLogIsOffUnlessAskedAndRotates()
{
    const EnvGuard mode("FAKE_CLAUDE_MODE", "ok");
    const EnvGuard result("FAKE_CLAUDE_RESULT", "fine");
    auto env = makeEnv(true, QStringLiteral("SELECT * FROM customers;"));
    QVERIFY(env);
    QVERIFY(!env->ai.keepLog()); // off by default
    AiChatController *chat = env->ai.chatFor(env->tabId);
    QVERIFY(chat->send(QStringLiteral("first, not logged")));
    QVERIFY(settles(chat));
    AiController::flushLog();
    QVERIFY(!QFileInfo::exists(env->ai.logFile()));

    QSignalSpy changed(&env->ai, &AiController::keepLogChanged);
    env->ai.setKeepLog(true);
    QCOMPARE(changed.size(), 1);
    QVERIFY(QSettings().value(QStringLiteral("ui/layout/ai/keepLog")).toBool()); // persisted
    QVERIFY(chat->send(QStringLiteral("second, logged")));
    QVERIFY(settles(chat));
    AiController::flushLog();
    const QString log = slurp(env->ai.logFile());
    // What is written is what the panel shows for that request, and nothing else.
    QVERIFY(log.contains(chat->contextText(1)));
    QVERIFY(log.contains(QStringLiteral("second, logged")));
    QVERIFY(!log.contains(QStringLiteral("first, not logged")));
    QVERIFY(!log.contains(kRowSentinel));
    QVERIFY(log.contains(QStringLiteral("Not sent: rows, results or connection details.")));

    // Rotation: past the limit the file becomes .1, then .2; no third copy.
    const QString dir = m_dir.filePath(QStringLiteral("rotate"));
    const QString text = QString(60, QLatin1Char('a')) + QLatin1Char('\n');
    for (int i = 0; i < 12; ++i)
        AiController::appendLog(dir, text, 200);
    const QString base = dir + QStringLiteral("/ai-requests.log");
    QVERIFY(QFileInfo::exists(base));
    QVERIFY(QFileInfo::exists(base + QStringLiteral(".1")));
    QVERIFY(QFileInfo::exists(base + QStringLiteral(".2")));
    QVERIFY(!QFileInfo::exists(base + QStringLiteral(".3")));
    QVERIFY(QFileInfo(base).size() <= 200);
    QCOMPARE(AiController::kMaxLogBytes, qint64(1024) * 1024);
    env->ai.setKeepLog(false);
}

void TstAiChatController::clearingTheChatForgetsEverything()
{
    const EnvGuard mode("FAKE_CLAUDE_MODE", "ok");
    const EnvGuard result("FAKE_CLAUDE_RESULT", "ok");
    auto env = makeEnv(true, QStringLiteral("SELECT 1;"));
    QVERIFY(env);
    AiChatController *chat = env->ai.chatFor(env->tabId);
    chat->setDraft(QStringLiteral("half a thought"));
    QVERIFY(chat->send(QStringLiteral("hello")));
    QVERIFY(settles(chat));
    QCOMPARE(chat->messages()->count(), 2);
    QCOMPARE(chat->contexts()->count(), 1);
    chat->clearChat();
    QCOMPARE(chat->messages()->count(), 0);
    QCOMPARE(chat->contexts()->count(), 0);
    QCOMPARE(chat->state(), QStringLiteral("idle"));
    QVERIFY(chat->diff() == nullptr);
    QCOMPARE(chat->draft(), QStringLiteral("half a thought")); // what was typed is not a message
    // The next request starts with no history.
    QVERIFY(chat->send(QStringLiteral("fresh")));
    QVERIFY(settles(chat));
    QCOMPARE(chat->contexts()->get(0).value(QStringLiteral("historyCount")).toInt(), 0);
}

QTEST_MAIN(TstAiChatController)
#include "tst_ai_chat_controller.moc"
