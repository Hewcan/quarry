#include "ui/connection_form_model.h"
#include "ui/connection_preflight.h"

#include <QSignalSpy>
#include <QtTest>

#include <optional>

using namespace quarry;
using namespace quarry::ui;

namespace {

// What the signals said, in order. Secrets is not a registered meta type, so no QSignalSpy.
struct Recorder
{
    struct SecretAsk
    {
        QString connectionId;
        QString name;
        bool passphrase = false;
        bool retry = false;
    };
    struct HostAsk
    {
        QString host;
        int port = 0;
        QString keyType;
        QString fingerprint;
    };
    struct Mismatch
    {
        QString host;
        QString known;
        QString received;
    };

    explicit Recorder(ConnectionPreflight &p)
    {
        QObject::connect(&p, &ConnectionPreflight::ready, &p, [this](int id, const Secrets &s) {
            readyIds << id;
            secrets << s;
        });
        QObject::connect(&p, &ConnectionPreflight::failed, &p, [this](int id, const Error &e) {
            failedIds << id;
            errors << e;
        });
        QObject::connect(&p, &ConnectionPreflight::cancelled, &p,
                         [this](int id) { cancelledIds << id; });
        QObject::connect(&p, &ConnectionPreflight::secretRequested, &p,
                         [this](const QString &id, const QString &name, bool pass, bool retry) {
                             asks.push_back({ id, name, pass, retry });
                         });
        QObject::connect(
            &p, &ConnectionPreflight::hostKeyRequested, &p,
            [this](const QString &host, int port, const QString &type, const QString &print) {
                hostAsks.push_back({ host, port, type, print });
            });
        QObject::connect(
            &p, &ConnectionPreflight::hostKeyMismatch, &p,
            [this](const QString &host, int, const QString &, const QString &known,
                   const QString &received) { mismatches.push_back({ host, known, received }); });
    }

    int total() const { return int(readyIds.size() + failedIds.size() + cancelledIds.size()); }

    QList<int> readyIds;
    QList<Secrets> secrets;
    QList<int> failedIds;
    QList<Error> errors;
    QList<int> cancelledIds;
    QList<SecretAsk> asks;
    QList<HostAsk> hostAsks;
    QList<Mismatch> mismatches;
};

// A keychain and a host key checker that answer from the event loop, like the real ones.
struct Fakes
{
    explicit Fakes(ConnectionPreflight &preflight)
        : p(preflight)
    {
        SshSecretBackend s;
        s.read = [this](QUuid id, SecretKind kind) {
            log << QStringLiteral("read:%1").arg(int(kind));
            QMetaObject::invokeMethod(
                &p,
                [this, id, kind] {
                    if (keychain.contains(kind))
                        p.onSecretRead(id, kind, keychain.value(kind));
                    else
                        p.onSecretFailed(
                            id, kind,
                            Error { ErrorKind::NotFound, QStringLiteral("none"), {}, {}, {} });
                },
                Qt::QueuedConnection);
        };
        s.session = [this](QUuid, SecretKind kind) -> std::optional<QString> {
            if (session.contains(kind))
                return session.value(kind);
            return std::nullopt;
        };
        s.remember
            = [this](QUuid, SecretKind kind, const QString &value) { session.insert(kind, value); };
        s.forget = [this](QUuid, SecretKind kind) {
            log << QStringLiteral("forget:%1").arg(int(kind));
            session.remove(kind);
        };
        p.setSecretBackend(s);

        HostKeyBackend h;
        h.check = [this](const SshTunnelConfig &config) {
            log << QStringLiteral("check:%1").arg(config.host);
            checks << config.host;
            QMetaObject::invokeMethod(
                &p,
                [this] {
                    if (checkFails) {
                        p.onHostKeyFailed(Error {
                            ErrorKind::Connection, QStringLiteral("unreachable"), {}, {}, {} });
                        return;
                    }
                    HostKeyInfo info;
                    info.status = status;
                    info.keyType = QStringLiteral("ssh-ed25519");
                    info.fingerprint = QStringLiteral("SHA256:received");
                    info.knownFingerprint = QStringLiteral("SHA256:known");
                    p.onHostKeyChecked(info);
                },
                Qt::QueuedConnection);
        };
        h.cancel = [this] { log << QStringLiteral("cancel-check"); };
        h.accept = [this](const HostKeyInfo &info, bool remember) {
            log << QStringLiteral("accept:%1").arg(remember ? "remember" : "once");
            if (acceptFails)
                p.onHostKeyFailed(Error {
                    ErrorKind::Internal, QStringLiteral("cannot write known_hosts"), {}, {}, {} });
            else
                p.onHostKeyAccepted(info); // answers before accept() returns
        };
        p.setHostKeyBackend(h);
    }

