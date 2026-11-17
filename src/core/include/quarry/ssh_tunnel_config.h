#pragma once

#include <QString>
#include <QVariantMap>

#include <cstdint>

namespace quarry {

enum class SshAuth : uint8_t { Password, Key, Agent };

// SSH tunnel settings of a connection profile. No secrets: the password or key passphrase travel
// in Secrets (sshPassword / sshPassphrase) and live in the keychain. Stored in
// ConnectionProfile::values under the "ssh." keys below.
struct SshTunnelConfig
{
    bool enabled = false;
    QString host;
    int port = 22;
    QString user;
    SshAuth auth = SshAuth::Password;
    QString keyPath; // private key file, only for SshAuth::Key

    // Keys in ConnectionProfile::values.
    static constexpr const char *kEnabledKey = "ssh.enabled";
    static constexpr const char *kHostKey = "ssh.host";
    static constexpr const char *kPortKey = "ssh.port";
    static constexpr const char *kUserKey = "ssh.user";
    static constexpr const char *kAuthKey = "ssh.auth"; // "password" | "key" | "agent"
    static constexpr const char *kKeyPathKey = "ssh.keyPath";

    // Missing or invalid entries fall back to the defaults above (port outside 1..65535 -> 22).
    static SshTunnelConfig fromValues(const QVariantMap &values);
    // Writes the six keys (only `enabled` when disabled); leaves the other entries alone.
    void toValues(QVariantMap &values) const;
    // Required fields for the chosen method; empty string when valid, otherwise a translated
    // message that names the missing field.
    QString validationProblem() const;
};

} // namespace quarry
