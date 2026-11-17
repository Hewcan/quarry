#include <quarry/ssh_host_key.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>

#include <optional>
#include <utility>

// shortcut: entries are matched by exact host name only (no wildcards, no hashed names, no
// @cert-authority); the file is Quarry's own and is written by remember(), so nothing else is
// needed. Revisit if users start sharing ~/.ssh/known_hosts with it.
//
// The file is parsed here instead of with libssh2_knownhost_*: libssh2 reports a different key
// type for a known host as "not found", while the contract needs "known with another key".

namespace quarry {

namespace {

QString hostToken(const QString &host, int port)
{
    QString lower = host.trimmed().toLower();
    if (port == 22)
        return lower;
    return QStringLiteral("[%1]:%2").arg(lower).arg(port);
}

QString fingerprintOf(const QByteArray &key)
{
    const QByteArray digest = QCryptographicHash::hash(key, QCryptographicHash::Sha256);
    return QStringLiteral("SHA256:")
        + QString::fromLatin1(
               digest.toBase64(QByteArray::Base64Encoding | QByteArray::OmitTrailingEquals));
}

// The wire format of a public key starts with a length-prefixed algorithm name.
QString keyTypeOf(const QByteArray &key)
{
    if (key.size() < 4)
        return {};
    const auto *bytes = reinterpret_cast<const uchar *>(key.constData());
    const qsizetype length = (qsizetype(bytes[0]) << 24) | (qsizetype(bytes[1]) << 16)
        | (qsizetype(bytes[2]) << 8) | qsizetype(bytes[3]);
    if (length <= 0 || length > key.size() - 4 || length > 64)
        return {};
    return QString::fromLatin1(key.constData() + 4, length);
}

HostKeyInfo normalized(HostKeyInfo info)
{
    if (info.keyType.isEmpty())
        info.keyType = keyTypeOf(info.key);
    if (info.fingerprint.isEmpty())
        info.fingerprint = fingerprintOf(info.key);
    return info;
}

struct Entry
{
    QString hosts; // the first field, as written
    QString keyType;
    QByteArray key;
};

// Lines that do not parse are ignored (and preserved by remember()).
std::optional<Entry> parseLine(const QString &line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith(u'#') || trimmed.startsWith(u'@'))
        return std::nullopt;
    const QStringList fields = trimmed.split(QRegularExpression(QStringLiteral("\\s+")));
    if (fields.size() < 3)
        return std::nullopt;
    // Strict: the lenient decoder would turn garbage into some other key and report a Mismatch.
    auto decoded = QByteArray::fromBase64Encoding(fields.at(2).toLatin1(),
                                                  QByteArray::Base64Encoding
                                                      | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.isEmpty())
        return std::nullopt;
    return Entry { fields.at(0), fields.at(1), std::move(decoded.decoded) };
}

bool hostsMatch(const QString &hosts, const QString &token)
{
    const QStringList patterns = hosts.split(u',', Qt::SkipEmptyParts);
    for (const QString &pattern : patterns) {
        if (pattern.compare(token, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

QStringList readLines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    // Tolerates a corrupt file: binary garbage just produces lines that do not parse.
    return QString::fromUtf8(file.readAll()).split(u'\n');
}

} // namespace

KnownHostsStore::KnownHostsStore(QString filePath)
    : m_path(std::move(filePath))
{ }

QString KnownHostsStore::defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QLatin1String("/known_hosts");
}

HostKeyInfo KnownHostsStore::classify(HostKeyInfo seen) const
{
    seen = normalized(std::move(seen));
    seen.knownFingerprint.clear();
    const QString token = hostToken(seen.host, seen.port);

    const std::lock_guard lock(m_mutex);

    QString otherKey; // fingerprint of a different key of the same type
    const QStringList lines = readLines(m_path);
    for (const QString &line : lines) {
        const std::optional<Entry> entry = parseLine(line);
        if (!entry || entry->keyType != seen.keyType || !hostsMatch(entry->hosts, token))
            continue;
        if (entry->key == seen.key) {
            seen.status = HostKeyStatus::Match;
            return seen;
        }
        if (otherKey.isEmpty())
            otherKey = fingerprintOf(entry->key);
    }

    for (const HostKeyInfo &trusted : m_session) {
        if (hostToken(trusted.host, trusted.port) == token && trusted.keyType == seen.keyType
            && trusted.key == seen.key) {
            seen.status = HostKeyStatus::Match;
            return seen;
        }
    }

    if (!otherKey.isEmpty()) {
        seen.status = HostKeyStatus::Mismatch;
        seen.knownFingerprint = otherKey;
    } else {
        seen.status = HostKeyStatus::Unknown;
    }
    return seen;
}

Result<bool> KnownHostsStore::remember(const HostKeyInfo &info)
{
    const HostKeyInfo key = normalized(info);
    const QString token = hostToken(key.host, key.port);
    const QString newLine = QStringLiteral("%1 %2 %3")
                                .arg(token, key.keyType, QString::fromLatin1(key.key.toBase64()));

    const std::lock_guard lock(m_mutex);

    QStringList kept;
    const QStringList lines = readLines(m_path);
    for (const QString &line : lines) {
        if (line.trimmed().isEmpty())
            continue;
        const std::optional<Entry> entry = parseLine(line);
        if (entry && entry->hosts.compare(token, Qt::CaseInsensitive) == 0
            && entry->keyType == key.keyType) {
            if (entry->key == key.key)
                return false; // already on file
            continue; // replaced below
        }
        kept.append(line.trimmed());
    }
    kept.append(newLine);

    // Parent directory first: the first remember() runs before anything else wrote there.
    const QString dir = QFileInfo(m_path).absolutePath();
    QSaveFile file(m_path);
    if (!QDir().mkpath(dir) || !file.open(QIODevice::WriteOnly)) {
        return fail(Error { ErrorKind::Internal,
                            QCoreApplication::translate("quarry::KnownHostsStore",
                                                        "The server key could not be saved."),
                            file.errorString(),
                            {},
                            {} });
    }
    file.write((kept.join(u'\n') + u'\n').toUtf8());
    if (!file.commit()) {
        return fail(Error { ErrorKind::Internal,
                            QCoreApplication::translate("quarry::KnownHostsStore",
                                                        "The server key could not be saved."),
                            file.errorString(),
                            {},
                            {} });
    }
    return true;
}

void KnownHostsStore::trustForSession(const HostKeyInfo &info)
{
    const std::lock_guard lock(m_mutex);
    m_session.push_back(normalized(info));
}

} // namespace quarry
