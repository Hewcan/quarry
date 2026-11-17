#include "ui/connection_controller.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include <algorithm>

using namespace quarry;
using namespace quarry::ui;

namespace {

const QString kPassword = QStringLiteral("Tr0ub4dor&3-UNIQUE-PW");

// Stands in for the system keychain: records the calls and answers like CredentialStore would.
struct FakeKeychain
{
    explicit FakeKeychain(ConnectionController &controller)
        : c(controller)
    {
        CredentialBackend b;
        b.write = [this](QUuid id, const QString &pw) {
            calls << QStringLiteral("write:%1:%2").arg(id.toString(QUuid::WithoutBraces), pw);
            QMetaObject::invokeMethod(
                &c,
                [this, id] {
                    if (failWrite)
                        c.onCredentialFailed(
                            id,
                            Error { ErrorKind::Unsupported,
                                    QStringLiteral("The system keychain is not available."),
                                    QStringLiteral("no backend"),
                                    {},
                                    {} });
                    else
                        c.onPasswordWritten(id);
                },
                Qt::QueuedConnection);
        };
        b.read = [this](QUuid id) {
            calls << QStringLiteral("read:%1").arg(id.toString(QUuid::WithoutBraces));
            QMetaObject::invokeMethod(
                &c, [this, id] { c.onPasswordRead(id, kPassword); }, Qt::QueuedConnection);
        };
        b.remove = [this](QUuid id) {
            calls << QStringLiteral("remove:%1").arg(id.toString(QUuid::WithoutBraces));
        };
        b.forget = [this](QUuid id) {
            calls << QStringLiteral("forget:%1").arg(id.toString(QUuid::WithoutBraces));
        };
        b.writeSecret = [this](QUuid id, SecretKind kind, const QString &value) {
            calls << QStringLiteral("ssh-write:%1:%2:%3")
                         .arg(id.toString(QUuid::WithoutBraces))
                         .arg(static_cast<int>(kind))
                         .arg(value);
            QMetaObject::invokeMethod(
                &c,
                [this, id, kind] {
                    if (failSshWrite)
                        c.onSecretFailed(id, kind,
                                         Error { ErrorKind::Unsupported,
                                                 QStringLiteral("The system keychain is not "
                                                                "available."),
                                                 {},
                                                 {},
                                                 {} });
                    else
                        c.onSecretWritten(id, kind);
                },
                Qt::QueuedConnection);
        };
        b.removeSecret = [this](QUuid id, SecretKind kind) {
            calls << QStringLiteral("ssh-delete:%1:%2")
                         .arg(id.toString(QUuid::WithoutBraces))
                         .arg(static_cast<int>(kind));
            QMetaObject::invokeMethod(
                &c, [this, id, kind] { c.onSecretDeleted(id, kind); }, Qt::QueuedConnection);
        };
        c.setCredentialBackend(b);
    }

    bool called(const QString &prefix) const
    {
        return std::ranges::any_of(calls, [&](const QString &s) { return s.startsWith(prefix); });
    }

    ConnectionController &c;
    QStringList calls;
    bool failWrite = false;
    bool failSshWrite = false;
};

// An engine with a Secret field, as PostgreSQL will have: the SQLite plugin has none.
std::vector<ConnectionField> fieldsWithSecret()
{
    ConnectionField file;
    file.key = QStringLiteral("file");
    file.label = QStringLiteral("Database file");
    file.kind = ConnectionField::Kind::Path;
    file.required = true;
    ConnectionField pw;
    pw.key = QStringLiteral("password");
    pw.kind = ConnectionField::Kind::Secret;
    return { file, pw };
}

QByteArray allDatabaseBytes(const QString &path)
{
    QByteArray all;
    for (const QString &suffix : { QString(), QStringLiteral("-wal"), QStringLiteral("-shm") }) {
        QFile f(path + suffix);
        if (f.open(QIODevice::ReadOnly))
            all += f.readAll();
    }
    return all;
}

} // namespace

class TstConnectionController : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString dbPath() const { return m_dir.filePath(QStringLiteral("quarry.db")); }
    QString dataFile(const QString &name = QStringLiteral("data.db")) const
    {
        return m_dir.filePath(name);
    }

    // A fresh store per test: the database file is deleted before each one.
    void startController(ConnectionController &c)
    {
        c.setDatabasePath(dbPath());
        c.setProviderDirectory(QStringLiteral(QUARRY_PLUGIN_DIR));
        c.start();
        QTRY_VERIFY_WITH_TIMEOUT(!c.model()->loading(), 10000);
    }

    QVariantMap sqliteDraft(const QString &name, const QString &file, const QString &id = {}) const
    {
        return { { QStringLiteral("id"), id },
                 { QStringLiteral("providerId"), QStringLiteral("sqlite") },
                 { QStringLiteral("name"), name },
                 { QStringLiteral("colorIndex"), 2 },
                 { QStringLiteral("production"), false },
                 { QStringLiteral("passwordMode"), 1 },
                 { QStringLiteral("password"), kPassword },
                 { QStringLiteral("passwordTouched"), true },
                 { QStringLiteral("file"), file } };
    }

    // Saves a SQLite draft; returns the new connection id.
    QString saveSqlite(ConnectionController &c, const QString &name, const QString &file)
    {
        c.prepare({});
        c.form()->setValue(0, file);
        QSignalSpy saved(&c, &ConnectionController::connectionSaved);
        if (!c.save(sqliteDraft(name, file)))
            return {};
        saved.wait(10000);
        return saved.isEmpty() ? QString() : saved.at(0).at(0).toString();
    }

