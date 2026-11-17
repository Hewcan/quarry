#pragma once

#include <quarry/result.h>
// Declares the Q_DECLARE_METATYPE for quarry::Error used by failed().
#include <quarry/session.h>

#include <QHash>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QUuid>

#include <array>
#include <cstdint>
#include <optional>

namespace quarry {

// What is stored per connection. Keychain account: connection/<uuid>/password, .../ssh-password,
// .../ssh-passphrase.
enum class SecretKind : uint8_t { Password, SshPassword, SshPassphrase };

// Secrets in the OS keychain (QtKeychain: Credential Manager, Keychain, Secret Service),
// service "dev.quarry.Quarry", account "connection/<uuid>/<kind>". All keychain calls are
// asynchronous jobs; results arrive as signals on the UI thread. There is never a fallback to disk.
// The *Password members are the original API for the database password (SecretKind::Password)
// and keep their behaviour and signals; the secret members take any kind.
class CredentialStore : public QObject
{
    Q_OBJECT

public:
    explicit CredentialStore(QObject *parent = nullptr);
    // Overwrites the session passwords before releasing them (best effort).
    ~CredentialStore() override;

    // "Ask every time": secrets live only in this object's memory. Synchronous, no I/O.
    void rememberForSession(QUuid id, QString password);
    std::optional<QString> sessionPassword(QUuid id) const;
    // Forgets all three kinds of the connection.
    void forgetForSession(QUuid id);
    void rememberForSession(QUuid id, SecretKind kind, QString value);
    std::optional<QString> sessionSecret(QUuid id, SecretKind kind) const;
    void forgetForSession(QUuid id, SecretKind kind);

public slots:
    // -> secretRead | secretFailed (kind NotFound if absent); Password also emits passwordRead /
    // failed
    void readSecret(QUuid id, quarry::SecretKind kind);
    // -> secretWritten | secretFailed; Password also emits passwordWritten / failed
    void writeSecret(QUuid id, quarry::SecretKind kind, QString value);
    // -> secretDeleted (also if absent) | secretFailed; Password also emits passwordDeleted /
    // failed
    void deleteSecret(QUuid id, quarry::SecretKind kind);

    void readPassword(QUuid id); // -> passwordRead | failed (kind NotFound if absent)
    void writePassword(QUuid id, QString password); // -> passwordWritten | failed
    void deletePassword(QUuid id); // -> passwordDeleted (also if absent) | failed

signals:
    void passwordRead(QUuid id, QString password);
    void passwordWritten(QUuid id);
    void passwordDeleted(QUuid id);
    void failed(QUuid id, quarry::Error error); // SecretKind::Password only
    void secretRead(QUuid id, quarry::SecretKind kind, QString value);
    void secretWritten(QUuid id, quarry::SecretKind kind);
    void secretDeleted(QUuid id, quarry::SecretKind kind);
    void secretFailed(QUuid id, quarry::SecretKind kind, quarry::Error error);

private:
    using SessionSecrets = std::array<std::optional<QString>, 3>;
    QHash<QUuid, SessionSecrets> m_session;
};

} // namespace quarry

Q_DECLARE_METATYPE(quarry::SecretKind)
