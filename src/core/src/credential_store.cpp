#include <quarry/credential_store.h>

#include <qt6keychain/keychain.h>

#include <algorithm>

namespace quarry {

namespace {

const QString kService = QStringLiteral("dev.quarry.Quarry");

QString accountFor(const QUuid &id, SecretKind kind)
{
    const char *suffix = "password";
    if (kind == SecretKind::SshPassword)
        suffix = "ssh-password";
    else if (kind == SecretKind::SshPassphrase)
        suffix = "ssh-passphrase";
    return QStringLiteral("connection/%1/%2")
        .arg(id.toString(QUuid::WithoutBraces), QLatin1String(suffix));
}

size_t slotOf(SecretKind kind)
{
    return static_cast<size_t>(kind);
}

QString notFoundMessage(SecretKind kind)
{
    switch (kind) {
    case SecretKind::SshPassword:
        return CredentialStore::tr("No saved SSH password was found for this connection.");
    case SecretKind::SshPassphrase:
        return CredentialStore::tr("No saved SSH key passphrase was found for this connection.");
    case SecretKind::Password:
        break;
    }
    return CredentialStore::tr("No saved password was found for this connection.");
}

Error toError(const QKeychain::Job &job, SecretKind kind)
{
    const QString detail = job.errorString();
    switch (job.error()) {
    case QKeychain::EntryNotFound:
        return { ErrorKind::NotFound, notFoundMessage(kind), detail, {}, {} };
    case QKeychain::AccessDenied:
    case QKeychain::AccessDeniedByUser:
        return { ErrorKind::Permission,
                 CredentialStore::tr("Access to the system keychain was denied."),
                 detail,
                 {},
                 {} };
    default: // NoBackendAvailable, NotImplemented, OtherError...
        return { ErrorKind::Unsupported,
                 CredentialStore::tr("The system keychain is not available. Choose \xE2\x80\x9C"
                                     "Ask every time\xE2\x80\x9D to enter the password when "
                                     "connecting."),
                 detail,
                 {},
                 {} };
    }
}

void wipe(std::optional<QString> &secret)
{
    if (secret)
        secret->fill(u'\0'); // see the shortcut note in ~CredentialStore
    secret.reset();
}

} // namespace

CredentialStore::CredentialStore(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<quarry::SecretKind>();
}

CredentialStore::~CredentialStore()
{
    // shortcut: QString::fill detaches a shared buffer, so copies held elsewhere (the caller's
    // argument, an old Secrets) are not wiped; the allocator may also have moved the data. Real
    // memory hygiene needs a locked, non-shared buffer; upgrade if the threat model asks for it.
    for (SessionSecrets &secrets : m_session) {
        for (std::optional<QString> &secret : secrets)
            wipe(secret);
    }
}

void CredentialStore::rememberForSession(QUuid id, QString password)
{
    rememberForSession(id, SecretKind::Password, std::move(password));
}

std::optional<QString> CredentialStore::sessionPassword(QUuid id) const
{
    return sessionSecret(id, SecretKind::Password);
}

void CredentialStore::forgetForSession(QUuid id)
{
    const auto it = m_session.find(id);
    if (it == m_session.end())
        return;
    for (std::optional<QString> &secret : *it)
        wipe(secret);
    m_session.erase(it);
}

void CredentialStore::rememberForSession(QUuid id, SecretKind kind, QString value)
{
    forgetForSession(id, kind);
    m_session[id][slotOf(kind)] = std::move(value);
}

std::optional<QString> CredentialStore::sessionSecret(QUuid id, SecretKind kind) const
{
    const auto it = m_session.constFind(id);
    if (it == m_session.constEnd())
        return std::nullopt;
    return (*it)[slotOf(kind)];
}

void CredentialStore::forgetForSession(QUuid id, SecretKind kind)
{
    const auto it = m_session.find(id);
    if (it == m_session.end())
        return;
    wipe((*it)[slotOf(kind)]);
    const bool empty
        = std::none_of(it->begin(), it->end(),
                       [](const std::optional<QString> &secret) { return secret.has_value(); });
    if (empty)
        m_session.erase(it);
}

void CredentialStore::readSecret(QUuid id, SecretKind kind)
{
    auto *job = new QKeychain::ReadPasswordJob(kService, this);
    job->setAutoDelete(true);
    job->setKey(accountFor(id, kind));
    connect(job, &QKeychain::Job::finished, this, [this, id, kind](QKeychain::Job *j) {
        if (j->error() == QKeychain::NoError) {
            const QString value = static_cast<QKeychain::ReadPasswordJob *>(j)->textData();
            emit secretRead(id, kind, value);
            if (kind == SecretKind::Password)
                emit passwordRead(id, value);
        } else {
            const Error error = toError(*j, kind);
            emit secretFailed(id, kind, error);
            if (kind == SecretKind::Password)
                emit failed(id, error);
        }
    });
    job->start();
}

void CredentialStore::writeSecret(QUuid id, SecretKind kind, QString value)
{
    auto *job = new QKeychain::WritePasswordJob(kService, this);
    job->setAutoDelete(true);
    job->setKey(accountFor(id, kind));
    job->setTextData(value);
    value.fill(u'\0'); // see the shortcut note in the destructor
    connect(job, &QKeychain::Job::finished, this, [this, id, kind](QKeychain::Job *j) {
        if (j->error() == QKeychain::NoError) {
            emit secretWritten(id, kind);
            if (kind == SecretKind::Password)
                emit passwordWritten(id);
        } else {
            const Error error = toError(*j, kind);
            emit secretFailed(id, kind, error);
            if (kind == SecretKind::Password)
                emit failed(id, error);
        }
    });
    job->start();
}

void CredentialStore::deleteSecret(QUuid id, SecretKind kind)
{
    auto *job = new QKeychain::DeletePasswordJob(kService, this);
    job->setAutoDelete(true);
    job->setKey(accountFor(id, kind));
    connect(job, &QKeychain::Job::finished, this, [this, id, kind](QKeychain::Job *j) {
        if (j->error() == QKeychain::NoError || j->error() == QKeychain::EntryNotFound) {
            emit secretDeleted(id, kind);
            if (kind == SecretKind::Password)
                emit passwordDeleted(id);
        } else {
            const Error error = toError(*j, kind);
            emit secretFailed(id, kind, error);
            if (kind == SecretKind::Password)
                emit failed(id, error);
        }
    });
    job->start();
}

void CredentialStore::readPassword(QUuid id)
{
    readSecret(id, SecretKind::Password);
}

void CredentialStore::writePassword(QUuid id, QString password)
{
    writeSecret(id, SecretKind::Password, std::move(password));
}

void CredentialStore::deletePassword(QUuid id)
{
    deleteSecret(id, SecretKind::Password);
}

} // namespace quarry
