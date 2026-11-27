#pragma once

#include <quarry/migration_source.h>

namespace quarry {

// Laravel: one `<yyyy_mm_dd_hhmmss>_<name>.php` file per migration in a folder (not recursive,
// like `php artisan migrate`), and a table (`migrations` by default) with the columns
// `migration` (file name without .php) and `batch`.
class LaravelMigrationSource final : public IMigrationSource
{
public:
    QString id() const override { return QStringLiteral("laravel"); }
    QString displayName() const override;
    bool usesTable() const override { return true; }
    QString defaultTable() const override { return QStringLiteral("migrations"); }

    std::optional<Error> validate(const MigrationSettings &settings) const override;
    Result<QStringList> scanFolder(const QString &folder,
                                   const CancellationToken &cancel) const override;
    QStringList filterFileNames(const QStringList &fileNames) const override;
    Result<Statement> appliedQuery(const MigrationSettings &settings, const ISqlDialect *dialect,
                                   const QString &providerId) const override;
    MigrationReport reconcile(const QStringList &files,
                              const std::vector<AppliedMigration> &applied) const override;
    QString filePath(const MigrationSettings &settings, const QString &name) const override;
    QString highlightingDefinition() const override { return QStringLiteral("PHP"); }
};

// Shared by the sources whose files and rows match by name: one entry per name, sorted by name
// (the date prefix makes that chronological); a duplicated row counts once.
MigrationReport reconcileByName(const QStringList &files,
                                const std::vector<AppliedMigration> &applied);
// Missing, relative, not a folder or not readable -> clear error; else nullopt. Touches the disk.
std::optional<Error> checkMigrationFolder(const QString &folder);
// Plain identifier or schema.identifier (letters, digits, underscore), so it can go unquoted
// into the SQL: no injection, and each engine folds the case as Laravel's own queries do.
bool isPlainTableName(const QString &table);

} // namespace quarry