private slots:
    void init()
    {
        QFile::remove(dbPath());
        QFile::remove(dbPath() + QStringLiteral("-wal"));
        QFile::remove(dbPath() + QStringLiteral("-shm"));
    }

    void startListsEnginesAndLoads();
    void createStoresNoPasswordForSqlite();
    void savedPasswordGoesToKeychainFirstAndSecretKeysAreStripped();
    void askEveryTimeNeverPersists();
    void untouchedPasswordIsNotRewritten();
    void switchingAwayFromSavedDeletesTheKeychainEntry();
    void keychainFailureStoresNothing();
    void invalidDraftIsRejected();
    void editUpdatesInPlace();
    void deleteRemovesTheConnectionAndItsCredential();
    void deleteOfAskEveryTimeSkipsTheKeychainEntry();
    void duplicateCopiesWithoutPassword();
    void openSqliteFileCreatesAndDeduplicates();
    void testConnectionSucceeds();
    void testConnectionFailureHasMessageHintAndDetails();
    void testWithUntouchedSavedPasswordReadsTheKeychain();
    void cancelTestGoesIdle();
    void historyOptOutIsSavedAndAsksAboutExistingHistory();
    void sshSettingsAreValuesAndTheSecretGoesToTheKeychain();
    void sshAskEveryTimeAndUntouchedSecretsAreNotWritten();
    void sshInvalidSettingsRejectTheSave();
    void sshSecretIsDeletedWhenItNoLongerBelongs();
    void deletingAnSshConnectionDeletesItsSecrets();
    void sshKeychainFailureStoresNothing();
    void testConnectionThroughTheTunnelStopsOnAChangedHostKey();
    void testConnectionThroughTheTunnelAsksForAnUnknownHostKey();
    void testConnectionThroughTheTunnelCarriesOnWhenTheKeyMatches();
    void engineWithoutSshHasNoTunnelSection();
};

void TstConnectionController::startListsEnginesAndLoads()
{
    ConnectionController c;
    QVERIFY(!c.started());
    QVERIFY(c.model()->loading());
    startController(c);
    QVERIFY(c.started());
    QVERIFY(!c.engines().isEmpty());
    const QVariantList engines = c.engines();
    QVERIFY(std::ranges::any_of(engines, [](const QVariant &e) {
        return e.toMap().value(QStringLiteral("id")).toString() == QLatin1String("sqlite");
    })); // other providers (postgres) sit beside it
    c.start(); // idempotent
    QCOMPARE(c.model()->count(), 0);

    // Loading the fields of the chosen engine is what the dialog needs.
    const QVariantMap d = c.prepare({});
    QCOMPARE(d.value(QStringLiteral("providerId")).toString(), QStringLiteral("sqlite"));
    QCOMPARE(c.form()->rowCount(), 3); // file, readOnly, busyTimeoutMs
    QVERIFY(c.form()->hasAdvanced());
    QVERIFY(!c.form()->hasSecret());
}

void TstConnectionController::createStoresNoPasswordForSqlite()
{
    ConnectionController c;
    FakeKeychain keychain(c);
    startController(c);
    const QString id = saveSqlite(c, QStringLiteral("Local"), dataFile());
    QVERIFY(!id.isEmpty());

    QCOMPARE(c.model()->count(), 1);
    const auto p = c.model()->profile(QUuid::fromString(id));
    if (!p.has_value())
        QFAIL("no profile");
    QCOMPARE(p->colorIndex, 2);
    QCOMPARE(p->passwordMode, PasswordMode::None); // no Secret field: nothing to keep or ask
    QVERIFY(keychain.calls.isEmpty());
    QVERIFY(c.lastSecretKeys().isEmpty());
    QVERIFY(!c.saving());
    // The password that was in the draft is nowhere on disk.
    QVERIFY(!allDatabaseBytes(dbPath()).contains(kPassword.toUtf8()));
}

void TstConnectionController::savedPasswordGoesToKeychainFirstAndSecretKeysAreStripped()
{
    ConnectionController c;
    FakeKeychain keychain(c);
    startController(c);
    c.prepare({});
    c.form()->setFields(fieldsWithSecret());
    c.form()->setValue(0, dataFile());

    QSignalSpy saved(&c, &ConnectionController::connectionSaved);
    QVERIFY(c.save(sqliteDraft(QStringLiteral("PG"), dataFile())));
    QVERIFY(c.saving());
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000);
    const QString id = saved.at(0).at(0).toString();

    QCOMPARE(c.lastSecretKeys(), QStringList { QStringLiteral("password") });
    QCOMPARE(keychain.calls, QStringList { QStringLiteral("write:%1:%2").arg(id, kPassword) });
    const auto p = c.model()->profile(QUuid::fromString(id));
    if (!p.has_value())
        QFAIL("no profile");
    QCOMPARE(p->passwordMode, PasswordMode::Saved);
    QVERIFY(!p->values.contains(QStringLiteral("password")));
    QVERIFY(!allDatabaseBytes(dbPath()).contains(kPassword.toUtf8()));
}

