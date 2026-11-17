#include "ssh_client.h"

#include <QCoreApplication>
#include <QFile>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace quarry::ssh {

namespace {

QString text(const char *source)
{
    return QCoreApplication::translate("quarry::SshTunnel", source);
}

constexpr int kSliceMs = 50;
constexpr qint64 kMaxKeyFileBytes
    = qint64(1024) * 1024; // a private key is a few KB; refuse a huge file

// Allocator handed to libssh2 so its memory is always released by the same C runtime that
// allocated it, whatever runtime the libssh2 DLL was built against.
LIBSSH2_ALLOC_FUNC(allocate)
{
    Q_UNUSED(abstract)
    return std::malloc(count);
}
LIBSSH2_REALLOC_FUNC(reallocate)
{
    Q_UNUSED(abstract)
    return std::realloc(ptr, count);
}
LIBSSH2_FREE_FUNC(release)
{
    Q_UNUSED(abstract)
    std::free(ptr);
}

// Password for the keyboard-interactive callback, reached through the session's abstract slot.
struct KeyboardState
{
    QByteArray password;
};

LIBSSH2_USERAUTH_KBDINT_RESPONSE_FUNC(answerPrompts)
{
    Q_UNUSED(name)
    Q_UNUSED(name_len)
    Q_UNUSED(instruction)
    Q_UNUSED(instruction_len)
    Q_UNUSED(prompts)
    const auto *state = static_cast<const KeyboardState *>(*abstract);
    for (int i = 0; i < num_prompts; ++i) {
        // Freed by libssh2 through the allocator above. A server asking more than one question
        // gets the password for each; that covers the usual "Password:" prompt.
        auto *copy = static_cast<char *>(
            std::malloc(static_cast<std::size_t>(state->password.size()) + 1));
        if (!copy) {
            responses[i].text = nullptr;
            responses[i].length = 0;
            continue;
        }
        std::memcpy(copy, state->password.constData(),
                    static_cast<std::size_t>(state->password.size()));
        copy[state->password.size()] = '\0';
        responses[i].text = copy;
        responses[i].length = static_cast<unsigned int>(state->password.size());
    }
}

bool isTransportError(int code)
{
    switch (code) {
    case LIBSSH2_ERROR_SOCKET_NONE:
    case LIBSSH2_ERROR_BANNER_RECV:
    case LIBSSH2_ERROR_BANNER_SEND:
    case LIBSSH2_ERROR_INVALID_MAC:
    case LIBSSH2_ERROR_KEX_FAILURE:
    case LIBSSH2_ERROR_SOCKET_SEND:
    case LIBSSH2_ERROR_KEY_EXCHANGE_FAILURE:
    case LIBSSH2_ERROR_TIMEOUT:
    case LIBSSH2_ERROR_SOCKET_DISCONNECT:
    case LIBSSH2_ERROR_PROTO:
    case LIBSSH2_ERROR_SOCKET_TIMEOUT:
    case LIBSSH2_ERROR_SOCKET_RECV:
    case LIBSSH2_ERROR_BAD_SOCKET:
        return true;
    default:
        return false;
    }
}

bool listContains(const QByteArray &list, const char *method)
{
    return list.split(',').contains(QByteArray(method));
}

} // namespace

Client::Client()
{
    initNetworking();
}

Client::~Client()
{
    disconnect();
}

QString Client::lastErrorText() const
{
    if (!m_session)
        return {};
    char *message = nullptr;
    int length = 0;
    libssh2_session_last_error(m_session, &message, &length, 0);
    return message ? QString::fromUtf8(message, length) : QString();
}

Error Client::stopped(const Interrupt &interrupt) const
{
    Q_UNUSED(interrupt)
    return stopError(m_stop, kConnectTimeoutSeconds);
}

Error Client::failure(int code, const QString &message, ErrorKind kind) const
{
    const QString detail = QStringLiteral("%1 (libssh2 error %2)").arg(lastErrorText()).arg(code);
    return { isTransportError(code) ? ErrorKind::Connection : kind, message, detail, {}, {} };
}

void Client::waitForSession(int timeoutMs) const
{
    const int directions = libssh2_session_block_directions(m_session);
    const bool read = (directions & LIBSSH2_SESSION_BLOCK_INBOUND) != 0;
    const bool write = (directions & LIBSSH2_SESSION_BLOCK_OUTBOUND) != 0;
    waitSocket(m_socket, read || !write, write, timeoutMs);
}

template<typename F> int Client::drive(const Interrupt &interrupt, F &&call)
{
    m_stop = StopReason::None;
    for (;;) {
        const int rc = call();
        if (rc != LIBSSH2_ERROR_EAGAIN)
            return rc;
        m_stop = interrupt.check();
        if (m_stop != StopReason::None)
            return LIBSSH2_ERROR_EAGAIN;
        waitForSession(std::min(kSliceMs, interrupt.remainingMs(kSliceMs)));
    }
}

