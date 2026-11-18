#include "ssh/ssh_client.h"

#include <quarry/cancellation.h>
#include <quarry/ssh_host_key.h>

#include <QLoggingCategory>
#include <QMetaObject>
#include <QThread>

#include <atomic>
#include <chrono>
#include <mutex>

Q_LOGGING_CATEGORY(lcHostKeyChecker, "quarry.ssh.hostkey")

namespace quarry {

namespace {

constexpr int kShutdownTimeoutMs = 5000;

void registerMetaTypes()
{
    static const bool done = [] {
        qRegisterMetaType<quarry::Error>();
        qRegisterMetaType<quarry::HostKeyInfo>();
        qRegisterMetaType<quarry::SshTunnelConfig>();
        return true;
    }();
    Q_UNUSED(done)
}

} // namespace

// Lives on the checker's thread; the UI side only calls the thread-safe begin()/abort().
class SshHostKeyChecker::Worker : public QObject
{
    Q_OBJECT

public:
    explicit Worker(std::shared_ptr<KnownHostsStore> store)
        : m_store(std::move(store))
    { }

    struct Ticket
    {
        quint64 generation = 0;
        CancellationToken token;
    };

    // A new check replaces the running one: its result is dropped by generation.
    Ticket begin()
    {
        const std::lock_guard lock(m_mutex);
        m_source.cancel();
        m_source = CancellationSource();
        return { ++m_generation, m_source.token() };
    }
    void abort()
    {
        const std::lock_guard lock(m_mutex);
        m_source.cancel();
        ++m_generation;
    }
    bool isCurrent(quint64 generation) const
    {
        const std::lock_guard lock(m_mutex);
        return generation == m_generation;
    }

    void run(quint64 generation, const SshTunnelConfig &config, const CancellationToken &token)
    {
        auto finish = [&](const HostKeyInfo &info, const Error &error, bool ok) {
            emit done(generation, info, error, ok);
        };
        if (token.isCancelled())
            return; // replaced or cancelled before it started: the facade drops it anyway

        const QString problem = config.validationProblem();
        if (!problem.isEmpty()) {
            finish({}, { ErrorKind::Connection, problem, {}, {}, {} }, false);
            return;
        }

        ssh::Client client;
        ssh::Interrupt interrupt;
        interrupt.cancelled = [token] { return token.isCancelled(); };
        interrupt.deadline
            = std::chrono::steady_clock::now() + std::chrono::seconds(ssh::kConnectTimeoutSeconds);
        const Result<void> connected = client.connectAndHandshake(
            config.host, config.port, interrupt, m_store->knownKeyTypes(config.host, config.port));
        if (!connected) {
            finish({}, connected.error(), false);
            return;
        }
        HostKeyInfo info = client.hostKey(config.host, config.port);
        client.disconnect(); // before authenticating: nothing secret has been sent
        if (info.key.isEmpty()) {
            finish({},
                   { ErrorKind::Connection,
                     tr("The SSH server did not send a host key."),
                     {},
                     {},
                     {} },
                   false);
            return;
        }
        finish(m_store->classify(std::move(info)), {}, true);
    }

signals:
    void done(quint64 generation, const quarry::HostKeyInfo &info, const quarry::Error &error,
              bool ok);

private:
    std::shared_ptr<KnownHostsStore> m_store;
    mutable std::mutex m_mutex;
    CancellationSource m_source;
    quint64 m_generation = 0;
};

SshHostKeyChecker::SshHostKeyChecker(std::shared_ptr<KnownHostsStore> store, QObject *parent)
    : QObject(parent)
    , m_store(std::move(store))
    , m_thread(new QThread) // no parent: must outlive this object if the join times out
{
    registerMetaTypes();
    m_worker = new Worker(m_store);
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(
        m_worker, &Worker::done, this,
        [this](quint64 generation, const HostKeyInfo &info, const Error &error, bool ok) {
            if (!m_worker->isCurrent(generation))
                return;
            if (ok)
                emit checked(info);
            else
                emit failed(error);
        },
        Qt::QueuedConnection);
    m_thread->start();
}

SshHostKeyChecker::~SshHostKeyChecker()
{
    m_worker->abort();
    m_thread->quit();
    if (m_thread->wait(kShutdownTimeoutMs)) {
        delete m_thread;
    } else {
        qCWarning(lcHostKeyChecker) << "Host key check did not stop within" << kShutdownTimeoutMs
                                    << "ms; leaking its thread until it returns";
        connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    }
}

void SshHostKeyChecker::check(const SshTunnelConfig &config)
{
    const Worker::Ticket ticket = m_worker->begin();
    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker, ticket, config] {
            worker->run(ticket.generation, config, ticket.token);
        },
        Qt::QueuedConnection);
}

void SshHostKeyChecker::cancel()
{
    m_worker->abort();
}

void SshHostKeyChecker::accept(const HostKeyInfo &info, bool remember)
{
    if (remember) {
        const Result<bool> saved = m_store->remember(info);
        if (!saved) {
            emit failed(saved.error());
            return;
        }
    } else {
        m_store->trustForSession(info);
    }
    emit accepted(info);
}

} // namespace quarry

#include "ssh_host_key_checker.moc"