void TstConnectionController::askEveryTimeNeverPersists()
{
    ConnectionController c;
    FakeKeychain keychain(c);
    startController(c);
    c.prepare({});
    c.form()->setFields(fieldsWithSecret());
    c.form()->setValue(0, dataFile());

    QVariantMap draft = sqliteDraft(QStringLiteral("Ask"), dataFile());
    draft.insert(QStringLiteral("passwordMode"), 2);
    QSignalSpy saved(&c, &ConnectionController::connectionSaved);
    QVERIFY(c.save(draft));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000);

    QVERIFY2(!keychain.called(QStringLiteral("write")), "AskEveryTime wrote to the keychain");
    QCOMPARE(c.lastSecretKeys(), QStringList { QStringLiteral("password") });
    const auto p = c.model()->profile(QUuid::fromString(saved.at(0).at(0).toString()));
    if (!p.has_value())
        QFAIL("no profile");
    QCOMPARE(p->passwordMode, PasswordMode::AskEveryTime);
    QVERIFY(!allDatabaseBytes(dbPath()).contains(kPassword.toUtf8()));
}

void TstConnectionController::untouchedPasswordIsNotRewritten()
{
    ConnectionController c;
    FakeKeychain keychain(c);
    startController(c);
    c.prepare({});
    c.form()->setFields(fieldsWithSecret());
    c.form()->setValue(0, dataFile());
    QVariantMap draft = sqliteDraft(QStringLiteral("Keep"), dataFile());
    draft.insert(QStringLiteral("passwordTouched"), false);
    draft.insert(QStringLiteral("password"), QString());
    QSignalSpy saved(&c, &ConnectionController::connectionSaved);
    QVERIFY(c.save(draft));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000);
    QVERIFY(keychain.calls.isEmpty());
    QCOMPARE(c.model()->get(0).value(QStringLiteral("passwordMode")).toInt(),
             static_cast<int>(PasswordMode::Saved));
}

void TstConnectionController::switchingAwayFromSavedDeletesTheKeychainEntry()
{
    ConnectionController c;
    FakeKeychain keychain(c);
    startController(c);
    c.prepare({});
    c.form()->setFields(fieldsWithSecret());
    c.form()->setValue(0, dataFile());
    QSignalSpy saved(&c, &ConnectionController::connectionSaved);
    QVERIFY(c.save(sqliteDraft(QStringLiteral("Switch"), dataFile())));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000);
    const QString id = saved.at(0).at(0).toString();
    keychain.calls.clear();

    c.prepare(id);
    c.form()->setFields(fieldsWithSecret()); // prepare() loaded the sqlite plugin's own fields
    c.form()->setValue(0, dataFile());
    QVariantMap draft = sqliteDraft(QStringLiteral("Switch"), dataFile(), id);
    draft.insert(QStringLiteral("passwordMode"), 2);
    draft.insert(QStringLiteral("passwordTouched"), false);
    QVERIFY(c.save(draft));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 2, 10000);
    QCOMPARE(keychain.calls, QStringList { QStringLiteral("remove:%1").arg(id) });
    QCOMPARE(c.model()->count(), 1);
}

void TstConnectionController::keychainFailureStoresNothing()
{
    ConnectionController c;
    FakeKeychain keychain(c);
    keychain.failWrite = true;
    startController(c);
    c.prepare({});
    c.form()->setFields(fieldsWithSecret());
    c.form()->setValue(0, dataFile());

    QSignalSpy failed(&c, &ConnectionController::saveFailed);
    QSignalSpy saved(&c, &ConnectionController::connectionSaved);
    QVERIFY(c.save(sqliteDraft(QStringLiteral("NoKeychain"), dataFile())));
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
    QVERIFY(!failed.at(0).at(0).toString().isEmpty());
    QVERIFY(failed.at(0).at(2).toBool()); // a keychain problem: the dialog offers "Ask every time"
    QCOMPARE(saved.count(), 0);
    QCOMPARE(c.model()->count(), 0);
    QVERIFY(!c.saving());

    // The one-click way out: the same draft as "Ask every time" saves.
    QVariantMap draft = sqliteDraft(QStringLiteral("NoKeychain"), dataFile());
    draft.insert(QStringLiteral("passwordMode"), 2);
    QVERIFY(c.save(draft));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000);
}

void TstConnectionController::invalidDraftIsRejected()
{
    ConnectionController c;
    startController(c);
    c.prepare({});
    QVERIFY(!c.form()->isValid()); // the file is required
    QVERIFY(!c.save(sqliteDraft(QStringLiteral("x"), QString())));
    c.form()->setValue(0, dataFile());
    QVERIFY(!c.save(sqliteDraft(QStringLiteral("   "), dataFile()))); // blank name
    QVERIFY(!c.testConnection({})); // no engine chosen
    QCOMPARE(c.model()->count(), 0);
    QVERIFY(!c.saving());
}

