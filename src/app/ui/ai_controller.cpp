#include "ai_controller.h"

#include <quarry/ai/claude_cli.h>
#include <quarry/connection_store.h>

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QUuid>
#include <QVariantList>

namespace quarry::ui {

namespace {

// Next to the layout keys of LayoutStore (the same preferences, never quarry.db).
const QString kCliPathKey = QStringLiteral("ui/layout/ai/cliPath");

QString idText(const QUuid &id)
{
    return id.toString(QUuid::WithoutBraces);
}

QStringList listOf(const QVariant &value)
{
    QStringList out;
    if (value.typeId() == QMetaType::QString) {
        // One text: split on commas and line breaks, like the field of the connection dialog.
        const QStringList parts = value.toString().split(
            QRegularExpression(QStringLiteral("[,\\n\\r;]")), Qt::SkipEmptyParts);
        out.reserve(parts.size());
        for (const QString &part : parts)
            out.append(part.trimmed());
        return out;
    }
    const QStringList raw = value.toStringList();
    out.reserve(raw.size());
    for (const QString &item : raw)
        out.append(item.trimmed());
    return out;
}

// "2.0.14 (Claude Code)" -> "2.0.14"
QString versionNumber(const QString &version)
{
    return version.trimmed().section(QLatin1Char(' '), 0, 0);
}

} // namespace

AiController::AiController(QObject *parent)
    : QObject(parent)
{ }

// NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDelete): QPointer false positive
AiController::~AiController() = default;

void AiController::setConnections(ConnectionController *connections)
{
    if (m_connections == connections)
        return;
    if (m_connections)
        m_connections->disconnect(this);
    m_connections = connections;
    if (m_connections) {
        // The cascade of core deletes the row of a removed connection: forget it here too.
        connect(m_connections, &ConnectionController::connectionRemoved, this,
                [this](const QString &id) {
                    if (m_settings.remove(id))
                        emit settingsChanged(id);
                });
    }
    emit connectionsChanged();
}

QString AiController::databasePath() const
{
    const QString path = m_connections ? m_connections->databasePath() : QString();
    return path.isEmpty() ? quarry::ConnectionStore::defaultDatabasePath() : path;
}

// ---- global: the CLI ----------------------------------------------------------------------------

QString AiController::cliPath() const
{
    if (!m_cliPathRead) {
        m_cliPath = QSettings().value(kCliPathKey).toString();
        m_cliPathRead = true;
    }
    return m_cliPath;
}

void AiController::setCliPath(const QString &path)
{
    const QString trimmed = path.trimmed();
    if (m_cliPathRead && trimmed == m_cliPath)
        return;
    m_cliPath = trimmed;
    m_cliPathRead = true;
    if (trimmed.isEmpty())
        QSettings().remove(kCliPathKey);
    else
        QSettings().setValue(kCliPathKey, trimmed);
    emit cliPathChanged();
    // What was found belongs to the old path.
    if (m_state != QLatin1String("unknown") && m_state != QLatin1String("checking"))
        setCli(QStringLiteral("unknown"), {}, {}, {}, {});
}

QString AiController::toLocalPath(const QUrl &url) const
{
    return url.isLocalFile() ? QDir::toNativeSeparators(url.toLocalFile()) : url.toString();
}

QString AiController::loginHint() const
{
    return quarry::ai::loginCommandHint();
}

quarry::ai::ClaudeRunner *AiController::runner()
{
    if (m_runner == nullptr) {
        m_runner = new quarry::ai::ClaudeRunner(this);
        connect(m_runner, &quarry::ai::ClaudeRunner::located, this, &AiController::onLocated);
        connect(m_runner, &quarry::ai::ClaudeRunner::locateFailed, this,
                &AiController::onLocateFailed);
        connect(m_runner, &quarry::ai::ClaudeRunner::checked, this, &AiController::onChecked);
    }
    return m_runner;
}

void AiController::setCli(const QString &state, const QString &version, const QString &location,
                          const QString &message, const QString &detail)
{
    m_state = state;
    m_version = version;
    m_location = location;
    m_message = message;
    m_detail = detail;
    emit cliChanged();
}

void AiController::testCli()
{
    cancelCli();
    const QString custom = cliPath().trimmed();
    if (!custom.isEmpty() && !QFileInfo(custom).isFile()) {
        // Not handed to the runner: it would fall back to the usual places and the user would be
        // told about a Claude Code that is not the one they typed.
        setCli(QStringLiteral("notFound"), {}, custom,
               tr("There is no file at %1. Fix the path or leave it empty to look for Claude Code "
                  "automatically.")
                   .arg(QDir::toNativeSeparators(custom)),
               {});
        return;
    }
    setCli(QStringLiteral("checking"), {}, {}, tr("Checking…"), {});
    m_locateId = runner()->locate(custom, true);
}

void AiController::cancelCli()
{
    const quint64 check = m_checkId;
    const bool was = m_locateId != 0 || check != 0;
    // Forget the ids first: cancelling may answer from inside the call.
    m_locateId = 0;
    m_checkId = 0;
    if (m_runner != nullptr && check != 0)
        m_runner->cancel(check);
    if (was && m_state == QLatin1String("checking"))
        setCli(QStringLiteral("unknown"), {}, {}, {}, {});
}

void AiController::onLocated(quint64 id, const QString &path)
{
    if (id == 0 || id != m_locateId)
        return;
    m_locateId = 0;
    m_location = path;
    m_checkId = runner()->check(path);
}

void AiController::onLocateFailed(quint64 id, const QString &detail)
{
    if (id == 0 || id != m_locateId)
        return;
    m_locateId = 0;
    // No URL: none is documented in ARCHITECTURE (UX.md says so); the user knows where theirs is.
    setCli(QStringLiteral("notFound"), {}, {},
           tr("Claude Code was not found. Install Claude Code first, or enter the path of its "
              "executable."),
           detail);
}

void AiController::onChecked(quint64 id, const quarry::ai::CliInfo &info)
{
    if (id == 0 || id != m_checkId)
        return;
    m_checkId = 0;
    const QString path = QDir::toNativeSeparators(info.path);
    using quarry::ai::AiErrorKind;
    if (info.error == AiErrorKind::None && info.compatible) {
        setCli(QStringLiteral("ok"), info.version, path,
               tr("Claude Code %1 found at %2").arg(versionNumber(info.version), path), {});
    } else if (info.error == AiErrorKind::IncompatibleVersion) {
        const QString number = versionNumber(info.version);
        setCli(QStringLiteral("incompatible"), info.version, path,
               tr("Version %1 is too old; update Claude Code.")
                   .arg(number.isEmpty() ? tr("(unknown)") : number),
               info.detail);
    } else if (info.error == AiErrorKind::CliNotFound) {
        setCli(QStringLiteral("notFound"), {}, path,
               tr("Claude Code could not be started from %1. Check that the file is Claude Code "
                  "and that you may run it.")
                   .arg(path),
               info.detail);
    } else {
        setCli(QStringLiteral("failed"), info.version, path,
               tr("Claude Code did not answer as expected."), info.detail);
    }
}

// ---- per connection -----------------------------------------------------------------------------

quarry::AiSettingsService *AiController::service()
{
    if (!m_service) {
        m_service = std::make_unique<quarry::AiSettingsService>(databasePath());
        connect(m_service.get(), &quarry::AiSettingsService::loaded, this, &AiController::onLoaded);
        connect(m_service.get(), &quarry::AiSettingsService::saved, this, &AiController::onSaved);
        connect(m_service.get(), &quarry::AiSettingsService::failed, this, &AiController::onFailed);
    }
    return m_service.get();
}

void AiController::ensureSettings()
{
    if (m_loadStarted)
        return;
    m_loadStarted = true;
    m_ops.push_back({ QUuid(), true });
    service()->loadAll();
}

QVariantMap AiController::toMap(const quarry::AiConnectionSettings &s)
{
    return { { QStringLiteral("enabled"), s.enabled },
             { QStringLiteral("excludedSchemas"), s.excludedSchemas },
             { QStringLiteral("excludedTables"), s.excludedTables },
             { QStringLiteral("sendComments"), s.sendComments },
             { QStringLiteral("sendDefaults"), s.sendDefaults },
             { QStringLiteral("sendChecks"), s.sendChecks },
             { QStringLiteral("sendEnumValues"), s.sendEnumValues },
             { QStringLiteral("sendDefinitions"), s.sendDefinitions },
             { QStringLiteral("maskLiterals"), s.maskLiterals },
             { QStringLiteral("maskNumbers"), s.maskNumbers },
             { QStringLiteral("maxTables"), s.maxTables } };
}

quarry::AiConnectionSettings AiController::fromMap(const QUuid &id, const QVariantMap &map)
{
    quarry::AiConnectionSettings s; // what the map does not say stays at the default
    s.connectionId = id;
    s.enabled = map.value(QStringLiteral("enabled"), s.enabled).toBool();
    s.excludedSchemas = listOf(map.value(QStringLiteral("excludedSchemas")));
    s.excludedTables = listOf(map.value(QStringLiteral("excludedTables")));
    s.sendComments = map.value(QStringLiteral("sendComments"), s.sendComments).toBool();
    s.sendDefaults = map.value(QStringLiteral("sendDefaults"), s.sendDefaults).toBool();
    s.sendChecks = map.value(QStringLiteral("sendChecks"), s.sendChecks).toBool();
    s.sendEnumValues = map.value(QStringLiteral("sendEnumValues"), s.sendEnumValues).toBool();
    s.sendDefinitions = map.value(QStringLiteral("sendDefinitions"), s.sendDefinitions).toBool();
    s.maskLiterals = map.value(QStringLiteral("maskLiterals"), s.maskLiterals).toBool();
    s.maskNumbers = map.value(QStringLiteral("maskNumbers"), s.maskNumbers).toBool();
    s.maxTables = map.value(QStringLiteral("maxTables"), s.maxTables).toInt();
    return s;
}

quarry::ai::ContextOptions AiController::optionsFrom(const quarry::AiConnectionSettings &s)
{
    quarry::ai::ContextOptions o;
    o.excludedSchemas = s.excludedSchemas;
    o.excludedTables = s.excludedTables;
    o.sendComments = s.sendComments;
    o.sendDefaults = s.sendDefaults;
    o.sendChecks = s.sendChecks;
    o.sendEnumValues = s.sendEnumValues;
    o.sendDefinitions = s.sendDefinitions;
    o.maxTables = s.maxTables;
    return o;
}

quarry::AiConnectionSettings AiController::storedSettings(const QString &connectionId)
{
    ensureSettings();
    const auto it = m_settings.constFind(connectionId);
    if (it != m_settings.constEnd())
        return it.value();
    quarry::AiConnectionSettings defaults;
    defaults.connectionId = QUuid::fromString(connectionId);
    return defaults;
}

QVariantMap AiController::settingsOf(const QString &connectionId)
{
    return toMap(storedSettings(connectionId));
}

bool AiController::isEnabled(const QString &connectionId)
{
    return storedSettings(connectionId).enabled;
}

void AiController::setLastError(const QString &message)
{
    if (m_lastError == message)
        return;
    m_lastError = message;
    emit lastErrorChanged();
}

bool AiController::saveSettings(const QString &connectionId, const QVariantMap &map)
{
    setLastError({});
    const QUuid id = QUuid::fromString(connectionId);
    const auto reject = [&](const QString &message) {
        setLastError(message);
        emit saveFailed(connectionId, message, {});
        return false;
    };
    if (id.isNull())
        return reject(tr("The connection is not known."));

    const quarry::AiConnectionSettings settings = fromMap(id, map);
    // Defense in depth: the dialog asks for the name, but the rule does not depend on the dialog.
    // Turning the AI ON for a production connection (it was not on) needs the written confirmation.
    if (settings.enabled && m_connections != nullptr) {
        const auto profile = m_connections->model()->profile(id);
        if (!profile)
            return reject(tr("The connection is not known."));
        if (profile->production && !storedSettings(connectionId).enabled
            && !map.value(QStringLiteral("confirmedProduction")).toBool()) {
            return reject(tr("This is a production connection. Type its name to turn the AI "
                             "assistant on."));
        }
    }
    ensureSettings();
    m_ops.push_back({ id, false });
    service()->save(settings);
    return true;
}

void AiController::onLoaded(const QList<quarry::AiConnectionSettings> &list)
{
    if (!m_ops.empty() && m_ops.front().load)
        m_ops.pop_front();
    // A save that finished before this answer is newer than the read: keep what is known.
    QHash<QString, quarry::AiConnectionSettings> fresh;
    for (const quarry::AiConnectionSettings &s : list)
        fresh.insert(idText(s.connectionId), s);
    for (auto it = m_settings.cbegin(); it != m_settings.cend(); ++it)
        fresh.insert(it.key(), it.value());
    m_settings = fresh;
    if (!m_ready) {
        m_ready = true;
        emit settingsReadyChanged();
    }
    emit settingsChanged(QString());
}

void AiController::onSaved(const quarry::AiConnectionSettings &settings)
{
    if (!m_ops.empty() && !m_ops.front().load)
        m_ops.pop_front();
    const QString id = idText(settings.connectionId);
    m_settings.insert(id, settings);
    emit settingsChanged(id);
}

void AiController::onFailed(const quarry::Error &error)
{
    const Op op = m_ops.empty() ? Op {} : m_ops.front();
    if (!m_ops.empty())
        m_ops.pop_front();
    setLastError(error.message);
    if (op.load) {
        m_loadStarted = false; // asked for again by the next settingsOf()
        emit saveFailed(QString(), error.message, error.detail);
        return;
    }
    emit saveFailed(idText(op.id), error.message, error.detail);
}

} // namespace quarry::ui
