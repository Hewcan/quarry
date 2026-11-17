#pragma once

#include "odbc_state.h"
#include "odbc_support.h"

#include <quarry/provider_api.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace quarry::odbc {

// Result of one statement. Rows are read in blocks with row-wise binding (SQLBindCol with a row
// size and SQL_ATTR_ROW_ARRAY_SIZE) for every column up to the first one of unbounded size;
// from there on the columns come from SQLGetData, one row per fetch (a driver may only call
// SQLGetData for columns after the bound ones, and not for a block).
class OdbcStream : public IResultStream
{
public:
    // Allocates a statement, runs `statement` and positions on its first result. The caller holds
    // a CallScope. A cancel that is pending fails with Cancelled before the driver is entered.
    // bindVariable: bind text, binary and decimal columns at the size the driver reports
    // (planColumn()).
    static Result<std::unique_ptr<OdbcStream>> run(const std::shared_ptr<OdbcState> &state,
                                                   const Statement &statement, bool bindVariable);
    // A stream of a statement that has no SQL (empty or comments only): nothing is sent.
    static std::unique_ptr<OdbcStream> createEmpty(std::shared_ptr<OdbcState> state);
    ~OdbcStream() override;
    OdbcStream(const OdbcStream &) = delete;
    OdbcStream &operator=(const OdbcStream &) = delete;

    const std::vector<ColumnInfo> &columns() const override { return m_columns; }
    Result<ResultBatch> fetch(qsizetype maxRows, const CancellationToken &token) override;
    bool atEnd() const override { return m_rowsetDone; }
    std::optional<qint64> affectedRows() const override { return m_affected; }
    QStringList notices() const override { return m_notices; }
    Result<bool> nextResult(const CancellationToken &token) override;

private:
    struct ColumnPlan
    {
        ValueType type = ValueType::Other;
        SQLSMALLINT cType = SQL_C_WCHAR;
        bool bound = false;
        size_t offset = 0; // of the indicator inside a row of the block
        SQLLEN capacity = 0; // bytes of the data buffer of a bound column
    };

    OdbcStream(std::shared_ptr<OdbcState> state, StatementHandle statement, qsizetype fetchSize,
               bool bindVariable);

    // Describes and binds the current result; steps over results that only carry a row count
    // when another one follows. Ends the stream if nothing follows.
    Outcome loadResult();
    Outcome describeColumns(SQLSMALLINT count);
    void bindColumns();
    // SQLMoreResults: true if another result is ready, false if there is none.
    Result<bool> moreResults();
    // The current result has no more rows: looks for the next one.
    Outcome endRowset();
    // Reads the next block of rows: false at the end of the result.
    Result<bool> fetchBlock();
    Outcome appendRow(ResultBatch &batch, size_t row);
    void appendBound(ColumnBuffer &out, const ColumnPlan &plan, size_t row);
    Outcome appendFetched(ColumnBuffer &out, const ColumnPlan &plan, size_t index);
    // Cursor closed, connection free for the next statement.
    void finish();
    // Same after an error or a cancel: the rest of the result is discarded unread.
    void abort();

    std::shared_ptr<OdbcState> m_state;
    StatementHandle m_statement;
    qsizetype m_fetchSize;
    bool m_bindVariable;
    bool m_owner = false; // holds the single active cursor of the connection

    std::vector<ColumnInfo> m_columns;
    std::vector<ColumnPlan> m_plans;
    size_t m_boundCount = 0;
    size_t m_rowSize = 0;
    // 8-byte units: keeps the indicators and the numbers of the rows aligned.
    std::vector<std::uint64_t> m_block;
    std::vector<SQLUSMALLINT> m_status;
    SQLULEN m_arraySize = 1;
    SQLULEN m_rowsFetched = 0;
    size_t m_blockRows = 0;
    size_t m_blockPos = 0;

    bool m_rowsetDone = true; // the current result has no more rows (or there is none)
    bool m_hasMore = false; // the driver is positioned on another result
    bool m_truncated = false;
    std::optional<qint64> m_affected;
    QStringList m_notices;
    std::vector<SQLWCHAR> m_wideScratch;
    QByteArray m_byteScratch;
};

} // namespace quarry::odbc