    ConnectionPreflight &p;
    QHash<SecretKind, QString> keychain;
    QHash<SecretKind, QString> session;
    HostKeyStatus status = HostKeyStatus::Match;
    bool checkFails = false;
    bool acceptFails = false;
    QStringList log;
    QStringList checks;
};

ConnectionProfile profile(const QString &auth, const QString &mode = QStringLiteral("keychain"),
                          bool ssh = true)
{
    ConnectionProfile p;
    p.id = QUuid::createUuid();
    p.name = QStringLiteral("Prod");
    p.providerId = QStringLiteral("postgres");
    p.values.insert(QStringLiteral("host"), QStringLiteral("db.internal"));
    SshTunnelConfig c;
    c.enabled = ssh;
    c.host = QStringLiteral("bastion");
    c.port = 2222;
    c.user = QStringLiteral("deploy");
    c.auth = auth == QLatin1String("key") ? SshAuth::Key
        : auth == QLatin1String("agent")  ? SshAuth::Agent
                                          : SshAuth::Password;
    c.keyPath = QStringLiteral("/k");
    c.toValues(p.values);
    p.values.insert(QLatin1String(kSshSecretModeKey), mode);
    return p;
}

} // namespace

class TstConnectionPreflight : public QObject
{
    Q_OBJECT

private slots:
    void profilesTellWhatTheyNeed();
    void keychainSecretAndMatchingKeyGiveReady();
    void aKeyWithoutSavedPassphraseGoesOnWithAnEmptyOne();
    void aKeychainFailureThatIsNotMissingIsAnError();
    void theAgentNeedsNoSecret();
    void askEveryTimePromptsOnceAndThenRemembers();
    void askEveryTimeNeverPromptsWhenNotInteractive();
    void aSecretGivenByTheCallerSkipsKeychainAndPrompt();
    void dismissingTheSecretPromptCancels();
    void unknownKeyAsksAndTrustOnceDoesNotRemember();
    void unknownKeyAndTrustAndRemember();
    void dismissingTheFingerprintPromptCancels();
    void unknownKeyNeverPromptsWhenNotInteractive();
    void aChangedKeyFailsWithBothFingerprintsAndCannotBeAccepted();
    void aCheckerFailureIsAnOrdinaryError();
    void aFailureToRememberTheKeyIsAnError();
    void requestsRunOneAtATime();
    void cancellingARequestSendsNothing();
    void cancellingWhileCheckingStopsTheChecker();
    void aNonSshProfileIsNotTouchedByTheOpenOutcome();
    void aRefusedSshLoginForgetsTheSecretAndTheNextPromptSaysSo();
    void aRaceOnTheHostKeyGetsTheSameTreatment();
};