Result<void> Client::connectAndHandshake(const QString &host, int port, const Interrupt &interrupt)
{
    disconnect();
    auto sock = connectTcp(host, port, interrupt);
    if (!sock)
        return fail(std::move(sock).error());
    m_socket = sock.value();

    m_session = libssh2_session_init_ex(allocate, release, reallocate, nullptr);
    if (!m_session) {
        disconnect();
        return fail(
            Error { ErrorKind::Internal, text("Could not start the SSH library."), {}, {}, {} });
    }
    libssh2_session_set_blocking(m_session, 0);

    const int rc = drive(interrupt, [&] { return libssh2_session_handshake(m_session, m_socket); });
    if (m_stop != StopReason::None) {
        Error error = stopped(interrupt);
        disconnect();
        return fail(std::move(error));
    }
    if (rc != 0) {
        Error error = failure(
            rc, text("The server at %1:%2 did not answer as an SSH server.").arg(host).arg(port),
            ErrorKind::Connection);
        error.kind = ErrorKind::Connection;
        disconnect();
        return fail(std::move(error));
    }
    return {};
}

HostKeyInfo Client::hostKey(const QString &host, int port) const
{
    HostKeyInfo info;
    info.host = host;
    info.port = port;
    size_t length = 0;
    int type = 0;
    const char *key = m_session ? libssh2_session_hostkey(m_session, &length, &type) : nullptr;
    if (key)
        info.key = QByteArray(key, static_cast<qsizetype>(length));
    return info;
}

Result<void> Client::authenticate(const SshTunnelConfig &config, const Secrets &secrets,
                                  const Interrupt &interrupt)
{
    const QByteArray user = config.user.toUtf8();
    switch (config.auth) {
    case SshAuth::Password:
        return authenticateWithPassword(user, secrets.sshPassword, interrupt);
    case SshAuth::Key:
        return authenticateWithKey(user, config, secrets.sshPassphrase, interrupt);
    case SshAuth::Agent:
        return authenticateWithAgent(user, interrupt);
    }
    return fail(
        Error { ErrorKind::Internal, text("Unknown SSH authentication method."), {}, {}, {} });
}

Result<void> Client::authenticateWithPassword(const QByteArray &user, const QString &password,
                                              const Interrupt &interrupt)
{
    char *rawList = nullptr;
    const int listRc = drive(interrupt, [&] {
        rawList = libssh2_userauth_list(m_session, user.constData(),
                                        static_cast<unsigned int>(user.size()));
        return rawList ? 0 : libssh2_session_last_errno(m_session);
    });
    if (m_stop != StopReason::None)
        return fail(stopped(interrupt));
    if (listRc != 0) {
        if (libssh2_userauth_authenticated(m_session) != 0)
            return {}; // the server let the user in without credentials
        return fail(
            failure(listRc, text("The SSH server rejected the login."), ErrorKind::Authentication));
    }
    const QByteArray methods(rawList);
    const bool passwordAllowed = listContains(methods, "password");
    const bool keyboardAllowed = listContains(methods, "keyboard-interactive");
    if (!passwordAllowed && !keyboardAllowed) {
        return fail(Error { ErrorKind::Authentication,
                            text("The SSH server does not accept password login. Try a key or the "
                                 "SSH agent."),
                            QString::fromLatin1(methods),
                            {},
                            {} });
    }

    const QByteArray secret = password.toUtf8();
    int rc = LIBSSH2_ERROR_AUTHENTICATION_FAILED;
    if (passwordAllowed) {
        rc = drive(interrupt, [&] {
            return libssh2_userauth_password_ex(
                m_session, user.constData(), static_cast<unsigned int>(user.size()),
                secret.constData(), static_cast<unsigned int>(secret.size()), nullptr);
        });
        if (m_stop != StopReason::None)
            return fail(stopped(interrupt));
        if (rc == 0)
            return {};
        if (rc == LIBSSH2_ERROR_PASSWORD_EXPIRED) {
            return fail(
                failure(rc, text("The SSH password has expired. Change it on the server first."),
                        ErrorKind::Authentication));
        }
    }
    // Many servers only offer the password through keyboard-interactive.
    if (keyboardAllowed && (rc == LIBSSH2_ERROR_AUTHENTICATION_FAILED || !passwordAllowed)) {
        KeyboardState state { secret };
        *libssh2_session_abstract(m_session) = &state;
        rc = drive(interrupt, [&] {
            return libssh2_userauth_keyboard_interactive_ex(
                m_session, user.constData(), static_cast<unsigned int>(user.size()), answerPrompts);
        });
        *libssh2_session_abstract(m_session) = nullptr;
        if (m_stop != StopReason::None)
            return fail(stopped(interrupt));
        if (rc == 0)
            return {};
    }
    return fail(failure(rc, text("SSH login failed. Check the SSH user name and password."),
                        ErrorKind::Authentication));
}

