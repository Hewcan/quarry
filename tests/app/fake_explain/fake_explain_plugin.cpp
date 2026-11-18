#include <quarry/provider_api.h>

#include <QObject>
#include <QThread>

// A test-only engine ("FakeDB") that can do what SQLite cannot: Explain analyze. Its EXPLAIN
// returns one row per plan line, the first one echoing the statement it was sent, so a test sees
// what the app executed. Words in the SQL steer it:
//   NOPLAN      no EXPLAIN at all           NOANALYZE   Plan only
//   BAD         parseExplain fails          NOCOST      the plan has no costs
//   SLOWPARSE   parseExplain takes 300 ms   BIG         the output is over 256 KiB
//   FAILEXEC    the EXPLAIN itself fails
namespace {

using namespace quarry;

class FakeDialect : public ISqlDialect
{
public:
    QString quoteIdentifier(QStringView name) const override { return name.toString(); }
    QString quoteLiteral(QStringView text) const override { return text.toString(); }
    QString highlightingDefinition() const override { return QStringLiteral("SQL"); }
    QStringList keywords() const override { return {}; }
    QStringList builtinFunctions() const override { return {}; }
    std::vector<StatementRange> splitStatements(QStringView script) const override
    {
        std::vector<StatementRange> out;
        qsizetype start = 0;
        for (qsizetype i = 0; i <= script.size(); ++i) {
            if (i < script.size() && script.at(i) != QLatin1Char(';'))
                continue;
            qsizetype b = start;
            qsizetype e = i;
            while (b < e && script.at(b).isSpace())
                ++b;
            while (e > b && script.at(e - 1).isSpace())
                --e;
            if (e > b) {
                StatementRange r;
                r.begin = b;
                r.end = e;
                out.push_back(r);
            }
            start = i + 1;
        }
        return out;
    }
    QString limitClause(qint64, qint64) const override { return {}; }
    QString placeholder(int) const override { return QStringLiteral("?"); }
    QString qualifiedName(const ObjectPath &) const override { return {}; }
    std::optional<QString> rowIdentifier() const override { return std::nullopt; }

    QString explainStatement(const QString &sql, ExplainMode mode,
                             const QString &product) const override
    {
        if (sql.contains(QLatin1String("NOPLAN")))
            return {};
        if (mode == ExplainMode::Analyze) {
            if (sql.contains(QLatin1String("NOANALYZE")))
                return {};
            return QStringLiteral("EXPLAIN ANALYZE [%1] %2").arg(product, sql);
        }
        return QStringLiteral("EXPLAIN [%1] %2").arg(product, sql);
    }

    Result<ExplainPlan> parseExplain(const QString &output, ExplainMode mode,
                                     const QString &) const override
    {
        if (output.contains(QLatin1String("BAD"))) {
            return fail(Error { ErrorKind::Syntax,
                                QStringLiteral("The plan could not be read."),
                                QStringLiteral("unexpected token near BAD"),
                                {},
                                {} });
        }
        if (output.contains(QLatin1String("SLOWPARSE")))
            QThread::msleep(300);
        const bool costs = !output.contains(QLatin1String("NOCOST"));
        ExplainPlan plan;
        plan.rawText = output;
        plan.engine = QStringLiteral("FakeDB");
        plan.analyzed = mode == ExplainMode::Analyze;
        if (plan.analyzed)
            plan.executionTimeMs = 6.0;
        // The wrapper without a label hides: the lines are the top level.
        const QStringList lines = output.split(QLatin1Char('\n'));
        double n = 0;
        for (const QString &line : lines) {
            ExplainNode node;
            node.label = line.left(60);
            ++n;
            if (costs)
                node.totalCost = n * 10;
            node.estimatedRows = n * 100;
            if (plan.analyzed) {
                node.actualRows = n * 90;
                node.actualTimeMs = n;
            }
            if (n == 1) {
                // Which thread parsed: a test compares it with its own.
                node.details.push_back(ExplainProperty {
                    QStringLiteral("thread"),
                    QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId())) });
                node.warnings.push_back(QStringLiteral("Full scan"));
            }
            plan.root.children.push_back(std::move(node));
        }
        plan.nodeCount = static_cast<qsizetype>(plan.root.children.size());
        return plan;
    }
};