void TstConnectionController::editUpdatesInPlace()
{
    ConnectionController c;
    startController(c);
    const QString id = saveSqlite(c, QStringLiteral("Before"), dataFile());

    const QVariantMap d = c.prepare(id);
    QCOMPARE(d.value(QStringLiteral("name")).toString(), QStringLiteral("Before"));
    QCOMPARE(c.form()->values().value(QStringLiteral("file")).toString(), dataFile());
    QSignalSpy saved(&c, &ConnectionController::connectionSaved);
    QVariantMap draft = sqliteDraft(QStringLiteral("After"), dataFile(), id);
    draft.insert(QStringLiteral("production"), true);
    QVERIFY(c.save(draft));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000);
    QCOMPARE(c.model()->count(), 1);
    QCOMPARE(c.model()->get(0).value(QStringLiteral("name")).toString(), QStringLiteral("After"));
    QVERIFY(c.model()->get(0).value(QStringLiteral("production")).toBool());
    QCOMPARE(c.model()->get(0).value(QStringLiteral("connectionId")).toString(), id);
}

void TstConnectionController::deleteRemovesTheConnectionAndItsCredential()
{
    ConnectionController c;
    FakeKeychain keychain(c);
    startController(c);
    c.prepare({});
    c.form()->setFields(fieldsWithSecret());
    c.form()->setValue(0, dataFile());
    QSignalSpy saved(&c, &ConnectionController::connectionSaved);
    QVERIFY(c.save(sqliteDraft(QStringLiteral("Gone"), dataFile())));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000);
    const QString id = saved.at(0).at(0).toString();
    keychain.calls.clear();

    QSignalSpy removed(&c, &ConnectionController::connectionRemoved);
    c.remove(id);
    QTRY_COMPARE_WITH_TIMEOUT(removed.count(), 1, 10000);
    QCOMPARE(c.model()->count(), 0);
    QVERIFY(keychain.called(QStringLiteral("remove:%1").arg(id)));
    QVERIFY(keychain.called(QStringLiteral("forget:%1").arg(id)));
}

void TstConnectionController::deleteOfAskEveryTimeSkipsTheKeychainEntry()
{
    ConnectionController c;
    FakeKeychain keychain(c);
    startController(c);
    const QString id = saveSqlite(c, QStringLiteral("Plain"), dataFile()); // mode None
    QSignalSpy removed(&c, &ConnectionController::connectionRemoved);
    c.remove(id);
    QTRY_COMPARE_WITH_TIMEOUT(removed.count(), 1, 10000);
    QVERIFY(!keychain.called(QStringLiteral("remove")));
    QVERIFY(keychain.called(QStringLiteral("forget:%1").arg(id))); // session memory is cleared
}

void TstConnectionController::duplicateCopiesWithoutPassword()
{
    ConnectionController c;
    FakeKeychain keychain(c);
    startController(c);
    c.prepare({});
    c.form()->setFields(fieldsWithSecret());
    c.form()->setValue(0, dataFile());
    QSignalSpy saved(&c, &ConnectionController::connectionSaved);
    QVERIFY(c.save(sqliteDraft(QStringLiteral("Orig"), dataFile())));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000);
    const QString id = saved.at(0).at(0).toString();
    keychain.calls.clear();

    QSignalSpy dup(&c, &ConnectionController::connectionDuplicated);
    c.duplicate(id);
    QTRY_COMPARE_WITH_TIMEOUT(dup.count(), 1, 10000);
    const QString copyId = dup.at(0).at(0).toString();
    QVERIFY(copyId != id);
    QCOMPARE(c.model()->count(), 2);
    const auto copy = c.model()->profile(QUuid::fromString(copyId));
    if (!copy.has_value())
        QFAIL("no profile");
    QCOMPARE(copy->name, QStringLiteral("Orig (copy)"));
    QCOMPARE(copy->passwordMode, PasswordMode::Saved); // the dialog tells the user to re-enter it
    const auto origin = c.model()->profile(QUuid::fromString(id));
    if (!origin.has_value())
        QFAIL("no profile");
    QCOMPARE(copy->values, origin->values);
    QVERIFY2(keychain.calls.isEmpty(), "a duplicate must not copy or write a password");
    QCOMPARE(saved.count(), 1); // not announced as a plain save
}

void TstConnectionController::openSqliteFileCreatesAndDeduplicates()
{
    ConnectionController c;
    startController(c);
    const QString file = dataFile(QStringLiteral("shop.sqlite"));
    QSignalSpy saved(&c, &ConnectionController::connectionSaved);
    c.openSqliteFile(QUrl::fromLocalFile(file));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000);
    QCOMPARE(c.model()->count(), 1);
    const auto row = c.model()->get(0);
    QCOMPARE(row.value(QStringLiteral("name")).toString(), QStringLiteral("shop.sqlite"));
    QCOMPARE(row.value(QStringLiteral("providerId")).toString(), QStringLiteral("sqlite"));
    QCOMPARE(row.value(QStringLiteral("passwordMode")).toInt(),
             static_cast<int>(PasswordMode::None));

    // The same file again selects the saved connection instead of adding a second one.
    c.openSqliteFile(QUrl::fromLocalFile(file));
    QCOMPARE(saved.count(), 2);
    QCOMPARE(saved.at(1).at(0), saved.at(0).at(0));
    QCOMPARE(c.model()->count(), 1);
}