void TstConnectionPreflight::profilesTellWhatTheyNeed()
{
    QVERIFY(ConnectionPreflight::usesSsh(profile(QStringLiteral("password"))));
    QVERIFY(!ConnectionPreflight::usesSsh(profile(QStringLiteral("password"), {}, false)));
    QCOMPARE(ConnectionPreflight::secretKindOf(profile(QStringLiteral("password"))),
             std::optional<SecretKind>(SecretKind::SshPassword));
    QCOMPARE(ConnectionPreflight::secretKindOf(profile(QStringLiteral("key"))),
             std::optional<SecretKind>(SecretKind::SshPassphrase));
    QVERIFY(!ConnectionPreflight::secretKindOf(profile(QStringLiteral("agent"))));
    QVERIFY(!ConnectionPreflight::secretKindOf(profile(QStringLiteral("password"), {}, false)));
    QVERIFY(ConnectionPreflight::asksSecret(
        profile(QStringLiteral("password"), QStringLiteral("ask"))));
    QVERIFY(!ConnectionPreflight::asksSecret(profile(QStringLiteral("password"))));
}

void TstConnectionPreflight::keychainSecretAndMatchingKeyGiveReady()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    fakes.keychain.insert(SecretKind::SshPassword, QStringLiteral("ssh-pw"));
    Recorder rec(p);

    const int id = p.begin(profile(QStringLiteral("password")), QStringLiteral("db-pw"));
    QCOMPARE(rec.total(), 0); // never from inside begin()
    QTRY_COMPARE(rec.readyIds.count(), 1);
    QCOMPARE(rec.readyIds.at(0), id);
    QCOMPARE(rec.secrets.at(0).password, QStringLiteral("db-pw"));
    QCOMPARE(rec.secrets.at(0).sshPassword, QStringLiteral("ssh-pw"));
    QVERIFY(rec.secrets.at(0).sshPassphrase.isEmpty()); // only the one that matches
    QCOMPARE(fakes.log,
             (QStringList { QStringLiteral("read:1"), QStringLiteral("check:bastion") }));
    QVERIFY(rec.asks.isEmpty());
}

void TstConnectionPreflight::aKeyWithoutSavedPassphraseGoesOnWithAnEmptyOne()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    Recorder rec(p);
    p.begin(profile(QStringLiteral("key")), {});
    QTRY_COMPARE(rec.readyIds.count(), 1);
    QVERIFY(rec.secrets.at(0).sshPassphrase.isEmpty());
    QVERIFY(rec.failedIds.isEmpty());

    fakes.keychain.insert(SecretKind::SshPassphrase, QStringLiteral("phrase"));
    p.begin(profile(QStringLiteral("key")), {});
    QTRY_COMPARE(rec.readyIds.count(), 2);
    QCOMPARE(rec.secrets.at(1).sshPassphrase, QStringLiteral("phrase"));
    QVERIFY(rec.secrets.at(1).sshPassword.isEmpty());
}

void TstConnectionPreflight::aKeychainFailureThatIsNotMissingIsAnError()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    SshSecretBackend s;
    s.read = [&p](QUuid id, SecretKind kind) {
        QMetaObject::invokeMethod(
            &p,
            [&p, id, kind] {
                p.onSecretFailed(id, kind,
                                 Error { ErrorKind::Unsupported,
                                         QStringLiteral("The system keychain is not available."),
                                         {},
                                         {},
                                         {} });
            },
            Qt::QueuedConnection);
    };
    p.setSecretBackend(s);
    Recorder rec(p);
    p.begin(profile(QStringLiteral("password")), {});
    QTRY_COMPARE(rec.failedIds.count(), 1);
    QCOMPARE(rec.errors.at(0).kind, ErrorKind::Unsupported);
    QVERIFY(fakes.checks.isEmpty()); // no fingerprint check for a connection that cannot log in
}

void TstConnectionPreflight::theAgentNeedsNoSecret()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    Recorder rec(p);
    p.begin(profile(QStringLiteral("agent"), QStringLiteral("ask")), QStringLiteral("db"));
    QTRY_COMPARE(rec.readyIds.count(), 1);
    QCOMPARE(fakes.log, QStringList { QStringLiteral("check:bastion") });
    QVERIFY(rec.asks.isEmpty());
}

