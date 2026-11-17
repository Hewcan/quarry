#include "connection_preflight.h"

#include "connection_form_model.h"

#include <QTimer>

#include <algorithm>
#include <utility>

namespace quarry::ui {

namespace {

QString toId(const QUuid &id)
{
    return id.toString(QUuid::WithoutBraces);
}

} // namespace

ConnectionPreflight::ConnectionPreflight(QObject *parent)
    : QObject(parent)
{ }

void ConnectionPreflight::useDefaults(CredentialStore &credentials,
                                      std::shared_ptr<KnownHostsStore> knownHosts)
{
    m_knownHosts = std::move(knownHosts);
    m_secrets.read
        = [&credentials](QUuid id, SecretKind kind) { credentials.readSecret(id, kind); };
    m_secrets.session
        = [&credentials](QUuid id, SecretKind kind) { return credentials.sessionSecret(id, kind); };
    m_secrets.remember = [&credentials](QUuid id, SecretKind kind, QString value) {
        credentials.rememberForSession(id, kind, std::move(value));
    };
    m_secrets.forget
        = [&credentials](QUuid id, SecretKind kind) { credentials.forgetForSession(id, kind); };
    // The new family only: for the SSH kinds the old password signals are never emitted.
    connect(&credentials, &CredentialStore::secretRead, this, &ConnectionPreflight::onSecretRead);
    connect(&credentials, &CredentialStore::secretFailed, this,
            &ConnectionPreflight::onSecretFailed);

    const auto ensureChecker = [this] {
        if (m_checker != nullptr)
            return;
        m_checker = new SshHostKeyChecker(m_knownHosts, this);
        connect(m_checker, &SshHostKeyChecker::checked, this,
                &ConnectionPreflight::onHostKeyChecked);
        connect(m_checker, &SshHostKeyChecker::accepted, this,
                &ConnectionPreflight::onHostKeyAccepted);
        connect(m_checker, &SshHostKeyChecker::failed, this, &ConnectionPreflight::onHostKeyFailed);
    };
    m_hostKeys.check = [this, ensureChecker](const SshTunnelConfig &config) {
        ensureChecker();
        m_checker->check(config);
    };
    m_hostKeys.cancel = [this] {
        if (m_checker != nullptr)
            m_checker->cancel();
    };
    m_hostKeys.accept = [this, ensureChecker](const HostKeyInfo &info, bool remember) {
        ensureChecker();
        m_checker->accept(info, remember);
    };
}

// ---- what a profile says -----------------------------------------------------------------------

bool ConnectionPreflight::usesSsh(const ConnectionProfile &profile)
{
    return profile.values.value(QLatin1String(SshTunnelConfig::kEnabledKey)).toBool();
}

bool ConnectionPreflight::asksSecret(const ConnectionProfile &profile)
{
    return profile.values.value(QLatin1String(kSshSecretModeKey)).toString()
        == QLatin1String("ask");
}

std::optional<SecretKind> ConnectionPreflight::secretKindOf(const ConnectionProfile &profile)
{
    if (!usesSsh(profile))
        return std::nullopt;
    switch (SshTunnelConfig::fromValues(profile.values).auth) {
    case SshAuth::Password:
        return SecretKind::SshPassword;
    case SshAuth::Key:
        return SecretKind::SshPassphrase;
    case SshAuth::Agent:
        break;
    }
    return std::nullopt;
}

bool ConnectionPreflight::isHostKeyError(const Error &error)
{
    return error.sqlState == QLatin1String(kSshUntrustedState)
        || error.sqlState == QLatin1String(kSshMismatchState);
}

bool ConnectionPreflight::isSshAuthError(const ConnectionProfile &profile, const Error &error)
{
    return usesSsh(profile) && error.kind == ErrorKind::Authentication
        && error.sqlState == QLatin1String(kSshAuthState);
}

Error ConnectionPreflight::mismatchError(const QString &host, const QString &known,
                                         const QString &received)
{
    QString message = tr("The SSH server's host key has changed, so Quarry did not connect. "
                         "Someone may be impersonating the server, or it was reinstalled. Ask its "
                         "administrator before you trust it again.");
    if (!known.isEmpty())
        message += QLatin1Char('\n') + tr("Known fingerprint: %1").arg(known);
    if (!received.isEmpty())
        message += QLatin1Char('\n') + tr("Received fingerprint: %1").arg(received);
    return { ErrorKind::Connection,
             message,
             host.isEmpty() ? QString() : tr("SSH server: %1").arg(host),
             QString::fromLatin1(kSshMismatchState),
             {} };
}

// ---- requests ----------------------------------------------------------------------------------

int ConnectionPreflight::begin(const ConnectionProfile &profile, QString password, bool interactive,
                               std::optional<QString> sshSecret)
{
    Request r;
    r.id = ++m_nextId;
    r.profile = profile;
    r.config = SshTunnelConfig::fromValues(profile.values);
    r.secrets.password = std::move(password);
    r.interactive = interactive;
    r.given = std::move(sshSecret);
    const int id = r.id;
    m_queue.push_back(std::move(r));
    schedule();
    return id;
}

ConnectionPreflight::Request *ConnectionPreflight::front(Stage stage)
{
    return !m_queue.empty() && m_queue.front().stage == stage ? &m_queue.front() : nullptr;
}

void ConnectionPreflight::schedule()
{
    if (m_active || m_queue.empty())
        return;
    m_active = true;
    const int id = m_queue.front().id;
    QTimer::singleShot(0, this, [this, id] { start(id); });
}

void ConnectionPreflight::start(int request)
{
    // The request may have been cancelled while the start was waiting for the event loop.
    if (front(Stage::Queued) == nullptr || m_queue.front().id != request)
        return;
    resolveSecret();
}

void ConnectionPreflight::cancel(int request)
{
    const auto it = std::ranges::find(m_queue, request, &Request::id);
    if (it == m_queue.end())
        return;
    if (it != m_queue.begin()) {
        m_queue.erase(it);
        return;
    }
    if (it->stage == Stage::Checking && m_hostKeys.cancel)
        m_hostKeys.cancel();
    m_queue.pop_front();
    m_active = false;
    schedule();
}

void ConnectionPreflight::finishReady()
{
    Request r = std::move(m_queue.front());
    m_queue.pop_front();
    m_active = false;
    emit ready(r.id, r.secrets);
    schedule();
}

void ConnectionPreflight::finishFailed(const Error &error)
{
    const int id = m_queue.front().id;
    m_queue.pop_front();
    m_active = false;
    emit failed(id, error);
    schedule();
}

void ConnectionPreflight::finishCancelled()
{
    const int id = m_queue.front().id;
    m_queue.pop_front();
    m_active = false;
    emit cancelled(id);
    schedule();
}

// ---- secret ------------------------------------------------------------------------------------

void ConnectionPreflight::resolveSecret()
{
    Request &r = m_queue.front();
    const auto kind = secretKindOf(r.profile);
    if (!kind) {
        checkHostKey();
        return;
    }
    const bool passphrase = *kind == SecretKind::SshPassphrase;
    if (r.given) {
        (passphrase ? r.secrets.sshPassphrase : r.secrets.sshPassword) = *r.given;
        checkHostKey();
        return;
    }
    if (asksSecret(r.profile)) {
        const auto remembered
            = m_secrets.session ? m_secrets.session(r.profile.id, *kind) : std::optional<QString>();
        if (remembered) {
            (passphrase ? r.secrets.sshPassphrase : r.secrets.sshPassword) = *remembered;
            checkHostKey();
            return;
        }
        if (!r.interactive) {
            finishFailed({ ErrorKind::Authentication,
                           passphrase
                               ? tr("This connection asks for the key passphrase of its SSH "
                                    "tunnel every time and it is not known yet. Connect to it "
                                    "from the connection list first.")
                               : tr("This connection asks for the SSH password of its tunnel "
                                    "every time and it is not known yet. Connect to it from the "
                                    "connection list first."),
                           {},
                           {},
                           {} });
            return;
        }
        r.stage = Stage::AwaitingSecret;
        emit secretRequested(toId(r.profile.id), r.profile.name, passphrase,
                             m_authFailed.contains(r.profile.id));
        return;
    }
    if (!m_secrets.read) {
        finishFailed(
            { ErrorKind::Unsupported, tr("The system keychain is not available."), {}, {}, {} });
        return;
    }
    r.stage = Stage::ReadingSecret;
    m_secrets.read(r.profile.id, *kind);
}

void ConnectionPreflight::onSecretRead(QUuid id, SecretKind kind, QString value)
{
    Request *r = front(Stage::ReadingSecret);
    if (r == nullptr || r->profile.id != id || kind != secretKindOf(r->profile))
        return;
    (kind == SecretKind::SshPassphrase ? r->secrets.sshPassphrase : r->secrets.sshPassword)
        = std::move(value);
    checkHostKey();
}

void ConnectionPreflight::onSecretFailed(QUuid id, SecretKind kind, const Error &error)
{
    Request *r = front(Stage::ReadingSecret);
    if (r == nullptr || r->profile.id != id || kind != secretKindOf(r->profile))
        return;
    // Nothing saved is not a failure: a key without passphrase, or a password left empty. The
    // server answers for itself.
    if (error.kind == ErrorKind::NotFound) {
        checkHostKey();
        return;
    }
    finishFailed(error);
}

void ConnectionPreflight::provideSecret(const QString &connectionId, const QString &secret)
{
    Request *r = front(Stage::AwaitingSecret);
    const auto kind = r != nullptr ? secretKindOf(r->profile) : std::nullopt;
    if (r == nullptr || !kind || toId(r->profile.id) != connectionId)
        return;
    if (m_secrets.remember)
        m_secrets.remember(r->profile.id, *kind, secret);
    (*kind == SecretKind::SshPassphrase ? r->secrets.sshPassphrase : r->secrets.sshPassword)
        = secret;
    checkHostKey();
}

void ConnectionPreflight::cancelSecret(const QString &connectionId)
{
    const Request *r = front(Stage::AwaitingSecret);
    if (r == nullptr || toId(r->profile.id) != connectionId)
        return;
    finishCancelled();
}

// ---- host key ----------------------------------------------------------------------------------

void ConnectionPreflight::checkHostKey()
{
    Request &r = m_queue.front();
    if (!m_hostKeys.check) {
        finishFailed({ ErrorKind::Unsupported,
                       tr("The SSH server's host key cannot be checked."),
                       {},
                       {},
                       {} });
        return;
    }
    r.stage = Stage::Checking;
    const SshTunnelConfig config = r.config; // check() may answer at once and drop the request
    m_hostKeys.check(config);
}

void ConnectionPreflight::onHostKeyChecked(const HostKeyInfo &info)
{
    Request *r = front(Stage::Checking);
    if (r == nullptr)
        return;
    switch (info.status) {
    case HostKeyStatus::Match:
        finishReady();
        return;
    case HostKeyStatus::Mismatch:
        emit hostKeyMismatch(r->config.host, r->config.port, info.keyType, info.knownFingerprint,
                             info.fingerprint);
        finishFailed(mismatchError(r->config.host, info.knownFingerprint, info.fingerprint));
        return;
    case HostKeyStatus::Unknown:
        break;
    }
    if (!r->interactive) {
        finishFailed({ ErrorKind::Connection,
                       tr("The SSH server %1 has not been trusted yet. Connect to this connection "
                          "from the connection list first and confirm its fingerprint.")
                           .arg(r->config.host),
                       info.fingerprint,
                       QString::fromLatin1(kSshUntrustedState),
                       {} });
        return;
    }
    r->info = info;
    r->stage = Stage::AwaitingHostKey;
    emit hostKeyRequested(r->config.host, r->config.port, info.keyType, info.fingerprint);
}

void ConnectionPreflight::trustHostKey(bool remember)
{
    Request *r = front(Stage::AwaitingHostKey);
    if (r == nullptr || !m_hostKeys.accept)
        return;
    r->stage = Stage::Accepting; // accept() answers at once, before it returns
    const HostKeyInfo info = r->info;
    m_hostKeys.accept(info, remember);
}

void ConnectionPreflight::cancelHostKey()
{
    if (front(Stage::AwaitingHostKey) != nullptr)
        finishCancelled();
}

void ConnectionPreflight::onHostKeyAccepted(const HostKeyInfo &)
{
    if (front(Stage::Accepting) != nullptr)
        finishReady();
}

void ConnectionPreflight::onHostKeyFailed(const Error &error)
{
    if (front(Stage::Checking) != nullptr || front(Stage::Accepting) != nullptr)
        finishFailed(error);
}

// ---- outcome of open() -------------------------------------------------------------------------

void ConnectionPreflight::openSucceeded(const QUuid &connectionId)
{
    m_authFailed.remove(connectionId);
}

Error ConnectionPreflight::openFailed(const ConnectionProfile &profile, const Error &error)
{
    if (!usesSsh(profile))
        return error;
    if (error.sqlState == QLatin1String(kSshMismatchState)) {
        const SshTunnelConfig config = SshTunnelConfig::fromValues(profile.values);
        emit hostKeyMismatch(config.host, config.port, {}, {}, error.detail);
        return mismatchError(config.host, {}, error.detail);
    }
    if (isSshAuthError(profile, error)) {
        m_authFailed.insert(profile.id);
        if (m_secrets.forget)
            if (const auto kind = secretKindOf(profile))
                m_secrets.forget(profile.id, *kind); // a wrong secret must not be tried again
    }
    return error;
}

} // namespace quarry::ui
