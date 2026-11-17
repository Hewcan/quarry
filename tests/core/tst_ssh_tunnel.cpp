#include "tcp_test_server.h"

#include <quarry/cancellation.h>
#include <quarry/ssh_host_key.h>

#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include <atomic>
#include <memory>
#include <optional>
#include <ssh/ssh_tunnel.h>
#include <thread>

using namespace quarry;
using quarry::test::TcpTestServer;

namespace {

// Real SSH server, from QUARRY_TEST_SSH_URL=ssh://user:password@host:port. The tests that use
// it are skipped when it is not set (see ARCHITECTURE.md section 6.8).
struct RealServer
{
    SshTunnelConfig config;
    Secrets secrets;
    std::optional<QString> targetHost; // QUARRY_TEST_SSH_TARGET=host:port, an echo server
    int targetPort = 0;
};

std::optional<RealServer> realServer()
{
    const QString raw = qEnvironmentVariable("QUARRY_TEST_SSH_URL");
    if (raw.isEmpty())
        return std::nullopt;
    const QUrl url(raw);
    if (!url.isValid() || url.host().isEmpty())
        return std::nullopt;
    RealServer server;
    server.config.enabled = true;
    server.config.host = url.host();
    server.config.port = url.port(22);
    server.config.user = QUrl::fromPercentEncoding(url.userName().toUtf8());
    server.config.auth = SshAuth::Password;
    server.secrets.sshPassword = QUrl::fromPercentEncoding(url.password().toUtf8());

    const QString target = qEnvironmentVariable("QUARRY_TEST_SSH_TARGET");
    const qsizetype colon = target.lastIndexOf(QLatin1Char(':'));
    if (colon > 0) {
        server.targetHost = target.left(colon);
        server.targetPort = target.mid(colon + 1).toInt();
    }
    return server;
}

#define REQUIRE_REAL_SERVER()                                                                      \
    const std::optional<RealServer> serverOpt = realServer();                                      \
    if (!serverOpt)                                                                                \
        QSKIP("set QUARRY_TEST_SSH_URL=ssh://user:password@host:port to test a real SSH server");  \
    const RealServer &server = *serverOpt

SshTunnelConfig offlineConfig(int port)
{
    SshTunnelConfig config;
    config.enabled = true;
    config.host = QStringLiteral("127.0.0.1");
    config.port = port;
    config.user = QStringLiteral("nobody");
    config.auth = SshAuth::Password;
    return config;
}

} // namespace

class TstSshTunnel : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    // No server needed
    void closeIsIdempotent();
    void incompleteConfigIsRejected();
    void unresolvableHostFailsAsConnection();
    void closedPortFailsFastAsConnection();
    void serverThatClosesFailsCleanly();
    void serverThatIsNotSshFailsCleanly();
    void alreadyCancelledTokenReturnsCancelled();
    void cancelTokenDuringHandshake();
    void requestCancelDuringHandshake();
    void checkerFailsAgainstClosingServer();
    void checkerCancelEmitsNothing();
    void checkerNewCheckReplacesRunningOne();
    void checkerRejectsIncompleteConfig();
    void checkerAcceptTrustsForSessionOrRemembers();
    void checkerAcceptReportsWriteFailure();

    // QUARRY_TEST_SSH_URL
    void realCheckerReportsUnknownKey();
    void realUnknownThenTrustedThenOpen();
    void realMismatchBlocksOpen();
    void realWrongPasswordIsAuthenticationError();
    void realForwardsEcho();
    void realManySimultaneousClients();
    void realLargeTransferArrivesIntact();
    void realCloseDuringTransfer();

private:
    // Learns the server key with the checker and trusts it for this run.
    HostKeyInfo trust(const std::shared_ptr<KnownHostsStore> &store, const SshTunnelConfig &config);
};

HostKeyInfo TstSshTunnel::trust(const std::shared_ptr<KnownHostsStore> &store,
                                const SshTunnelConfig &config)
{
    SshHostKeyChecker checker(store);
    QSignalSpy checked(&checker, &SshHostKeyChecker::checked);
    QSignalSpy failed(&checker, &SshHostKeyChecker::failed);
    checker.check(config);
    if (!QTest::qWaitFor([&] { return checked.count() + failed.count() > 0; }, 20000))
        return {};
    if (checked.count() != 1)
        return {};
    const auto info = checked.at(0).at(0).value<HostKeyInfo>();
    checker.accept(info, false);
    return info;
}