void TstConnectionPreflight::askEveryTimePromptsOnceAndThenRemembers()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    Recorder rec(p);
    const ConnectionProfile prof = profile(QStringLiteral("password"), QStringLiteral("ask"));

    p.begin(prof, QStringLiteral("db"));
    QTRY_COMPARE(rec.asks.count(), 1);
    QCOMPARE(rec.asks.at(0).connectionId, prof.id.toString(QUuid::WithoutBraces));
    QCOMPARE(rec.asks.at(0).name, QStringLiteral("Prod"));
    QVERIFY(!rec.asks.at(0).passphrase);
    QVERIFY(!rec.asks.at(0).retry);
    QVERIFY(!fakes.log.contains(QStringLiteral("read:1"))); // the keychain is not asked

    p.provideSecret(QStringLiteral("someone-else"), QStringLiteral("x")); // not the one asked
    QTest::qWait(20);
    QCOMPARE(rec.total(), 0);

    p.provideSecret(prof.id.toString(QUuid::WithoutBraces), QStringLiteral("typed"));
    QTRY_COMPARE(rec.readyIds.count(), 1);
    QCOMPARE(rec.secrets.at(0).sshPassword, QStringLiteral("typed"));

    // The next connection of this run does not ask again.
    p.begin(prof, QStringLiteral("db"));
    QTRY_COMPARE(rec.readyIds.count(), 2);
    QCOMPARE(rec.secrets.at(1).sshPassword, QStringLiteral("typed"));
    QCOMPARE(rec.asks.count(), 1);

    // A key asks for its passphrase, and an empty one is an answer.
    const ConnectionProfile key = profile(QStringLiteral("key"), QStringLiteral("ask"));
    p.begin(key, {});
    QTRY_COMPARE(rec.asks.count(), 2);
    QVERIFY(rec.asks.at(1).passphrase);
    p.provideSecret(key.id.toString(QUuid::WithoutBraces), QString());
    QTRY_COMPARE(rec.readyIds.count(), 3);
}

void TstConnectionPreflight::askEveryTimeNeverPromptsWhenNotInteractive()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    Recorder rec(p);
    const ConnectionProfile prof = profile(QStringLiteral("password"), QStringLiteral("ask"));
    p.begin(prof, {}, false);
    QTRY_COMPARE(rec.failedIds.count(), 1);
    QVERIFY(rec.asks.isEmpty());
    QVERIFY(!rec.errors.at(0).message.isEmpty());

    // Known for this run: it goes on.
    fakes.session.insert(SecretKind::SshPassword, QStringLiteral("remembered"));
    p.begin(prof, {}, false);
    QTRY_COMPARE(rec.readyIds.count(), 1);
    QCOMPARE(rec.secrets.at(0).sshPassword, QStringLiteral("remembered"));
}

void TstConnectionPreflight::aSecretGivenByTheCallerSkipsKeychainAndPrompt()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    Recorder rec(p);
    p.begin(profile(QStringLiteral("password"), QStringLiteral("ask")), QStringLiteral("db"), true,
            QStringLiteral("from-the-form"));
    QTRY_COMPARE(rec.readyIds.count(), 1);
    QCOMPARE(rec.secrets.at(0).sshPassword, QStringLiteral("from-the-form"));
    QVERIFY(rec.asks.isEmpty());
    QVERIFY(!fakes.log.contains(QStringLiteral("read:1")));
    QVERIFY(fakes.session.isEmpty()); // typing it to test it does not make Quarry remember it
}

void TstConnectionPreflight::dismissingTheSecretPromptCancels()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    Recorder rec(p);
    const ConnectionProfile prof = profile(QStringLiteral("password"), QStringLiteral("ask"));
    const int id = p.begin(prof, {});
    QTRY_COMPARE(rec.asks.count(), 1);
    p.cancelSecret(prof.id.toString(QUuid::WithoutBraces));
    QCOMPARE(rec.cancelledIds, QList<int> { id });
    QVERIFY(rec.failedIds.isEmpty()); // no error for the user to read
    QVERIFY(fakes.checks.isEmpty());
}

