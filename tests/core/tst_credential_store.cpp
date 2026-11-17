#include <quarry/credential_store.h>

#include <QSignalSpy>
#include <QtTest>

using namespace quarry;

class TstCredentialStore : public QObject
{
    Q_OBJECT

private slots:
    void sessionCacheSemantics();
    void sessionCacheKeepsKindsApart();
    void forgetForSessionClearsEveryKind();
    void keychainRoundtrip();
    void keychainRoundtripSshKinds();
};

void TstCredentialStore::sessionCacheSemantics()
{
    CredentialStore store;
    const QUuid a = QUuid::createUuid();
    const QUuid b = QUuid::createUuid();
    QVERIFY(!store.sessionPassword(a).has_value());

    store.rememberForSession(a, QStringLiteral("one"));
    store.rememberForSession(b, QStringLiteral("two"));
    QCOMPARE(store.sessionPassword(a).value_or(QString()), QStringLiteral("one"));

    store.rememberForSession(a, QStringLiteral("three")); // overwrite
    QCOMPARE(store.sessionPassword(a).value_or(QString()), QStringLiteral("three"));
    QCOMPARE(store.sessionPassword(b).value_or(QString()), QStringLiteral("two"));

    store.rememberForSession(a, QString()); // an empty password is still a remembered answer
    QVERIFY(store.sessionPassword(a).has_value());

    store.forgetForSession(a);
    QVERIFY(!store.sessionPassword(a).has_value());
    QVERIFY(store.sessionPassword(b).has_value());
    store.forgetForSession(a); // idempotent
}

void TstCredentialStore::sessionCacheKeepsKindsApart()
{
    CredentialStore store;
    const QUuid id = QUuid::createUuid();
    store.rememberForSession(id, QStringLiteral("db"));
    store.rememberForSession(id, SecretKind::SshPassword, QStringLiteral("ssh"));
    store.rememberForSession(id, SecretKind::SshPassphrase, QStringLiteral("phrase"));

    QCOMPARE(store.sessionPassword(id).value_or(QString()), QStringLiteral("db"));
    QCOMPARE(store.sessionSecret(id, SecretKind::Password).value_or(QString()),
             QStringLiteral("db"));
    QCOMPARE(store.sessionSecret(id, SecretKind::SshPassword).value_or(QString()),
             QStringLiteral("ssh"));
    QCOMPARE(store.sessionSecret(id, SecretKind::SshPassphrase).value_or(QString()),
             QStringLiteral("phrase"));

    store.forgetForSession(id, SecretKind::SshPassword);
    QVERIFY(!store.sessionSecret(id, SecretKind::SshPassword).has_value());
    QVERIFY(store.sessionSecret(id, SecretKind::SshPassphrase).has_value());
    QVERIFY(store.sessionPassword(id).has_value());

    store.rememberForSession(id, SecretKind::SshPassphrase, QStringLiteral("again"));
    QCOMPARE(store.sessionSecret(id, SecretKind::SshPassphrase).value_or(QString()),
             QStringLiteral("again"));
}

void TstCredentialStore::forgetForSessionClearsEveryKind()
{
    CredentialStore store;
    const QUuid id = QUuid::createUuid();
    const QUuid other = QUuid::createUuid();
    store.rememberForSession(id, QStringLiteral("db"));
    store.rememberForSession(id, SecretKind::SshPassword, QStringLiteral("ssh"));
    store.rememberForSession(id, SecretKind::SshPassphrase, QStringLiteral("phrase"));
    store.rememberForSession(other, SecretKind::SshPassword, QStringLiteral("keep"));

    store.forgetForSession(id);
    QVERIFY(!store.sessionPassword(id).has_value());
    QVERIFY(!store.sessionSecret(id, SecretKind::SshPassword).has_value());
    QVERIFY(!store.sessionSecret(id, SecretKind::SshPassphrase).has_value());
    QVERIFY(store.sessionSecret(other, SecretKind::SshPassword).has_value());
}