void TstConnectionController::testConnectionSucceeds()
{
    ConnectionController c;
    startController(c);
    c.prepare({});
    c.form()->setValue(0, dataFile());
    QVERIFY(c.testConnection(sqliteDraft(QStringLiteral("t"), dataFile())));
    QCOMPARE(c.testState(), QStringLiteral("testing"));
    QTRY_COMPARE_WITH_TIMEOUT(c.testState(), QStringLiteral("success"), 15000);
    QVERIFY2(c.testMessage().startsWith(QLatin1String("Connected to SQLite")),
             qPrintable(c.testMessage()));
    c.resetTest();
    QCOMPARE(c.testState(), QStringLiteral("idle"));
}

void TstConnectionController::testConnectionFailureHasMessageHintAndDetails()
{
    ConnectionController c;
    startController(c);
    c.prepare({});
    c.form()->setValue(0, dataFile(QStringLiteral("missing.db")));
    c.form()->setValue(1, true); // read-only: a missing file is an error
    QVERIFY(
        c.testConnection(sqliteDraft(QStringLiteral("t"), dataFile(QStringLiteral("missing.db")))));
    QTRY_COMPARE_WITH_TIMEOUT(c.testState(), QStringLiteral("failed"), 15000);
    QVERIFY(!c.testMessage().isEmpty());
    QVERIFY(c.testDetail().contains(QLatin1String("missing.db")));
}

void TstConnectionController::testWithUntouchedSavedPasswordReadsTheKeychain()
{
    ConnectionController c;
    FakeKeychain keychain(c);
    startController(c);
    c.prepare({});
    c.form()->setFields(fieldsWithSecret());
    c.form()->setValue(0, dataFile());
    QSignalSpy saved(&c, &ConnectionController::connectionSaved);
    QVERIFY(c.save(sqliteDraft(QStringLiteral("Stored"), dataFile())));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000);
    const QString id = saved.at(0).at(0).toString();
    keychain.calls.clear();

    c.prepare(id);
    c.form()->setFields(fieldsWithSecret());
    c.form()->setValue(0, dataFile());
    QVariantMap draft = sqliteDraft(QStringLiteral("Stored"), dataFile(), id);
    draft.insert(QStringLiteral("passwordTouched"), false);
    draft.insert(QStringLiteral("password"), QString());
    QVERIFY(c.testConnection(draft));
    QVERIFY(keychain.called(QStringLiteral("read:%1").arg(id)));
    QTRY_COMPARE_WITH_TIMEOUT(c.testState(), QStringLiteral("success"), 15000);

    // A typed password is used as it is, without asking the keychain.
    keychain.calls.clear();
    draft.insert(QStringLiteral("passwordTouched"), true);
    draft.insert(QStringLiteral("password"), QStringLiteral("typed"));
    QVERIFY(c.testConnection(draft));
    QVERIFY(!keychain.called(QStringLiteral("read")));
    QTRY_COMPARE_WITH_TIMEOUT(c.testState(), QStringLiteral("success"), 15000);
}

void TstConnectionController::cancelTestGoesIdle()
{
    ConnectionController c;
    startController(c);
    c.prepare({});
    c.form()->setValue(0, dataFile());
    QVERIFY(c.testConnection(sqliteDraft(QStringLiteral("t"), dataFile())));
    c.cancelTest();
    QCOMPARE(c.testState(), QStringLiteral("idle"));
    QTest::qWait(300); // a late result of the dropped test must not come back
    QCOMPARE(c.testState(), QStringLiteral("idle"));
}

// H4 privacy: recordHistory is part of the draft; turning it off on a connection that was
// recording asks (historyDisabled) after saving, once, and deletes nothing by itself.
void TstConnectionController::historyOptOutIsSavedAndAsksAboutExistingHistory()
{
    ConnectionController c;
    startController(c);
    const QString id = saveSqlite(c, QStringLiteral("Db"), dataFile());
    QVERIFY(c.prepare(id).value(QStringLiteral("recordHistory")).toBool()); // on by default
    QVERIFY(c.prepare({}).value(QStringLiteral("recordHistory")).toBool());

    QSignalSpy saved(&c, &ConnectionController::connectionSaved);
    QSignalSpy disabled(&c, &ConnectionController::historyDisabled);
    c.prepare(id);
    QVariantMap draft = sqliteDraft(QStringLiteral("Db"), dataFile(), id);
    draft.insert(QStringLiteral("recordHistory"), false);
    QVERIFY(c.save(draft));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000);
    QCOMPARE(disabled.count(), 1);
    QCOMPARE(disabled.at(0).at(0).toString(), id);
    QCOMPARE(disabled.at(0).at(1).toString(), QStringLiteral("Db"));
    QVERIFY(!c.prepare(id).value(QStringLiteral("recordHistory")).toBool()); // stored

    // Saving again with it still off is not a change: no question.
    c.prepare(id);
    QVERIFY(c.save(draft));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 2, 10000);
    QCOMPARE(disabled.count(), 1);

    // A new connection that starts without history has nothing to delete.
    c.prepare({});
    c.form()->setValue(0, dataFile(QStringLiteral("other.db")));
    QVariantMap fresh = sqliteDraft(QStringLiteral("Quiet"), dataFile(QStringLiteral("other.db")));
    fresh.insert(QStringLiteral("recordHistory"), false);
    QVERIFY(c.save(fresh));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 3, 10000);
    QCOMPARE(disabled.count(), 1);
}

