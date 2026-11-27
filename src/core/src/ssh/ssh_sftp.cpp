#include "ssh_sftp.h"

#include "ssh_client.h"

#include <QCoreApplication>

#include <algorithm>
#include <chrono>
#include <libssh2_sftp.h>
#include <memory>

namespace quarry::ssh {

namespace {

constexpr int kHandshakeSeconds = 15;
constexpr int kAuthSeconds = 30;
constexpr int kSftpOpSeconds = 30;
constexpr int kCloseSliceMs = 50;
constexpr int kCloseTimeoutMs = 500;
constexpr int kReadBufferBytes = 16 * 1024;
constexpr int kNameBufferBytes = 4096;

QString text(const char *source)
{
    return QCoreApplication::translate("quarry::SshSftp", source);
}

Error stoppedError(StopReason reason, const QString &operation)
{
    if (reason == StopReason::Cancelled)
        return { ErrorKind::Cancelled, text("Reading the migrations was cancelled."), {}, {}, {} };
    return {
        ErrorKind::Timeout, text("Timed out while waiting for the SSH server."), operation, {}, {}
    };
}

Error sftpError(Client &client, LIBSSH2_SFTP *sftp, int rc, const QString &path, bool forFolder)
{
    if (rc == LIBSSH2_ERROR_SFTP_PROTOCOL && sftp != nullptr) {
        const unsigned long fx = libssh2_sftp_last_error(sftp);
        if (fx == LIBSSH2_FX_NO_SUCH_FILE || fx == LIBSSH2_FX_NO_SUCH_PATH) {
            return { ErrorKind::NotFound,
                     forFolder ? text("The migrations folder does not exist.")
                               : text("The migration file was not found."),
                     path,
                     {},
                     {} };
        }
        if (fx == LIBSSH2_FX_PERMISSION_DENIED) {
            return { ErrorKind::Permission,
                     forFolder ? text("Quarry cannot read the migrations folder.")
                               : text("The migration file cannot be read."),
                     path,
                     {},
                     {} };
        }
        return { ErrorKind::Connection,
                 forFolder ? text("Quarry cannot read the migrations folder on the SSH server.")
                           : text("The migration file cannot be read from the SSH server."),
                 QStringLiteral("SFTP status %1 (%2)").arg(fx).arg(path),
                 {},
                 {} };
    }
    return { ErrorKind::Connection,
             forFolder ? text("Quarry cannot read the migrations folder on the SSH server.")
                       : text("The migration file cannot be read from the SSH server."),
             client.lastErrorText(),
             {},
             {} };
}

template<typename F>
int driveSftpInt(Client &client, const Interrupt &interrupt, StopReason &stop, F &&call)
{
    stop = StopReason::None;
    for (;;) {
        const int rc = call();
        if (rc != LIBSSH2_ERROR_EAGAIN)
            return rc;
        stop = interrupt.check();
        if (stop != StopReason::None)
            return LIBSSH2_ERROR_EAGAIN;
        client.waitForSession(interrupt.remainingMs(100));
    }
}

template<typename T, typename F>
T *driveSftpPtr(Client &client, const Interrupt &interrupt, StopReason &stop, int &lastErr,
                F &&call)
{
    stop = StopReason::None;
    lastErr = 0;
    for (;;) {
        T *ptr = call();
        if (ptr != nullptr)
            return ptr;
        lastErr = libssh2_session_last_errno(client.session());
        if (lastErr != LIBSSH2_ERROR_EAGAIN)
            return nullptr;
        stop = interrupt.check();
        if (stop != StopReason::None)
            return nullptr;
        client.waitForSession(interrupt.remainingMs(100));
    }
}

void bestEffortCloseHandle(Client &client, LIBSSH2_SFTP_HANDLE *handle)
{
    if (handle == nullptr)
        return;
    const auto deadline
        = std::chrono::steady_clock::now() + std::chrono::milliseconds(kCloseTimeoutMs);
    while (libssh2_sftp_close_handle(handle) == LIBSSH2_ERROR_EAGAIN) {
        if (std::chrono::steady_clock::now() >= deadline)
            break;
        client.waitForSession(kCloseSliceMs);
    }
}

void bestEffortShutdownSftp(Client &client, LIBSSH2_SFTP *sftp)
{
    if (sftp == nullptr)
        return;
    const auto deadline
        = std::chrono::steady_clock::now() + std::chrono::milliseconds(kCloseTimeoutMs);
    while (libssh2_sftp_shutdown(sftp) == LIBSSH2_ERROR_EAGAIN) {
        if (std::chrono::steady_clock::now() >= deadline)
            break;
        client.waitForSession(kCloseSliceMs);
    }
}

struct SftpSession
{
    Client client;
    LIBSSH2_SFTP *sftp = nullptr;

