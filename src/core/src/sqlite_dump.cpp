#include "sqlite_dump.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

namespace quarry::sqlite_dump {

namespace {

QString trText(const char *text)
{
    return QCoreApplication::translate("quarry::DumpRunner", text);
}

Error makeError(ErrorKind kind, QString message, QString detail = {})
{
    return Error { kind, std::move(message), std::move(detail), {}, {} };
}

Error cancelledError()
{
    return makeError(ErrorKind::Cancelled, trText("Cancelled."));
}

struct DbCloser
{
    void operator()(sqlite3 *db) const { sqlite3_close_v2(db); }
};
struct StmtFinalizer
{
    void operator()(sqlite3_stmt *stmt) const { sqlite3_finalize(stmt); }
};
using Db = std::unique_ptr<sqlite3, DbCloser>;
using Stmt = std::unique_ptr<sqlite3_stmt, StmtFinalizer>;

QString lastError(sqlite3 *db)
{
    return QString::fromUtf8(sqlite3_errmsg(db));
}

QByteArray quoteIdentifier(const QByteArray &name)
{
    QByteArray escaped = name;
    escaped.replace('"', "\"\"");
    return '"' + escaped + '"';
}

QByteArray quoteText(const QByteArray &text)
{
    if (text.contains('\0'))
        return "CAST(X'" + text.toHex().toUpper() + "' AS TEXT)";
    QByteArray escaped = text;
    escaped.replace('\'', "''");
    return '\'' + escaped + '\'';
}

QByteArray formatReal(double value)
{
    if (std::isnan(value))
        return "NULL";
    if (std::isinf(value))
        return value > 0 ? "1e999" : "-1e999";
    QByteArray text = QByteArray::number(value, 'g', 17);
    // Without a dot or an exponent it would come back as an INTEGER in a column without affinity.
    if (!text.contains('.') && !text.contains('e'))
        text += ".0";
    return text;
}

QByteArray formatValue(sqlite3_stmt *stmt, int column)
{
    switch (sqlite3_column_type(stmt, column)) {
    case SQLITE_INTEGER:
        return QByteArray::number(sqlite3_column_int64(stmt, column));
    case SQLITE_FLOAT:
        return formatReal(sqlite3_column_double(stmt, column));
    case SQLITE_TEXT: {
        const auto *data = reinterpret_cast<const char *>(sqlite3_column_text(stmt, column));
        return quoteText(QByteArray(data, sqlite3_column_bytes(stmt, column)));
    }
    case SQLITE_BLOB: {
        const auto *data = static_cast<const char *>(sqlite3_column_blob(stmt, column));
        const int size = sqlite3_column_bytes(stmt, column);
        return "X'" + QByteArray(data, size).toHex().toUpper() + '\'';
    }
    default:
        return "NULL";
    }
}

struct SchemaObject
{
    QByteArray type;
    QByteArray name;
    QByteArray table;
    QByteArray sql;
    bool isVirtual = false;
};

class Writer
{
public:
    explicit Writer(QSaveFile &file)
        : m_file(file)
    { }
    void add(const QByteArray &text)
    {
        m_buffer += text;
        if (m_buffer.size() >= 1024 * 1024)
            flush();
    }
    bool flush()
    {
        if (!m_buffer.isEmpty() && m_file.write(m_buffer) != m_buffer.size())
            m_ok = false;
        m_buffer.clear();
        return m_ok;
    }
    bool ok() const { return m_ok; }

private:
    QSaveFile &m_file;
    QByteArray m_buffer;
    bool m_ok = true;
};

Result<qint64> dumpRows(sqlite3 *db, const SchemaObject &table, Writer &writer,
                        CancelToken &cancel)
{
    const QByteArray quoted = quoteIdentifier(table.name);
    sqlite3_stmt *raw = nullptr;
    // Generated and hidden columns cannot be inserted into: list the rest.
    if (sqlite3_prepare_v2(db, "PRAGMA main.table_xinfo(" + quoted + ")", -1, &raw, nullptr)
        != SQLITE_OK)
        return fail(makeError(ErrorKind::Driver, trText("The table could not be read."),
                              lastError(db)));
    Stmt info(raw);
    QList<QByteArray> columns;
    while (sqlite3_step(info.get()) == SQLITE_ROW) {
        if (sqlite3_column_int(info.get(), 6) != 0)
            continue;
        columns << quoteIdentifier(
                reinterpret_cast<const char *>(sqlite3_column_text(info.get(), 1)));
    }
    if (columns.isEmpty())
        return qint64(0);
    const QByteArray list = columns.join(',');
    if (sqlite3_prepare_v2(db, "SELECT " + list + " FROM main." + quoted, -1, &raw, nullptr)
        != SQLITE_OK)
        return fail(makeError(ErrorKind::Driver, trText("The table could not be read."),
                              lastError(db)));
    Stmt rows(raw);
    const QByteArray prefix = "INSERT INTO " + quoted + '(' + list + ") VALUES(";
    const int count = static_cast<int>(columns.size());
    qint64 done = 0;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(rows.get())) == SQLITE_ROW) {
        QByteArray line = prefix;
        for (int i = 0; i < count; ++i) {
            if (i > 0)
                line += ',';
            line += formatValue(rows.get(), i);
        }
        line += ");\n";
        writer.add(line);
        if (++done % 1000 == 0 && (cancel.isCancelled() || !writer.ok()))
            break;
    }
    if (cancel.isCancelled())
        return fail(cancelledError());
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        return fail(makeError(ErrorKind::Driver, trText("The table could not be read."),
                              lastError(db)));
    return done;
}

} // namespace