void TstConnectionPreflight::unknownKeyAsksAndTrustOnceDoesNotRemember()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    fakes.status = HostKeyStatus::Unknown;
    Recorder rec(p);
    p.begin(profile(QStringLiteral("agent")), {});
    QTRY_COMPARE(rec.hostAsks.count(), 1);
    QCOMPARE(rec.hostAsks.at(0).host, QStringLiteral("bastion"));
    QCOMPARE(rec.hostAsks.at(0).port, 2222);
    QCOMPARE(rec.hostAsks.at(0).keyType, QStringLiteral("ssh-ed25519"));
    QCOMPARE(rec.hostAsks.at(0).fingerprint, QStringLiteral("SHA256:received"));
    QCOMPARE(rec.total(), 0); // waits for the answer

    p.trustHostKey(false);
    QCOMPARE(rec.readyIds.count(), 1);
    QCOMPARE(fakes.log.last(), QStringLiteral("accept:once"));
}

void TstConnectionPreflight::unknownKeyAndTrustAndRemember()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    fakes.status = HostKeyStatus::Unknown;
    Recorder rec(p);
    p.begin(profile(QStringLiteral("agent")), QStringLiteral("db"));
    QTRY_COMPARE(rec.hostAsks.count(), 1);
    p.trustHostKey(true);
    QCOMPARE(rec.readyIds.count(), 1);
    QCOMPARE(rec.secrets.at(0).password, QStringLiteral("db"));
    QCOMPARE(fakes.log.last(), QStringLiteral("accept:remember"));
    p.trustHostKey(true); // nothing is waiting any more
    QCOMPARE(rec.readyIds.count(), 1);
}

void TstConnectionPreflight::dismissingTheFingerprintPromptCancels()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    fakes.status = HostKeyStatus::Unknown;
    Recorder rec(p);
    const int id = p.begin(profile(QStringLiteral("agent")), {});
    QTRY_COMPARE(rec.hostAsks.count(), 1);
    p.cancelHostKey();
    QCOMPARE(rec.cancelledIds, QList<int> { id });
    QVERIFY(rec.failedIds.isEmpty());
    QVERIFY(rec.readyIds.isEmpty());
    for (const QString &entry : std::as_const(fakes.log))
        QVERIFY(!entry.startsWith(QLatin1String("accept"))); // nothing was trusted
}

void TstConnectionPreflight::unknownKeyNeverPromptsWhenNotInteractive()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    fakes.status = HostKeyStatus::Unknown;
    Recorder rec(p);
    p.begin(profile(QStringLiteral("agent")), {}, false);
    QTRY_COMPARE(rec.failedIds.count(), 1);
    QVERIFY(rec.hostAsks.isEmpty());
    QCOMPARE(rec.errors.at(0).sqlState, QString::fromLatin1(kSshUntrustedState));
    QVERIFY(rec.errors.at(0).message.contains(QStringLiteral("bastion")));
}

void TstConnectionPreflight::aChangedKeyFailsWithBothFingerprintsAndCannotBeAccepted()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    fakes.status = HostKeyStatus::Mismatch;
    Recorder rec(p);
    p.begin(profile(QStringLiteral("agent")), {});
    QTRY_COMPARE(rec.failedIds.count(), 1);
    const Error e = rec.errors.at(0);
    QCOMPARE(e.sqlState, QString::fromLatin1(kSshMismatchState));
    QVERIFY(e.message.contains(QStringLiteral("host key has changed")));
    QVERIFY(e.message.contains(QStringLiteral("SHA256:known")));
    QVERIFY(e.message.contains(QStringLiteral("SHA256:received")));
    QVERIFY(rec.hostAsks.isEmpty());
    QCOMPARE(rec.mismatches.count(), 1);
    QCOMPARE(rec.mismatches.at(0).known, QStringLiteral("SHA256:known"));
    QCOMPARE(rec.mismatches.at(0).received, QStringLiteral("SHA256:received"));
    p.trustHostKey(true); // there is no way to accept it
    for (const QString &entry : std::as_const(fakes.log))
        QVERIFY(!entry.startsWith(QLatin1String("accept")));
}

