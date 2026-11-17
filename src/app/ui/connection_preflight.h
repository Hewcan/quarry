#pragma once

#include <quarry/connection_profile.h>
#include <quarry/credential_store.h>
#include <quarry/provider_api.h>
#include <quarry/result.h>
// Declares the Q_DECLARE_METATYPE for quarry::Error used by the signals.
#include <quarry/session.h>
#include <quarry/ssh_host_key.h>
#include <quarry/ssh_tunnel_config.h>

#include <QObject>
#include <QSet>
#include <QString>
#include <QUuid>
#include <QtQml/qqmlregistration.h>

#include <deque>
#include <functional>
#include <memory>
#include <optional>

namespace quarry::ui {

// What the preflight needs from the keychain and the per-run secret memory. The controller wires
// it to CredentialStore; tests replace it. Results come back through onSecretRead() and
// onSecretFailed().
struct SshSecretBackend
{
    std::function<void(QUuid, quarry::SecretKind)> read;
    std::function<std::optional<QString>(QUuid, quarry::SecretKind)> session;
    std::function<void(QUuid, quarry::SecretKind, QString)> remember;
    std::function<void(QUuid, quarry::SecretKind)> forget;
};

// What the preflight needs from SshHostKeyChecker. Results come back through onHostKeyChecked(),
// onHostKeyAccepted() and onHostKeyFailed().
struct HostKeyBackend
{
    std::function<void(const quarry::SshTunnelConfig &)> check;
    std::function<void()> cancel;
    std::function<void(const quarry::HostKeyInfo &, bool)> accept;
};

// Everything that has to happen before a connection with an SSH tunnel is created, shared by the
// four places that create one (schema tree, query tabs, export, "Test connection"): resolve the
// SSH secret (keychain, per-run memory or a prompt) and check the server's host key (confirm an
// unknown one, block a changed one). Callers hand over the database password they already have and
// get Secrets{password, sshPassword, sshPassphrase} back in ready().
//
// Requests run one at a time, in order: the checker handles one server at a time and the user can
// answer one prompt at a time. The answers of the prompts belong to the request that is running.
// Every signal is delivered from the event loop, never from inside begin().
class ConnectionPreflight : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by ConnectionController")

public:
    explicit ConnectionPreflight(QObject *parent = nullptr);

    // Wires the default backends: the keychain of `credentials` and a SshHostKeyChecker on
    // `knownHosts`, created on the first check so it costs nothing at startup.
    void useDefaults(quarry::CredentialStore &credentials,
                     std::shared_ptr<quarry::KnownHostsStore> knownHosts);
    void setSecretBackend(SshSecretBackend backend) { m_secrets = std::move(backend); }
    void setHostKeyBackend(HostKeyBackend backend) { m_hostKeys = std::move(backend); }

    static bool usesSsh(const quarry::ConnectionProfile &profile);
    // True for "Ask every time" tunnel secrets (the profile's ssh.secretMode).
    static bool asksSecret(const quarry::ConnectionProfile &profile);
    // The secret the tunnel's method needs: nullopt for the agent or when the tunnel is off.
    static std::optional<quarry::SecretKind> secretKindOf(const quarry::ConnectionProfile &profile);
    // The error of a connection attempt that went wrong in the tunnel's authentication (the SSH
    // server refused the login) and not in the database's.
    static bool isSshAuthError(const quarry::ConnectionProfile &profile,
                               const quarry::Error &error);
    static bool isHostKeyError(const quarry::Error &error);

    // Starts a request; the result is one of the signals below, with the returned id. With
    // `interactive` false nothing is ever asked of the user: a missing "ask every time" secret or
    // an unconfirmed host key ends in failed().
    // `sshSecret` is used as it is (no keychain, no prompt) when the caller already has it, like
    // the one typed in the connection dialog for "Test connection".
    int begin(const quarry::ConnectionProfile &profile, QString password, bool interactive = true,
              std::optional<QString> sshSecret = std::nullopt);
    // Drops a request, whatever its stage, without a signal.
    void cancel(int request);

    // Prompt answers (for the request that is running). An empty secret is allowed.
    Q_INVOKABLE void provideSecret(const QString &connectionId, const QString &secret);
    Q_INVOKABLE void cancelSecret(const QString &connectionId);
    Q_INVOKABLE void trustHostKey(bool remember);
    Q_INVOKABLE void cancelHostKey();

    // To call with the outcome of every open() of a profile that uses the tunnel. openFailed()
    // returns the error to show: a changed host key gets the full explanation, and a refused SSH
    // login forgets the secret remembered for this run and makes the next prompt say so.
    void openSucceeded(const QUuid &connectionId);
    quarry::Error openFailed(const quarry::ConnectionProfile &profile, const quarry::Error &error);

public slots:
    void onSecretRead(QUuid id, quarry::SecretKind kind, QString value);
    void onSecretFailed(QUuid id, quarry::SecretKind kind, const quarry::Error &error);
    void onHostKeyChecked(const quarry::HostKeyInfo &info);
    void onHostKeyAccepted(const quarry::HostKeyInfo &info);
    void onHostKeyFailed(const quarry::Error &error);

signals:
    void ready(int request, const quarry::Secrets &secrets);
    void failed(int request, const quarry::Error &error);
    // The user dismissed a prompt.
    void cancelled(int request);
    // "Ask every time" and nothing remembered for this run. `passphrase`: the key's passphrase,
    // otherwise the SSH password. `retry`: the last one was refused.
    void secretRequested(const QString &connectionId, const QString &name, bool passphrase,
                         bool retry);
    // The server is not known yet: show its fingerprint and ask.
    void hostKeyRequested(const QString &host, int port, const QString &keyType,
                          const QString &fingerprint);
    // The server's key is not the one on file. The request (or the open) fails as well; this is
    // for the prominent notice. `known` is empty when only the received fingerprint is at hand.
    void hostKeyMismatch(const QString &host, int port, const QString &keyType,
                         const QString &known, const QString &received);

private:
    enum class Stage {
        Queued,
        ReadingSecret,
        AwaitingSecret,
        Checking,
        AwaitingHostKey,
        Accepting
    };
    struct Request
    {
        int id = 0;
        quarry::ConnectionProfile profile;
        quarry::SshTunnelConfig config;
        quarry::Secrets secrets;
        bool interactive = true;
        std::optional<QString> given;
        Stage stage = Stage::Queued;
        quarry::HostKeyInfo info;
    };

    Request *front(Stage stage);
    void schedule();
    void start(int request);
    void resolveSecret();
    void checkHostKey();
    void finishReady();
    void finishFailed(const quarry::Error &error);
    void finishCancelled();
    static quarry::Error mismatchError(const QString &host, const QString &known,
                                       const QString &received);

    SshSecretBackend m_secrets;
    HostKeyBackend m_hostKeys;
    std::shared_ptr<quarry::KnownHostsStore> m_knownHosts;
    // A child, created on the first check.
    quarry::SshHostKeyChecker *m_checker = nullptr;
    std::deque<Request> m_queue; // the first one is the one running (or about to)
    bool m_active = false; // the first request has been started or its start is scheduled
    int m_nextId = 0;
    QSet<QUuid> m_authFailed; // connections whose last SSH secret was refused
};

} // namespace quarry::ui
