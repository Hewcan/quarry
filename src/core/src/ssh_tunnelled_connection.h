#pragma once

#include "ssh/ssh_tunnel.h"

#include <quarry/provider_api.h>
#include <quarry/ssh_host_key.h>
#include <quarry/ssh_tunnel_config.h>

#include <memory>
#include <mutex>

namespace quarry {

// IConnection that reaches the real one through an SSH tunnel (ARCHITECTURE.md section 6.8).
// open() builds the tunnel and then the provider's connection with host/port pointing at the
// local end, so providers know nothing about SSH. Same threading rules as any IConnection:
// everything on the session thread except requestCancel().
class SshTunnelledConnection : public IConnection
{
public:
    // `provider` must outlive this object (the registry keeps it loaded).
    SshTunnelledConnection(const IDatabaseProvider *provider, ConnectionConfig config,
                           Secrets secrets, SshTunnelConfig tunnel,
                           std::shared_ptr<KnownHostsStore> knownHosts);
    ~SshTunnelledConnection() override;

    Result<ServerInfo> open(const CancellationToken &token) override;
    void close() override;
    bool isOpen() const override;
    Result<std::vector<SchemaNode>> children(const ObjectPath &parent,
                                             const CancellationToken &token) override;
    Result<TableDetails> describe(const ObjectPath &table, const CancellationToken &token) override;
    Result<std::unique_ptr<IResultStream>> execute(const Statement &statement,
                                                   const CancellationToken &token) override;
    void requestCancel() override;
    Result<void> begin() override;
    Result<void> commit() override;
    Result<void> rollback() override;

    // Where the SSH server connects to, taken from the profile (see defaultPort()).
    QString destinationHost() const { return m_destHost; }
    int destinationPort() const { return m_destPort; }

private:
    Error notOpen() const;
    void closeInner();

    const IDatabaseProvider *m_provider;
    ConnectionConfig m_config;
    Secrets m_secrets;
    SshTunnelConfig m_tunnelConfig;
    std::shared_ptr<KnownHostsStore> m_knownHosts;
    QString m_destHost;
    int m_destPort = 0;

    SshTunnel m_tunnel;
    // Replaced only on the session thread; requestCancel() reads it from another thread, hence
    // the mutex around every change and around that read.
    std::mutex m_mutex;
    std::unique_ptr<IConnection> m_inner;
};

// Port the engine listens on by default; used as tunnel destination when the profile has none.
int defaultPortFor(const IDatabaseProvider &provider);

} // namespace quarry
