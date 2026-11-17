#include "ui/connection_form_model.h"

#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QtTest>

using namespace quarry;
using namespace quarry::ui;

namespace {

std::vector<ConnectionField> postgresLikeFields()
{
    ConnectionField host;
    host.key = QStringLiteral("host");
    host.label = QStringLiteral("Host");
    host.required = true;
    ConnectionField port;
    port.key = QStringLiteral("port");
    port.label = QStringLiteral("Port");
    port.kind = ConnectionField::Kind::Number;
    port.defaultValue = 5432;
    ConnectionField password;
    password.key = QStringLiteral("password");
    password.kind = ConnectionField::Kind::Secret;
    return { host, port, password };
}

// A form that offers the tunnel, with the database host filled in.
void fill(ConnectionFormModel &form, bool ssh = true)
{
    form.setFields(postgresLikeFields(), ssh);
    form.setValue(0, QStringLiteral("db.internal"));
}

} // namespace

class TstConnectionFormModel : public QObject
{
    Q_OBJECT

private slots:
    void modelPassesTheItemModelTester();
    void tunnelSectionOnlyWhenTheEngineHasTheCapability();
    void defaultsAreAnOffTunnelOnPort22WithPassword();
    void eachFieldHasItsOwnMessage();
    void messagesAgreeWithTheCoresValidation();
    void keyPathIsOnlyRequiredForAKey();
    void valuesUseTheSshTunnelConfigKeys();
    void agentAndDisabledDoNotStoreASecretMode();
    void valuesComeBackIntoTheForm();
    void changingTheEngineResetsTheDraft();
    void changesAreAnnounced();
    void databaseFieldsStillDecideValidityWithoutSsh();
};

void TstConnectionFormModel::modelPassesTheItemModelTester()
{
    ConnectionFormModel form;
    QAbstractItemModelTester tester(&form, QAbstractItemModelTester::FailureReportingMode::QtTest);
    fill(form);
    form.setSshEnabled(true);
    form.setSshHost(QStringLiteral("bastion"));
    form.setValues({ { QStringLiteral("host"), QStringLiteral("other") } });
    form.setFields({}, true);
    QCOMPARE(form.rowCount(), 0);
}

void TstConnectionFormModel::tunnelSectionOnlyWhenTheEngineHasTheCapability()
{
    ConnectionFormModel form;
    fill(form, false); // SQLite, ODBC
    QVERIFY(!form.sshAvailable());
    form.setSshEnabled(true);
    form.setSshHost(QString());
    QVERIFY(!form.sshEnabled()); // the section is not shown, so it cannot be on
    QVERIFY(form.sshHostError().isEmpty());
    QVERIFY(form.isValid()); // the missing SSH host does not count
    const QVariantMap values = form.values();
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        QVERIFY2(!it.key().startsWith(QLatin1String("ssh.")), qPrintable(it.key()));

    fill(form, true); // PostgreSQL, MySQL, SQL Server
    QVERIFY(form.sshAvailable());
    QVERIFY(!form.sshEnabled()); // off until the user switches it on
}

void TstConnectionFormModel::defaultsAreAnOffTunnelOnPort22WithPassword()
{
    ConnectionFormModel form;
    fill(form);
    QCOMPARE(form.sshPort(), QStringLiteral("22"));
    QCOMPARE(form.sshAuth(), QStringLiteral("password"));
    QCOMPARE(form.sshSecretMode(), 1);
    QVERIFY(form.sshHost().isEmpty());
    QVERIFY(!form.sshNeedsSecret()); // off
    QVERIFY(form.isValid());
    form.setSshEnabled(true);
    QVERIFY(form.sshNeedsSecret());
}

void TstConnectionFormModel::eachFieldHasItsOwnMessage()
{
    ConnectionFormModel form;
    fill(form);
    form.setSshEnabled(true);
    QVERIFY(!form.isValid());
    QVERIFY(!form.sshHostError().isEmpty());
    QVERIFY(!form.sshUserError().isEmpty());
    QVERIFY(form.sshPortError().isEmpty());
    QVERIFY(form.sshKeyPathError().isEmpty()); // a password, no key file

    form.setSshHost(QStringLiteral("  "));
    QVERIFY(!form.sshHostError().isEmpty()); // blanks are empty
    form.setSshHost(QStringLiteral("bastion"));
    form.setSshUser(QStringLiteral("deploy"));
    QVERIFY(form.isValid());

    for (const QString &bad : { QStringLiteral("abc"), QStringLiteral("0"), QStringLiteral("65536"),
                                QStringLiteral(""), QStringLiteral("22.5") }) {
        form.setSshPort(bad);
        QVERIFY2(!form.sshPortError().isEmpty(), qPrintable(bad));
        QVERIFY2(!form.isValid(), qPrintable(bad));
    }
    form.setSshPort(QStringLiteral(" 2222 "));
    QVERIFY(form.sshPortError().isEmpty());
    QVERIFY(form.isValid());
}

