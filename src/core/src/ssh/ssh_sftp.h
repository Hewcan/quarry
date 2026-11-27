#pragma once

// Read-only SFTP operations over one quarry::ssh::Client session (H12 remote migrations).
// Blocking; always call on a worker thread, never on the UI thread. Private to core.

#include <quarry/cancellation.h>
#include <quarry/provider_api.h>
#include <quarry/result.h>
#include <quarry/ssh_host_key.h>
#include <quarry/ssh_tunnel_config.h>

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace quarry::ssh {

// Lists regular (and non-directory) file names in `remotePath` on the SSH server.
// Fails with NotFound, Permission, Constraint (not a directory), Connection, Authentication,
// Timeout or Cancelled.
Result<QStringList> listRemoteDirectory(const SshTunnelConfig &config, const Secrets &secrets,
                                        const KnownHostsStore &knownHosts,
                                        const QString &remotePath, const CancellationToken &token);

// Reads a remote regular file up to `maxBytes` bytes. Fails if missing, not a regular file,
// larger than `maxBytes`, or on SSH/SFTP error.
Result<QByteArray> readRemoteFile(const SshTunnelConfig &config, const Secrets &secrets,
                                  const KnownHostsStore &knownHosts, const QString &remotePath,
                                  qint64 maxBytes, const CancellationToken &token);

} // namespace quarry::ssh