class FakeStream : public IResultStream
{
public:
    FakeStream(std::vector<ColumnInfo> columns, QStringList rows, std::optional<qint64> affected)
        : m_columns(std::move(columns))
        , m_rows(std::move(rows))
        , m_affected(affected)
    { }
    const std::vector<ColumnInfo> &columns() const override { return m_columns; }
    Result<ResultBatch> fetch(qsizetype, const CancellationToken &) override
    {
        ResultBatch batch;
        if (!m_done && !m_columns.empty()) {
            ColumnBuffer column;
            for (const QString &row : std::as_const(m_rows))
                column.appendBytes(row.toUtf8(), ValueType::Text);
            batch.rowCount = m_rows.size();
            batch.columns.push_back(std::move(column));
        }
        m_done = true;
        return batch;
    }
    bool atEnd() const override { return m_done; }
    std::optional<qint64> affectedRows() const override { return m_affected; }
    QStringList notices() const override { return {}; }
    Result<bool> nextResult(const CancellationToken &) override { return false; }

private:
    std::vector<ColumnInfo> m_columns;
    QStringList m_rows;
    std::optional<qint64> m_affected;
    bool m_done = false;
};

class FakeConnection : public IConnection
{
public:
    Result<ServerInfo> open(const CancellationToken &) override
    {
        m_open = true;
        ServerInfo info;
        info.product = QStringLiteral("FakeDB");
        info.version = QStringLiteral("1.0");
        return info;
    }
    void close() override { m_open = false; }
    bool isOpen() const override { return m_open; }
    Result<std::vector<SchemaNode>> children(const ObjectPath &, const CancellationToken &) override
    {
        return std::vector<SchemaNode> {};
    }
    Result<TableDetails> describe(const ObjectPath &, const CancellationToken &) override
    {
        return unsupported();
    }
    Result<std::unique_ptr<IResultStream>> execute(const Statement &statement,
                                                   const CancellationToken &) override
    {
        const QString sql = statement.sql;
        std::vector<ColumnInfo> columns;
        QStringList rows;
        std::optional<qint64> affected;
        if (sql.startsWith(QLatin1String("EXPLAIN"))) {
            if (sql.contains(QLatin1String("FAILEXEC"))) {
                return fail(Error { ErrorKind::Syntax,
                                    QStringLiteral("The query has an error."),
                                    QStringLiteral("syntax error at FAILEXEC"),
                                    {},
                                    {} });
            }
            ColumnInfo c;
            c.name = QStringLiteral("QUERY PLAN");
            c.type = ValueType::Text;
            c.nativeType = QStringLiteral("text");
            columns.push_back(c);
            rows << QStringLiteral("Echo: ") + sql;
            if (sql.contains(QLatin1String("BIG")))
                rows << QStringLiteral("Big ")
                        + QString(qsizetype { 300 } * 1024, QLatin1Char('x'));
            else
                rows << QStringLiteral("Seq Scan on t") << QStringLiteral("Sort");
        } else if (sql.contains(QLatin1String("INSERT"))) {
            affected = 1;
        } else {
            ColumnInfo c;
            c.name = QStringLiteral("x");
            c.type = ValueType::Text;
            c.nativeType = QStringLiteral("text");
            columns.push_back(c);
            rows << QStringLiteral("1");
        }
        return std::unique_ptr<IResultStream>(
            std::make_unique<FakeStream>(std::move(columns), std::move(rows), affected));
    }
    void requestCancel() override { }
    Result<void> begin() override { return unsupported(); }
    Result<void> commit() override { return unsupported(); }
    Result<void> rollback() override { return unsupported(); }

private:
    static Unexpected unsupported()
    {
        return fail(Error { ErrorKind::Unsupported, QStringLiteral("Unsupported."), {}, {}, {} });
    }
    bool m_open = false;
};

class FakeProvider : public IDatabaseProvider
{
public:
    ProviderInfo info() const override
    {
        return { QStringLiteral("fakeexplain"), QStringLiteral("FakeDB"), QVersionNumber(0, 1, 0),
                 Capabilities(Capability::Explain) };
    }
    std::vector<ConnectionField> connectionFields() const override { return {}; }
    const ISqlDialect &dialect() const override { return m_dialect; }
    std::unique_ptr<IConnection> createConnection(const ConnectionConfig &,
                                                  const Secrets &) const override
    {
        return std::make_unique<FakeConnection>();
    }

private:
    FakeDialect m_dialect;
};

} // namespace

class FakeExplainPlugin : public QObject, public quarry::IProviderPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QUARRY_PROVIDER_IID FILE "fake_explain.json")
    Q_INTERFACES(quarry::IProviderPlugin)

public:
    std::unique_ptr<quarry::IDatabaseProvider> createProvider() override
    {
        return std::make_unique<FakeProvider>();
    }
};

#include "fake_explain_plugin.moc"