void TstCredentialStore::keychainRoundtrip()
{
    if (qEnvironmentVariable("QUARRY_TEST_KEYCHAIN") != QLatin1String("1"))
        QSKIP("set QUARRY_TEST_KEYCHAIN=1 to test the OS keychain");

    CredentialStore store;
    QSignalSpy read(&store, &CredentialStore::passwordRead);
    QSignalSpy written(&store, &CredentialStore::passwordWritten);
    QSignalSpy deleted(&store, &CredentialStore::passwordDeleted);
    QSignalSpy failed(&store, &CredentialStore::failed);

    const QUuid id = QUuid::createUuid(); // unique per run
    const QString password = QStringLiteral("päss wörd € 123");
    // Always clean up, even if a check below fails.
    const auto cleanup = qScopeGuard([&] {
        const qsizetype count = deleted.count();
        store.deletePassword(id);
        static_cast<void>(QTest::qWaitFor([&] { return deleted.count() > count; }, 10000));
    });

    store.writePassword(id, password);
    QTRY_COMPARE_WITH_TIMEOUT(written.count(), 1, 10000);
    QCOMPARE(failed.count(), 0);

    store.readPassword(id);
    QTRY_COMPARE_WITH_TIMEOUT(read.count(), 1, 10000);
    QCOMPARE(read.at(0).at(0).toUuid(), id);
    QCOMPARE(read.at(0).at(1).toString(), password);

    store.deletePassword(id);
    QTRY_COMPARE_WITH_TIMEOUT(deleted.count(), 1, 10000);

    store.readPassword(id);
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
    QCOMPARE(failed.at(0).at(0).toUuid(), id);
    QCOMPARE(failed.at(0).at(1).value<Error>().kind, ErrorKind::NotFound);

    store.deletePassword(id); // deleting a missing entry counts as success
    QTRY_COMPARE_WITH_TIMEOUT(deleted.count(), 2, 10000);
}

void TstCredentialStore::keychainRoundtripSshKinds()
{
    if (qEnvironmentVariable("QUARRY_TEST_KEYCHAIN") != QLatin1String("1"))
        QSKIP("set QUARRY_TEST_KEYCHAIN=1 to test the OS keychain");

    CredentialStore store;
    QSignalSpy read(&store, &CredentialStore::secretRead);
    QSignalSpy written(&store, &CredentialStore::secretWritten);
    QSignalSpy deleted(&store, &CredentialStore::secretDeleted);
    QSignalSpy failed(&store, &CredentialStore::secretFailed);
    QSignalSpy legacyFailed(&store, &CredentialStore::failed);

    const QUuid id = QUuid::createUuid();
    const auto cleanup = qScopeGuard([&] {
        for (const SecretKind kind : { SecretKind::SshPassword, SecretKind::SshPassphrase }) {
            const qsizetype count = deleted.count();
            store.deleteSecret(id, kind);
            static_cast<void>(QTest::qWaitFor([&] { return deleted.count() > count; }, 10000));
        }
    });

    store.writeSecret(id, SecretKind::SshPassword, QStringLiteral("ssh-secret"));
    store.writeSecret(id, SecretKind::SshPassphrase, QStringLiteral("phrase-secret"));
    QTRY_COMPARE_WITH_TIMEOUT(written.count(), 2, 10000);

    store.readSecret(id, SecretKind::SshPassphrase);
    QTRY_COMPARE_WITH_TIMEOUT(read.count(), 1, 10000);
    QCOMPARE(read.at(0).at(1).value<SecretKind>(), SecretKind::SshPassphrase);
    QCOMPARE(read.at(0).at(2).toString(), QStringLiteral("phrase-secret"));

    store.deleteSecret(id, SecretKind::SshPassword);
    QTRY_COMPARE_WITH_TIMEOUT(deleted.count(), 1, 10000);
    store.readSecret(id, SecretKind::SshPassword);
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
    QCOMPARE(failed.at(0).at(2).value<Error>().kind, ErrorKind::NotFound);
    QCOMPARE(legacyFailed.count(), 0); // the old signal is for the database password only
}

QTEST_MAIN(TstCredentialStore)
#include "tst_credential_store.moc"