namespace {

// A connection form that offers the SSH tunnel (as PostgreSQL does), filled in for a bastion.
void sshForm(ConnectionController &c, const QString &file, const QString &auth = {})
{
    c.prepare({});
    c.form()->setFields(fieldsWithSecret(), true);
    c.form()->setValue(0, file);
    c.form()->setSshEnabled(true);
    c.form()->setSshHost(QStringLiteral(" bastion.example.com "));
    c.form()->setSshUser(QStringLiteral("deploy"));
    if (!auth.isEmpty())
        c.form()->setSshAuth(auth);
    if (auth == QLatin1String("key"))
        c.form()->setSshKeyPath(QStringLiteral("C:/keys/id_ed25519"));
}

const QString kSshSecret = QStringLiteral("S3cret-SSH-UNIQUE-PW");

} // namespace

void TstConnectionController::sshSettingsAreValuesAndTheSecretGoesToTheKeychain()
{
    ConnectionController c;
    FakeKeychain keychain(c);
    startController(c);
    sshForm(c, dataFile());

    QVariantMap draft = sqliteDraft(QStringLiteral("Tunnelled"), dataFile());
    draft.insert(QStringLiteral("sshSecret"), kSshSecret);
    draft.insert(QStringLiteral("sshSecretTouched"), true);
    QSignalSpy saved(&c, &ConnectionController::connectionSaved);
    QVERIFY(c.save(draft));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000);
    const QString id = saved.at(0).at(0).toString();

    // The database password first, then the tunnel's; both before the profile is stored.
    QCOMPARE(keychain.calls,
             (QStringList { QStringLiteral("write:%1:%2").arg(id, kPassword),
                            QStringLiteral("ssh-write:%1:%2:%3")
                                .arg(id)
                                .arg(static_cast<int>(SecretKind::SshPassword))
                                .arg(kSshSecret) }));
    const auto p = c.model()->profile(QUuid::fromString(id));
    if (!p.has_value())
        QFAIL("no profile");
    const SshTunnelConfig config = SshTunnelConfig::fromValues(p->values);
    QVERIFY(config.enabled);
    QCOMPARE(config.host, QStringLiteral("bastion.example.com")); // trimmed
    QCOMPARE(config.port, 22);
    QCOMPARE(config.user, QStringLiteral("deploy"));
    QCOMPARE(config.auth, SshAuth::Password);
    QCOMPARE(p->values.value(QStringLiteral("ssh.secretMode")).toString(),
             QStringLiteral("keychain"));
    QVERIFY(!allDatabaseBytes(dbPath()).contains(kSshSecret.toUtf8()));
    const QModelIndex row = c.model()->index(0);
    QVERIFY(c.model()->data(row, ConnectionListModel::SshSecretSavedRole).toBool());
}

void TstConnectionController::sshAskEveryTimeAndUntouchedSecretsAreNotWritten()
{
    ConnectionController c;
    FakeKeychain keychain(c);
    startController(c);
    sshForm(c, dataFile(), QStringLiteral("key"));
    c.form()->setSshSecretMode(2);
    QVariantMap draft = sqliteDraft(QStringLiteral("Ask"), dataFile());
    draft.insert(QStringLiteral("passwordMode"), 2);
    draft.insert(QStringLiteral("sshSecret"), kSshSecret);
    draft.insert(QStringLiteral("sshSecretTouched"), true);
    QSignalSpy saved(&c, &ConnectionController::connectionSaved);
    QVERIFY(c.save(draft));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000);
    QVERIFY2(!keychain.called(QStringLiteral("ssh-write")), "Ask every time wrote the secret");
    QVERIFY(!allDatabaseBytes(dbPath()).contains(kSshSecret.toUtf8()));
    const auto p = c.model()->profile(QUuid::fromString(saved.at(0).at(0).toString()));
    if (!p.has_value())
        QFAIL("no profile");
    QCOMPARE(p->values.value(QStringLiteral("ssh.secretMode")).toString(), QStringLiteral("ask"));
    QCOMPARE(p->values.value(QStringLiteral("ssh.keyPath")).toString(),
             QStringLiteral("C:/keys/id_ed25519"));
    QVERIFY(
        !c.model()->data(c.model()->index(0), ConnectionListModel::SshSecretSavedRole).toBool());

    // Keychain mode with an untouched field: nothing to write either.
    ConnectionController c2;
    FakeKeychain keychain2(c2);
    startController(c2);
    sshForm(c2, dataFile(QStringLiteral("two.db")));
    QVariantMap untouched = sqliteDraft(QStringLiteral("Kept"), dataFile(QStringLiteral("two.db")));
    untouched.insert(QStringLiteral("passwordMode"), 2);
    QSignalSpy saved2(&c2, &ConnectionController::connectionSaved);
    QVERIFY(c2.save(untouched));
    QTRY_COMPARE_WITH_TIMEOUT(saved2.count(), 1, 10000);
    QVERIFY(!keychain2.called(QStringLiteral("ssh-write")));
}

