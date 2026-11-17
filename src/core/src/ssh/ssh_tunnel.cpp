#include "ssh_tunnel.h"

#include "ssh_client.h"
#include "ssh_net.h"

#include <QCoreApplication>
#include <QLoggingCategory>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#endif

Q_LOGGING_CATEGORY(lcSshTunnel, "quarry.ssh")

namespace quarry {

namespace {

using namespace ssh;

constexpr int kHandshakeSeconds = 15;
constexpr int kAuthSeconds = 30;
constexpr int kKeepaliveSeconds = 30;
constexpr int kIdleSliceMs = 50; // how long the forwarding loop sleeps when nothing happens
constexpr std::size_t kChunk = std::size_t(16) * 1024;
constexpr std::size_t kBufferLimit = std::size_t(64) * 1024; // per direction and connection
constexpr std::size_t kWriteChunk = std::size_t(32) * 1024;
constexpr std::size_t kMaxConnections = 256;

QString text(const char *source)
{
    return QCoreApplication::translate("quarry::SshTunnel", source);
}

struct Buffer
{
    std::vector<char> bytes;
    std::size_t head = 0;

    std::size_t size() const { return bytes.size() - head; }
    bool empty() const { return head == bytes.size(); }
    const char *data() const { return bytes.data() + head; }
    void append(const char *source, std::size_t count)
    {
        bytes.insert(bytes.end(), source, source + count);
    }
    void consume(std::size_t count)
    {
        head += count;
        if (head == bytes.size()) {
            bytes.clear();
            head = 0;
        } else if (head > kBufferLimit) {
            bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(head));
            head = 0;
        }
    }
};

// One accepted local connection and its SSH channel.
struct Connection
{
    NativeSocket socket = kInvalidSocket;
    LIBSSH2_CHANNEL *channel = nullptr;
    Buffer up; // client -> server, waiting for the channel
    Buffer down; // server -> client, waiting for the socket
    std::size_t pendingWrite = 0; // libssh2 wants the same length again after an EAGAIN
    bool openStarted = false; // channel open in flight: the entry must not be dropped yet
    bool clientEof = false;
    bool eofSent = false;
    bool remoteEof = false;
    bool failed = false;
};

#ifdef _WIN32
using PollFd = WSAPOLLFD;
int pollSockets(PollFd *fds, std::size_t count, int timeoutMs)
{
    return ::WSAPoll(fds, static_cast<ULONG>(count), timeoutMs);
}
#else
using PollFd = pollfd;
int pollSockets(PollFd *fds, std::size_t count, int timeoutMs)
{
    return ::poll(fds, static_cast<nfds_t>(count), timeoutMs);
}
#endif

bool isTransportCode(std::ptrdiff_t code)
{
    return code == LIBSSH2_ERROR_SOCKET_DISCONNECT || code == LIBSSH2_ERROR_SOCKET_SEND
        || code == LIBSSH2_ERROR_SOCKET_RECV || code == LIBSSH2_ERROR_SOCKET_TIMEOUT
        || code == LIBSSH2_ERROR_TIMEOUT || code == LIBSSH2_ERROR_BAD_SOCKET
        || code == LIBSSH2_ERROR_SOCKET_NONE;
}

} // namespace

struct SshTunnel::Impl
{
    Client client;
    NativeSocket listener = kInvalidSocket;
    int port = 0;
    QByteArray destHost;
    int destPort = 0;
    std::thread thread;
    std::atomic<bool> stop { false };
    std::atomic<bool> cancel { false };
    std::atomic<bool> open { false };
    // Only touched by the forwarding thread.
    std::vector<std::unique_ptr<Connection>> connections;
    bool sessionDead = false;

    void run();
    bool acceptClients();
    bool openChannel(Connection &conn);
    bool pump(Connection &conn);
    bool pumpClientToChannel(Connection &conn);
    bool pumpChannelToClient(Connection &conn);
    void closeConnection(Connection &conn);
    void noteSessionError(std::ptrdiff_t code);
    void teardown();
};

void SshTunnel::Impl::noteSessionError(std::ptrdiff_t code)
{
    if (isTransportCode(code) && !sessionDead) {
        sessionDead = true;
        qCWarning(lcSshTunnel) << "SSH connection lost:" << client.lastErrorText();
    }
}

