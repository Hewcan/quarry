#include <quarry/ssh_host_key.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <thread>
#include <vector>

using namespace quarry;

namespace {

void appendString(QByteArray &blob, const QByteArray &text)
{
    const auto length = static_cast<quint32>(text.size());
    blob.append(static_cast<char>((length >> 24) & 0xff));
    blob.append(static_cast<char>((length >> 16) & 0xff));
    blob.append(static_cast<char>((length >> 8) & 0xff));
    blob.append(static_cast<char>(length & 0xff));
    blob.append(text);
}

// A syntactically valid public key blob (algorithm name + payload); never used for crypto.
QByteArray makeKey(const QByteArray &type, char seed)
{
    QByteArray blob;
    appendString(blob, type);
    appendString(blob, QByteArray(32, seed));
    return blob;
}

HostKeyInfo infoFor(const QString &host, int port, const QByteArray &key)
{
    HostKeyInfo info;
    info.host = host;
    info.port = port;
    info.key = key;
    return info;
}

QString expectedFingerprint(const QByteArray &key)
{
    return QStringLiteral("SHA256:")
        + QString::fromLatin1(
               QCryptographicHash::hash(key, QCryptographicHash::Sha256)
                   .toBase64(QByteArray::Base64Encoding | QByteArray::OmitTrailingEquals));
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class TstKnownHostsStore : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void defaultPathIsInAppData();
    void unknownThenMatchAfterRemember();
    void fingerprintFormat();
    void nonDefaultPortUsesBrackets();
    void mismatchReportsKnownFingerprint();
    void otherKeyTypeIsUnknown();
    void hostNameIsCaseInsensitiveAndPortSensitive();
    void trustForSessionWritesNothing();
    void rememberIsIdempotentAndReplacesSameType();
    void corruptFileIsToleratedAndPreserved();
    void unwritablePathFails();
    void atomicWriteLeavesNoTemporaryFiles();
    void concurrentUse();
};

void TstKnownHostsStore::defaultPathIsInAppData()
{
    const QString path = KnownHostsStore::defaultPath();
    QVERIFY(path.endsWith(QLatin1String("/known_hosts")));
    QVERIFY2(path.contains(QLatin1String("qttest"), Qt::CaseInsensitive),
             qPrintable(path)); // test mode: isolated
}

void TstKnownHostsStore::unknownThenMatchAfterRemember()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("sub/known_hosts")); // directory is created
    KnownHostsStore store(path);

    const HostKeyInfo seen
        = infoFor(QStringLiteral("db.example.com"), 22, makeKey("ssh-ed25519", 'a'));
    const HostKeyInfo first = store.classify(seen);
    QCOMPARE(first.status, HostKeyStatus::Unknown);
    QCOMPARE(first.keyType, QStringLiteral("ssh-ed25519"));
    QVERIFY(!QFileInfo::exists(path)); // classify never writes

    const Result<bool> saved = store.remember(first);
    QVERIFY(saved.has_value());
    QVERIFY(saved.value());
    QCOMPARE(store.classify(seen).status, HostKeyStatus::Match);

    // A new store reads the file: the key really was persisted.
    KnownHostsStore other(path);
    QCOMPARE(other.classify(seen).status, HostKeyStatus::Match);

    const QByteArray content = readFile(path);
    QVERIFY(content.startsWith("db.example.com ssh-ed25519 "));
    QVERIFY(content.endsWith('\n'));
}

void TstKnownHostsStore::fingerprintFormat()
{
    QTemporaryDir dir;
    KnownHostsStore store(dir.filePath(QStringLiteral("known_hosts")));
    const QByteArray key = makeKey("ssh-ed25519", 'f');
    const HostKeyInfo result = store.classify(infoFor(QStringLiteral("h"), 22, key));
    QCOMPARE(result.fingerprint, expectedFingerprint(key));
    QVERIFY(result.fingerprint.startsWith(QLatin1String("SHA256:")));
    QVERIFY(!result.fingerprint.contains(QLatin1Char('=')));
}

void TstKnownHostsStore::nonDefaultPortUsesBrackets()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("known_hosts"));
    KnownHostsStore store(path);
    const HostKeyInfo seen
        = infoFor(QStringLiteral("db.example.com"), 2222, makeKey("ssh-ed25519", 'b'));
    QVERIFY(store.remember(seen).has_value());
    QVERIFY(readFile(path).startsWith("[db.example.com]:2222 ssh-ed25519 "));
    QCOMPARE(store.classify(seen).status, HostKeyStatus::Match);

    HostKeyInfo defaultPort = seen;
    defaultPort.port = 22;
    QCOMPARE(store.classify(defaultPort).status, HostKeyStatus::Unknown);
}

void TstKnownHostsStore::mismatchReportsKnownFingerprint()
{
    QTemporaryDir dir;
    KnownHostsStore store(dir.filePath(QStringLiteral("known_hosts")));
    const QByteArray known = makeKey("ssh-ed25519", '1');
    const QByteArray changed = makeKey("ssh-ed25519", '2');
    QVERIFY(store.remember(infoFor(QStringLiteral("srv"), 22, known)).has_value());

    const HostKeyInfo result = store.classify(infoFor(QStringLiteral("srv"), 22, changed));
    QCOMPARE(result.status, HostKeyStatus::Mismatch);
    QCOMPARE(result.knownFingerprint, expectedFingerprint(known));
    QCOMPARE(result.fingerprint, expectedFingerprint(changed));
}