void TstSshTunnel::closeIsIdempotent()
{
    SshTunnel tunnel;
    tunnel.close();
    tunnel.close();
    QVERIFY(!tunnel.isOpen());
    QCOMPARE(tunnel.localPort(), 0);
    tunnel.requestCancel(); // nothing to cancel: must not crash
}

void TstSshTunnel::incompleteConfigIsRejected()
{
    QTemporaryDir dir;
    KnownHostsStore store(dir.filePath(QStringLiteral("known_hosts")));
    SshTunnel tunnel;
    SshTunnelConfig config = offlineConfig(22);
    config.host.clear();
    const Result<int> result
        = tunnel.open(config, {}, store, QStringLiteral("db"), 5432, CancellationToken());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().kind, ErrorKind::Connection);
    QVERIFY(!result.error().message.isEmpty());
    tunnel.close();
    tunnel.close();
}

void TstSshTunnel::unresolvableHostFailsAsConnection()
{
    QTemporaryDir dir;
    KnownHostsStore store(dir.filePath(QStringLiteral("known_hosts")));
    SshTunnel tunnel;
    SshTunnelConfig config = offlineConfig(22);
    config.host = QStringLiteral("not a valid host name");
    QElapsedTimer timer;
    timer.start();
    const Result<int> result
        = tunnel.open(config, {}, store, QStringLiteral("db"), 5432, CancellationToken());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().kind, ErrorKind::Connection);
    QVERIFY(result.error().message.contains(QLatin1String("not a valid host name")));
    QVERIFY2(timer.elapsed() < 10000, qPrintable(QString::number(timer.elapsed())));
}

void TstSshTunnel::closedPortFailsFastAsConnection()
{
    QTemporaryDir dir;
    KnownHostsStore store(dir.filePath(QStringLiteral("known_hosts")));
    SshTunnel tunnel;
    QElapsedTimer timer;
    timer.start();
    const Result<int> result = tunnel.open(offlineConfig(test::unusedPort()), {}, store,
                                           QStringLiteral("db"), 5432, CancellationToken());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().kind, ErrorKind::Connection);
    QVERIFY(!result.error().detail.isEmpty()); // the operating system's text
    QVERIFY2(timer.elapsed() < 10000, qPrintable(QString::number(timer.elapsed())));
    QVERIFY(!tunnel.isOpen());
}

void TstSshTunnel::serverThatClosesFailsCleanly()
{
    TcpTestServer server(TcpTestServer::Mode::Close);
    QTemporaryDir dir;
    KnownHostsStore store(dir.filePath(QStringLiteral("known_hosts")));
    SshTunnel tunnel;
    QElapsedTimer timer;
    timer.start();
    const Result<int> result = tunnel.open(offlineConfig(server.port()), {}, store,
                                           QStringLiteral("db"), 5432, CancellationToken());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().kind, ErrorKind::Connection);
    QVERIFY(!result.error().message.isEmpty());
    QVERIFY(timer.elapsed() < 10000);
}

void TstSshTunnel::serverThatIsNotSshFailsCleanly()
{
    TcpTestServer server(TcpTestServer::Mode::Garbage);
    QTemporaryDir dir;
    KnownHostsStore store(dir.filePath(QStringLiteral("known_hosts")));
    SshTunnel tunnel;
    QElapsedTimer timer;
    timer.start();
    const Result<int> result = tunnel.open(offlineConfig(server.port()), {}, store,
                                           QStringLiteral("db"), 5432, CancellationToken());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().kind, ErrorKind::Connection);
    QVERIFY(result.error().message.contains(QLatin1String("SSH")));
    QVERIFY(timer.elapsed() < 10000);
    tunnel.close();
}

void TstSshTunnel::alreadyCancelledTokenReturnsCancelled()
{
    TcpTestServer server(TcpTestServer::Mode::Silent);
    QTemporaryDir dir;
    KnownHostsStore store(dir.filePath(QStringLiteral("known_hosts")));
    CancellationSource source;
    source.cancel();
    SshTunnel tunnel;
    const Result<int> result = tunnel.open(offlineConfig(server.port()), {}, store,
                                           QStringLiteral("db"), 5432, source.token());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().kind, ErrorKind::Cancelled);
}