bool SshTunnel::Impl::acceptClients()
{
    bool progress = false;
    for (;;) {
        const NativeSocket sock = ::accept(listener, nullptr, nullptr);
        if (sock == kInvalidSocket)
            break;
        progress = true;
        if (sessionDead || connections.size() >= kMaxConnections || !setNonBlocking(sock)) {
            closeSocket(sock);
            continue;
        }
#ifdef SO_NOSIGPIPE
        const int one = 1;
        ::setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char *>(&one),
                     sizeof(one));
#endif
        const int noDelay = 1;
        ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&noDelay),
                     sizeof(noDelay));
        auto conn = std::make_unique<Connection>();
        conn->socket = sock;
        connections.push_back(std::move(conn));
    }
    return progress;
}

// libssh2 allows one channel open at a time per session, so only the oldest connection without
// a channel advances; the others keep buffering what their client sends.
bool SshTunnel::Impl::openChannel(Connection &conn)
{
    conn.openStarted = true;
    LIBSSH2_CHANNEL *channel = libssh2_channel_direct_tcpip_ex(
        client.session(), destHost.constData(), destPort, "127.0.0.1", port);
    if (channel) {
        conn.channel = channel;
        return true;
    }
    const int code = libssh2_session_last_errno(client.session());
    if (code == LIBSSH2_ERROR_EAGAIN)
        return false;
    qCDebug(lcSshTunnel) << "direct-tcpip refused:" << client.lastErrorText();
    noteSessionError(code);
    conn.failed = true;
    return true;
}

bool SshTunnel::Impl::pumpClientToChannel(Connection &conn)
{
    bool progress = false;
    if (!conn.clientEof && conn.up.size() < kBufferLimit) {
        char buffer[kChunk];
        const std::ptrdiff_t n = receiveSome(conn.socket, buffer, sizeof(buffer));
        if (n > 0) {
            conn.up.append(buffer, static_cast<std::size_t>(n));
            progress = true;
        } else if (n == 0) {
            conn.clientEof = true;
            progress = true;
        } else if (!isWouldBlock(lastSocketError())) {
            conn.clientEof = true; // reset by the peer: nothing more will come
            conn.failed = true;
            return true;
        }
    }
    if (!conn.channel)
        return progress;

    while (!conn.up.empty()) {
        const std::size_t length = conn.pendingWrite
            ? conn.pendingWrite
            : std::min<std::size_t>(conn.up.size(), kWriteChunk);
        const std::ptrdiff_t written = libssh2_channel_write(conn.channel, conn.up.data(), length);
        if (written == LIBSSH2_ERROR_EAGAIN) {
            conn.pendingWrite = length;
            break;
        }
        conn.pendingWrite = 0;
        if (written < 0) {
            noteSessionError(written);
            conn.failed = true;
            return true;
        }
        conn.up.consume(static_cast<std::size_t>(written));
        progress = true;
    }
    if (conn.clientEof && conn.up.empty() && !conn.eofSent) {
        const int rc = libssh2_channel_send_eof(conn.channel);
        if (rc == 0) {
            conn.eofSent = true;
            progress = true;
        } else if (rc != LIBSSH2_ERROR_EAGAIN) {
            noteSessionError(rc);
            conn.failed = true;
            return true;
        }
    }
    return progress;
}

bool SshTunnel::Impl::pumpChannelToClient(Connection &conn)
{
    bool progress = false;
    if (conn.channel && !conn.remoteEof && conn.down.size() < kBufferLimit) {
        char buffer[kChunk];
        const std::ptrdiff_t n = libssh2_channel_read(conn.channel, buffer, sizeof(buffer));
        if (n > 0) {
            conn.down.append(buffer, static_cast<std::size_t>(n));
            progress = true;
        } else if (n == 0) {
            if (libssh2_channel_eof(conn.channel) == 1) {
                conn.remoteEof = true;
                progress = true;
            }
        } else if (n != LIBSSH2_ERROR_EAGAIN) {
            noteSessionError(n);
            conn.failed = true;
            return true;
        }
    }
    while (!conn.down.empty()) {
        const std::ptrdiff_t sent = sendSome(conn.socket, conn.down.data(), conn.down.size());
        if (sent > 0) {
            conn.down.consume(static_cast<std::size_t>(sent));
            progress = true;
            continue;
        }
        if (sent < 0 && !isWouldBlock(lastSocketError()))
            conn.failed = true;
        break;
    }
    return progress;
}