void TstKnownHostsStore::otherKeyTypeIsUnknown()
{
    QTemporaryDir dir;
    KnownHostsStore store(dir.filePath(QStringLiteral("known_hosts")));
    QVERIFY(store.remember(infoFor(QStringLiteral("srv"), 22, makeKey("ssh-ed25519", '1')))
                .has_value());
    const HostKeyInfo result
        = store.classify(infoFor(QStringLiteral("srv"), 22, makeKey("ecdsa-sha2-nistp256", '1')));
    QCOMPARE(result.status, HostKeyStatus::Unknown);
    QVERIFY(result.knownFingerprint.isEmpty());
}

void TstKnownHostsStore::hostNameIsCaseInsensitiveAndPortSensitive()
{
    QTemporaryDir dir;
    KnownHostsStore store(dir.filePath(QStringLiteral("known_hosts")));
    const QByteArray key = makeKey("ssh-ed25519", '3');
    QVERIFY(store.remember(infoFor(QStringLiteral("Example.COM"), 22, key)).has_value());
    QCOMPARE(store.classify(infoFor(QStringLiteral("example.com"), 22, key)).status,
             HostKeyStatus::Match);
    QCOMPARE(store.classify(infoFor(QStringLiteral("other.com"), 22, key)).status,
             HostKeyStatus::Unknown);
}

void TstKnownHostsStore::trustForSessionWritesNothing()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("known_hosts"));
    KnownHostsStore store(path);
    const HostKeyInfo seen = infoFor(QStringLiteral("srv"), 22, makeKey("ssh-ed25519", '4'));
    QCOMPARE(store.classify(seen).status, HostKeyStatus::Unknown);
    store.trustForSession(seen);
    QCOMPARE(store.classify(seen).status, HostKeyStatus::Match);
    QVERIFY(!QFileInfo::exists(path));

    KnownHostsStore fresh(path); // "until the store is destroyed"
    QCOMPARE(fresh.classify(seen).status, HostKeyStatus::Unknown);
}

void TstKnownHostsStore::rememberIsIdempotentAndReplacesSameType()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("known_hosts"));
    KnownHostsStore store(path);
    const HostKeyInfo one = infoFor(QStringLiteral("srv"), 22, makeKey("ssh-ed25519", '5'));
    QVERIFY(store.remember(one).value());
    const QByteArray before = readFile(path);
    const Result<bool> again = store.remember(one);
    QVERIFY(again.has_value());
    QVERIFY(!again.value()); // already on file
    QCOMPARE(readFile(path), before);

    const HostKeyInfo two = infoFor(QStringLiteral("srv"), 22, makeKey("ssh-ed25519", '6'));
    QVERIFY(store.remember(two).value());
    QCOMPARE(store.classify(two).status, HostKeyStatus::Match);
    QCOMPARE(store.classify(one).status, HostKeyStatus::Mismatch);
    QCOMPARE(readFile(path).count('\n'), 1);
}

void TstKnownHostsStore::corruptFileIsToleratedAndPreserved()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("known_hosts"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("# comment\nnot a valid line\n");
        const char binary[] = { '\x00', '\xff', '\xfe', ' ', 'b', '\x01', '\x02', '\n' };
        file.write(binary, sizeof(binary));
        file.write("srv ssh-ed25519 !!!not-base64\n");
        file.write("onlyone\n\n\n");
    }
    KnownHostsStore store(path);
    const HostKeyInfo seen = infoFor(QStringLiteral("srv"), 22, makeKey("ssh-ed25519", '7'));
    QCOMPARE(store.classify(seen).status, HostKeyStatus::Unknown);

    QVERIFY(store.remember(seen).has_value());
    QCOMPARE(store.classify(seen).status, HostKeyStatus::Match);
    const QByteArray content = readFile(path);
    QVERIFY(content.contains("# comment"));
    QVERIFY(content.contains("not a valid line"));
}

void TstKnownHostsStore::unwritablePathFails()
{
    QTemporaryDir dir;
    const QString blocker = dir.filePath(QStringLiteral("file"));
    QFile file(blocker);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    // The "directory" is a regular file, so neither mkpath nor the save can work.
    KnownHostsStore store(blocker + QLatin1String("/known_hosts"));
    const Result<bool> saved
        = store.remember(infoFor(QStringLiteral("srv"), 22, makeKey("ssh-ed25519", '8')));
    QVERIFY(!saved.has_value());
    QVERIFY(!saved.error().message.isEmpty());
}

void TstKnownHostsStore::atomicWriteLeavesNoTemporaryFiles()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("known_hosts"));
    KnownHostsStore store(path);
    for (int i = 0; i < 5; ++i) {
        QVERIFY(store
                    .remember(infoFor(QStringLiteral("h%1").arg(i), 22,
                                      makeKey("ssh-ed25519", static_cast<char>('a' + i))))
                    .has_value());
    }
    const QStringList files = QDir(dir.path()).entryList(QDir::Files);
    QCOMPARE(files, QStringList { QStringLiteral("known_hosts") });
    QCOMPARE(readFile(path).count('\n'), 5);
}

void TstKnownHostsStore::concurrentUse()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("known_hosts"));
    KnownHostsStore store(path);

    constexpr int kThreads = 4;
    constexpr int kPerThread = 15;
    std::atomic<int> failures { 0 };
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&store, &failures, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const HostKeyInfo info
                    = infoFor(QStringLiteral("host-%1-%2").arg(t).arg(i), 22,
                              makeKey("ssh-ed25519", static_cast<char>(t * 16 + i + 1)));
                if (!store.remember(info).has_value())
                    ++failures;
                if (store.classify(info).status != HostKeyStatus::Match)
                    ++failures;
                store.trustForSession(info);
            }
        });
    }
    for (std::thread &thread : threads)
        thread.join();
    QCOMPARE(failures.load(), 0);
    QCOMPARE(readFile(path).count('\n'), kThreads * kPerThread);
}

QTEST_GUILESS_MAIN(TstKnownHostsStore)
#include "tst_known_hosts_store.moc"
