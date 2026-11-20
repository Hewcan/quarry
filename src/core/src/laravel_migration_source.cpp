#include "laravel_migration_source.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace quarry {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("quarry::MigrationSource", text);
}

Error settingsError(const QString &message, const QString &detail = {})
{
    return { ErrorKind::Constraint, message, detail, {}, {} };
}

} // namespace

QList<const IMigrationSource *> migrationSources()
{
    static const LaravelMigrationSource laravel;
    return { &laravel };
}

const IMigrationSource *findMigrationSource(QStringView id)
{
    if (id.isEmpty())
        return nullptr;
    for (const IMigrationSource *source : migrationSources()) {
        if (source->id() == id)
            return source;
    }
    return nullptr;
}

bool isPlainTableName(const QString &table)
{
    static const QRegularExpression re(
        QStringLiteral("^[A-Za-z_][A-Za-z0-9_]{0,62}(\\.[A-Za-z_][A-Za-z0-9_]{0,62})?$"));
    return re.match(table).hasMatch();
}

std::optional<Error> checkMigrationFolder(const QString &folder)
{
    if (folder.trimmed().isEmpty())
        return settingsError(tr("Enter the folder that holds the migrations."));
    if (QDir::isRelativePath(folder))
        return settingsError(tr("Enter the full path of the migrations folder."), folder);
    const QFileInfo info(folder);
    if (!info.exists())
        return Error { ErrorKind::NotFound, tr("The migrations folder does not exist."), folder,
                       {}, {} };
    if (!info.isDir())
        return settingsError(tr("The migrations path is a file, not a folder."), folder);
    // QFileInfo::isReadable() is unreliable on Windows ACLs; opening the listing is the test.
    const QDir dir(folder);
    if (!info.isReadable() || !dir.isReadable())
        return Error { ErrorKind::Permission, tr("Quarry cannot read the migrations folder."),
                       folder, {}, {} };
    return std::nullopt;
}

MigrationReport reconcileByName(const QStringList &files,
                                const std::vector<AppliedMigration> &applied)
{
    QHash<QString, std::optional<qint64>> rows;
    rows.reserve(static_cast<qsizetype>(applied.size()));
    for (const AppliedMigration &row : applied) {
        if (!rows.contains(row.name))
            rows.insert(row.name, row.batch);
    }

    MigrationReport report;
    report.entries.reserve(static_cast<size_t>(files.size()) + applied.size());
    QSet<QString> seen;
    for (const QString &file : files) {
        if (seen.contains(file))
            continue;
        seen.insert(file);
        ++report.files;
        const auto row = rows.constFind(file);
        if (row == rows.cend()) {
            report.entries.push_back({ file, MigrationState::Pending, std::nullopt });
            ++report.pending;
        } else {
            report.entries.push_back({ file, MigrationState::Applied, *row });
            ++report.applied;
        }
    }
    for (auto it = rows.cbegin(); it != rows.cend(); ++it) {
        if (!seen.contains(it.key())) {
            report.entries.push_back({ it.key(), MigrationState::Orphan, it.value() });
            ++report.orphans;
        }
    }
    std::sort(report.entries.begin(), report.entries.end(),
              [](const MigrationEntry &a, const MigrationEntry &b) { return a.name < b.name; });
    return report;
}

QString LaravelMigrationSource::displayName() const
{
    return tr("Laravel");
}

std::optional<Error> LaravelMigrationSource::validate(const MigrationSettings &settings) const
{
    if (settings.folder.trimmed().isEmpty())
        return settingsError(tr("Enter the folder that holds the migrations."));
    if (QDir::isRelativePath(settings.folder))
        return settingsError(tr("Enter the full path of the migrations folder."),
                             settings.folder);
    if (!settings.table.isEmpty() && !isPlainTableName(settings.table))
        return settingsError(tr("The table name can only have letters, digits and underscores, "
                                "optionally after a schema and a dot."),
                             settings.table);
    return std::nullopt;
}

Result<QStringList> LaravelMigrationSource::scanFolder(const QString &folder,
                                                       const CancellationToken &cancel) const
{
    if (auto error = checkMigrationFolder(folder))
        return fail(*error);
    QStringList names;
    QDirIterator it(folder, { QStringLiteral("*.php") }, QDir::Files | QDir::Readable);
    while (it.hasNext()) {
        if (cancel.isCancelled())
            return fail({ ErrorKind::Cancelled, tr("Reading the migrations was cancelled."), {},
                          {}, {} });
        const QString name = it.nextFileInfo().fileName();
        names.append(name.left(name.size() - 4)); // drop ".php"
    }
    names.sort();
    return names;
}

Result<Statement> LaravelMigrationSource::appliedQuery(const MigrationSettings &settings) const
{
    const QString table = settings.table.isEmpty() ? defaultTable() : settings.table;
    if (!isPlainTableName(table))
        return fail(settingsError(tr("The table name can only have letters, digits and "
                                     "underscores, optionally after a schema and a dot."),
                                  table));
    Statement statement;
    statement.sql = QStringLiteral("SELECT migration, batch FROM %1").arg(table);
    return statement;
}

MigrationReport LaravelMigrationSource::reconcile(const QStringList &files,
                                                  const std::vector<AppliedMigration> &applied) const
{
    return reconcileByName(files, applied);
}

} // namespace quarry