    ~SftpSession()
    {
        bestEffortShutdownSftp(client, sftp);
        client.disconnect();
    }
};

struct HandleGuard
{
    Client &client;
    LIBSSH2_SFTP_HANDLE *handle = nullptr;

    ~HandleGuard() { bestEffortCloseHandle(client, handle); }
};

Result<std::unique_ptr<SftpSession>> openSftpSession(const SshTunnelConfig &config,
                                                     const Secrets &secrets,
                                                     const KnownHostsStore &knownHosts,
                                                     const CancellationToken &token)
{
    const QString problem = config.validationProblem();
    if (!problem.isEmpty())
        return fail(Error { ErrorKind::Connection, problem, {}, {}, {} });

    auto session = std::make_unique<SftpSession>();
    Interrupt interrupt;
    interrupt.cancelled = [token] { return token.isCancelled(); };

    interrupt.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kHandshakeSeconds);
    Result<void> connected = session->client.connectAndHandshake(
        config.host, config.port, interrupt, knownHosts.knownKeyTypes(config.host, config.port));
    if (!connected)
        return fail(std::move(connected).error());

    const HostKeyInfo seen = knownHosts.classify(session->client.hostKey(config.host, config.port));
    if (seen.status == HostKeyStatus::Unknown) {
        return fail(Error { ErrorKind::Connection,
                            text("The SSH server's host key is not trusted yet. Connect again and "
                                 "confirm its fingerprint."),
                            seen.fingerprint,
                            QString::fromLatin1(kSshUntrustedState),
                            {} });
    }
    if (seen.status == HostKeyStatus::Mismatch) {
        return fail(Error { ErrorKind::Connection,
                            text("The SSH server's host key has changed. Someone may be "
                                 "impersonating the server; the connection was blocked."),
                            seen.fingerprint,
                            QString::fromLatin1(kSshMismatchState),
                            {} });
    }

    interrupt.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kAuthSeconds);
    Result<void> authenticated = session->client.authenticate(config, secrets, interrupt);
    if (!authenticated)
        return fail(std::move(authenticated).error());

    interrupt.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kSftpOpSeconds);
    StopReason stop = StopReason::None;
    int lastErr = 0;
    session->sftp = driveSftpPtr<LIBSSH2_SFTP>(session->client, interrupt, stop, lastErr, [&] {
        return libssh2_sftp_init(session->client.session());
    });
    if (session->sftp == nullptr) {
        if (stop != StopReason::None)
            return fail(stoppedError(stop, QStringLiteral("sftp_init")));
        return fail(Error { ErrorKind::Connection,
                            text("Could not start SFTP on the SSH server."),
                            session->client.lastErrorText(),
                            {},
                            {} });
    }
    return session;
}

} // namespace

Result<QStringList> listRemoteDirectory(const SshTunnelConfig &config, const Secrets &secrets,
                                        const KnownHostsStore &knownHosts,
                                        const QString &remotePath, const CancellationToken &token)
{
    auto opened = openSftpSession(config, secrets, knownHosts, token);
    if (!opened)
        return fail(std::move(opened).error());
    SftpSession &s = *opened.value();

    Interrupt interrupt;
    interrupt.cancelled = [token] { return token.isCancelled(); };
    interrupt.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kSftpOpSeconds);

    const QByteArray pathBytes = remotePath.toUtf8();
    LIBSSH2_SFTP_ATTRIBUTES attrs {};
    StopReason stop = StopReason::None;
    const int statRc = driveSftpInt(s.client, interrupt, stop, [&] {
        return libssh2_sftp_stat_ex(s.sftp, pathBytes.constData(),
                                    static_cast<unsigned int>(pathBytes.size()), LIBSSH2_SFTP_STAT,
                                    &attrs);
    });
    if (stop != StopReason::None)
        return fail(stoppedError(stop, remotePath));
    if (statRc != 0)
        return fail(sftpError(s.client, s.sftp, statRc, remotePath, true));
    if ((attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) != 0
        && !LIBSSH2_SFTP_S_ISDIR(attrs.permissions)) {
        return fail(Error { ErrorKind::Constraint,
                            text("The migrations path is a file, not a folder."),
                            remotePath,
                            {},
                            {} });
    }

    int lastErr = 0;
    HandleGuard dir { s.client,
                      driveSftpPtr<LIBSSH2_SFTP_HANDLE>(s.client, interrupt, stop, lastErr, [&] {
                          return libssh2_sftp_open_ex(s.sftp, pathBytes.constData(),
                                                      static_cast<unsigned int>(pathBytes.size()),
                                                      0, 0, LIBSSH2_SFTP_OPENDIR);
                      }) };
    if (dir.handle == nullptr) {
        if (stop != StopReason::None)
            return fail(stoppedError(stop, remotePath));
        return fail(sftpError(s.client, s.sftp, lastErr, remotePath, true));
    }

    QStringList names;
    QByteArray nameBuf(kNameBufferBytes, '\0');
    for (;;) {
        LIBSSH2_SFTP_ATTRIBUTES entryAttrs {};
        const int rc = driveSftpInt(s.client, interrupt, stop, [&] {
            return libssh2_sftp_readdir_ex(dir.handle, nameBuf.data(),
                                           static_cast<size_t>(nameBuf.size()), nullptr, 0,
                                           &entryAttrs);
        });
        if (stop != StopReason::None)
            return fail(stoppedError(stop, remotePath));
        if (rc < 0)
            return fail(sftpError(s.client, s.sftp, rc, remotePath, true));
        if (rc == 0)
            break;
        const QString name = QString::fromUtf8(nameBuf.constData(), rc);
        if (name == QLatin1String(".") || name == QLatin1String(".."))
            continue;
        if ((entryAttrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) != 0
            && LIBSSH2_SFTP_S_ISDIR(entryAttrs.permissions))
            continue;
        names.append(name);
    }
    return names;
}

