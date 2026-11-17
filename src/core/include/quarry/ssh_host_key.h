#pragma once

#include <quarry/result.h>
#include <quarry/ssh_tunnel_config.h>
// Declares the Q_DECLARE_METATYPE for quarry::Error used by SshHostKeyChecker::failed().
#include <quarry/session.h>

#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <mutex>
#include <vector>

class QThread;

namespace quarry {

enum class HostKeyStatus : uint8_t {
    Match, // known and equal
    Unknown, // never seen: the user must confirm the fingerprint
    Mismatch // known with another key: blocked, never accepted from the connection dialog
};

struct HostKeyInfo
{
    HostKeyStatus status = HostKeyStatus::Unknown;
    QString host; // as typed in the profile
    int port = 22;
    QString keyType; // "ssh-ed25519", "ecdsa-sha2-nistp256", "ssh-rsa"...
    QByteArray key; // raw public key as sent by the server
    QString fingerprint; // "SHA256:<base64 without padding>", what OpenSSH prints
    QString knownFingerprint; // only for Mismatch: the fingerprint on file
};

// The sentinel that the tunnel's open() puts in Error::sqlState when the server key is not
// trusted yet (Unknown) or does not match (Mismatch). Error::detail holds the fingerprint.
inline constexpr const char *kSshUntrustedState = "quarry.ssh.untrusted";
inline constexpr const char *kSshMismatchState = "quarry.ssh.mismatch";
// Login to the SSH server failed (kind Authentication); tells it apart from the database login.
inline constexpr const char *kSshAuthState = "quarry.ssh.auth";

// Quarry's own known_hosts file (OpenSSH format, hashed host names off), never ~/.ssh.
// Thread-safe: shared by the checker (UI side) and by the tunnels (session threads).
class KnownHostsStore
{
public:
    // Default path: <AppDataLocation>/known_hosts. The file is created on the first remember().
    explicit KnownHostsStore(QString filePath);
    static QString defaultPath();

    // Compares a key seen in a handshake with the file and with the keys trusted for this run.
    // Fills `status` and, for Mismatch, `knownFingerprint`.
    HostKeyInfo classify(HostKeyInfo seen) const;
    // Key types on file for this host, in file order. The handshake asks for these first so the
    // server presents the key we know instead of one of another type (which would read as Unknown).
    QStringList knownKeyTypes(const QString &host, int port) const;
    // Persists the key (atomic write). Fails with a translated Error if the file cannot be written.
    Result<bool> remember(const HostKeyInfo &info);
    // Trusted until the store is destroyed; nothing is written ("accept once").
    void trustForSession(const HostKeyInfo &info);

private:
    QString m_path;
    mutable std::mutex m_mutex;
    std::vector<HostKeyInfo> m_session;
};

// UI-thread facade (own worker thread, like Session): connects to the SSH server, reads the host
// key during the handshake, disconnects before authenticating and reports it classified.
class SshHostKeyChecker : public QObject
{
    Q_OBJECT

public:
    explicit SshHostKeyChecker(std::shared_ptr<KnownHostsStore> store, QObject *parent = nullptr);
    ~SshHostKeyChecker() override;

public slots:
    // One check at a time; a running one is dropped silently. Result: checked | failed (kind
    // Connection: host unreachable or not an SSH server; kind Timeout after 15 s).
    void check(const quarry::SshTunnelConfig &config);
    void cancel(); // aborts the running check, no signal
    // Persists (`remember`) or trusts for this run only. Synchronous; result in `accepted` or
    // `failed`.
    void accept(const quarry::HostKeyInfo &info, bool remember);

signals:
    void checked(quarry::HostKeyInfo info);
    void accepted(quarry::HostKeyInfo info);
    void failed(quarry::Error error);

private:
    std::shared_ptr<KnownHostsStore> m_store;
    class Worker;
    Worker *m_worker = nullptr; // lives on m_thread
    QThread *m_thread = nullptr;
};

} // namespace quarry

Q_DECLARE_METATYPE(quarry::HostKeyInfo)
Q_DECLARE_METATYPE(quarry::SshTunnelConfig)