Result<void> Client::authenticateWithKey(const QByteArray &user, const SshTunnelConfig &config,
                                         const QString &passphrase, const Interrupt &interrupt)
{
    // Read here instead of letting libssh2 fopen() the path: that call uses the ANSI code page on
    // Windows and would fail on non-ASCII folder names.
    QFile file(config.keyPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxKeyFileBytes) {
        return fail(
            Error { ErrorKind::Authentication,
                    text("The SSH private key file \"%1\" could not be read.").arg(config.keyPath),
                    file.errorString(),
                    {},
                    {} });
    }
    const QByteArray privateKey = file.readAll();
    const QByteArray phrase = passphrase.toUtf8();

    const int rc = drive(interrupt, [&] {
        return libssh2_userauth_publickey_frommemory(
            m_session, user.constData(), static_cast<size_t>(user.size()), nullptr, 0,
            privateKey.constData(), static_cast<size_t>(privateKey.size()),
            phrase.isEmpty() ? nullptr : phrase.constData());
    });
    if (m_stop != StopReason::None)
        return fail(stopped(interrupt));
    if (rc == 0)
        return {};
    if (rc == LIBSSH2_ERROR_FILE || rc == LIBSSH2_ERROR_KEYFILE_AUTH_FAILED
        || rc == LIBSSH2_ERROR_METHOD_NOT_SUPPORTED) {
        return fail(failure(rc,
                            text("The SSH private key could not be read. Check the passphrase and "
                                 "that the file is a supported private key."),
                            ErrorKind::Authentication));
    }
    return fail(failure(rc, text("The SSH server rejected the key for this user."),
                        ErrorKind::Authentication));
}

Result<void> Client::authenticateWithAgent(const QByteArray &user, const Interrupt &interrupt)
{
    LIBSSH2_AGENT *agent = libssh2_agent_init(m_session);
    if (!agent) {
        return fail(Error {
            ErrorKind::Authentication, text("SSH agent not available."), lastErrorText(), {}, {} });
    }
    struct AgentGuard
    {
        LIBSSH2_AGENT *agent;
        bool connected = false;
        ~AgentGuard()
        {
            if (connected)
                libssh2_agent_disconnect(agent);
            libssh2_agent_free(agent);
        }
    } guard { agent };

    // Windows: Pageant, then the OpenSSH agent named pipe, handled inside libssh2. Elsewhere:
    // SSH_AUTH_SOCK.
    int rc = drive(interrupt, [&] { return libssh2_agent_connect(agent); });
    if (m_stop != StopReason::None)
        return fail(stopped(interrupt));
    if (rc != 0) {
        return fail(Error { ErrorKind::Authentication,
                            text("SSH agent not available. Start the agent (Pageant or the "
                                 "OpenSSH agent on Windows, ssh-agent elsewhere) and try again."),
                            lastErrorText(),
                            {},
                            {} });
    }
    guard.connected = true;

    rc = drive(interrupt, [&] { return libssh2_agent_list_identities(agent); });
    if (m_stop != StopReason::None)
        return fail(stopped(interrupt));
    if (rc != 0) {
        return fail(failure(rc, text("Could not read the keys from the SSH agent."),
                            ErrorKind::Authentication));
    }

    libssh2_agent_publickey *identity = nullptr;
    libssh2_agent_publickey *previous = nullptr;
    bool anyKey = false;
    while (libssh2_agent_get_identity(agent, &identity, previous) == 0) {
        anyKey = true;
        rc = drive(interrupt,
                   [&] { return libssh2_agent_userauth(agent, user.constData(), identity); });
        if (m_stop != StopReason::None)
            return fail(stopped(interrupt));
        if (rc == 0)
            return {};
        if (isTransportError(rc))
            return fail(failure(rc, text("The SSH connection was lost during login."),
                                ErrorKind::Connection));
        previous = identity;
    }
    if (!anyKey) {
        return fail(Error { ErrorKind::Authentication,
                            text("The SSH agent has no keys loaded. Add one with ssh-add."),
                            {},
                            {},
                            {} });
    }
    return fail(failure(rc, text("The SSH server rejected every key from the agent."),
                        ErrorKind::Authentication));
}

void Client::disconnect()
{
    if (m_session) {
        // Best effort and bounded: the peer may already be gone.
        for (int attempt = 0; attempt < 5; ++attempt) {
            const int rc = libssh2_session_disconnect_ex(m_session, SSH_DISCONNECT_BY_APPLICATION,
                                                         "Quarry closing the tunnel", "");
            if (rc != LIBSSH2_ERROR_EAGAIN)
                break;
            waitForSession(20);
        }
        libssh2_session_free(m_session);
        m_session = nullptr;
    }
    closeSocket(m_socket);
    m_socket = kInvalidSocket;
}

} // namespace quarry::ssh