Result<QByteArray> readRemoteFile(const SshTunnelConfig &config, const Secrets &secrets,
                                  const KnownHostsStore &knownHosts, const QString &remotePath,
                                  qint64 maxBytes, const CancellationToken &token)
{
    auto opened = openSftpSession(config, secrets, knownHosts, token);
    if (!opened)
        return fail(std::move(opened).error());
    SftpSession &s = *opened.value();

    Interrupt interrupt;
    interrupt.cancelled = [token] { return token.isCancelled(); };
    interrupt.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kSftpOpSeconds);

    const QByteArray pathBytes = remotePath.toUtf8();
    LIBSSH2_SFTP_ATTRIBUTES attrs {};
    StopReason stop = StopReason::None;
    const int statRc = driveSftpInt(s.client, interrupt, stop, [&] {
        return libssh2_sftp_stat_ex(s.sftp, pathBytes.constData(),
                                    static_cast<unsigned int>(pathBytes.size()), LIBSSH2_SFTP_STAT,
                                    &attrs);
    });
    if (stop != StopReason::None)
        return fail(stoppedError(stop, remotePath));
    if (statRc != 0)
        return fail(sftpError(s.client, s.sftp, statRc, remotePath, false));
    if ((attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) != 0
        && !LIBSSH2_SFTP_S_ISREG(attrs.permissions)) {
        return fail(Error { ErrorKind::Constraint,
                            text("The migration path is not a regular file."),
                            remotePath,
                            {},
                            {} });
    }
    if ((attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) != 0
        && attrs.filesize > static_cast<libssh2_uint64_t>(maxBytes)) {
        return fail(Error { ErrorKind::Constraint,
                            text("The migration file is larger than 1 MB."),
                            text("%1 bytes").arg(static_cast<qint64>(attrs.filesize)),
                            {},
                            {} });
    }

    int lastErr = 0;
    HandleGuard file { s.client,
                       driveSftpPtr<LIBSSH2_SFTP_HANDLE>(s.client, interrupt, stop, lastErr, [&] {
                           return libssh2_sftp_open_ex(s.sftp, pathBytes.constData(),
                                                       static_cast<unsigned int>(pathBytes.size()),
                                                       LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
                       }) };
    if (file.handle == nullptr) {
        if (stop != StopReason::None)
            return fail(stoppedError(stop, remotePath));
        return fail(sftpError(s.client, s.sftp, lastErr, remotePath, false));
    }

    QByteArray out;
    if ((attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) != 0)
        out.reserve(static_cast<qsizetype>(std::min<libssh2_uint64_t>(attrs.filesize, maxBytes)));
    char chunk[kReadBufferBytes];
    for (;;) {
        const auto rc = driveSftpInt(s.client, interrupt, stop, [&] {
            return static_cast<int>(libssh2_sftp_read(file.handle, chunk, sizeof(chunk)));
        });
        if (stop != StopReason::None)
            return fail(stoppedError(stop, remotePath));
        if (rc < 0)
            return fail(sftpError(s.client, s.sftp, rc, remotePath, false));
        if (rc == 0)
            break;
        out.append(chunk, rc);
        if (out.size() > maxBytes) {
            return fail(Error { ErrorKind::Constraint,
                                text("The migration file is larger than 1 MB."),
                                text("%1 bytes").arg(out.size()),
                                {},
                                {} });
        }
    }
    return out;
}

} // namespace quarry::ssh
