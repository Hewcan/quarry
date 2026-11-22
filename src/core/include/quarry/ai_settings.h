#pragma once

#include <quarry/result.h>
// Declares the Q_DECLARE_METATYPE for quarry::Error used by the signals.
#include <quarry/session.h>

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QUuid>

namespace quarry {

// Per-connection settings of the Claude Code integration (H13 phase B, ARCHITECTURE §6.13). Lives
// in its own table of quarry.db (`ai_settings`, migration 8), never in ConnectionProfile, so it
// never travels with an exported connection. A connection without a row has the defaults below:
// the AI is OFF everywhere until the user turns it on for that connection, and nothing about
// the database leaves the machine through the app in any case (the CLI is the user's own). The
// field names match quarry::ai::ContextOptions (core does not link quarry_ai: the app maps one to
// the other).
struct AiConnectionSettings
{
    QUuid connectionId;
    bool enabled = false;
    QStringList excludedSchemas;
    QStringList excludedTables; // "table" or "schema.table"
    bool sendComments = false;
    bool sendDefaults = false;
    bool sendChecks = false;
    bool sendEnumValues = false;
    bool sendDefinitions = false;
    bool maskLiterals = true; // string literals of the script become __V1__ ... before sending
    bool maskNumbers = false;
    int maxTables = 40; // 1..200

    // Equal to what a connection without a row has (a row of defaults is not stored).
    bool isDefault() const;
    friend bool operator==(const AiConnectionSettings &, const AiConnectionSettings &) = default;
};

// UI-thread facade for `ai_settings`, with the same shape as MigrationService: one worker thread
// for quarry.db, every method returns at once and the outcome arrives as a queued signal.
class AiSettingsService : public QObject
{
    Q_OBJECT

public:
    explicit AiSettingsService(QString databasePath, QObject *parent = nullptr);
    // Finishes the queued disk work and joins the thread (up to 5 s).
    ~AiSettingsService() override;

    // -> loaded(every stored row) | failed(Error)
    void loadAll();
    // Inserts or replaces the row of settings.connectionId; a default one deletes the row. The
    // connection must exist in quarry.db; maxTables is clamped to 1..200 and the lists are
    // trimmed, de-duplicated (case-insensitive) and stripped of empty entries before storing.
    // -> saved(the settings as stored) | failed(Error)
    void save(const quarry::AiConnectionSettings &settings);

    static AiConnectionSettings normalized(AiConnectionSettings settings);

signals:
    void loaded(QList<quarry::AiConnectionSettings> settings);
    void saved(quarry::AiConnectionSettings settings);
    void failed(quarry::Error error);

private:
    QThread *m_thread = nullptr;
    QObject *m_worker = nullptr; // lives on m_thread, deleted there
};

} // namespace quarry

Q_DECLARE_METATYPE(quarry::AiConnectionSettings)
Q_DECLARE_METATYPE(QList<quarry::AiConnectionSettings>)
