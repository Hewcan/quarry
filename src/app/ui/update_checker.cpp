#include "update_checker.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>

namespace quarry::ui {

namespace {

constexpr auto kLatestRelease = "https://api.github.com/repos/Hewcan/quarry/releases/latest";
constexpr int kCheckTimeoutMs = 15000;
constexpr int kStallTimeoutMs = 30000; // no bytes for this long ends the download

QList<int> numbersOf(const QString &version, bool *pre)
{
    const qsizetype dash = version.indexOf(QLatin1Char('-'));
    *pre = dash >= 0;
    QList<int> out;
    const auto parts = QStringView(version).left(dash < 0 ? version.size() : dash).split(u'.');
    for (const QStringView part : parts)
        out.append(part.toInt());
    return out;
}

} // namespace

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
    , m_endpoint(QString::fromLatin1(kLatestRelease))
    , m_suffix(platformSuffix())
{ }

UpdateChecker::~UpdateChecker() = default;

QString UpdateChecker::currentVersion() const
{
    return m_currentVersion.isEmpty() ? QCoreApplication::applicationVersion() : m_currentVersion;
}

QString UpdateChecker::platformSuffix()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("-setup.exe");
#elif defined(Q_OS_LINUX)
    return QStringLiteral(".AppImage");
#else
    return {}; // no package yet (macOS): the dialog links to the release page
#endif
}

bool UpdateChecker::isValidVersion(const QString &v)
{
    static const QRegularExpression re(
        QStringLiteral(R"(^\d{1,6}(\.\d{1,6}){1,3}(-[0-9A-Za-z.-]{1,32})?$)"));
    return re.match(v).hasMatch();
}

int UpdateChecker::compareVersions(const QString &a, const QString &b)
{
    bool preA = false;
    bool preB = false;
    QList<int> na = numbersOf(a, &preA);
    QList<int> nb = numbersOf(b, &preB);
    const qsizetype n = std::max(na.size(), nb.size());
    na.resize(n);
    nb.resize(n);
    for (qsizetype i = 0; i < n; ++i) {
        if (na[i] != nb[i])
            return na[i] < nb[i] ? -1 : 1;
    }
    if (preA != preB)
        return preA ? -1 : 1;
    return 0;
}

bool UpdateChecker::parseRelease(const QByteArray &json, const QString &suffix, bool allowFile,
                                 ReleaseInfo *out, QString *error)
{
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        *error = tr("The update server sent an answer Quarry does not understand.");
        return false;
    }
    const QJsonObject root = doc.object();
    QString version = root.value(QLatin1String("tag_name")).toString();
    if (version.startsWith(QLatin1Char('v')))
        version.remove(0, 1);
    if (!isValidVersion(version)) {
        *error = tr("The latest release has no valid version number.");
        return false;
    }

    const auto acceptable = [allowFile](const QUrl &url) {
        return url.scheme() == QLatin1String("https")
            || (allowFile && url.scheme() == QLatin1String("file"));
    };
    ReleaseInfo info;
    info.version = version;
    info.notes = root.value(QLatin1String("body")).toString().left(8000);
    const QUrl page(root.value(QLatin1String("html_url")).toString());
    if (page.scheme() == QLatin1String("https"))
        info.pageUrl = page;

    static const QRegularExpression safeName(QStringLiteral("^[A-Za-z0-9._-]{1,120}$"));
    static const QRegularExpression sha(QStringLiteral("^sha256:([0-9a-fA-F]{64})$"));
    if (!suffix.isEmpty()) {
        const QJsonArray assets = root.value(QLatin1String("assets")).toArray();
        for (const QJsonValue &v : assets) {
            const QJsonObject asset = v.toObject();
            const QString name = asset.value(QLatin1String("name")).toString();
            const QUrl url(asset.value(QLatin1String("browser_download_url")).toString());
            if (!name.endsWith(suffix, Qt::CaseSensitive) || !safeName.match(name).hasMatch()
                || !acceptable(url))
                continue;
            info.assetName = name;
            info.assetUrl = url;
            info.size = static_cast<qint64>(asset.value(QLatin1String("size")).toDouble());
            const auto m = sha.match(asset.value(QLatin1String("digest")).toString());
            if (m.hasMatch())
                info.sha256 = m.captured(1).toLower();
            break;
        }
    }
    *out = info;
    return true;
}

QNetworkAccessManager *UpdateChecker::network()
{
    if (m_network == nullptr)
        m_network = new QNetworkAccessManager(this);
    return m_network;
}

void UpdateChecker::setState(State s)
{
    m_state = s;
    emit stateChanged();
}

void UpdateChecker::fail(const QString &message)
{
    m_error = message;
    setState(Failed);
}

void UpdateChecker::startupCheck()
{
    if (currentVersion().isEmpty() || qEnvironmentVariableIsSet("QUARRY_NO_UPDATE_CHECK"))
        return;
    check(true);
}