void TstConnectionFormModel::messagesAgreeWithTheCoresValidation()
{
    ConnectionFormModel form;
    fill(form);
    form.setSshEnabled(true);
    form.setSshAuth(QStringLiteral("key"));
    // Every state is valid for the form exactly when SshTunnelConfig::validationProblem() says so.
    for (const QString &host : { QString(), QStringLiteral("h") })
        for (const QString &user : { QString(), QStringLiteral("u") })
            for (const QString &key : { QString(), QStringLiteral("k") }) {
                form.setSshHost(host);
                form.setSshUser(user);
                form.setSshKeyPath(key);
                QCOMPARE(form.isValid(), form.sshConfig().validationProblem().isEmpty());
                const bool anyMessage = !form.sshHostError().isEmpty()
                    || !form.sshUserError().isEmpty() || !form.sshKeyPathError().isEmpty();
                QCOMPARE(anyMessage, !form.isValid());
            }
}

void TstConnectionFormModel::keyPathIsOnlyRequiredForAKey()
{
    ConnectionFormModel form;
    fill(form);
    form.setSshEnabled(true);
    form.setSshHost(QStringLiteral("h"));
    form.setSshUser(QStringLiteral("u"));
    QVERIFY(form.isValid());
    form.setSshAuth(QStringLiteral("key"));
    QVERIFY(!form.isValid());
    QVERIFY(!form.sshKeyPathError().isEmpty());
    form.setSshKeyPath(QStringLiteral("C:/keys/id_ed25519"));
    QVERIFY(form.isValid());
    form.setSshAuth(QStringLiteral("agent"));
    form.setSshKeyPath(QString());
    QVERIFY(form.isValid());
    QVERIFY(!form.sshNeedsSecret());
    form.setSshAuth(QStringLiteral("nonsense")); // anything else is a password
    QCOMPARE(form.sshAuth(), QStringLiteral("password"));
}

void TstConnectionFormModel::valuesUseTheSshTunnelConfigKeys()
{
    ConnectionFormModel form;
    fill(form);
    form.setSshEnabled(true);
    form.setSshHost(QStringLiteral(" bastion.example.com "));
    form.setSshPort(QStringLiteral("2222"));
    form.setSshUser(QStringLiteral("deploy"));
    form.setSshAuth(QStringLiteral("key"));
    form.setSshKeyPath(QStringLiteral("C:/keys/id_ed25519"));
    const QVariantMap v = form.values();
    QCOMPARE(v.value(QLatin1String(SshTunnelConfig::kEnabledKey)).toBool(), true);
    QCOMPARE(v.value(QLatin1String(SshTunnelConfig::kHostKey)).toString(),
             QStringLiteral("bastion.example.com"));
    QCOMPARE(v.value(QLatin1String(SshTunnelConfig::kPortKey)).toInt(), 2222);
    QCOMPARE(v.value(QLatin1String(SshTunnelConfig::kUserKey)).toString(),
             QStringLiteral("deploy"));
    QCOMPARE(v.value(QLatin1String(SshTunnelConfig::kAuthKey)).toString(), QStringLiteral("key"));
    QCOMPARE(v.value(QLatin1String(SshTunnelConfig::kKeyPathKey)).toString(),
             QStringLiteral("C:/keys/id_ed25519"));
    QCOMPARE(v.value(QLatin1String(kSshSecretModeKey)).toString(), QStringLiteral("keychain"));
    // The database fields are still there, and no secret value is.
    QCOMPARE(v.value(QStringLiteral("host")).toString(), QStringLiteral("db.internal"));
    QVERIFY(!v.contains(QStringLiteral("password")));
    QVERIFY(!v.contains(QStringLiteral("ssh.password")));
    QVERIFY(!v.contains(QStringLiteral("ssh.passphrase")));

    form.setSshSecretMode(2);
    QCOMPARE(form.values().value(QLatin1String(kSshSecretModeKey)).toString(),
             QStringLiteral("ask"));
    form.setSshSecretMode(7); // anything but 2 is the keychain
    QCOMPARE(form.sshSecretMode(), 1);
}