void TstConnectionPreflight::aCheckerFailureIsAnOrdinaryError()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    fakes.checkFails = true;
    Recorder rec(p);
    p.begin(profile(QStringLiteral("agent")), {});
    QTRY_COMPARE(rec.failedIds.count(), 1);
    QCOMPARE(rec.errors.at(0).message, QStringLiteral("unreachable"));
    QVERIFY(rec.mismatches.isEmpty());
    QVERIFY(rec.hostAsks.isEmpty());
}

void TstConnectionPreflight::aFailureToRememberTheKeyIsAnError()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    fakes.status = HostKeyStatus::Unknown;
    fakes.acceptFails = true;
    Recorder rec(p);
    p.begin(profile(QStringLiteral("agent")), {});
    QTRY_COMPARE(rec.hostAsks.count(), 1);
    p.trustHostKey(true);
    QCOMPARE(rec.failedIds.count(), 1);
    QCOMPARE(rec.errors.at(0).message, QStringLiteral("cannot write known_hosts"));
    QVERIFY(rec.readyIds.isEmpty());
}

void TstConnectionPreflight::requestsRunOneAtATime()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    fakes.status = HostKeyStatus::Unknown;
    Recorder rec(p);
    const int first = p.begin(profile(QStringLiteral("agent")), QStringLiteral("one"));
    const int second = p.begin(profile(QStringLiteral("agent")), QStringLiteral("two"));
    QVERIFY(second > first);
    QTRY_COMPARE(rec.hostAsks.count(), 1);
    QTest::qWait(30);
    QCOMPARE(fakes.checks.count(), 1); // the second waits: one fingerprint question at a time
    p.trustHostKey(false);
    QCOMPARE(rec.readyIds, QList<int> { first });
    QTRY_COMPARE(rec.hostAsks.count(), 2);
    p.trustHostKey(false);
    QCOMPARE(rec.readyIds, (QList<int> { first, second }));
    QCOMPARE(rec.secrets.at(1).password, QStringLiteral("two"));
}

void TstConnectionPreflight::cancellingARequestSendsNothing()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    Recorder rec(p);
    const int dropped = p.begin(profile(QStringLiteral("agent")), {});
    const int kept = p.begin(profile(QStringLiteral("agent")), {});
    p.cancel(dropped); // before its start even ran
    QTRY_COMPARE(rec.readyIds.count(), 1);
    QCOMPARE(rec.readyIds, QList<int> { kept });
    QTest::qWait(30);
    QCOMPARE(rec.total(), 1);
    p.cancel(12345); // unknown: nothing happens
}

void TstConnectionPreflight::cancellingWhileCheckingStopsTheChecker()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    Recorder rec(p);
    const int id = p.begin(profile(QStringLiteral("agent")), {});
    QTRY_VERIFY(fakes.checks.count() == 1);
    p.cancel(id);
    QVERIFY(fakes.log.contains(QStringLiteral("cancel-check")));
    QTest::qWait(30); // the late answer of the checker finds nobody waiting
    QCOMPARE(rec.total(), 0);
}

