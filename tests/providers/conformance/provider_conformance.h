#pragma once

#include <quarry/change_set.h>
#include <quarry/provider_api.h>
#include <quarry/session.h>

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

namespace quarry::conformance {

// A script and the statements splitStatements() must return for it (text of each range, which
// excludes the terminating ';' and is trimmed).
struct SplitCase
{
    QString script;
    QStringList expected;
};

// Everything engine specific that the shared tests need. One fixture instance is created per
// test, so a fixture can hand out a fresh empty database on every config() call.
class Fixture
{
public:
    virtual ~Fixture() = default;

    virtual IDatabaseProvider &provider() = 0;
    // Points at an empty database that the tests may modify.
    virtual ConnectionConfig config() = 0;
    // Secrets handed to createConnection(): the password, for engines that have no environment
    // variable for it (SQL Server). Default: none.
    virtual Secrets secrets() const { return {}; }

    // DDL script (several statements) creating the sample schema:
    //   authors(id INTEGER PK, name TEXT NOT NULL UNIQUE)
    //   books(id INTEGER PK, title TEXT NOT NULL, author_id INTEGER NOT NULL REFERENCES
    //         authors(id), price REAL NULL, cover BLOB NULL), index idx_books_title(title)
    //   view v_titles(id, title) over books
    virtual QString sampleSchemaSql() const = 0;
    // A statement that never finishes on its own and produces no row before running forever.
    virtual QString slowQuerySql() const = 0;
    // One column "n" with the integers 1..count.
    virtual QString seriesSql(int count) const = 0;
    // Positional placeholder syntax; index is 1-based.
    virtual QString placeholder(int index) const
    {
        Q_UNUSED(index)
        return QStringLiteral("?");
    }
    // Name of the hidden row identifier column: rowid or ctid.
    virtual QString rowIdentifierName() const = 0;
    // False for engines without a hidden row identifier (MySQL): dialect().rowIdentifier() must
    // then be empty and the row identifier tests are skipped.
    virtual bool hasRowIdentifier() const { return true; }
    // False for engines whose syntax errors carry no position (MySQL).
    virtual bool reportsErrorPosition() const { return true; }
    virtual ObjectPath booksPath() const = 0;
    virtual ObjectPath viewPath() const = 0;
    // Type of the literal 2.5: SQLite and MySQL give a double, PostgreSQL an exact numeric.
    virtual ValueType fractionalLiteralType() const { return ValueType::Double; }
    // Dialect-specific split cases, run in addition to the shared ones.
    virtual std::vector<SplitCase> extraSplitCases() const { return {}; }
};

// Shared provider tests as a Qt Test base class: derive, implement createFixture(), add your own
// private slots, and run with QTEST_GUILESS_MAIN. Inherited private slots are executed too.
class ProviderConformanceTest : public QObject
{
    Q_OBJECT

protected:
    virtual std::unique_ptr<Fixture> createFixture() = 0;

    struct Drained
    {
        bool ok = false;
        Error error; // when !ok
        std::vector<ColumnInfo> columns;
        std::vector<ResultBatch> batches;
        qint64 rows = 0;
        std::optional<qint64> affected;
        QString errorText() const { return error.message + QStringLiteral(" | ") + error.detail; }
    };

    // Opens a connection on a fresh database (null after a recorded failure), optionally
    // creating the sample schema through splitStatements().
    std::unique_ptr<IConnection> openConnection(bool withSchema = true);
    // Executes one statement and reads the stream to its end in batches of 500.
    Drained drain(IConnection &connection, const QString &sql, const QVariantList &parameters = {});
    // Result rows as parseExplain() wants them: cells joined by a tab, rows by a newline. Records
    // a failure and returns an empty string when the statement fails.
    QString drainAsText(IConnection &connection, const QString &sql);
    Fixture &fixture() { return *m_fixture; }

private slots:
    void init();
    void cleanup();

    void openCloseServerInfo();
    void emptyResultKeepsColumns();
    void typesRoundTripWithNulls();
    void parametersAreBoundNotConcatenated();
    void streamsInBatches();
    void affectedRowsForUpdateAndDelete();
    void syntaxErrorHasPosition();
    void constraintViolationKind();
    void missingTableIsNotFound();
    void cancelLongQueryIsFastAndConnectionSurvives();
    void staleCancelWhileIdleIsIgnored();
    void staleCancelBetweenFetchesIsIgnored();
    void cancelledTokenIsHonored();
    void introspectionTree();
    void columnsFolderPath();
    void usersFolder();
    void schemaIndexesFolder();
    void describeTable();
    void quotingWorksInsideStatements();
    void splitStatements();
    void dialectEditingHelpers();
    void applyChangesCommitsBatch();
    void applyChangesRollsBackOnFailingStatement();
    void applyChangesRollsBackOnZeroRowUpdate();
    void applyChangesWithRowIdentifierKey();
    void applyChangesNullInSet();
    void applyChangesPassesExactlyThePlannedStatements();
    void applyChangesWhileBusyFailsWithoutTouchingAnything();
    void cancelDuringApplyRollsBack();

private:
    // First cell of the first row as text ("NULL" for NULL); fails the test on error.
    QString scalar(IConnection &connection, const QString &sql);
    EditTarget booksTarget() const;
    // Seeds authors 1-2 and books 1-3 (titles a, b, c).
    bool seedBooks(IConnection &connection);

    std::unique_ptr<Fixture> m_fixture;
};

} // namespace quarry::conformance
