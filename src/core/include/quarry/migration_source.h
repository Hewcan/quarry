#pragma once

#include <quarry/cancellation.h>
#include <quarry/provider_api.h>
#include <quarry/result.h>

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <optional>
#include <vector>

namespace quarry {

// Local, per-connection link to the migrations of an external project (ARCHITECTURE §6.11).
// Lives in its own table of quarry.db, never in ConnectionProfile, so it never travels with a
// connection. An empty `kind` means "no project".
struct MigrationSettings
{
    QUuid connectionId;
    QString kind; // IMigrationSource::id(): "laravel"; empty = none
    QString folder; // absolute path of the folder with the migration files
    QString table; // empty = the source's default table

    bool isEnabled() const { return !kind.isEmpty(); }
    friend bool operator==(const MigrationSettings &, const MigrationSettings &) = default;
};

enum class MigrationState : uint8_t {
    Pending, // file without a row in the table
    Applied, // file and row
    Orphan, // row whose file no longer exists
};

// One row of the migrations table.
struct AppliedMigration
{
    QString name;
    std::optional<qint64> batch;
};

struct MigrationEntry
{
    QString name; // file name without extension, as stored in the table
    MigrationState state = MigrationState::Pending;
    std::optional<qint64> batch; // Applied and Orphan
};

struct MigrationReport
{
    std::vector<MigrationEntry> entries; // chronological (date prefix of the name)
    int pending = 0;
    int applied = 0;
    int orphans = 0;
    int files = 0; // migration files found in the folder
    // The migrations table does not exist: nothing is applied, so every file is pending.
    bool tableMissing = false;
};

// Adapter for one migration system (Laravel, and later Django, Rails, Flyway…). Read-only by
// contract: it lists file names and builds one SELECT; it never runs, reverts or edits a
// migration and never executes project code. Implementations are stateless and thread-safe.
class IMigrationSource
{
public:
    virtual ~IMigrationSource() = default;

    virtual QString id() const = 0; // stored in MigrationSettings::kind
    virtual QString displayName() const = 0; // translated
    // Whether the settings carry a table name, and its default.
    virtual bool usesTable() const = 0;
    virtual QString defaultTable() const = 0;

    // Cheap checks without touching the disk (empty or relative folder, bad table name).
    virtual std::optional<Error> validate(const MigrationSettings &settings) const = 0;
    // Worker thread. Names (without extension) of the migration files in `folder`, sorted.
    // Fails with a clear message if the folder is missing, not a folder or not readable.
    virtual Result<QStringList> scanFolder(const QString &folder,
                                           const CancellationToken &cancel) const = 0;
    // The SELECT that reads the applied migrations: column 0 = name, column 1 = batch.
    virtual Result<Statement> appliedQuery(const MigrationSettings &settings) const = 0;
    // Crosses the files with the rows of the table.
    virtual MigrationReport reconcile(const QStringList &files,
                                      const std::vector<AppliedMigration> &applied) const = 0;
};

// The built-in sources, in display order. Adding a framework = one more implementation here.
QList<const IMigrationSource *> migrationSources();
// nullptr for an unknown (or empty) id.
const IMigrationSource *findMigrationSource(QStringView id);

} // namespace quarry

Q_DECLARE_METATYPE(quarry::MigrationSettings)
Q_DECLARE_METATYPE(quarry::MigrationReport)