bool SshTunnel::Impl::pump(Connection &conn)
{
    const bool a = pumpClientToChannel(conn);
    const bool b = conn.failed ? false : pumpChannelToClient(conn);
    return a || b;
}

void SshTunnel::Impl::closeConnection(Connection &conn)
{
    if (conn.channel) {
        // Bounded: a dead peer must not hold the loop.
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (libssh2_channel_close(conn.channel) != LIBSSH2_ERROR_EAGAIN)
                break;
            client.waitForSession(10);
        }
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (libssh2_channel_free(conn.channel) != LIBSSH2_ERROR_EAGAIN)
                break;
            client.waitForSession(10);
        }
        conn.channel = nullptr;
    }
    closeSocket(conn.socket);
    conn.socket = kInvalidSocket;
}

void SshTunnel::Impl::teardown()
{
    // Channels are released with the session; no per-channel handshake while shutting down.
    for (auto &conn : connections)
        closeSocket(conn->socket);
    connections.clear();
}

void SshTunnel::Impl::run()
{
    using Clock = std::chrono::steady_clock;
    auto nextKeepalive = Clock::now() + std::chrono::seconds(kKeepaliveSeconds);
    bool progress = false;

    while (!stop.load()) {
        progress = acceptClients();

        // Channel opens first so a new client does not wait behind a busy transfer.
        for (auto &conn : connections) {
            if (conn->channel || conn->failed)
                continue;
            if (sessionDead) {
                conn->failed = true;
                continue;
            }
            progress = openChannel(*conn) || progress;
            break; // one open at a time
        }

        for (auto &conn : connections) {
            if (conn->failed)
                continue;
            if (sessionDead) {
                conn->failed = true;
                continue;
            }
            progress = pump(*conn) || progress;
            if (conn->remoteEof && conn->down.empty())
                conn->failed = true; // finished cleanly: same cleanup path
        }

        if (Clock::now() >= nextKeepalive && !sessionDead) {
            int seconds = kKeepaliveSeconds;
            const int rc = libssh2_keepalive_send(client.session(), &seconds);
            if (rc == 0)
                nextKeepalive = Clock::now() + std::chrono::seconds(std::max(seconds, 1));
            else if (rc != LIBSSH2_ERROR_EAGAIN)
                noteSessionError(rc);
        }

        // Drop finished or failed connections, except one whose channel open is still in flight.
        for (auto it = connections.begin(); it != connections.end();) {
            Connection &conn = **it;
            const bool opening = conn.openStarted && !conn.channel && !conn.failed;
            if (conn.failed && !opening) {
                closeConnection(conn);
                it = connections.erase(it);
                progress = true;
            } else {
                ++it;
            }
        }

        if (progress)
            continue;

        // Nothing to do: sleep until a socket is ready or the slice ends (the slice also bounds
        // how long close() waits for this thread).
        std::vector<PollFd> fds;
        fds.reserve(connections.size() + 2);
        auto add = [&](NativeSocket socket, short events) {
            PollFd entry {};
            entry.fd = socket;
            entry.events = events;
            fds.push_back(entry);
        };
        add(listener, POLLIN);
        bool wantServer = false;
        for (const auto &conn : connections) {
            short events = 0;
            if (conn->up.size() < kBufferLimit && !conn->clientEof)
                events |= POLLIN;
            if (!conn->down.empty())
                events |= POLLOUT;
            add(conn->socket, events);
            if (!conn->channel || (!conn->remoteEof && conn->down.size() < kBufferLimit))
                wantServer = true;
        }
        // Without a reader the SSH socket would stay readable and spin the loop.
        if (wantServer)
            add(client.socket(), POLLIN);
        pollSockets(fds.data(), fds.size(), kIdleSliceMs);
    }
    teardown();
}

SshTunnel::SshTunnel()
    : m_impl(std::make_unique<Impl>())
{ }

SshTunnel::~SshTunnel()
{
    close();
}

bool SshTunnel::isOpen() const
{
    return m_impl->open.load();
}

int SshTunnel::localPort() const
{
    return m_impl->open.load() ? m_impl->port : 0;
}