void TstConnectionPreflight::aNonSshProfileIsNotTouchedByTheOpenOutcome()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    const ConnectionProfile plain = profile(QStringLiteral("password"), {}, false);
    const Error e { ErrorKind::Authentication, QStringLiteral("no"), {}, {}, {} };
    QCOMPARE(p.openFailed(plain, e).message, QStringLiteral("no"));
    QVERIFY(!ConnectionPreflight::isSshAuthError(plain, e));
    QVERIFY(fakes.log.isEmpty());

    // A database login refusal (it has a SQLSTATE) is not the tunnel's.
    const ConnectionProfile tunnelled = profile(QStringLiteral("password"));
    const Error db {
        ErrorKind::Authentication, QStringLiteral("no"), {}, QStringLiteral("28P01"), {}
    };
    QVERIFY(!ConnectionPreflight::isSshAuthError(tunnelled, db));
    p.openFailed(tunnelled, db);
    QVERIFY(fakes.log.isEmpty());
}

void TstConnectionPreflight::aRefusedSshLoginForgetsTheSecretAndTheNextPromptSaysSo()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    Recorder rec(p);
    const ConnectionProfile prof = profile(QStringLiteral("password"), QStringLiteral("ask"));
    const QString id = prof.id.toString(QUuid::WithoutBraces);
    p.begin(prof, {});
    QTRY_COMPARE(rec.asks.count(), 1);
    p.provideSecret(id, QStringLiteral("wrong"));
    QTRY_COMPARE(rec.readyIds.count(), 1);
    QCOMPARE(fakes.session.value(SecretKind::SshPassword), QStringLiteral("wrong"));

    const Error refused { ErrorKind::Authentication,
                          QStringLiteral("The SSH server rejected the login."),
                          {},
                          QString::fromLatin1(kSshAuthState),
                          {} };
    QVERIFY(ConnectionPreflight::isSshAuthError(prof, refused));
    QCOMPARE(p.openFailed(prof, refused).message, refused.message);
    QVERIFY(!fakes.session.contains(SecretKind::SshPassword)); // not tried again silently

    p.begin(prof, {});
    QTRY_COMPARE(rec.asks.count(), 2);
    QVERIFY(rec.asks.at(1).retry);

    p.provideSecret(id, QStringLiteral("right"));
    QTRY_COMPARE(rec.readyIds.count(), 2);
    p.openSucceeded(prof.id);
    p.begin(prof, {}); // remembered: no prompt
    QTRY_COMPARE(rec.readyIds.count(), 3);
    fakes.session.clear();
    p.begin(prof, {});
    QTRY_COMPARE(rec.asks.count(), 3);
    QVERIFY(!rec.asks.at(2).retry); // a success clears the warning
}

void TstConnectionPreflight::aRaceOnTheHostKeyGetsTheSameTreatment()
{
    ConnectionPreflight p;
    Fakes fakes(p);
    Recorder rec(p);
    const ConnectionProfile prof = profile(QStringLiteral("agent"));
    const Error raced { ErrorKind::Connection,
                        QStringLiteral("changed"),
                        QStringLiteral("SHA256:late"),
                        QString::fromLatin1(kSshMismatchState),
                        {} };
    QVERIFY(ConnectionPreflight::isHostKeyError(raced));
    const Error shown = p.openFailed(prof, raced);
    QVERIFY(shown.message.contains(QStringLiteral("host key has changed")));
    QVERIFY(shown.message.contains(QStringLiteral("SHA256:late")));
    QCOMPARE(shown.sqlState, QString::fromLatin1(kSshMismatchState));
    QCOMPARE(rec.mismatches.count(), 1); // the same prominent notice
    QVERIFY(fakes.checks.isEmpty()); // and no automatic retry

    // Untrusted: the core's own message is kept, and there is no retry either.
    const Error untrusted { ErrorKind::Connection,
                            QStringLiteral("not trusted yet"),
                            {},
                            QString::fromLatin1(kSshUntrustedState),
                            {} };
    QVERIFY(ConnectionPreflight::isHostKeyError(untrusted));
    QCOMPARE(p.openFailed(prof, untrusted).message, untrusted.message);
    QVERIFY(fakes.checks.isEmpty());
}

QTEST_MAIN(TstConnectionPreflight)
#include "tst_connection_preflight.moc"