void UpdateChecker::check(bool silent)
{
    if (m_state == Checking || m_state == Downloading)
        return;
    m_silent = silent;
    m_error.clear();
    m_release = {};
    setState(Checking);

    QNetworkRequest req(m_endpoint);
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Quarry-updater"));
    req.setTransferTimeout(kCheckTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = network()->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] { onCheckFinished(reply); });
}

void UpdateChecker::onCheckFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    if (m_state != Checking)
        return;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 404) { // GitHub answers 404 for "latest" while nothing is published
        setState(UpToDate);
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        fail(tr("Quarry could not reach the update server. Check your connection and try again."));
        return;
    }
    ReleaseInfo info;
    QString error;
    if (!parseRelease(reply->readAll(), m_suffix, m_allowFile, &info, &error)) {
        fail(error);
        return;
    }
    m_release = info;
    if (compareVersions(info.version, currentVersion()) > 0) {
        setState(Available);
        if (m_silent)
            emit promptRequested();
    } else {
        setState(UpToDate);
    }
}

QString UpdateChecker::downloadDir() const
{
    if (!m_downloadDir.isEmpty())
        return m_downloadDir;
#if defined(Q_OS_LINUX)
    // An AppImage updates itself in place: the new file goes next to the running one.
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    if (!appImage.isEmpty()) {
        const QFileInfo dir(QFileInfo(appImage).absolutePath());
        if (dir.isWritable())
            return dir.absoluteFilePath();
    }
#endif
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/quarry-update");
}

void UpdateChecker::download()
{
    if (m_state != Available || !canInstall())
        return;
    const QString dir = downloadDir();
    if (!QDir().mkpath(dir)) {
        fail(tr("Quarry could not create the folder for the update."));
        return;
    }
    m_finalPath = dir + QLatin1Char('/') + m_release.assetName;
    m_file = std::make_unique<QFile>(m_finalPath + QStringLiteral(".part"));
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_file.reset();
        fail(tr("Quarry could not save the update to disk."));
        return;
    }
    m_hasher = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    m_progress = 0;
    emit progressChanged();
    setState(Downloading);

    QNetworkRequest req(m_release.assetUrl);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Quarry-updater"));
    req.setTransferTimeout(kStallTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    m_reply = network()->get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        const QByteArray chunk = m_reply->readAll();
        if (m_file && m_file->write(chunk) != chunk.size()) {
            m_file->close(); // disk full: the finished handler reports it
            m_reply->abort();
            return;
        }
        m_hasher->addData(chunk);
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        const qint64 expected = total > 0 ? total : m_release.size;
        m_progress = expected > 0 ? std::min(1.0, double(got) / double(expected)) : 0;
        emit progressChanged();
    });
    connect(m_reply, &QNetworkReply::finished, this, &UpdateChecker::onDownloadFinished);
}

void UpdateChecker::cancelDownload()
{
    if (m_state != Downloading || m_reply == nullptr)
        return;
    m_reply->abort(); // onDownloadFinished cleans up and goes back to Available
}

void UpdateChecker::onDownloadFinished()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();
    const QString part = m_file ? m_file->fileName() : QString();
    const bool writeFailed = m_file && !m_file->isOpen();
    if (m_file)
        m_file->close();
    m_file.reset();
    const auto discard = [&part] { QFile::remove(part); };

    if (reply->error() == QNetworkReply::OperationCanceledError && !writeFailed) {
        discard();
        setState(Available); // the user cancelled (or the stall timeout hit: same way back)
        return;
    }
    if (writeFailed) {
        discard();
        fail(tr("Quarry could not save the update to disk."));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        discard();
        fail(tr("The download of the update failed. Try again later."));
        return;
    }
    const QByteArray got = m_hasher->result().toHex();
    m_hasher.reset();
    if (!m_release.sha256.isEmpty() && QString::fromLatin1(got) != m_release.sha256) {
        discard();
        fail(tr("The downloaded update does not match its checksum, so it was discarded."));
        return;
    }
    QFile::remove(m_finalPath);
    if (!QFile::rename(part, m_finalPath)) {
        discard();
        fail(tr("Quarry could not save the update to disk."));
        return;
    }
    QFile::setPermissions(m_finalPath, QFile::permissions(m_finalPath) | QFileDevice::ExeOwner);
    m_progress = 1;
    emit progressChanged();
    setState(Ready);
}

void UpdateChecker::installAndQuit()
{
    if (m_state != Ready)
        return;
    QString toRun = m_finalPath;
#if defined(Q_OS_LINUX)
    // Replace the running AppImage with the new one; unlinking a running file is safe on Linux.
    const QString current = qEnvironmentVariable("APPIMAGE");
    if (!current.isEmpty()
        && QFileInfo(current).absolutePath() == QFileInfo(m_finalPath).absolutePath()
        && QFile::remove(current) && QFile::rename(m_finalPath, current))
        toRun = current;
#endif
    if (!QProcess::startDetached(toRun, {})) {
        fail(tr("Quarry could not start the update. The file is at %1.").arg(m_finalPath));
        return;
    }
    QCoreApplication::quit();
}

} // namespace quarry::ui