void CancelToken::cancel()
{
    m_cancelled.store(true);
    const QMutexLocker lock(&m_mutex);
    if (m_db != nullptr)
        sqlite3_interrupt(m_db);
}

void CancelToken::setDatabase(sqlite3 *db)
{
    const QMutexLocker lock(&m_mutex);
    m_db = db;
}

Result<void> dump(const QString &databaseFile, const DumpOptions &options, CancelToken &cancel,
                  const Callbacks &callbacks)
{
    if (!QFileInfo(databaseFile).isFile())
        return fail(makeError(ErrorKind::NotFound, trText("The database file does not exist."),
                              databaseFile));
    sqlite3 *raw = nullptr;
    const int openRc = sqlite3_open_v2(databaseFile.toUtf8().constData(), &raw,
                                       SQLITE_OPEN_READONLY, nullptr);
    Db db(raw);
    if (openRc != SQLITE_OK)
        return fail(makeError(ErrorKind::Connection, trText("The database could not be opened."),
                              raw != nullptr ? lastError(raw) : QString()));
    cancel.setDatabase(db.get());
    const auto clearDb = qScopeGuard([&cancel] { cancel.setDatabase(nullptr); });
    sqlite3_busy_timeout(db.get(), 5000);
    // One read transaction: every table comes from the same snapshot.
    sqlite3_exec(db.get(), "BEGIN", nullptr, nullptr, nullptr);

    sqlite3_stmt *stmtRaw = nullptr;
    if (sqlite3_prepare_v2(db.get(),
                           "SELECT type, name, tbl_name, sql FROM main.sqlite_master "
                           "WHERE sql IS NOT NULL ORDER BY rowid",
                           -1, &stmtRaw, nullptr)
        != SQLITE_OK)
        return fail(makeError(ErrorKind::Driver, trText("The database could not be read."),
                              lastError(db.get())));
    QList<SchemaObject> objects;
    bool hasSequence = false;
    {
        Stmt schema(stmtRaw);
        while (sqlite3_step(schema.get()) == SQLITE_ROW) {
            SchemaObject object;
            const auto column = [&schema](int i) {
                return QByteArray(
                        reinterpret_cast<const char *>(sqlite3_column_text(schema.get(), i)));
            };
            object.type = column(0);
            object.name = column(1);
            object.table = column(2);
            object.sql = column(3);
            if (object.name == "sqlite_sequence") {
                hasSequence = true;
                continue;
            }
            if (object.name.startsWith("sqlite_"))
                continue;
            object.isVirtual = object.sql.trimmed().left(20).toUpper() == "CREATE VIRTUAL TABLE";
            objects.append(object);
        }
    }

    QSet<QByteArray> wanted;
    for (const DumpTable &table : options.tables)
        wanted.insert(table.name.toUtf8().toLower());
    QSet<QByteArray> virtualNames;
    for (const SchemaObject &object : std::as_const(objects)) {
        if (object.isVirtual)
            virtualNames.insert(object.name.toLower());
    }
    // shortcut: the shadow tables of a virtual table (FTS and the like) are left out and the
    // virtual table is dumped without rows, so its content is lost; the shell's .dump writes
    // sqlite_master directly. Mejorar si alguien usa FTS en una base que vuelca.
    const auto isShadow = [&virtualNames](const QByteArray &name) {
        const qsizetype underscore = name.lastIndexOf('_');
        return underscore > 0 && virtualNames.contains(name.left(underscore).toLower());
    };
    const auto selected = [&](const QByteArray &table) {
        return wanted.isEmpty() || wanted.contains(table.toLower());
    };

    QList<SchemaObject> tables;
    QList<SchemaObject> rest;
    QSet<QByteArray> dumped;
    for (const SchemaObject &object : std::as_const(objects)) {
        if (object.type == "table") {
            if (!selected(object.name) || isShadow(object.name))
                continue;
            tables.append(object);
            dumped.insert(object.name.toLower());
        } else if (object.type == "view") {
            // A view may read any table: with a selection it is left out.
            if (wanted.isEmpty())
                rest.append(object);
        } else if (dumped.contains(object.table.toLower()) || selected(object.table)) {
            rest.append(object);
        }
    }
    for (const QByteArray &name : std::as_const(wanted)) {
        if (!dumped.contains(name))
            return fail(makeError(ErrorKind::NotFound, trText("A selected table does not exist."),
                                  QString::fromUtf8(name)));
    }

    QSaveFile file(options.outputPath);
    if (!file.open(QIODevice::WriteOnly))
        return fail(makeError(ErrorKind::Permission, trText("The dump file could not be created."),
                              file.errorString()));
    Writer writer(file);
    writer.add("PRAGMA foreign_keys=OFF;\nBEGIN TRANSACTION;\n");
    const bool withSchema = options.content != DumpContent::DataOnly;
    const bool withData = options.content != DumpContent::SchemaOnly;
    // Data only goes into a database that already has the schema: its triggers would fire on
    // every INSERT (SQLite cannot turn them off), so they are dropped and re-created around the
    // rows, inside the same transaction. Like pg_dump --disable-triggers.
    QList<SchemaObject> triggers;
    if (!withSchema) {
        for (const SchemaObject &object : std::as_const(rest)) {
            if (object.type == "trigger") {
                triggers.append(object);
                writer.add("DROP TRIGGER IF EXISTS " + quoteIdentifier(object.name) + ";\n");
            }
        }
    }
    const int total = static_cast<int>(tables.size()) + 1;
    for (int i = 0; i < tables.size(); ++i) {
        const SchemaObject &table = tables.at(i);
        const QString name = QString::fromUtf8(table.name);
        if (callbacks.progress)
            callbacks.progress(i, total, name);
        if (withSchema)
            writer.add(table.sql + ";\n");
        if (withData && !table.isVirtual) {
            const Result<qint64> rows = dumpRows(db.get(), table, writer, cancel);
            if (!rows)
                return fail(rows.error());
            if (callbacks.log)
                callbacks.log(trText("%1: %2 rows").arg(name).arg(*rows));
        }
        if (cancel.isCancelled())
            return fail(cancelledError());
        if (!writer.ok())
            break;
    }
    if (callbacks.progress)
        callbacks.progress(total - 1, total, trText("Indexes, triggers and views"));
    if (withSchema) {
        for (const SchemaObject &object : std::as_const(rest))
            writer.add(object.sql + ";\n");
    }
    for (const SchemaObject &trigger : std::as_const(triggers))
        writer.add(trigger.sql + ";\n");
    if (withData && hasSequence) {
        if (sqlite3_prepare_v2(db.get(), "SELECT name, seq FROM main.sqlite_sequence", -1,
                               &stmtRaw, nullptr)
            == SQLITE_OK) {
            Stmt sequence(stmtRaw);
            while (sqlite3_step(sequence.get()) == SQLITE_ROW) {
                const QByteArray name(
                        reinterpret_cast<const char *>(sqlite3_column_text(sequence.get(), 0)));
                if (!dumped.contains(name.toLower()))
                    continue;
                const QByteArray seq = QByteArray::number(sqlite3_column_int64(sequence.get(), 1));
                writer.add("DELETE FROM sqlite_sequence WHERE name=" + quoteText(name) + ";\n"
                           "INSERT INTO sqlite_sequence(name,seq) VALUES(" + quoteText(name) + ','
                           + seq + ");\n");
            }
        }
    }
    writer.add("COMMIT;\n");
    if (!writer.flush() || !file.commit())
        return fail(makeError(ErrorKind::Permission, trText("The dump file could not be written."),
                              file.errorString()));
    if (callbacks.progress)
        callbacks.progress(total, total, {});
    return {};
}

