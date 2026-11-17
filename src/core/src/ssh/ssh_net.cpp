#include "ssh_net.h"

#include <QCoreApplication>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <libssh2.h>
#include <mutex>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace quarry::ssh {

namespace {

constexpr int kSliceMs = 50; // longest a wait goes without looking at the cancel flag

QString text(const char *source)
{
    return QCoreApplication::translate("quarry::SshTunnel", source);
}

} // namespace

StopReason Interrupt::check() const
{
    if (cancelled && cancelled())
        return StopReason::Cancelled;
    if (std::chrono::steady_clock::now() >= deadline)
        return StopReason::Timeout;
    return StopReason::None;
}

int Interrupt::remainingMs(int cap) const
{
    if (deadline == (std::chrono::steady_clock::time_point::max)())
        return cap;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    return static_cast<int>(std::clamp<long long>(left.count(), 0, cap));
}

void initNetworking()
{
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef _WIN32
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        libssh2_init(0);
    });
}

void closeSocket(NativeSocket socket)
{
    if (socket == kInvalidSocket)
        return;
#ifdef _WIN32
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

bool setNonBlocking(NativeSocket socket)
{
#ifdef _WIN32
    u_long enable = 1;
    return ::ioctlsocket(socket, FIONBIO, &enable) == 0;
#else
    const int flags = ::fcntl(socket, F_GETFL, 0);
    return flags >= 0 && ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

int lastSocketError()
{
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

bool isWouldBlock(int error)
{
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return error == EWOULDBLOCK || error == EAGAIN || error == EINPROGRESS || error == EINTR;
#endif
}

QString socketErrorText(int error)
{
#ifdef _WIN32
    char *buffer = nullptr;
    const DWORD length = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(error), 0, reinterpret_cast<char *>(&buffer), 0, nullptr);
    QString result = length > 0 ? QString::fromLocal8Bit(buffer, static_cast<qsizetype>(length))
                                : QStringLiteral("error %1").arg(error);
    if (buffer)
        ::LocalFree(buffer);
    return result.trimmed();
#else
    return QString::fromLocal8Bit(std::strerror(error));
#endif
}

bool waitSocket(NativeSocket socket, bool forRead, bool forWrite, int timeoutMs)
{
    fd_set readSet;
    fd_set writeSet;
    fd_set errorSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    FD_ZERO(&errorSet);
    if (forRead)
        FD_SET(socket, &readSet);
    if (forWrite)
        FD_SET(socket, &writeSet);
    FD_SET(socket, &errorSet);
    timeval timeout {};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    // The first argument is ignored by Winsock; POSIX wants highest fd + 1.
    const int ready = ::select(static_cast<int>(socket) + 1, forRead ? &readSet : nullptr,
                               forWrite ? &writeSet : nullptr, &errorSet, &timeout);
    return ready > 0;
}

std::ptrdiff_t sendSome(NativeSocket socket, const char *data, std::size_t size)
{
#ifdef _WIN32
    return ::send(socket, data, static_cast<int>(std::min<std::size_t>(size, 1U << 20)), 0);
#elif defined(MSG_NOSIGNAL)
    return ::send(socket, data, size, MSG_NOSIGNAL);
#else
    return ::send(socket, data, size, 0); // macOS: SO_NOSIGPIPE is set on the socket instead
#endif
}

std::ptrdiff_t receiveSome(NativeSocket socket, char *data, std::size_t size)
{
#ifdef _WIN32
    return ::recv(socket, data, static_cast<int>(std::min<std::size_t>(size, 1U << 20)), 0);
#else
    return ::recv(socket, data, size, 0);
#endif
}

Error stopError(StopReason reason, int timeoutSeconds)
{
    if (reason == StopReason::Timeout) {
        return { ErrorKind::Timeout,
                 text("The SSH server did not answer in %1 seconds.").arg(timeoutSeconds),
                 {},
                 {},
                 {} };
    }
    return { ErrorKind::Cancelled, text("The SSH connection was cancelled."), {}, {}, {} };
}

Result<NativeSocket> connectTcp(const QString &host, int port, const Interrupt &interrupt)
{
    initNetworking();

    auto connectionError = [&](const QString &message, const QString &detail) {
        return fail(Error { ErrorKind::Connection, message, detail, {}, {} });
    };
    const QString unreachable
        = text("Could not connect to the SSH server %1:%2. Check the host and port and that the "
               "server is running.")
              .arg(host)
              .arg(port);

    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *list = nullptr;
    // shortcut: getaddrinfo cannot be interrupted, so a cancel during a slow DNS lookup is only
    // noticed when it returns; move the lookup to a helper thread if that ever hurts.
    const int resolved = ::getaddrinfo(host.toUtf8().constData(),
                                       QByteArray::number(port).constData(), &hints, &list);
    if (resolved != 0 || !list) {
#ifdef _WIN32
        const QString detail = socketErrorText(resolved);
#else
        const QString detail = QString::fromLocal8Bit(::gai_strerror(resolved));
#endif
        return connectionError(text("The SSH server name \"%1\" could not be resolved.").arg(host),
                               detail);
    }
    struct ListGuard
    {
        addrinfo *list;
        ~ListGuard() { ::freeaddrinfo(list); }
    } guard { list };

    int addressesLeft = 0;
    for (const addrinfo *ai = list; ai; ai = ai->ai_next)
        ++addressesLeft;

    QString lastDetail;
    for (const addrinfo *ai = list; ai; ai = ai->ai_next, --addressesLeft) {
        const StopReason before = interrupt.check();
        if (before != StopReason::None)
            return fail(stopError(before, 15));

        const NativeSocket sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == kInvalidSocket) {
            lastDetail = socketErrorText(lastSocketError());
            continue;
        }
#ifdef SO_NOSIGPIPE
        const int one = 1;
        ::setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char *>(&one),
                     sizeof(one));
#endif
        if (!setNonBlocking(sock)) {
            lastDetail = socketErrorText(lastSocketError());
            closeSocket(sock);
            continue;
        }

        // Split what is left of the deadline between the addresses still to try, so a black-holed
        // first address does not eat the whole budget.
        const auto attemptEnd = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(interrupt.remainingMs(15000) / std::max(addressesLeft, 1));

        int rc = ::connect(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        bool connected = rc == 0;
        if (!connected) {
            const int error = lastSocketError();
            if (!isWouldBlock(error)) {
                lastDetail = socketErrorText(error);
                closeSocket(sock);
                continue;
            }
            for (;;) {
                const StopReason reason = interrupt.check();
                if (reason != StopReason::None) {
                    closeSocket(sock);
                    return fail(stopError(reason, 15));
                }
                if (std::chrono::steady_clock::now() >= attemptEnd) {
                    lastDetail = text("The connection attempt timed out.");
                    break;
                }
                if (!waitSocket(sock, false, true, kSliceMs))
                    continue;
                int soError = 0;
#ifdef _WIN32
                int length = sizeof(soError);
                ::getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soError),
                             &length);
#else
                socklen_t length = sizeof(soError);
                ::getsockopt(sock, SOL_SOCKET, SO_ERROR, &soError, &length);
#endif
                if (soError == 0)
                    connected = true;
                else
                    lastDetail = socketErrorText(soError);
                break;
            }
        }
        if (connected) {
            const int noDelay = 1; // interactive protocol: do not let Nagle batch small packets
            ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&noDelay),
                         sizeof(noDelay));
            return sock;
        }
        closeSocket(sock);
    }
    return connectionError(unreachable, lastDetail);
}

} // namespace quarry::ssh