void TstSshTunnel::cancelTokenDuringHandshake()
{
    TcpTestServer server(TcpTestServer::Mode::Silent); // accepts, never sends the banner
    QTemporaryDir dir;
    KnownHostsStore store(dir.filePath(QStringLiteral("known_hosts")));
    CancellationSource source;
    std::thread canceller([&source] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        source.cancel();
    });
    SshTunnel tunnel;
    QElapsedTimer timer;
    timer.start();
    const Result<int> result = tunnel.open(offlineConfig(server.port()), {}, store,
                                           QStringLiteral("db"), 5432, source.token());
    canceller.join();
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().kind, ErrorKind::Cancelled);
    QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
    QVERIFY(!tunnel.isOpen());
}

void TstSshTunnel::requestCancelDuringHandshake()
{
    TcpTestServer server(TcpTestServer::Mode::Silent);
    QTemporaryDir dir;
    KnownHostsStore store(dir.filePath(QStringLiteral("known_hosts")));
    SshTunnel tunnel;
    std::thread canceller([&tunnel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        tunnel.requestCancel();
    });
    QElapsedTimer timer;
    timer.start();
    const Result<int> result = tunnel.open(offlineConfig(server.port()), {}, store,
                                           QStringLiteral("db"), 5432, CancellationToken());
    canceller.join();
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().kind, ErrorKind::Cancelled);
    QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));

    // The next open() starts with a clean slate.
    const Result<int> again = tunnel.open(offlineConfig(test::unusedPort()), {}, store,
                                          QStringLiteral("db"), 5432, CancellationToken());
    QVERIFY(!again.has_value());
    QCOMPARE(again.error().kind, ErrorKind::Connection);
}

void TstSshTunnel::checkerFailsAgainstClosingServer()
{
    TcpTestServer server(TcpTestServer::Mode::Close);
    QTemporaryDir dir;
    auto store = std::make_shared<KnownHostsStore>(dir.filePath(QStringLiteral("known_hosts")));
    SshHostKeyChecker checker(store);
    QSignalSpy checked(&checker, &SshHostKeyChecker::checked);
    QSignalSpy failed(&checker, &SshHostKeyChecker::failed);

    checker.check(offlineConfig(server.port()));
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 20000);
    QCOMPARE(checked.count(), 0);
    QCOMPARE(failed.at(0).at(0).value<Error>().kind, ErrorKind::Connection);

    // A checker can be reused after a failure.
    checker.check(offlineConfig(test::unusedPort()));
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 2, 20000);
    QCOMPARE(failed.at(1).at(0).value<Error>().kind, ErrorKind::Connection);
}

void TstSshTunnel::checkerCancelEmitsNothing()
{
    TcpTestServer server(TcpTestServer::Mode::Silent);
    QTemporaryDir dir;
    auto store = std::make_shared<KnownHostsStore>(dir.filePath(QStringLiteral("known_hosts")));
    SshHostKeyChecker checker(store);
    QSignalSpy checked(&checker, &SshHostKeyChecker::checked);
    QSignalSpy failed(&checker, &SshHostKeyChecker::failed);

    checker.check(offlineConfig(server.port()));
    QTest::qWait(300);
    checker.cancel();
    QTest::qWait(700);
    QCOMPARE(checked.count(), 0);
    QCOMPARE(failed.count(), 0);
}

void TstSshTunnel::checkerNewCheckReplacesRunningOne()
{
    TcpTestServer silent(TcpTestServer::Mode::Silent);
    TcpTestServer closing(TcpTestServer::Mode::Close);
    QTemporaryDir dir;
    auto store = std::make_shared<KnownHostsStore>(dir.filePath(QStringLiteral("known_hosts")));
    SshHostKeyChecker checker(store);
    QSignalSpy failed(&checker, &SshHostKeyChecker::failed);

    checker.check(offlineConfig(silent.port()));
    QTest::qWait(200);
    checker.check(offlineConfig(closing.port()));
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 20000);
    QTest::qWait(300);
    QCOMPARE(failed.count(), 1); // the replaced check stayed silent
    QCOMPARE(failed.at(0).at(0).value<Error>().kind, ErrorKind::Connection);
}

void TstSshTunnel::checkerRejectsIncompleteConfig()
{
    QTemporaryDir dir;
    auto store = std::make_shared<KnownHostsStore>(dir.filePath(QStringLiteral("known_hosts")));
    SshHostKeyChecker checker(store);
    QSignalSpy failed(&checker, &SshHostKeyChecker::failed);
    SshTunnelConfig config = offlineConfig(22);
    config.user.clear();
    checker.check(config);
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
    QVERIFY(!failed.at(0).at(0).value<Error>().message.isEmpty());
}

