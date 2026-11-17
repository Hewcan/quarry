#include "tcp_test_server.h"

#include <quarry/provider_registry.h>
#include <quarry/ssh_host_key.h>
#include <quarry/ssh_tunnel_config.h>

#include <QTemporaryDir>
#include <QtTest>

using namespace quarry;

class TstProviderRegistry : public QObject
{
    Q_OBJECT

private slots:
    void scanListsWithoutLoading();
    void providerLoadsLazilyAndCaches();
    void incompatibleApiIsRejected();
    void unknownIdFails();
    void missingDirectoryIsEmpty();
    void knownHostsDefaultsAndSetter();
    void sshOnEngineWithoutCapabilityIsUnsupported();
    void sshWithIncompleteSettingsFails();
    void sshWrapsTheConnectionAndOpensTheTunnelFirst();
};

void TstProviderRegistry::scanListsWithoutLoading()
{
    ProviderRegistry registry;
    registry.scan(QStringLiteral(QUARRY_TEST_PLUGIN_DIR));

    QCOMPARE(registry.available().size(), size_t(1));
    QCOMPARE(registry.available().front().id, QStringLiteral("dummy"));
    QCOMPARE(registry.available().front().displayName, QStringLiteral("Dummy"));
    QCOMPARE(registry.available().front().version, QVersionNumber(0, 1, 0));
    QVERIFY(!registry.isLoaded(QStringLiteral("dummy")));
}

void TstProviderRegistry::providerLoadsLazilyAndCaches()
{
    ProviderRegistry registry;
    registry.scan(QStringLiteral(QUARRY_TEST_PLUGIN_DIR));

    QString error;
    IDatabaseProvider *p = registry.provider(QStringLiteral("dummy"), &error);
    QVERIFY2(p, qPrintable(error));
    QCOMPARE(p->info().id, QStringLiteral("dummy"));
    QVERIFY(registry.isLoaded(QStringLiteral("dummy")));
    QCOMPARE(registry.provider(QStringLiteral("dummy")), p);
}

void TstProviderRegistry::incompatibleApiIsRejected()
{
    ProviderRegistry registry;
    registry.scan(QStringLiteral(QUARRY_TEST_PLUGIN_DIR));

    QCOMPARE(registry.rejected().size(), size_t(1));
    QVERIFY(registry.rejected().front().reason.contains(QStringLiteral("API version"),
                                                        Qt::CaseInsensitive));

    QString error;
    QVERIFY(registry.provider(QStringLiteral("future"), &error) == nullptr);
    QVERIFY(!error.isEmpty());
}

void TstProviderRegistry::unknownIdFails()
{
    ProviderRegistry registry;
    registry.scan(QStringLiteral(QUARRY_TEST_PLUGIN_DIR));

    QString error;
    QVERIFY(registry.provider(QStringLiteral("nope"), &error) == nullptr);
    QVERIFY(!error.isEmpty());
}

void TstProviderRegistry::missingDirectoryIsEmpty()
{
    ProviderRegistry registry;
    registry.scan(QStringLiteral(QUARRY_TEST_PLUGIN_DIR "/does-not-exist"));
    QVERIFY(registry.available().empty());
    QVERIFY(registry.rejected().empty());
}

namespace {

ConnectionProfile sshProfile(const QString &providerId, int sshPort)
{
    ConnectionProfile profile;
    profile.id = QUuid::createUuid();
    profile.providerId = providerId;
    SshTunnelConfig tunnel;
    tunnel.enabled = true;
    tunnel.host = QStringLiteral("127.0.0.1");
    tunnel.port = sshPort;
    tunnel.user = QStringLiteral("nobody");
    tunnel.toValues(profile.values);
    return profile;
}

} // namespace

void TstProviderRegistry::knownHostsDefaultsAndSetter()
{
    ProviderRegistry registry;
    const std::shared_ptr<KnownHostsStore> lazy = registry.knownHosts();
    QVERIFY(lazy);
    QCOMPARE(registry.knownHosts(), lazy); // created once

    const auto custom = std::make_shared<KnownHostsStore>(QStringLiteral("unused"));
    registry.setKnownHosts(custom);
    QCOMPARE(registry.knownHosts(), custom);
}

void TstProviderRegistry::sshOnEngineWithoutCapabilityIsUnsupported()
{
    ProviderRegistry registry;
    registry.scan(QStringLiteral(QUARRY_PLUGIN_DIR));
    const auto result = registry.createConnection(sshProfile(QStringLiteral("sqlite"), 22), {});
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().kind, ErrorKind::Unsupported);
    QVERIFY(!result.error().message.isEmpty());
}

void TstProviderRegistry::sshWithIncompleteSettingsFails()
{
    ProviderRegistry registry;
    registry.scan(QStringLiteral(QUARRY_PLUGIN_DIR));
    ConnectionProfile profile = sshProfile(QStringLiteral("postgres"), 22);
    profile.values.insert(QStringLiteral("ssh.host"), QString());
    const auto result = registry.createConnection(profile, {});
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().kind, ErrorKind::Connection);
}

void TstProviderRegistry::sshWrapsTheConnectionAndOpensTheTunnelFirst()
{
    QTemporaryDir dir;
    ProviderRegistry registry;
    registry.scan(QStringLiteral(QUARRY_PLUGIN_DIR));
    registry.setKnownHosts(
        std::make_shared<KnownHostsStore>(dir.filePath(QStringLiteral("known_hosts"))));

    ConnectionProfile profile = sshProfile(QStringLiteral("postgres"), quarry::test::unusedPort());
    profile.values.insert(QStringLiteral("database"), QStringLiteral("db"));
    profile.values.insert(QStringLiteral("user"), QStringLiteral("u"));
    auto result = registry.createConnection(profile, {});
    QVERIFY2(result.has_value(), result.has_value() ? "" : qPrintable(result.error().message));
    std::unique_ptr<IConnection> connection = std::move(result).value();
    QVERIFY(!connection->isOpen());

    // Nothing listens on the SSH port: only the decorator can fail like this, because the plain
    // PostgreSQL connection never looks at the ssh.* settings.
    const auto opened = connection->open(CancellationToken());
    QVERIFY(!opened.has_value());
    QCOMPARE(opened.error().kind, ErrorKind::Connection);
    QVERIFY2(opened.error().message.contains(QLatin1String("SSH")),
             qPrintable(opened.error().message));
    QVERIFY(!connection->isOpen());
    connection->close(); // idempotent
    connection->requestCancel(); // idle: must not crash
}

QTEST_GUILESS_MAIN(TstProviderRegistry)
#include "tst_provider_registry.moc"
