#pragma once

#include "postgres_state.h"

#include <quarry/provider_api.h>

#include <memory>
#include <optional>
#include <vector>

namespace quarry::postgres {

class PostgresStream : public IResultStream
{
public:
    // Reads the first result of a command already sent with PQsendQueryParams and chunked
    // rows mode. The caller holds a CallScope. Fails with the server error, if any.
    static Result<std::unique_ptr<PostgresStream>> create(std::shared_ptr<PgState> state,
                                                          const QString &sql);
    ~PostgresStream() override;
    PostgresStream(const PostgresStream &) = delete;
    PostgresStream &operator=(const PostgresStream &) = delete;

    const std::vector<ColumnInfo> &columns() const override { return m_columns; }
    Result<ResultBatch> fetch(qsizetype maxRows, const CancellationToken &token) override;
    bool atEnd() const override { return m_finished && !m_current; }
    std::optional<qint64> affectedRows() const override { return m_affected; }
    QStringList notices() const override { return m_notices; }
    Result<bool> nextResult(const CancellationToken &) override { return false; }

private:
    PostgresStream(std::shared_ptr<PgState> state, QString sql);

    // Takes ownership of `result` (the next one from the server, null at the end).
    Result<void> accept(PGresult *result);
    void appendRow(ResultBatch &batch, int row) const;
    void noteAffected(const PGresult *result);
    void takeNotices();
    // Drops what is left of the command; with `cancelServer` the server is asked to stop first.
    void discard(bool cancelServer);
    Result<ResultBatch> abort(Error error, bool cancelServer);

    std::shared_ptr<PgState> m_state;
    QString m_sql;
    std::vector<ColumnInfo> m_columns;
    std::vector<ValueType> m_types;
    PGresult *m_current = nullptr; // rows not yet handed out
    int m_row = 0; // next row of m_current
    bool m_finished = false; // the server has nothing more to send
    std::optional<qint64> m_affected;
    QStringList m_notices;
};

} // namespace quarry::postgres
