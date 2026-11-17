#include <quarry/ssh_tunnel_config.h>

#include <QCoreApplication>

namespace quarry {

namespace {

constexpr int kDefaultSshPort = 22;

QString authToString(SshAuth auth)
{
    switch (auth) {
    case SshAuth::Key:
        return QStringLiteral("key");
    case SshAuth::Agent:
        return QStringLiteral("agent");
    case SshAuth::Password:
        break;
    }
    return QStringLiteral("password");
}

QString text(const char *source)
{
    return QCoreApplication::translate("quarry::SshTunnelConfig", source);
}

} // namespace

SshTunnelConfig SshTunnelConfig::fromValues(const QVariantMap &values)
{
    SshTunnelConfig config;
    config.enabled = values.value(QLatin1String(kEnabledKey)).toBool();
    config.host = values.value(QLatin1String(kHostKey)).toString().trimmed();
    config.user = values.value(QLatin1String(kUserKey)).toString().trimmed();
    config.keyPath = values.value(QLatin1String(kKeyPathKey)).toString().trimmed();

    bool ok = false;
    const int port = values.value(QLatin1String(kPortKey)).toString().toInt(&ok);
    config.port = (ok && port >= 1 && port <= 65535) ? port : kDefaultSshPort;

    const QString auth = values.value(QLatin1String(kAuthKey)).toString().trimmed().toLower();
    if (auth == QLatin1String("key"))
        config.auth = SshAuth::Key;
    else if (auth == QLatin1String("agent"))
        config.auth = SshAuth::Agent;
    else
        config.auth = SshAuth::Password;
    return config;
}

void SshTunnelConfig::toValues(QVariantMap &values) const
{
    values.insert(QLatin1String(kEnabledKey), enabled);
    if (!enabled)
        return;
    values.insert(QLatin1String(kHostKey), host);
    values.insert(QLatin1String(kPortKey), port);
    values.insert(QLatin1String(kUserKey), user);
    values.insert(QLatin1String(kAuthKey), authToString(auth));
    values.insert(QLatin1String(kKeyPathKey), keyPath);
}

QString SshTunnelConfig::validationProblem() const
{
    if (!enabled)
        return {};
    if (host.trimmed().isEmpty())
        return text("Enter the SSH server host.");
    if (port < 1 || port > 65535)
        return text("The SSH port must be between 1 and 65535.");
    if (user.trimmed().isEmpty())
        return text("Enter the SSH user name.");
    if (auth == SshAuth::Key && keyPath.trimmed().isEmpty())
        return text("Choose the SSH private key file.");
    return {};
}

} // namespace quarry
