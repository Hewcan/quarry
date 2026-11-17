#pragma once

#include "mysql_state.h"

#include <quarry/provider_api.h>

#include <memory>
#include <optional>
#include <vector>

namespace quarry::mysql {

class MysqlStream : public IResultStream
{
public:
    // Reads the response to a statement already sent with mysql_real_query: the rows come
    // through mysql_use_result, a statement without rows gives its affected-rows count. The
    // caller holds a CallScope. Fails with the server error, or with Cancelled when a cancel
    // arrived meanwhile (SLEEP() and friends return normally after KILL QUERY).
    static Result<std::unique_ptr<MysqlStream>> create(std::shared_ptr<MyState> state);
    // A stream of a statement that has no SQL (empty or comments only): nothing is sent.
    static std::unique_ptr<MysqlStream> createEmpty(std::shared_ptr<MyState> state);
    ~MysqlStream() override;
    MysqlStream(const MysqlStream &) = delete;
    MysqlStream &operator=(const MysqlStream &) = delete;

    const std::vector<ColumnInfo> &columns() const override { return m_columns; }
    Result<ResultBatch> fetch(qsizetype maxRows, const CancellationToken &token) override;
    bool atEnd() const override { return m_finished; }
    std::optional<qint64> affectedRows() const override { return m_affected; }
    QStringList notices() const override { return m_notices; }
    // shortcut: only the first result set of a CALL is read, the others are dropped; when
    // procedures matter, advance with mysql_next_result here and set MultipleResultSets.
    Result<bool> nextResult(const CancellationToken &) override { return false; }

private:
    explicit MysqlStream(std::shared_ptr<MyState> state);

    void setColumns(MYSQL_RES *result);
    void appendRow(ResultBatch &batch, MYSQL_ROW row, const unsigned long *lengths) const;
    // The server has nothing more: frees the result, reads further results, collects warnings.
    void finish();
    void takeWarnings(unsigned count);
    Result<ResultBatch> failWith(Error error);

    std::shared_ptr<MyState> m_state;
    std::vector<ColumnInfo> m_columns;
    std::vector<ValueType> m_types;
    std::vector<bool> m_isBit;
    bool m_finished = false;
    std::optional<qint64> m_affected;
    QStringList m_notices;
};

} // namespace quarry::mysql