void TstConnectionFormModel::agentAndDisabledDoNotStoreASecretMode()
{
    ConnectionFormModel form;
    fill(form);
    form.setSshEnabled(true);
    form.setSshHost(QStringLiteral("h"));
    form.setSshUser(QStringLiteral("u"));
    form.setSshAuth(QStringLiteral("agent"));
    QVERIFY(!form.values().contains(QLatin1String(kSshSecretModeKey)));
    QCOMPARE(form.values().value(QLatin1String(SshTunnelConfig::kAuthKey)).toString(),
             QStringLiteral("agent"));
    form.setSshEnabled(false);
    const QVariantMap v = form.values();
    QCOMPARE(v.value(QLatin1String(SshTunnelConfig::kEnabledKey)).toBool(), false);
    QVERIFY(!v.contains(QLatin1String(SshTunnelConfig::kHostKey)));
    QVERIFY(!v.contains(QLatin1String(kSshSecretModeKey)));
}

void TstConnectionFormModel::valuesComeBackIntoTheForm()
{
    ConnectionFormModel a;
    fill(a);
    a.setSshEnabled(true);
    a.setSshHost(QStringLiteral("bastion"));
    a.setSshPort(QStringLiteral("2200"));
    a.setSshUser(QStringLiteral("deploy"));
    a.setSshAuth(QStringLiteral("key"));
    a.setSshKeyPath(QStringLiteral("/home/u/.ssh/id"));
    a.setSshSecretMode(2);

    ConnectionFormModel b;
    b.setFields(postgresLikeFields(), true);
    b.setValues(a.values());
    QVERIFY(b.sshEnabled());
    QCOMPARE(b.sshHost(), QStringLiteral("bastion"));
    QCOMPARE(b.sshPort(), QStringLiteral("2200"));
    QCOMPARE(b.sshUser(), QStringLiteral("deploy"));
    QCOMPARE(b.sshAuth(), QStringLiteral("key"));
    QCOMPARE(b.sshKeyPath(), QStringLiteral("/home/u/.ssh/id"));
    QCOMPARE(b.sshSecretMode(), 2);
    QVERIFY(b.isValid());

    // A connection saved without a tunnel loads as "off", whatever was in the form before.
    b.setValues({ { QStringLiteral("host"), QStringLiteral("x") } });
    QVERIFY(!b.sshEnabled());
    QVERIFY(b.sshHost().isEmpty());

    // The engine has no tunnel: stored ssh keys are ignored.
    ConnectionFormModel c;
    c.setFields(postgresLikeFields(), false);
    c.setValues(a.values());
    QVERIFY(!c.sshEnabled());
}

void TstConnectionFormModel::changingTheEngineResetsTheDraft()
{
    ConnectionFormModel form;
    fill(form);
    form.setSshEnabled(true);
    form.setSshHost(QStringLiteral("bastion"));
    form.setSshAuth(QStringLiteral("agent"));
    form.setFields(postgresLikeFields(), true);
    QVERIFY(!form.sshEnabled());
    QVERIFY(form.sshHost().isEmpty());
    QCOMPARE(form.sshAuth(), QStringLiteral("password"));
}

void TstConnectionFormModel::changesAreAnnounced()
{
    ConnectionFormModel form;
    fill(form);
    QSignalSpy changed(&form, &ConnectionFormModel::sshChanged);
    QSignalSpy validity(&form, &ConnectionFormModel::validChanged);
    form.setSshEnabled(true);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(validity.count(), 1);
    form.setSshHost(QStringLiteral("h"));
    form.setSshHost(QStringLiteral("h")); // same value: no signal
    QCOMPARE(changed.count(), 2);
}

void TstConnectionFormModel::databaseFieldsStillDecideValidityWithoutSsh()
{
    ConnectionFormModel form;
    form.setFields(postgresLikeFields(), true);
    QVERIFY(!form.isValid()); // the required database host is empty
    form.setValue(0, QStringLiteral("db"));
    QVERIFY(form.isValid());
    form.setSshEnabled(true);
    QVERIFY(!form.isValid()); // and now the tunnel's host and user too
    form.setSshEnabled(false);
    QVERIFY(form.isValid());
}

QTEST_MAIN(TstConnectionFormModel)
#include "tst_connection_form_model.moc"
