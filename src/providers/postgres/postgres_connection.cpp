#include "postgres_connection.h"

#include "postgres_errors.h"
#include "postgres_stream.h"
#include "postgres_support.h"
#include "postgres_text.h"

#include <QDate>
#include <QDateTime>
#include <QUuid>

namespace quarry::postgres {

namespace {

// Text form of a bound value; a null QByteArray stays NULL on the wire.
QByteArray paramText(const QVariant &value)
{
    if (value.isNull())
        return {};
    switch (value.metaType().id()) {
    case QMetaType::Bool:
        return value.toBool() ? "t" : "f";
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::Long:
    case QMetaType::LongLong:
    case QMetaType::Short:
    case QMetaType::UShort:
    case QMetaType::Char:
    case QMetaType::SChar:
    case QMetaType::UChar:
        return QByteArray::number(value.toLongLong());
    case QMetaType::ULong:
    case QMetaType::ULongLong:
        return QByteArray::number(value.toULongLong());
    case QMetaType::Double:
    case QMetaType::Float:
        return QByteArray::number(value.toDouble(), 'g', 17);
    case QMetaType::QByteArray:
        return "\\x" + value.toByteArray().toHex();
    case QMetaType::QDateTime:
        return value.toDateTime().toString(Qt::ISODateWithMs).toUtf8();
    case QMetaType::QDate:
        return value.toDate().toString(Qt::ISODate).toUtf8();
    case QMetaType::QUuid:
        return value.toUuid().toString(QUuid::WithoutBraces).toUtf8();
    default:
        return value.toString().toUtf8();
    }
}

void noticeReceiver(void *arg, const PGresult *result)
{
    auto *state = static_cast<PgState *>(arg);
    const char *severity = PQresultErrorField(result, PG_DIAG_SEVERITY_NONLOCALIZED);
    const char *message = PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY);
    if (!message)
        return;
    state->notices.push_back(
        (severity ? QString::fromUtf8(severity) + QLatin1String(": ") : QString())
        + QString::fromUtf8(message));
}

} // namespace

PostgresConnection::PostgresConnection(ConnectionConfig config, QString password)
    : m_config(std::move(config))
    , m_password(std::move(password))
    , m_state(std::make_shared<PgState>())
{ }

PostgresConnection::~PostgresConnection()
{
    closeConnection();
}

bool PostgresConnection::isOpen() const
{
    return m_state->conn && PQstatus(m_state->conn) == CONNECTION_OK;
}

Result<ServerInfo> PostgresConnection::open(const CancellationToken &token)
{
    CallScope scope(*m_state);
    if (m_state->conn)
        return fail(
            Error { ErrorKind::Internal, Text::tr("The connection is already open."), {}, {}, {} });
    if (token.isCancelled())
        return fail(cancelledError());

    auto params = buildConnParams(m_config.values, m_password);
    if (!params)
        return fail(std::move(params).error());
    std::vector<const char *> keywords;
    std::vector<const char *> values;
    keywords.reserve(params.value().size() + 1);
    values.reserve(params.value().size() + 1);
    for (const auto &[key, value] : params.value()) {
        keywords.push_back(key.constData());
        values.push_back(value.constData());
    }
    keywords.push_back(nullptr);
    values.push_back(nullptr);

    PGconn *conn = PQconnectdbParams(keywords.data(), values.data(), 0);
    if (!conn)
        return fail(Error {
            ErrorKind::Internal, Text::tr("Could not prepare the connection."), {}, {}, {} });
    if (PQstatus(conn) != CONNECTION_OK) {
        Error error = connectError(QString::fromUtf8(PQerrorMessage(conn)));
        PQfinish(conn);
        return fail(std::move(error));
    }
    PQsetNoticeReceiver(conn, &noticeReceiver, m_state.get());
    {
        std::lock_guard lock(m_state->mutex);
        m_state->conn = conn;
    }
    const char *version = PQparameterStatus(conn, "server_version");
    return ServerInfo { QStringLiteral("PostgreSQL"),
                        version ? QString::fromLatin1(version) : QString() };
}

