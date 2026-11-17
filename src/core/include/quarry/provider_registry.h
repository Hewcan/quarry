#pragma once

#include <quarry/connection_profile.h>
#include <quarry/provider_api.h>
#include <quarry/result.h>

#include <QPluginLoader>
#include <QString>
#include <QVersionNumber>

#include <map>
#include <memory>
#include <vector>

namespace quarry {

class KnownHostsStore;

struct ProviderEntry
{
    QString id;
    QString displayName;
    QVersionNumber version;
    int apiVersion = 0;
    QString filePath;
};

struct RejectedPlugin
{
    QString filePath;
    QString reason;
};

class ProviderRegistry
{
public:
    ProviderRegistry();
    ~ProviderRegistry();
    ProviderRegistry(const ProviderRegistry &) = delete;
    ProviderRegistry &operator=(const ProviderRegistry &) = delete;

    // Reads embedded metadata only; never loads a library. Replaces previous results.
    void scan(const QString &directory);

    const std::vector<ProviderEntry> &available() const { return m_available; }
    const std::vector<RejectedPlugin> &rejected() const { return m_rejected; }

    // Loads the plugin on first use and caches the provider. Returns nullptr on failure.
    IDatabaseProvider *provider(const QString &id, QString *errorMessage = nullptr);
    bool isLoaded(const QString &id) const;

    // Loads the plugin if needed and creates an unopened connection for the profile. With
    // ssh.enabled in the profile the connection is wrapped so it opens an SSH tunnel first.
    // Errors: NotFound (engine unavailable, detail = loader message), Internal (provider
    // returned no connection), Unsupported (SSH requested but the engine has no Capability::Ssh),
    // Connection (the SSH settings are incomplete).
    Result<std::unique_ptr<IConnection>> createConnection(const ConnectionProfile &profile,
                                                          const Secrets &secrets);

    // Trusted SSH host keys, shared by the tunnels this registry creates and by the app's
    // SshHostKeyChecker. Without setKnownHosts() a store on KnownHostsStore::defaultPath() is
    // created on first use. Not thread-safe: call from the thread that owns the registry.
    void setKnownHosts(std::shared_ptr<KnownHostsStore> store);
    std::shared_ptr<KnownHostsStore> knownHosts();

    static QString defaultPluginDirectory();

private:
    struct Slot
    {
        std::unique_ptr<QPluginLoader> loader;
        // Declared after the loader so it is destroyed first.
        std::unique_ptr<IDatabaseProvider> provider;
    };

    void clear();

    std::vector<ProviderEntry> m_available;
    std::vector<RejectedPlugin> m_rejected;
    std::map<QString, Slot> m_slots;
    std::shared_ptr<KnownHostsStore> m_knownHosts;
};

} // namespace quarry
