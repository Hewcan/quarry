#pragma once

// One libssh2 session over one TCP socket, driven in non-blocking mode. Not thread-safe: a
// libssh2 session must only ever be touched by one thread at a time. Private to core.

#include "ssh_net.h"

#include <quarry/provider_api.h>
#include <quarry/result.h>
#include <quarry/ssh_host_key.h>
#include <quarry/ssh_tunnel_config.h>

#include <QByteArray>

#include <libssh2.h>

namespace quarry::ssh {

inline constexpr int kConnectTimeoutSeconds = 15;

class Client
{
public:
    Client();
    ~Client();
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    // TCP connect + SSH handshake (no authentication). Errors: Connection (unreachable, not an
    // SSH server), Timeout, Cancelled.
    Result<void> connectAndHandshake(const QString &host, int port, const Interrupt &interrupt);
    // Host key received in the handshake; status is Unknown until classified by a store.
    HostKeyInfo hostKey(const QString &host, int port) const;
    // Errors: Authentication (rejected, unreadable key, agent unavailable), Connection, Timeout,
    // Cancelled.
    Result<void> authenticate(const SshTunnelConfig &config, const Secrets &secrets,
                              const Interrupt &interrupt);
    // Sends a polite disconnect (best effort), frees the session and closes the socket.
    void disconnect();

    LIBSSH2_SESSION *session() const { return m_session; }
    NativeSocket socket() const { return m_socket; }

    // Waits (one slice) for the direction libssh2 is blocked on.
    void waitForSession(int timeoutMs) const;
    // Detail text of the last libssh2 error.
    QString lastErrorText() const;

private:
    // Repeats `call` while libssh2 says EAGAIN; stops early on cancel or timeout (m_stop).
    template<typename F> int drive(const Interrupt &interrupt, F &&call);
    Result<void> authenticateWithPassword(const QByteArray &user, const QString &password,
                                          const Interrupt &interrupt);
    Result<void> authenticateWithKey(const QByteArray &user, const SshTunnelConfig &config,
                                     const QString &passphrase, const Interrupt &interrupt);
    Result<void> authenticateWithAgent(const QByteArray &user, const Interrupt &interrupt);
    // Transport-level libssh2 codes always become Connection; the rest take `kind`.
    Error failure(int code, const QString &message, ErrorKind kind) const;
    Error stopped(const Interrupt &interrupt) const;

    LIBSSH2_SESSION *m_session = nullptr;
    NativeSocket m_socket = kInvalidSocket;
    StopReason m_stop = StopReason::None;
};

} // namespace quarry::ssh
