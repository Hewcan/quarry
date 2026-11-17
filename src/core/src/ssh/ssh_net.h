#pragma once

// Portable raw-socket helpers for the SSH code (core links Qt6::Core only, so no QtNetwork).

#include <quarry/result.h>

#include <QString>

#include <chrono>
#include <cstddef>
#include <functional>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/types.h>
#endif

namespace quarry::ssh {

#ifdef _WIN32
using NativeSocket = SOCKET;
inline constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
inline constexpr NativeSocket kInvalidSocket = -1;
#endif

// Why a blocking-style wait stopped early.
enum class StopReason { None, Cancelled, Timeout };

// Cancellation and overall deadline of one open() / check().
struct Interrupt
{
    std::function<bool()> cancelled; // may be empty
    std::chrono::steady_clock::time_point deadline = (std::chrono::steady_clock::time_point::max)();

    StopReason check() const;
    // Milliseconds left before the deadline, at least 0 and at most `cap`.
    int remainingMs(int cap) const;
};

// Starts Winsock on Windows (idempotent) and initialises libssh2 once per process.
void initNetworking();

void closeSocket(NativeSocket socket);
bool setNonBlocking(NativeSocket socket);
int lastSocketError();
bool isWouldBlock(int error);
QString socketErrorText(int error);

// Single-socket wait in a bounded slice. Returns true if the socket became ready.
bool waitSocket(NativeSocket socket, bool forRead, bool forWrite, int timeoutMs);

// send() that never raises SIGPIPE; returns bytes sent or -1 (check lastSocketError()).
std::ptrdiff_t sendSome(NativeSocket socket, const char *data, std::size_t size);
std::ptrdiff_t receiveSome(NativeSocket socket, char *data, std::size_t size);

Error stopError(StopReason reason, int timeoutSeconds);

// Non-blocking connect with cancellation and a deadline; tries every resolved address.
// Errors: Connection (resolution or connect failure), Timeout, Cancelled.
Result<NativeSocket> connectTcp(const QString &host, int port, const Interrupt &interrupt);

} // namespace quarry::ssh