void SshTunnel::requestCancel()
{
    m_impl->cancel.store(true);
}

void SshTunnel::close()
{
    Impl &impl = *m_impl;
    impl.stop.store(true);
    if (impl.thread.joinable())
        impl.thread.join();
    impl.open.store(false);
    impl.client.disconnect();
    closeSocket(impl.listener);
    impl.listener = kInvalidSocket;
    impl.port = 0;
}

Result<int> SshTunnel::open(const SshTunnelConfig &config, const Secrets &secrets,
                            const KnownHostsStore &knownHosts, const QString &destHost,
                            int destPort, const CancellationToken &token)
{
    close();
    Impl &impl = *m_impl;
    impl.stop.store(false);
    impl.cancel.store(false);
    impl.sessionDead = false;
    impl.destHost = destHost.toUtf8();
    impl.destPort = destPort;

    auto failWith = [&](Error error) -> Result<int> {
        impl.client.disconnect();
        closeSocket(impl.listener);
        impl.listener = kInvalidSocket;
        return fail(std::move(error));
    };

    const QString problem = config.validationProblem();
    if (!problem.isEmpty())
        return failWith({ ErrorKind::Connection, problem, {}, {}, {} });

    Interrupt interrupt;
    interrupt.cancelled = [&impl, token] { return token.isCancelled() || impl.cancel.load(); };

    interrupt.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kHandshakeSeconds);
    Result<void> connected = impl.client.connectAndHandshake(config.host, config.port, interrupt);
    if (!connected)
        return failWith(std::move(connected).error());

    const HostKeyInfo seen = knownHosts.classify(impl.client.hostKey(config.host, config.port));
    if (seen.status == HostKeyStatus::Unknown) {
        return failWith({ ErrorKind::Connection,
                          text("The SSH server's host key is not trusted yet. Connect again and "
                               "confirm its fingerprint."),
                          seen.fingerprint,
                          QString::fromLatin1(kSshUntrustedState),
                          {} });
    }
    if (seen.status == HostKeyStatus::Mismatch) {
        return failWith({ ErrorKind::Connection,
                          text("The SSH server's host key has changed. Someone may be "
                               "impersonating the server; the connection was blocked."),
                          seen.fingerprint,
                          QString::fromLatin1(kSshMismatchState),
                          {} });
    }

    interrupt.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kAuthSeconds);
    Result<void> authenticated = impl.client.authenticate(config, secrets, interrupt);
    if (!authenticated)
        return failWith(std::move(authenticated).error());

    // Loopback only: the forwarded port must not be reachable from the network.
    initNetworking();
    const NativeSocket listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener == kInvalidSocket) {
        return failWith({ ErrorKind::Connection,
                          text("Could not open the local port for the SSH tunnel."),
                          socketErrorText(lastSocketError()),
                          {},
                          {} });
    }
    impl.listener = listener;
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sockaddr_in bound {};
#ifdef _WIN32
    int boundLength = sizeof(bound);
#else
    socklen_t boundLength = sizeof(bound);
#endif
    if (::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0
        || ::listen(listener, 32) != 0
        || ::getsockname(listener, reinterpret_cast<sockaddr *>(&bound), &boundLength) != 0
        || !setNonBlocking(listener)) {
        return failWith({ ErrorKind::Connection,
                          text("Could not open the local port for the SSH tunnel."),
                          socketErrorText(lastSocketError()),
                          {},
                          {} });
    }
    impl.port = ntohs(bound.sin_port);

    // Keepalives are sent by the forwarding loop (non-blocking sessions must do it themselves).
    libssh2_keepalive_config(impl.client.session(), 0, kKeepaliveSeconds);

    // A cancel that arrived after the last check still wins.
    if (interrupt.cancelled())
        return failWith(stopError(StopReason::Cancelled, kHandshakeSeconds));

    try {
        impl.thread = std::thread([&impl] {
            try {
                impl.run();
            } catch (...) {
                qCWarning(lcSshTunnel) << "The SSH forwarding thread stopped unexpectedly.";
                impl.teardown();
            }
        });
    } catch (...) {
        return failWith(
            { ErrorKind::Internal, text("Could not start the SSH tunnel thread."), {}, {}, {} });
    }
    impl.open.store(true);
    return impl.port;
}

} // namespace quarry