void TstSshTunnel::checkerAcceptTrustsForSessionOrRemembers()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("known_hosts"));
    auto store = std::make_shared<KnownHostsStore>(path);
    SshHostKeyChecker checker(store);
    QSignalSpy accepted(&checker, &SshHostKeyChecker::accepted);
    QSignalSpy failed(&checker, &SshHostKeyChecker::failed);

    HostKeyInfo once;
    once.host = QStringLiteral("a.example");
    once.key = QByteArray("\0\0\0\x0bssh-ed25519\0\0\0\x04wxyz", 4 + 11 + 4 + 4);
    checker.accept(once, false);
    QCOMPARE(accepted.count(), 1);
    QCOMPARE(store->classify(once).status, HostKeyStatus::Match);
    QVERIFY(!QFileInfo::exists(path));

    HostKeyInfo saved = once;
    saved.host = QStringLiteral("b.example");
    checker.accept(saved, true);
    QCOMPARE(accepted.count(), 2);
    QVERIFY(QFileInfo::exists(path));
    QCOMPARE(KnownHostsStore(path).classify(saved).status, HostKeyStatus::Match);
    QCOMPARE(failed.count(), 0);
}

void TstSshTunnel::checkerAcceptReportsWriteFailure()
{
    QTemporaryDir dir;
    const QString blocker = dir.filePath(QStringLiteral("file"));
    QFile file(blocker);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    auto store = std::make_shared<KnownHostsStore>(blocker + QLatin1String("/known_hosts"));
    SshHostKeyChecker checker(store);
    QSignalSpy accepted(&checker, &SshHostKeyChecker::accepted);
    QSignalSpy failed(&checker, &SshHostKeyChecker::failed);

    HostKeyInfo info;
    info.host = QStringLiteral("a.example");
    info.key = QByteArray("\0\0\0\x0bssh-ed25519\0\0\0\x04wxyz", 4 + 11 + 4 + 4);
    checker.accept(info, true);
    QCOMPARE(accepted.count(), 0);
    QCOMPARE(failed.count(), 1);
}

void TstSshTunnel::realCheckerReportsUnknownKey()
{
    REQUIRE_REAL_SERVER();
    QTemporaryDir dir;
    auto store = std::make_shared<KnownHostsStore>(dir.filePath(QStringLiteral("known_hosts")));
    SshHostKeyChecker checker(store);
    QSignalSpy checked(&checker, &SshHostKeyChecker::checked);
    QSignalSpy failed(&checker, &SshHostKeyChecker::failed);

    checker.check(server.config);
    QTRY_COMPARE_WITH_TIMEOUT(checked.count() + failed.count(), 1, 20000);
    QCOMPARE(failed.count(), 0);
    const auto info = checked.at(0).at(0).value<HostKeyInfo>();
    QCOMPARE(info.status, HostKeyStatus::Unknown);
    QVERIFY(info.fingerprint.startsWith(QLatin1String("SHA256:")));
    QVERIFY(info.keyType.startsWith(QLatin1String("ssh-"))
            || info.keyType.startsWith(QLatin1String("ecdsa-")));
    QVERIFY(!info.key.isEmpty());
}

void TstSshTunnel::realUnknownThenTrustedThenOpen()
{
    REQUIRE_REAL_SERVER();
    QTemporaryDir dir;
    auto store = std::make_shared<KnownHostsStore>(dir.filePath(QStringLiteral("known_hosts")));
    const QString targetHost = server.targetHost.value_or(QStringLiteral("127.0.0.1"));
    const int targetPort = server.targetHost ? server.targetPort : 5432;

    SshTunnel tunnel;
    const Result<int> untrusted = tunnel.open(server.config, server.secrets, *store, targetHost,
                                              targetPort, CancellationToken());
    QVERIFY(!untrusted.has_value());
    QCOMPARE(untrusted.error().kind, ErrorKind::Connection);
    QCOMPARE(untrusted.error().sqlState, QString::fromLatin1(kSshUntrustedState));
    QVERIFY(untrusted.error().detail.startsWith(QLatin1String("SHA256:")));

    const HostKeyInfo info = trust(store, server.config);
    QVERIFY(!info.key.isEmpty());
    QCOMPARE(untrusted.error().detail, info.fingerprint);

    const Result<int> opened = tunnel.open(server.config, server.secrets, *store, targetHost,
                                           targetPort, CancellationToken());
    QVERIFY2(opened.has_value(),
             qPrintable(opened.has_value() ? QString() : opened.error().detail));
    QVERIFY(opened.value() > 0);
    QVERIFY(tunnel.isOpen());
    QCOMPARE(tunnel.localPort(), opened.value());
    tunnel.close();
    tunnel.close();
    QVERIFY(!tunnel.isOpen());
}

