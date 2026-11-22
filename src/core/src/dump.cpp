#include <quarry/dump.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <QVersionNumber>

#include <algorithm>

namespace quarry {

namespace {

QString trText(const char *text)
{
    return QCoreApplication::translate("quarry::DumpRunner", text);
}

QString toolName(DumpTool tool)
{
    switch (tool) {
    case DumpTool::PgDump:
        return QStringLiteral("pg_dump");
    case DumpTool::PgRestore:
        return QStringLiteral("pg_restore");
    case DumpTool::Psql:
        return QStringLiteral("psql");
    }
    return {};
}

QString executableName(const QString &name)
{
#ifdef Q_OS_WIN
    return name + QStringLiteral(".exe");
#else
    return name;
#endif
}

bool isRunnableFile(const QFileInfo &info)
{
    return info.isFile() && info.isExecutable();
}

// Folders like ".../PostgreSQL/16/bin", newest version first.
QStringList versionedBins(const QString &parent, const QString &prefix, const QString &suffix)
{
    QDir dir(parent);
    if (!dir.exists())
        return {};
    QList<std::pair<QVersionNumber, QString>> found;
    const QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        if (!entry.startsWith(prefix))
            continue;
        const QVersionNumber version = QVersionNumber::fromString(entry.mid(prefix.size()));
        if (version.isNull())
            continue;
        found.append({ version, dir.filePath(entry) + suffix });
    }
    std::sort(found.begin(), found.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });
    QStringList out;
    for (const auto &entry : std::as_const(found))
        out << entry.second;
    return out;
}

QStringList candidateFolders()
{
    QStringList out;
#if defined(Q_OS_WIN)
    for (const char *variable : { "ProgramFiles", "ProgramW6432", "ProgramFiles(x86)" }) {
        const QString root = qEnvironmentVariable(variable);
        if (root.isEmpty())
            continue;
        out << versionedBins(root + QStringLiteral("/PostgreSQL"), QString(),
                             QStringLiteral("/bin"));
        out << root + QStringLiteral("/pgAdmin 4/runtime");
    }
#elif defined(Q_OS_MACOS)
    out << QStringLiteral("/opt/homebrew/opt/libpq/bin")
        << QStringLiteral("/usr/local/opt/libpq/bin")
        << QStringLiteral("/Applications/Postgres.app/Contents/Versions/latest/bin")
        << QStringLiteral("/opt/homebrew/bin") << QStringLiteral("/usr/local/bin");
#else
    out << versionedBins(QStringLiteral("/usr/lib/postgresql"), QString(), QStringLiteral("/bin"));
    out << versionedBins(QStringLiteral("/usr"), QStringLiteral("pgsql-"), QStringLiteral("/bin"));
    out << QStringLiteral("/usr/bin") << QStringLiteral("/usr/local/bin");
#endif
    out.removeDuplicates();
    return out;
}

