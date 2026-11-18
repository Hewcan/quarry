#pragma once

// Tiny blocking-socket helpers for the SSH tests (no QtNetwork: core only links Qt6::Core).

#include <QByteArray>
#include <QString>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <ssh/ssh_net.h>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace quarry::test {

using ssh::NativeSocket;

// 127.0.0.1 listener that behaves according to `Mode` for every accepted connection.
class TcpTestServer
{
public:
    enum class Mode {
        Close, // accepts and closes at once
        Garbage, // sends a line that is not an SSH banner, then closes
        Silent, // accepts and never answers
        Echo // sends back everything it receives
    };

    explicit TcpTestServer(Mode mode)
        : m_mode(mode)
    {
        ssh::initNetworking();
        m_listener = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        static_cast<void>(
            ::bind(m_listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
        static_cast<void>(::listen(m_listener, 16));
#ifdef _WIN32
        int length = sizeof(address);
#else
        socklen_t length = sizeof(address);
#endif
        ::getsockname(m_listener, reinterpret_cast<sockaddr *>(&address), &length);
        m_port = ntohs(address.sin_port);
        ssh::setNonBlocking(m_listener);
        m_thread = std::thread([this] { acceptLoop(); });
    }

    ~TcpTestServer()
    {
        m_stop = true;
        m_thread.join();
        for (std::thread &worker : m_workers)
            worker.join();
        for (const NativeSocket held : m_held)
            ssh::closeSocket(held);
        ssh::closeSocket(m_listener);
    }

    TcpTestServer(const TcpTestServer &) = delete;
    TcpTestServer &operator=(const TcpTestServer &) = delete;

    int port() const { return m_port; }

private:
    void acceptLoop()
    {
        while (!m_stop) {
            if (!ssh::waitSocket(m_listener, true, false, 50))
                continue;
            const NativeSocket client = ::accept(m_listener, nullptr, nullptr);
            if (client == ssh::kInvalidSocket)
                continue;
            switch (m_mode) {
            case Mode::Close:
                ssh::closeSocket(client);
                break;
            case Mode::Garbage: {
                static const char text[] = "this is definitely not ssh\r\n";
                ssh::sendSome(client, text, sizeof(text) - 1);
                ssh::closeSocket(client);
                break;
            }
            case Mode::Silent:
                m_held.push_back(client);
                break;
            case Mode::Echo:
                m_workers.emplace_back([this, client] { echo(client); });
                break;
            }
        }
    }

    void echo(NativeSocket client)
    {
        char buffer[8192];
        while (!m_stop) {
            if (!ssh::waitSocket(client, true, false, 50))
                continue;
            const std::ptrdiff_t n = ssh::receiveSome(client, buffer, sizeof(buffer));
            if (n <= 0)
                break;
            std::ptrdiff_t done = 0;
            while (done < n && !m_stop) {
                if (!ssh::waitSocket(client, false, true, 50))
                    continue;
                const std::ptrdiff_t sent
                    = ssh::sendSome(client, buffer + done, static_cast<std::size_t>(n - done));
                if (sent < 0)
                    break;
                done += sent;
            }
        }
        ssh::closeSocket(client);
    }

    Mode m_mode;
    NativeSocket m_listener = ssh::kInvalidSocket;
    int m_port = 0;
    std::atomic<bool> m_stop { false };
    std::thread m_thread;
    std::vector<std::thread> m_workers; // only touched by m_thread until the destructor
    std::vector<NativeSocket> m_held;
};

// A port that nothing listens on (bind, read the number, close).
inline int unusedPort()
{
    ssh::initNetworking();
    const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    static_cast<void>(::bind(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    ::getsockname(socket, reinterpret_cast<sockaddr *>(&address), &length);
    const int port = ntohs(address.sin_port);
    ssh::closeSocket(socket);
    return port;
}

// Blocking client connected to 127.0.0.1:port; kInvalidSocket on failure.
inline NativeSocket connectLocal(int port)
{
    ssh::initNetworking();
    const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(port));
    if (::connect(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        ssh::closeSocket(socket);
        return ssh::kInvalidSocket;
    }
    return socket;
}

inline bool sendAll(NativeSocket socket, const QByteArray &data, int timeoutMs = 10000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    qsizetype done = 0;
    while (done < data.size()) {
        if (std::chrono::steady_clock::now() > end)
            return false;
        if (!ssh::waitSocket(socket, false, true, 50))
            continue;
        const std::ptrdiff_t sent = ssh::sendSome(socket, data.constData() + done,
                                                  static_cast<std::size_t>(data.size() - done));
        if (sent < 0)
            return false;
        done += sent;
    }
    return true;
}

// Reads until `count` bytes arrived, the peer closed, or the timeout expired.
inline QByteArray receiveUpTo(NativeSocket socket, qsizetype count, int timeoutMs = 10000)
{
    QByteArray result;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    char buffer[16384];
    while (result.size() < count && std::chrono::steady_clock::now() < end) {
        if (!ssh::waitSocket(socket, true, false, 50))
            continue;
        const std::ptrdiff_t n = ssh::receiveSome(
            socket, buffer,
            static_cast<std::size_t>(std::min<qsizetype>(count - result.size(), sizeof(buffer))));
        if (n <= 0)
            break;
        result.append(buffer, static_cast<qsizetype>(n));
    }
    return result;
}

} // namespace quarry::test
