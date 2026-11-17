#include <quarry/ssh_tunnel_config.h>

#include <QtTest>

using namespace quarry;

class TstSshTunnelConfig : public QObject
{
    Q_OBJECT

private slots:
    void defaultsWhenEmpty();
    void readsAllKeys();
    void authParsing_data();
    void authParsing();
    void invalidPortFallsBack_data();
    void invalidPortFallsBack();
    void toValuesRoundtrip();
    void toValuesDisabledWritesOnlyEnabled();
    void toValuesKeepsOtherEntries();
    void validationProblem_data();
    void validationProblem();
};

void TstSshTunnelConfig::defaultsWhenEmpty()
{
    const SshTunnelConfig config = SshTunnelConfig::fromValues({});
    QVERIFY(!config.enabled);
    QVERIFY(config.host.isEmpty());
    QCOMPARE(config.port, 22);
    QVERIFY(config.user.isEmpty());
    QCOMPARE(config.auth, SshAuth::Password);
    QVERIFY(config.keyPath.isEmpty());
}

void TstSshTunnelConfig::readsAllKeys()
{
    QVariantMap values;
    values.insert(QStringLiteral("ssh.enabled"), true);
    values.insert(QStringLiteral("ssh.host"), QStringLiteral(" bastion.example.com "));
    values.insert(QStringLiteral("ssh.port"), 2222);
    values.insert(QStringLiteral("ssh.user"), QStringLiteral("deploy"));
    values.insert(QStringLiteral("ssh.auth"), QStringLiteral("key"));
    values.insert(QStringLiteral("ssh.keyPath"), QStringLiteral("C:/keys/id_ed25519"));
    values.insert(QStringLiteral("host"), QStringLiteral("db.internal")); // unrelated

    const SshTunnelConfig config = SshTunnelConfig::fromValues(values);
    QVERIFY(config.enabled);
    QCOMPARE(config.host, QStringLiteral("bastion.example.com"));
    QCOMPARE(config.port, 2222);
    QCOMPARE(config.user, QStringLiteral("deploy"));
    QCOMPARE(config.auth, SshAuth::Key);
    QCOMPARE(config.keyPath, QStringLiteral("C:/keys/id_ed25519"));
}

void TstSshTunnelConfig::authParsing_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<int>("expected");
    QTest::newRow("password") << "password" << int(SshAuth::Password);
    QTest::newRow("key") << "key" << int(SshAuth::Key);
    QTest::newRow("agent") << "agent" << int(SshAuth::Agent);
    QTest::newRow("upper case") << "AGENT" << int(SshAuth::Agent);
    QTest::newRow("unknown") << "kerberos" << int(SshAuth::Password);
    QTest::newRow("empty") << "" << int(SshAuth::Password);
}

void TstSshTunnelConfig::authParsing()
{
    QFETCH(QString, text);
    QFETCH(int, expected);
    QVariantMap values;
    values.insert(QStringLiteral("ssh.auth"), text);
    QCOMPARE(int(SshTunnelConfig::fromValues(values).auth), expected);
}

void TstSshTunnelConfig::invalidPortFallsBack_data()
{
    QTest::addColumn<QVariant>("port");
    QTest::addColumn<int>("expected");
    QTest::newRow("zero") << QVariant(0) << 22;
    QTest::newRow("negative") << QVariant(-5) << 22;
    QTest::newRow("too big") << QVariant(70000) << 22;
    QTest::newRow("text") << QVariant(QStringLiteral("abc")) << 22;
    QTest::newRow("empty") << QVariant(QString()) << 22;
    QTest::newRow("numeric text") << QVariant(QStringLiteral("2200")) << 2200;
    QTest::newRow("upper bound") << QVariant(65535) << 65535;
    QTest::newRow("lower bound") << QVariant(1) << 1;
}

void TstSshTunnelConfig::invalidPortFallsBack()
{
    QFETCH(QVariant, port);
    QFETCH(int, expected);
    QVariantMap values;
    values.insert(QStringLiteral("ssh.port"), port);
    QCOMPARE(SshTunnelConfig::fromValues(values).port, expected);
}

