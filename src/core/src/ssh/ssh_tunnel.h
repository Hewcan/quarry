#pragma once

// Local port forwarding over SSH (ARCHITECTURE.md section 6.8). Private to core: nothing outside
// src/core/src includes this header and libssh2 never leaks past src/core/src/ssh/.

#include <quarry/cancellation.h>
#include <quarry/provider_api.h>
#include <quarry/result.h>
#include <quarry/ssh_host_key.h>
#include <quarry/ssh_tunnel_config.h>

#include <QString>

#include <memory>

namespace quarry {

class SshTunnel
{
public:
    SshTunnel();
    // Calls close().
    ~SshTunnel();
    SshTunnel(const SshTunnel &) = delete;
    SshTunnel &operator=(const SshTunnel &) = delete;

    // Blocking: connect, verify the host key against `knownHosts`, authenticate and start
    // listening on 127.0.0.1:<random port>, forwarding every connection to destHost:destPort as
    // seen from the SSH server. Call it from the session thread, never from the UI thread.
    // Errors: Connection (unreachable; untrusted or changed host key with sqlState
    // kSshUntrustedState / kSshMismatchState and the fingerprint in detail), Authentication,
    // Timeout, Cancelled. Returns the local port.
    Result<int> open(const SshTunnelConfig &config, const Secrets &secrets,
                     const KnownHostsStore &knownHosts, const QString &destHost, int destPort,
                     const CancellationToken &token);
    // Thread-safe. Makes an open() in progress return Cancelled; ignored once the tunnel is up.
    void requestCancel();
    // Idempotent and bounded: stops the forwarding thread (transfers in flight are dropped) and
    // releases the SSH session. Safe to call concurrently with nothing else; not with open().
    void close();

    bool isOpen() const;
    int localPort() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace quarry