Result<void> restore(const QString &databaseFile, const RestoreOptions &options,
                     CancelToken &cancel, const Callbacks &callbacks)
{
    QFile input(options.inputPath);
    if (!input.open(QIODevice::ReadOnly))
        return fail(makeError(ErrorKind::NotFound, trText("The dump file could not be opened."),
                              input.errorString()));
    if (input.peek(5) == QByteArrayLiteral("PGDMP"))
        return fail(makeError(ErrorKind::Unsupported,
                              trText("This file is a PostgreSQL archive: it can only be restored "
                                     "into PostgreSQL.")));
    if (databaseFile.isEmpty())
        return fail(makeError(ErrorKind::NotFound, trText("The connection has no database file.")));

    sqlite3 *raw = nullptr;
    const int openRc = sqlite3_open_v2(databaseFile.toUtf8().constData(), &raw,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    Db db(raw);
    if (openRc != SQLITE_OK)
        return fail(makeError(ErrorKind::Connection, trText("The database could not be opened."),
                              raw != nullptr ? lastError(raw) : QString()));
    cancel.setDatabase(db.get());
    const auto clearDb = qScopeGuard([&cancel] { cancel.setDatabase(nullptr); });
    sqlite3_busy_timeout(db.get(), 5000);

    const qint64 size = std::max<qint64>(1, input.size());
    qint64 lineNumber = 0;
    qint64 statementLine = 1;
    int lastPermille = -1;
    QByteArray buffer;

    const auto failWith = [&](Error error) -> Result<void> {
        const bool open = sqlite3_get_autocommit(db.get()) == 0;
        if (open)
            sqlite3_exec(db.get(), "ROLLBACK", nullptr, nullptr, nullptr);
        if (error.kind != ErrorKind::Cancelled) {
            error.message = open ? trText("The script failed and was rolled back: the database "
                                          "is as it was before.")
                                 : trText("The script failed partway: what ran before the error "
                                          "stays in the database.");
        }
        return fail(std::move(error));
    };
    const auto run = [&]() -> std::optional<Error> {
        const char *tail = buffer.constData();
        while (*tail != '\0') {
            if (cancel.isCancelled())
                return cancelledError();
            sqlite3_stmt *stmt = nullptr;
            const char *next = nullptr;
            int rc = sqlite3_prepare_v2(db.get(), tail, -1, &stmt, &next);
            Stmt owned(stmt);
            if (rc == SQLITE_OK && stmt != nullptr) {
                while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) { }
                if (rc == SQLITE_DONE)
                    rc = SQLITE_OK;
            }
            if (rc != SQLITE_OK) {
                if (rc == SQLITE_INTERRUPT || cancel.isCancelled())
                    return cancelledError();
                return makeError(ErrorKind::Driver, {},
                                 trText("Line %1: %2").arg(statementLine).arg(lastError(db.get())));
            }
            tail = next;
        }
        return std::nullopt;
    };

    while (!input.atEnd()) {
        const QByteArray line = input.readLine();
        ++lineNumber;
        if (buffer.isEmpty())
            statementLine = lineNumber;
        buffer += line;
        if (sqlite3_complete(buffer.constData()) == 0)
            continue;
        if (const std::optional<Error> error = run())
            return failWith(*error);
        buffer.clear();
        const int permille = static_cast<int>(input.pos() * 1000 / size);
        if (callbacks.progress && permille != lastPermille) {
            lastPermille = permille;
            callbacks.progress(permille, 1000, {});
        }
    }
    if (!buffer.trimmed().isEmpty()) {
        if (const std::optional<Error> error = run())
            return failWith(*error);
    }
    if (cancel.isCancelled())
        return failWith(cancelledError());
    if (sqlite3_get_autocommit(db.get()) == 0) {
        // A script that opened a transaction and never closed it: keep nothing half done.
        return failWith(makeError(ErrorKind::Driver, {},
                                  trText("The script ended without COMMIT.")));
    }
    if (callbacks.progress)
        callbacks.progress(1000, 1000, {});
    return {};
}

} // namespace quarry::sqlite_dump