// A conninfo value in single quotes, with backslash and quote escaped (libpq's rules).
QString conninfoValue(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('\''), QStringLiteral("\\'"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

// Everything but the password, as one --dbname argument: a database name with "=" or a leading
// "-" can never be read as something else.
QString conninfo(const DumpTarget &target)
{
    QStringList parts;
    const auto add = [&parts](const char *key, const QString &value) {
        parts << QString::fromLatin1(key) + QLatin1Char('=') + conninfoValue(value);
    };
    add("host", target.host.isEmpty() ? QStringLiteral("localhost") : target.host);
    add("port", QString::number(target.port > 0 ? target.port : 5432));
    if (!target.database.isEmpty())
        add("dbname", target.database);
    if (!target.user.isEmpty())
        add("user", target.user);
    add("sslmode", target.sslmode.isEmpty() ? QStringLiteral("prefer") : target.sslmode);
    if (!target.sslrootcert.isEmpty())
        add("sslrootcert", target.sslrootcert);
    add("connect_timeout", QStringLiteral("15"));
    add("application_name", QStringLiteral("Quarry"));
    return parts.join(QLatin1Char(' '));
}

// A pg_dump pattern that matches exactly this name: double quotes make every character literal.
QString exactPattern(const QString &name)
{
    QString escaped = name;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString pgpassField(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char(':'), QStringLiteral("\\:"));
    // A line break would end the entry: libpq has no escape for it.
    escaped.remove(QLatin1Char('\n'));
    escaped.remove(QLatin1Char('\r'));
    return escaped;
}

} // namespace

DumpTarget dumpTargetFromProfile(const ConnectionProfile &profile, const QString &password,
                                 const QString &database)
{
    const auto text = [&profile](const char *key) {
        return profile.values.value(QString::fromLatin1(key)).toString().trimmed();
    };
    DumpTarget target;
    target.providerId = profile.providerId;
    target.production = profile.production;
    target.password = password;
    if (profile.providerId == QLatin1String("sqlite")) {
        target.database = profile.values.value(QStringLiteral("file")).toString();
        return target;
    }
    target.host = text("host");
    target.port = text("port").toInt();
    target.database = database.isEmpty() ? text("database") : database;
    target.user = text("user");
    target.sslmode = text("sslmode");
    target.sslrootcert = text("sslrootcert");
    target.sshTunnel = profile.values.value(QStringLiteral("ssh.enabled")).toBool();
    return target;
}

bool dumpSupported(const QString &providerId)
{
    return providerId == QLatin1String("sqlite") || providerId == QLatin1String("postgres");
}

QList<DumpTable> dependencyClosure(const QList<DumpTable> &selected,
                                   const QList<DumpTableEdge> &edges)
{
    const auto key = [](const DumpTable &table) {
        return table.schema + QChar(0) + table.name;
    };
    QList<DumpTable> out;
    QSet<QString> seen;
    for (const DumpTable &table : selected) {
        if (!seen.contains(key(table))) {
            seen.insert(key(table));
            out.append(table);
        }
    }
    // Breadth first over the growing list: `out` is the queue.
    for (qsizetype i = 0; i < out.size(); ++i) {
        const DumpTable current = out.at(i);
        for (const DumpTableEdge &edge : edges) {
            if (!(edge.from == current) || seen.contains(key(edge.to)))
                continue;
            seen.insert(key(edge.to));
            out.append(edge.to);
        }
    }
    return out;
}

Result<QString> resolveDumpTool(DumpTool tool, const DumpToolPaths &paths)
{
    const QString name = toolName(tool);
    QString configured;
    switch (tool) {
    case DumpTool::PgDump:
        configured = paths.pgDump;
        break;
    case DumpTool::PgRestore:
        configured = paths.pgRestore;
        break;
    case DumpTool::Psql:
        configured = paths.psql;
        break;
    }
    configured = configured.trimmed();

    if (!configured.isEmpty()) {
        QFileInfo info(configured);
        if (info.isDir())
            info = QFileInfo(QDir(configured).filePath(executableName(name)));
        if (info.isAbsolute() && isRunnableFile(info))
            return QDir::cleanPath(info.absoluteFilePath());
        return fail(Error { ErrorKind::NotFound,
                            trText("%1 was not found at the path set in the dump settings.")
                                    .arg(name),
                            configured,
                            {},
                            {} });
    }

    if (QString found = QStandardPaths::findExecutable(name); !found.isEmpty())
        return found;
    const QStringList folders = candidateFolders();
    for (const QString &folder : folders) {
        const QFileInfo info(QDir(folder).filePath(executableName(name)));
        if (isRunnableFile(info))
            return QDir::cleanPath(info.absoluteFilePath());
    }
    return fail(Error { ErrorKind::NotFound,
                        trText("%1 was not found. Install the PostgreSQL client tools or set "
                               "their folder in the dump settings.")
                                .arg(name),
                        folders.join(QLatin1Char('\n')),
                        {},
                        {} });
}

QStringList pgDumpArguments(const DumpTarget &target, const DumpOptions &options,
                            const QString &outputFile)
{
    QStringList args { QStringLiteral("--dbname=") + conninfo(target),
                       QStringLiteral("--no-password"), QStringLiteral("--verbose"),
                       QStringLiteral("--file=") + outputFile,
                       options.format == DumpFormat::PostgresCustom
                               ? QStringLiteral("--format=custom")
                               : QStringLiteral("--format=plain") };
    if (options.content == DumpContent::SchemaOnly)
        args << QStringLiteral("--schema-only");
    else if (options.content == DumpContent::DataOnly)
        args << QStringLiteral("--data-only");
    if (!options.includeOwnership)
        args << QStringLiteral("--no-owner") << QStringLiteral("--no-privileges");
    for (const QString &schema : options.schemas)
        args << QStringLiteral("--schema=") + exactPattern(schema);
    for (const DumpTable &table : options.tables) {
        const QString name = table.schema.isEmpty()
                ? exactPattern(table.name)
                : exactPattern(table.schema) + QLatin1Char('.') + exactPattern(table.name);
        args << QStringLiteral("--table=") + name;
    }
    return args;
}

QStringList psqlRestoreArguments(const DumpTarget &target, const QString &inputFile)
{
    return { QStringLiteral("--dbname=") + conninfo(target), QStringLiteral("--no-password"),
             QStringLiteral("--no-psqlrc"),     QStringLiteral("--set=ON_ERROR_STOP=1"),
             QStringLiteral("--single-transaction"), QStringLiteral("--echo-errors"),
             QStringLiteral("--file=") + inputFile };
}

QStringList pgRestoreArguments(const DumpTarget &target, const QString &inputFile)
{
    return { QStringLiteral("--dbname=") + conninfo(target), QStringLiteral("--no-password"),
             QStringLiteral("--verbose"),        QStringLiteral("--exit-on-error"),
             QStringLiteral("--single-transaction"), QStringLiteral("--no-owner"),
             inputFile };
}

QByteArray pgPassFileLine(const DumpTarget &target)
{
    // The file exists only for one process of one target, so any host, port, base and user.
    return QByteArrayLiteral("*:*:*:*:") + pgpassField(target.password).toUtf8() + '\n';
}

bool isPostgresCustomArchive(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    return file.read(5) == QByteArrayLiteral("PGDMP");
}

} // namespace quarry