void PostgresConnection::close()
{
    closeConnection();
}

void PostgresConnection::closeConnection()
{
    m_state->closeConnection();
}

Result<void> PostgresConnection::exec(const char *sql)
{
    CallScope scope(*m_state);
    PGconn *conn = m_state->conn;
    if (!conn)
        return fail(notOpenError());
    PGresult *result = PQexec(conn, sql);
    if (PQresultStatus(result) != PGRES_COMMAND_OK) {
        Error error = errorFromResult(result, conn, QString::fromUtf8(sql));
        PQclear(result);
        return fail(std::move(error));
    }
    PQclear(result);
    return {};
}

Result<PostgresConnection::Rows> PostgresConnection::queryRows(const QString &sql,
                                                               const QStringList &args)
{
    PGconn *conn = m_state->conn;
    const QByteArray utf8 = sql.toUtf8();
    std::vector<QByteArray> storage;
    std::vector<const char *> values;
    storage.reserve(static_cast<size_t>(args.size()));
    values.reserve(static_cast<size_t>(args.size()));
    for (const QString &arg : args)
        storage.push_back(arg.toUtf8());
    for (const QByteArray &arg : storage)
        values.push_back(arg.constData());
    PGresult *result = PQexecParams(conn, utf8.constData(), static_cast<int>(values.size()),
                                    nullptr, values.data(), nullptr, nullptr, 0);
    if (PQresultStatus(result) != PGRES_TUPLES_OK) {
        Error error = errorFromResult(result, conn, sql);
        PQclear(result);
        return fail(std::move(error));
    }
    Rows rows;
    const int count = PQntuples(result);
    const int columns = PQnfields(result);
    rows.reserve(static_cast<size_t>(count));
    for (int r = 0; r < count; ++r) {
        QVariantList row;
        row.reserve(columns);
        for (int c = 0; c < columns; ++c) {
            row.push_back(PQgetisnull(result, r, c)
                              ? QVariant()
                              : QVariant(QString::fromUtf8(PQgetvalue(result, r, c),
                                                           PQgetlength(result, r, c))));
        }
        rows.push_back(std::move(row));
    }
    PQclear(result);
    return rows;
}

Result<std::unique_ptr<IResultStream>> PostgresConnection::execute(const Statement &statement,
                                                                   const CancellationToken &token)
{
    CallScope scope(*m_state);
    PGconn *conn = m_state->conn;
    if (!conn)
        return fail(notOpenError());
    if (token.isCancelled())
        return fail(cancelledError());

    const QByteArray sql = statement.sql.toUtf8();
    std::vector<QByteArray> storage;
    storage.reserve(static_cast<size_t>(statement.parameters.size()));
    for (const QVariant &parameter : statement.parameters)
        storage.push_back(paramText(parameter));
    std::vector<const char *> values;
    values.reserve(storage.size());
    for (size_t i = 0; i < storage.size(); ++i)
        values.push_back(statement.parameters.at(static_cast<qsizetype>(i)).isNull()
                             ? nullptr
                             : storage[i].constData());

    if (!PQsendQueryParams(conn, sql.constData(), static_cast<int>(values.size()), nullptr,
                           values.data(), nullptr, nullptr, 0))
        return fail(sendError(conn));
    if (!PQsetChunkedRowsMode(conn, statement.fetchSize > 0 ? statement.fetchSize : 500)) {
        discardResults(conn);
        return fail(
            Error { ErrorKind::Internal, Text::tr("Could not start streaming."), {}, {}, {} });
    }
    // A cancel that arrived before the statement reached the server was a no-op there.
    if (m_state->cancel.load(std::memory_order_acquire))
        m_state->sendCancel();

    auto stream = PostgresStream::create(m_state, statement.sql);
    if (!stream)
        return fail(std::move(stream).error());
    return std::unique_ptr<IResultStream>(std::move(stream).value());
}

} // namespace quarry::postgres