void TstConnectionController::sshInvalidSettingsRejectTheSave()
{
    ConnectionController c;
    FakeKeychain keychain(c);
    startController(c);
    sshForm(c, dataFile());
    QVERIFY(c.form()->isValid());
    c.form()->setSshHost(QString());
    QVERIFY(!c.form()->isValid());
    QVERIFY(!c.form()->sshHostError().isEmpty());
    QVERIFY(!c.save(sqliteDraft(QStringLiteral("Bad"), dataFile())));
    QVERIFY(!c.testConnection(sqliteDraft(QStringLiteral("Bad"), dataFile())));
    c.form()->setSshHost(QStringLiteral("h"));
    c.form()->setSshPort(QStringLiteral("70000"));
    QVERIFY(!c.form()->isValid());
    QVERIFY(!c.form()->sshPortError().isEmpty());
    c.form()->setSshEnabled(false); // switched off: its fields do not matter
    QVERIFY(c.form()->isValid());
}

void TstConnectionController::sshSecretIsDeletedWhenItNoLongerBelongs()
{
    ConnectionController c;
    FakeKeychain keychain(c);
    startController(c);
    sshForm(c, dataFile());
    QVariantMap draft = sqliteDraft(QStringLiteral("Switch"), dataFile());
    draft.insert(QStringLiteral("sshSecret"), kSshSecret);
    draft.insert(QStringLiteral("sshSecretTouched"), true);
    QSignalSpy saved(&c, &ConnectionController::connectionSaved);
    QVERIFY(c.save(draft));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000);
    const QString id = saved.at(0).at(0).toString();
    keychain.calls.clear();

    // Same kind, untouched: the entry stays.
    QVariantMap again = sqliteDraft(QStringLiteral("Switch"), dataFile(), id);
    again.insert(QStringLiteral("passwordMode"), 2);
    QVERIFY(c.save(again));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 2, 10000);
    QVERIFY(!keychain.called(QStringLiteral("ssh-delete")));

    // Password -> key: the old password entry is deleted.
    c.form()->setSshAuth(QStringLiteral("key"));
    c.form()->setSshKeyPath(QStringLiteral("C:/k"));
    QVERIFY(c.save(again));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 3, 10000);
    QVERIFY(keychain.called(
        QStringLiteral("ssh-delete:%1:%2").arg(id).arg(static_cast<int>(SecretKind::SshPassword))));
    keychain.calls.clear();

    // A key may have kept a passphrase there: switching the tunnel off deletes that one too.
    c.form()->setSshEnabled(false);
    QVERIFY(c.save(again));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 4, 10000);
    QVERIFY(keychain.called(QStringLiteral("ssh-delete:%1:%2")
                                .arg(id)
                                .arg(static_cast<int>(SecretKind::SshPassphrase))));
}

void TstConnectionController::deletingAnSshConnectionDeletesItsSecrets()
{
    ConnectionController c;
    FakeKeychain keychain(c);
    startController(c);
    sshForm(c, dataFile());
    QSignalSpy saved(&c, &ConnectionController::connectionSaved);
    QVERIFY(c.save(sqliteDraft(QStringLiteral("Gone"), dataFile())));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000);
    const QString id = saved.at(0).at(0).toString();
    keychain.calls.clear();

    QSignalSpy removed(&c, &ConnectionController::connectionRemoved);
    c.remove(id);
    QTRY_COMPARE_WITH_TIMEOUT(removed.count(), 1, 10000);
    QVERIFY(keychain.called(QStringLiteral("remove:%1").arg(id))); // the database password
    QVERIFY(keychain.called(
        QStringLiteral("ssh-delete:%1:%2").arg(id).arg(static_cast<int>(SecretKind::SshPassword))));
    QVERIFY(keychain.called(QStringLiteral("ssh-delete:%1:%2")
                                .arg(id)
                                .arg(static_cast<int>(SecretKind::SshPassphrase))));
    QVERIFY(keychain.called(QStringLiteral("forget:%1").arg(id)));
}

void TstConnectionController::sshKeychainFailureStoresNothing()
{
    ConnectionController c;
    FakeKeychain keychain(c);
    keychain.failSshWrite = true;
    startController(c);
    sshForm(c, dataFile());
    QVariantMap draft = sqliteDraft(QStringLiteral("NoKeychain"), dataFile());
    draft.insert(QStringLiteral("passwordMode"), 2);
    draft.insert(QStringLiteral("sshSecret"), kSshSecret);
    draft.insert(QStringLiteral("sshSecretTouched"), true);
    QSignalSpy failed(&c, &ConnectionController::saveFailed);
    QVERIFY(c.save(draft));
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
    QVERIFY(failed.at(0).at(2).toBool()); // a keychain problem: the dialog offers "Ask every time"
    QVERIFY(!c.saving());
    QCOMPARE(c.model()->count(), 0);
}