void TstSshTunnel::realMismatchBlocksOpen()
{
    REQUIRE_REAL_SERVER();
    QTemporaryDir dir;
    auto store = std::make_shared<KnownHostsStore>(dir.filePath(QStringLiteral("known_hosts")));
    SshHostKeyChecker checker(store);
    QSignalSpy checked(&checker, &SshHostKeyChecker::checked);
    checker.check(server.config);
    QTRY_COMPARE_WITH_TIMEOUT(checked.count(), 1, 20000);
    const auto real = checked.at(0).at(0).value<HostKeyInfo>();

    // Same algorithm, different key: what a man in the middle would present.
    HostKeyInfo forged = real;
    forged.key = real.key;
    forged.key[forged.key.size() - 1] = static_cast<char>(~forged.key[forged.key.size() - 1]);
    forged.fingerprint.clear();
    QVERIFY(store->remember(forged).has_value());

    SshTunnel tunnel;
    const Result<int> result = tunnel.open(server.config, server.secrets, *store,
                                           QStringLiteral("127.0.0.1"), 5432, CancellationToken());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().kind, ErrorKind::Connection);
    QCOMPARE(result.error().sqlState, QString::fromLatin1(kSshMismatchState));
    QCOMPARE(result.error().detail, real.fingerprint);
}

void TstSshTunnel::realWrongPasswordIsAuthenticationError()
{
    REQUIRE_REAL_SERVER();
    QTemporaryDir dir;
    auto store = std::make_shared<KnownHostsStore>(dir.filePath(QStringLiteral("known_hosts")));
    QVERIFY(!trust(store, server.config).key.isEmpty());

    Secrets wrong = server.secrets;
    wrong.sshPassword = server.secrets.sshPassword + QStringLiteral("-wrong");
    SshTunnel tunnel;
    const Result<int> result = tunnel.open(server.config, wrong, *store,
                                           QStringLiteral("127.0.0.1"), 5432, CancellationToken());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().kind, ErrorKind::Authentication);
    QVERIFY(!tunnel.isOpen());
}

void TstSshTunnel::realForwardsEcho()
{
    REQUIRE_REAL_SERVER();
    TcpTestServer echo(TcpTestServer::Mode::Echo);
    const QString targetHost = server.targetHost.value_or(QStringLiteral("127.0.0.1"));
    const int targetPort = server.targetHost ? server.targetPort : echo.port();

    QTemporaryDir dir;
    auto store = std::make_shared<KnownHostsStore>(dir.filePath(QStringLiteral("known_hosts")));
    QVERIFY(!trust(store, server.config).key.isEmpty());
    SshTunnel tunnel;
    const Result<int> opened = tunnel.open(server.config, server.secrets, *store, targetHost,
                                           targetPort, CancellationToken());
    QVERIFY2(opened.has_value(),
             qPrintable(opened.has_value() ? QString() : opened.error().detail));

    const ssh::NativeSocket client = test::connectLocal(opened.value());
    QVERIFY(client != ssh::kInvalidSocket);
    const QByteArray payload("hello through the tunnel");
    QVERIFY(test::sendAll(client, payload));
    QCOMPARE(test::receiveUpTo(client, payload.size()), payload);
    ssh::closeSocket(client);
}

