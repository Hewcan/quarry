#include "ssh_tunnelled_connection.h"

#include <QCoreApplication>

namespace quarry {

namespace {

constexpr const char *kLoopback = "127.0.0.1";

QString text(const char *source)
{
    return QCoreApplication::translate("quarry::SshTunnelledConnection", source);
}

// SQL Server profiles use "server" (optionally host\instance); the others use "host".
QString hostKeyFor(const IDatabaseProvider &provider)
{
    const std::vector<ConnectionField> fields = provider.connectionFields();
    for (const ConnectionField &field : fields) {
        if (field.key == QLatin1String("server"))
            return QStringLiteral("server");
    }
    return QStringLiteral("host");
}

} // namespace

int defaultPortFor(const IDatabaseProvider &provider)
{
    const std::vector<ConnectionField> fields = provider.connectionFields();
    for (const ConnectionField &field : fields) {
        if (field.key == QLatin1String("port") && field.defaultValue.toInt() > 0)
            return field.defaultValue.toInt();
    }
    const QString id = provider.info().id;
    if (id == QLatin1String("mysql"))
        return 3306;
    if (id == QLatin1String("mssql"))
        return 1433;
    return 5432;
}

SshTunnelledConnection::SshTunnelledConnection(const IDatabaseProvider *provider,
                                               ConnectionConfig config, Secrets secrets,
                                               SshTunnelConfig tunnel,
                                               std::shared_ptr<KnownHostsStore> knownHosts)
    : m_provider(provider)
    , m_config(std::move(config))
    , m_secrets(std::move(secrets))
    , m_tunnelConfig(std::move(tunnel))
    , m_knownHosts(std::move(knownHosts))
{
    // shortcut: a SQL Server named instance (host\instance) is tunnelled to the port in the
    // profile, or 1433 without one; the SQL Browser lookup does not go through the tunnel. Add it
    // when instance users ask.
    QString host = m_config.values.value(hostKeyFor(*m_provider)).toString().trimmed();
    if (const qsizetype slash = host.indexOf(QLatin1Char('\\')); slash >= 0)
        host = host.left(slash).trimmed();
    m_destHost = host.isEmpty() ? QStringLiteral("localhost") : host;

    bool ok = false;
    const int port = m_config.values.value(QStringLiteral("port")).toString().trimmed().toInt(&ok);
    m_destPort = (ok && port >= 1 && port <= 65535) ? port : defaultPortFor(*m_provider);
}

SshTunnelledConnection::~SshTunnelledConnection()
{
    closeInner();
    m_tunnel.close();
}

Error SshTunnelledConnection::notOpen() const
{
    return { ErrorKind::Connection, text("The connection is not open."), {}, {}, {} };
}

void SshTunnelledConnection::closeInner()
{
    std::unique_ptr<IConnection> inner;
    {
        const std::lock_guard lock(m_mutex);
        inner = std::move(m_inner);
    }
    // Destroyed outside the lock: requestCancel() on another thread cannot be using it any more
    // because it only reaches m_inner under the lock.
    if (inner)
        inner->close();
}

Result<ServerInfo> SshTunnelledConnection::open(const CancellationToken &token)
{
    closeInner();
    m_tunnel.close();

    auto tunnelPort
        = m_tunnel.open(m_tunnelConfig, m_secrets, *m_knownHosts, m_destHost, m_destPort, token);
    if (!tunnelPort)
        return fail(std::move(tunnelPort).error());

    ConnectionConfig config = m_config;
    config.values.insert(hostKeyFor(*m_provider), QString::fromLatin1(kLoopback));
    config.values.insert(QStringLiteral("port"), tunnelPort.value());
    std::unique_ptr<IConnection> inner = m_provider->createConnection(config, m_secrets);
    if (!inner) {
        m_tunnel.close();
        return fail(
            Error { ErrorKind::Internal, text("Could not prepare the connection."), {}, {}, {} });
    }
    IConnection *raw = inner.get();
    {
        const std::lock_guard lock(m_mutex);
        m_inner = std::move(inner);
    }
    Result<ServerInfo> opened = raw->open(token);
    if (!opened) {
        closeInner();
        m_tunnel.close();
    }
    return opened;
}

void SshTunnelledConnection::close()
{
    closeInner();
    m_tunnel.close();
}

bool SshTunnelledConnection::isOpen() const
{
    return m_inner && m_inner->isOpen();
}

Result<std::vector<SchemaNode>> SshTunnelledConnection::children(const ObjectPath &parent,
                                                                 const CancellationToken &token)
{
    if (!m_inner)
        return fail(notOpen());
    return m_inner->children(parent, token);
}

Result<TableDetails> SshTunnelledConnection::describe(const ObjectPath &table,
                                                      const CancellationToken &token)
{
    if (!m_inner)
        return fail(notOpen());
    return m_inner->describe(table, token);
}

Result<std::unique_ptr<IResultStream>>
SshTunnelledConnection::execute(const Statement &statement, const CancellationToken &token)
{
    if (!m_inner)
        return fail(notOpen());
    return m_inner->execute(statement, token);
}

void SshTunnelledConnection::requestCancel()
{
    // Aborts a tunnel that is still being established; harmless once it is up.
    m_tunnel.requestCancel();
    const std::lock_guard lock(m_mutex);
    if (m_inner)
        m_inner->requestCancel();
}

Result<void> SshTunnelledConnection::begin()
{
    if (!m_inner)
        return fail(notOpen());
    return m_inner->begin();
}

Result<void> SshTunnelledConnection::commit()
{
    if (!m_inner)
        return fail(notOpen());
    return m_inner->commit();
}

Result<void> SshTunnelledConnection::rollback()
{
    if (!m_inner)
        return fail(notOpen());
    return m_inner->rollback();
}

} // namespace quarry