void TstSshTunnelConfig::toValuesRoundtrip()
{
    for (const SshAuth auth : { SshAuth::Password, SshAuth::Key, SshAuth::Agent }) {
        SshTunnelConfig config;
        config.enabled = true;
        config.host = QStringLiteral("h");
        config.port = 2022;
        config.user = QStringLiteral("u");
        config.auth = auth;
        config.keyPath = QStringLiteral("/k");

        QVariantMap values;
        config.toValues(values);
        const SshTunnelConfig back = SshTunnelConfig::fromValues(values);
        QVERIFY(back.enabled);
        QCOMPARE(back.host, config.host);
        QCOMPARE(back.port, config.port);
        QCOMPARE(back.user, config.user);
        QCOMPARE(int(back.auth), int(config.auth));
        QCOMPARE(back.keyPath, config.keyPath);
    }
}

void TstSshTunnelConfig::toValuesDisabledWritesOnlyEnabled()
{
    SshTunnelConfig config;
    config.host = QStringLiteral("ignored");
    QVariantMap values;
    config.toValues(values);
    QCOMPARE(values.size(), 1);
    QCOMPARE(values.value(QStringLiteral("ssh.enabled")).toBool(), false);
}

void TstSshTunnelConfig::toValuesKeepsOtherEntries()
{
    QVariantMap values;
    values.insert(QStringLiteral("host"), QStringLiteral("db"));
    SshTunnelConfig config;
    config.enabled = true;
    config.host = QStringLiteral("h");
    config.toValues(values);
    QCOMPARE(values.value(QStringLiteral("host")).toString(), QStringLiteral("db"));
    QCOMPARE(values.size(), 7);
}

void TstSshTunnelConfig::validationProblem_data()
{
    QTest::addColumn<bool>("enabled");
    QTest::addColumn<QString>("host");
    QTest::addColumn<int>("port");
    QTest::addColumn<QString>("user");
    QTest::addColumn<int>("auth");
    QTest::addColumn<QString>("keyPath");
    QTest::addColumn<QString>("mentions"); // empty: valid

    const int password = int(SshAuth::Password);
    const int key = int(SshAuth::Key);
    const int agent = int(SshAuth::Agent);
    QTest::newRow("disabled needs nothing") << false << "" << 22 << "" << password << "" << "";
    QTest::newRow("valid password") << true << "h" << 22 << "u" << password << "" << "";
    QTest::newRow("valid agent") << true << "h" << 22 << "u" << agent << "" << "";
    QTest::newRow("valid key") << true << "h" << 22 << "u" << key << "/k" << "";
    QTest::newRow("no host") << true << "  " << 22 << "u" << password << "" << "host";
    QTest::newRow("no user") << true << "h" << 22 << "" << password << "" << "user";
    QTest::newRow("key without file") << true << "h" << 22 << "u" << key << "" << "key";
    QTest::newRow("bad port") << true << "h" << 0 << "u" << password << "" << "port";
}

void TstSshTunnelConfig::validationProblem()
{
    QFETCH(bool, enabled);
    QFETCH(QString, host);
    QFETCH(int, port);
    QFETCH(QString, user);
    QFETCH(int, auth);
    QFETCH(QString, keyPath);
    QFETCH(QString, mentions);

    SshTunnelConfig config;
    config.enabled = enabled;
    config.host = host;
    config.port = port;
    config.user = user;
    config.auth = static_cast<SshAuth>(auth);
    config.keyPath = keyPath;

    const QString problem = config.validationProblem();
    if (mentions.isEmpty()) {
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
    } else {
        QVERIFY(!problem.isEmpty());
        QVERIFY2(problem.contains(mentions, Qt::CaseInsensitive), qPrintable(problem));
    }
}

QTEST_GUILESS_MAIN(TstSshTunnelConfig)
#include "tst_ssh_tunnel_config.moc"
