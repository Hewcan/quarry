#pragma once

#include <quarry/connection_profile.h>
#include <quarry/result.h>

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

#include <cstdint>

// Dump and restore of a whole database or part of it (H14, ARCHITECTURE §6.14). Types and pure
// helpers; DumpRunner (dump_runner.h) runs them.
namespace quarry {

enum class DumpContent : uint8_t { SchemaAndData, SchemaOnly, DataOnly };

enum class DumpFormat : uint8_t {
    PlainSql, // a .sql script: psql or Quarry's SQLite restore reads it
    PostgresCustom, // pg_dump --format=custom; only pg_restore reads it
};

// A table to dump. `schema` is empty for engines without schemas (SQLite: "main" is ignored).
struct DumpTable
{
    QString schema;
    QString name;
    friend bool operator==(const DumpTable &, const DumpTable &) = default;
};

// `from` has a foreign key that points at `to`.
struct DumpTableEdge
{
    DumpTable from;
    DumpTable to;
};

struct DumpOptions
{
    DumpContent content = DumpContent::SchemaAndData;
    DumpFormat format = DumpFormat::PlainSql; // PostgresCustom only for PostgreSQL
    QStringList schemas; // empty = all (PostgreSQL only)
    QList<DumpTable> tables; // empty = all
    QString outputPath; // written only when the dump succeeds (partial files are removed)
    bool includeOwnership = false; // PostgreSQL: false adds --no-owner --no-privileges
};

struct RestoreOptions
{
    QString inputPath; // a plain SQL script, or a PostgreSQL custom archive
};

// Where the native tools are. Each entry is empty (search the PATH and the usual install
// folders), the absolute path of the program or the folder that holds it.
struct DumpToolPaths
{
    QString pgDump;
    QString pgRestore;
    QString psql;
};

enum class DumpTool : uint8_t { PgDump, PgRestore, Psql };

// The database a dump or restore works on. Built from the profile plus the password from the
// keychain; the password only ever reaches the tool through a temporary password file.
struct DumpTarget
{
    QString providerId; // "sqlite" or "postgres"
    QString host;
    int port = 0; // 0 = the engine's default
    QString database; // the file path for sqlite
    QString user;
    QString password;
    QString sslmode;
    QString sslrootcert;
    bool production = false;
    bool sshTunnel = false; // the native tools cannot go through Quarry's tunnel
};

DumpTarget dumpTargetFromProfile(const ConnectionProfile &profile, const QString &password,
                                 const QString &database = {});

// Whether Quarry can dump and restore this engine ("sqlite", "postgres").
bool dumpSupported(const QString &providerId);

// `selected` plus every table they reach following foreign keys from the referencing side to the
// referenced one (a selected table pulls in what it points at, never what points at it),
// transitively. Order: `selected` as given, then the others in the order they are reached.
QList<DumpTable> dependencyClosure(const QList<DumpTable> &selected,
                                   const QList<DumpTableEdge> &edges);

// Finds a native tool (see DumpToolPaths). Errors say which program is missing and where to set
// it, in plain translated language. Touches the disk, starts nothing.
Result<QString> resolveDumpTool(DumpTool tool, const DumpToolPaths &paths);

// The arguments after the program. Never a password: the tools read it from PGPASSFILE.
QStringList pgDumpArguments(const DumpTarget &target, const DumpOptions &options,
                            const QString &outputFile);
QStringList psqlRestoreArguments(const DumpTarget &target, const QString &inputFile);
QStringList pgRestoreArguments(const DumpTarget &target, const QString &inputFile);

// The one line of a pgpass file for the target (fields escaped with a backslash).
QByteArray pgPassFileLine(const DumpTarget &target);

// Whether the file starts like a pg_dump custom archive ("PGDMP").
bool isPostgresCustomArchive(const QString &path);

} // namespace quarry

Q_DECLARE_METATYPE(quarry::DumpTarget)
Q_DECLARE_METATYPE(quarry::DumpOptions)
Q_DECLARE_METATYPE(quarry::RestoreOptions)
