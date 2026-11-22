#include "ai_settings_worker.h"

#include <quarry/ai_settings.h>

#include <QLoggingCategory>
#include <QSet>
#include <QThread>

#include <algorithm>

namespace quarry {

Q_LOGGING_CATEGORY(lcAiSettings, "quarry.aisettings")

namespace {

constexpr int kShutdownTimeoutMs = 5000;
constexpr int kMinTables = 1;
constexpr int kMaxTables = 200;

QStringList cleaned(const QStringList &list)
{
    QStringList out;
    QSet<QString> seen;
    out.reserve(list.size());
    for (const QString &item : list) {
        const QString trimmed = item.trimmed();
        if (trimmed.isEmpty() || seen.contains(trimmed.toCaseFolded()))
            continue;
        seen.insert(trimmed.toCaseFolded());
        out.append(trimmed);
    }
    return out;
}

} // namespace

bool AiConnectionSettings::isDefault() const
{
    AiConnectionSettings other = *this;
    other.connectionId = QUuid();
    return other == AiConnectionSettings();
}

AiConnectionSettings AiSettingsService::normalized(AiConnectionSettings settings)
{
    settings.excludedSchemas = cleaned(settings.excludedSchemas);
    settings.excludedTables = cleaned(settings.excludedTables);
    settings.maxTables = std::clamp(settings.maxTables, kMinTables, kMaxTables);
    return settings;
}

AiSettingsService::AiSettingsService(QString databasePath, QObject *parent)
    : QObject(parent)
    , m_thread(new QThread) // no parent: must outlive this object if the join times out
{
    qRegisterMetaType<quarry::Error>();
    qRegisterMetaType<quarry::AiConnectionSettings>();
    qRegisterMetaType<QList<quarry::AiConnectionSettings>>();

    auto *worker = new AiSettingsWorker(std::move(databasePath));
    m_worker = worker;
    worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, worker, &QObject::deleteLater);
    const auto q = Qt::QueuedConnection;
    connect(worker, &AiSettingsWorker::loaded, this, &AiSettingsService::loaded, q);
    connect(worker, &AiSettingsWorker::saved, this, &AiSettingsService::saved, q);
    connect(worker, &AiSettingsWorker::failed, this, &AiSettingsService::failed, q);
    m_thread->start();
}

AiSettingsService::~AiSettingsService()
{
    QMetaObject::invokeMethod(
        m_worker,
        [w = static_cast<AiSettingsWorker *>(m_worker)] {
            w->closeDatabase();
            QThread::currentThread()->quit();
        },
        Qt::QueuedConnection);
    if (m_thread->wait(kShutdownTimeoutMs)) {
        delete m_thread;
    } else {
        qCWarning(lcAiSettings) << "AI settings thread did not stop within" << kShutdownTimeoutMs
                                << "ms; leaking it until the disk call returns";
        connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    }
}

void AiSettingsService::loadAll()
{
    QMetaObject::invokeMethod(
        m_worker, [w = static_cast<AiSettingsWorker *>(m_worker)] { w->load(); },
        Qt::QueuedConnection);
}

void AiSettingsService::save(const AiConnectionSettings &settings)
{
    QMetaObject::invokeMethod(
        m_worker,
        [w = static_cast<AiSettingsWorker *>(m_worker), value = normalized(settings)] {
            w->save(value);
        },
        Qt::QueuedConnection);
}

} // namespace quarry