void TstSshTunnel::realManySimultaneousClients()
{
    REQUIRE_REAL_SERVER();
    TcpTestServer echo(TcpTestServer::Mode::Echo);
    const QString targetHost = server.targetHost.value_or(QStringLiteral("127.0.0.1"));
    const int targetPort = server.targetHost ? server.targetPort : echo.port();

    QTemporaryDir dir;
    auto store = std::make_shared<KnownHostsStore>(dir.filePath(QStringLiteral("known_hosts")));
    QVERIFY(!trust(store, server.config).key.isEmpty());
    SshTunnel tunnel;
    const Result<int> opened = tunnel.open(server.config, server.secrets, *store, targetHost,
                                           targetPort, CancellationToken());
    QVERIFY2(opened.has_value(),
             qPrintable(opened.has_value() ? QString() : opened.error().detail));

    constexpr int kClients = 8;
    std::vector<ssh::NativeSocket> clients;
    for (int i = 0; i < kClients; ++i) {
        const ssh::NativeSocket client = test::connectLocal(opened.value());
        QVERIFY(client != ssh::kInvalidSocket);
        clients.push_back(client);
    }
    // All connected before any data moves: they really are simultaneous.
    for (int i = 0; i < kClients; ++i)
        QVERIFY(test::sendAll(clients[i],
                              QByteArray("client-%1").replace("%1", QByteArray::number(i))));
    for (int i = 0; i < kClients; ++i) {
        const QByteArray expected = QByteArray("client-") + QByteArray::number(i);
        QCOMPARE(test::receiveUpTo(clients[i], expected.size()), expected);
    }
    for (const ssh::NativeSocket client : clients)
        ssh::closeSocket(client);
}

void TstSshTunnel::realLargeTransferArrivesIntact()
{
    REQUIRE_REAL_SERVER();
    TcpTestServer echo(TcpTestServer::Mode::Echo);
    const QString targetHost = server.targetHost.value_or(QStringLiteral("127.0.0.1"));
    const int targetPort = server.targetHost ? server.targetPort : echo.port();

    QTemporaryDir dir;
    auto store = std::make_shared<KnownHostsStore>(dir.filePath(QStringLiteral("known_hosts")));
    QVERIFY(!trust(store, server.config).key.isEmpty());
    SshTunnel tunnel;
    const Result<int> opened = tunnel.open(server.config, server.secrets, *store, targetHost,
                                           targetPort, CancellationToken());
    QVERIFY2(opened.has_value(),
             qPrintable(opened.has_value() ? QString() : opened.error().detail));

    QByteArray payload(qsizetype(2) * 1024 * 1024, '\0');
    QRandomGenerator generator(42);
    for (char &byte : payload)
        byte = static_cast<char>(generator.bounded(256));

    const ssh::NativeSocket client = test::connectLocal(opened.value());
    QVERIFY(client != ssh::kInvalidSocket);
    std::atomic<bool> sent { false };
    std::thread sender([&] { sent = test::sendAll(client, payload, 60000); });
    const QByteArray received = test::receiveUpTo(client, payload.size(), 60000);
    sender.join();
    ssh::closeSocket(client);
    QVERIFY(sent.load());
    QCOMPARE(received.size(), payload.size());
    QVERIFY(received == payload);
}

void TstSshTunnel::realCloseDuringTransfer()
{
    REQUIRE_REAL_SERVER();
    TcpTestServer echo(TcpTestServer::Mode::Echo);
    const QString targetHost = server.targetHost.value_or(QStringLiteral("127.0.0.1"));
    const int targetPort = server.targetHost ? server.targetPort : echo.port();

    QTemporaryDir dir;
    auto store = std::make_shared<KnownHostsStore>(dir.filePath(QStringLiteral("known_hosts")));
    QVERIFY(!trust(store, server.config).key.isEmpty());
    SshTunnel tunnel;
    const Result<int> opened = tunnel.open(server.config, server.secrets, *store, targetHost,
                                           targetPort, CancellationToken());
    QVERIFY2(opened.has_value(),
             qPrintable(opened.has_value() ? QString() : opened.error().detail));

    const ssh::NativeSocket client = test::connectLocal(opened.value());
    QVERIFY(client != ssh::kInvalidSocket);
    std::atomic<bool> stop { false };
    // Keeps pushing data and never reads: the forwarder is busy when close() arrives.
    std::thread sender([&] {
        const QByteArray chunk(qsizetype(64) * 1024, 'x');
        while (!stop.load() && test::sendAll(client, chunk, 200)) { }
    });
    QTest::qWait(500);

    QElapsedTimer timer;
    timer.start();
    tunnel.close();
    QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
    QVERIFY(!tunnel.isOpen());
    stop = true;
    sender.join();
    ssh::closeSocket(client);
}

QTEST_GUILESS_MAIN(TstSshTunnel)
#include "tst_ssh_tunnel.moc"