namespace {

// Host key answers without a server: `status` is what the "checker" finds.
struct FakeHostKeys
{
    explicit FakeHostKeys(ConnectionController &controller)
        : c(controller)
    {
        HostKeyBackend b;
        b.check = [this](const SshTunnelConfig &config) {
            checked << config.host;
            QMetaObject::invokeMethod(
                c.preflight(),
                [this] {
                    HostKeyInfo info;
                    info.status = status;
                    info.keyType = QStringLiteral("ssh-ed25519");
                    info.fingerprint = QStringLiteral("SHA256:abc");
                    info.knownFingerprint = QStringLiteral("SHA256:old");
                    c.preflight()->onHostKeyChecked(info);
                },
                Qt::QueuedConnection);
        };
        b.cancel = [] {};
        b.accept = [this](const HostKeyInfo &info, bool remember) {
            accepted << (remember ? QStringLiteral("remember") : QStringLiteral("once"));
            c.preflight()->onHostKeyAccepted(info);
        };
        c.preflight()->setHostKeyBackend(b);
        SshSecretBackend s;
        s.read = [](QUuid, SecretKind) {};
        s.session = [](QUuid, SecretKind) { return std::optional<QString>(); };
        s.remember = [](QUuid, SecretKind, const QString &) {};
        s.forget = [](QUuid, SecretKind) {};
        c.preflight()->setSecretBackend(s);
    }

    ConnectionController &c;
    HostKeyStatus status = HostKeyStatus::Match;
    QStringList checked;
    QStringList accepted;
};

} // namespace

void TstConnectionController::testConnectionThroughTheTunnelStopsOnAChangedHostKey()
{
    ConnectionController c;
    FakeKeychain keychain(c);
    FakeHostKeys hostKeys(c);
    hostKeys.status = HostKeyStatus::Mismatch;
    startController(c);
    sshForm(c, dataFile(), QStringLiteral("agent"));
    QSignalSpy mismatch(c.preflight(), &ConnectionPreflight::hostKeyMismatch);
    QVariantMap draft = sqliteDraft(QStringLiteral("T"), dataFile());
    draft.insert(QStringLiteral("passwordMode"), 2);
    QVERIFY(c.testConnection(draft));
    QCOMPARE(c.testState(), QStringLiteral("testing"));
    QTRY_COMPARE_WITH_TIMEOUT(c.testState(), QStringLiteral("failed"), 10000);
    QVERIFY(c.testMessage().contains(QStringLiteral("host key has changed")));
    QVERIFY(c.testMessage().contains(QStringLiteral("SHA256:old")));
    QVERIFY(c.testMessage().contains(QStringLiteral("SHA256:abc")));
    QCOMPARE(mismatch.count(), 1);
    QVERIFY(hostKeys.accepted.isEmpty()); // there is no way to accept it
}

void TstConnectionController::testConnectionThroughTheTunnelAsksForAnUnknownHostKey()
{
    ConnectionController c;
    FakeKeychain keychain(c);
    FakeHostKeys hostKeys(c);
    hostKeys.status = HostKeyStatus::Unknown;
    startController(c);
    sshForm(c, dataFile(), QStringLiteral("agent"));
    QSignalSpy asked(c.preflight(), &ConnectionPreflight::hostKeyRequested);
    QVariantMap draft = sqliteDraft(QStringLiteral("T"), dataFile());
    draft.insert(QStringLiteral("passwordMode"), 2);

    QVERIFY(c.testConnection(draft));
    QTRY_COMPARE_WITH_TIMEOUT(asked.count(), 1, 10000);
    QCOMPARE(asked.at(0).at(0).toString(), QStringLiteral("bastion.example.com"));
    QCOMPARE(asked.at(0).at(3).toString(), QStringLiteral("SHA256:abc"));
    // Cancel: the test ends quietly, without an error message.
    c.preflight()->cancelHostKey();
    QCOMPARE(c.testState(), QStringLiteral("idle"));
    QVERIFY(c.testMessage().isEmpty());
}

void TstConnectionController::testConnectionThroughTheTunnelCarriesOnWhenTheKeyMatches()
{
    ConnectionController c;
    FakeKeychain keychain(c);
    FakeHostKeys hostKeys(c);
    hostKeys.status = HostKeyStatus::Match;
    startController(c);
    sshForm(c, dataFile(), QStringLiteral("agent"));
    QVariantMap draft = sqliteDraft(QStringLiteral("T"), dataFile());
    draft.insert(QStringLiteral("passwordMode"), 2);
    QVERIFY(c.testConnection(draft));
    QTRY_COMPARE_WITH_TIMEOUT(c.testState(), QStringLiteral("failed"), 10000);
    // The fingerprint passed; SQLite has no tunnel, so the registry is what refuses now. That the
    // error comes from there shows the test went on to create the connection.
    QCOMPARE(hostKeys.checked, QStringList { QStringLiteral("bastion.example.com") });
    QVERIFY(!c.testMessage().contains(QStringLiteral("host key")));
}

void TstConnectionController::engineWithoutSshHasNoTunnelSection()
{
    ConnectionController c;
    startController(c);
    c.prepare({}); // SQLite
    QVERIFY(!c.form()->sshAvailable());
    c.form()->setSshEnabled(true);
    QVERIFY(!c.form()->sshEnabled()); // the section is not there: it can't be on
    QVERIFY(!c.form()->values().contains(QStringLiteral("ssh.enabled")));
    c.form()->setValue(0, dataFile());
    QVERIFY(c.form()->isValid()); // the missing SSH host does not count
}

QTEST_MAIN(TstConnectionController)
#include "tst_connection_controller.moc"
