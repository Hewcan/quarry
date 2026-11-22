#pragma once

#include "plan_model.h"
#include "query_messages_model.h"
#include "result_table_model.h"
#include "table_edit_model.h"

#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

namespace quarry::ui {

// What the UI shows about the last run of one editor tab (UX.md section 3, "Ejecucion"): the
// status bar line, the summary of the Results tab, the Messages and the error to underline.
// QueryController owns it and is the only one that changes it; QML only reads.
//
// The rows of the result the grid shows live in results() (H1.7).
class QueryState : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS

    // "idle" | "running" | "done" | "cancelled" | "failed"
    Q_PROPERTY(QString state READ state NOTIFY changed FINAL)
    Q_PROPERTY(bool running READ running NOTIFY changed FINAL)
    // "Running… 00:03", "120 rows · 15 ms", "3 rows affected · 4 ms", "Query cancelled"...
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed FINAL)
    Q_PROPERTY(QString elapsedText READ elapsedText NOTIFY changed FINAL)
    Q_PROPERTY(bool hasSummary READ hasSummary NOTIFY changed FINAL)
    Q_PROPERTY(qint64 rows READ rows NOTIFY changed FINAL)
    Q_PROPERTY(qint64 affectedRows READ affectedRows NOTIFY changed FINAL) // -1 = none
    Q_PROPERTY(qint64 elapsedMs READ elapsedMs NOTIFY changed FINAL)
    Q_PROPERTY(QStringList columns READ columns NOTIFY changed FINAL)
    Q_PROPERTY(bool atEnd READ atEnd NOTIFY changed FINAL)
    Q_PROPERTY(quarry::ui::QueryMessagesModel *messages READ messages CONSTANT FINAL)
    // The result of the last statement of the plan that returned columns.
    Q_PROPERTY(quarry::ui::ResultTableModel *results READ results CONSTANT FINAL)
    // The pending changes of a data tab (inactive for a query tab). Its rows are the ones of
    // results() plus the new ones; the grid of a data tab shows this model.
    Q_PROPERTY(quarry::ui::TableEditModel *edits READ edits CONSTANT FINAL)
    // The query plan of the last Explain of this tab. It outlives normal runs (until the next
    // Explain, or until the tab's connection changes).
    Q_PROPERTY(quarry::ui::PlanModel *plan READ plan CONSTANT FINAL)
    // "Count rows" of a data tab: a COUNT(*) of the view is running / its exact total (-1 = not
    // counted for this view) / why it failed ("" = no failure). A new load of the view clears them.
    Q_PROPERTY(bool counting READ counting NOTIFY countChanged FINAL)
    Q_PROPERTY(qint64 countedTotal READ countedTotal NOTIFY countChanged FINAL)
    Q_PROPERTY(QString countError READ countError NOTIFY countChanged FINAL)
    // A data tab opened by "Go to related row" (H11 phase 4): "Filtered from orders.customer_id =
    // 5",
    // "" for any other view. Shown under the bar with a "Clear" button.
    Q_PROPERTY(QString filterOrigin READ filterOrigin NOTIFY filterOriginChanged FINAL)
    // The error to underline: offset and length in the tab's text, 1-based line and column.
    // -1 = none.
    Q_PROPERTY(int errorOffset READ errorOffset NOTIFY errorChanged FINAL)
    Q_PROPERTY(int errorLength READ errorLength NOTIFY errorChanged FINAL)
    Q_PROPERTY(int errorLine READ errorLine NOTIFY errorChanged FINAL)
    Q_PROPERTY(int errorColumn READ errorColumn NOTIFY errorChanged FINAL)

public:
    enum class State { Idle, Running, Done, Cancelled, Failed };

    explicit QueryState(QObject *parent = nullptr);

    QString state() const;
    bool running() const { return m_state == State::Running; }
    QString statusText() const;
    QString elapsedText() const;
    bool hasSummary() const { return m_hasSummary; }
    qint64 rows() const { return m_rows; }
    qint64 affectedRows() const { return m_affected; }
    qint64 elapsedMs() const { return m_elapsedMs; }
    QStringList columns() const { return m_columns; }
    bool atEnd() const { return m_atEnd; }
    QueryMessagesModel *messages() { return &m_messages; }
    ResultTableModel *results() { return &m_results; }
    TableEditModel *edits() { return &m_edits; }
    PlanModel *plan() { return &m_plan; }
    bool counting() const { return m_counting; }
    qint64 countedTotal() const { return m_countedTotal; }
    QString countError() const { return m_countError; }
    QString filterOrigin() const { return m_filterOrigin; }
    int errorOffset() const { return m_errorOffset; }
    int errorLength() const { return m_errorLength; }
    int errorLine() const { return m_errorLine; }
    int errorColumn() const { return m_errorColumn; }

    // Seconds since begin() (whole seconds, as the status bar shows them).
    int elapsedSeconds() const;
    // "120 rows", "3 rows affected" or "Done" for the summary; no time.
    QString resultText() const;

    // Controller side.
    // Running, with the previous summary, messages and error cleared. keepResults: the rows of the
    // grid stay (an Explain shows its own tab and leaves the last result where it was).
    void begin(bool keepResults = false);
    void setSummary(qint64 rows, qint64 affected, qint64 elapsedMs, const QStringList &columns,
                    bool atEnd);
    void finish(State state);
    void reset(); // back to idle (the connection of the tab changed)
    void setError(int offset, int length, int line, int column);
    Q_INVOKABLE void clearError();
    // The row count of the view: running, done (total >= 0) or failed (error set); all clear = not
    // counted.
    void setCount(bool counting, qint64 total, const QString &error);
    void setFilterOrigin(const QString &text);

signals:
    void changed();
    void countChanged();
    void filterOriginChanged();
    void errorChanged();

private:
    State m_state = State::Idle;
    bool m_hasSummary = false;
    qint64 m_rows = 0;
    qint64 m_affected = -1;
    qint64 m_elapsedMs = 0;
    QStringList m_columns;
    bool m_atEnd = true;
    bool m_counting = false;
    qint64 m_countedTotal = -1;
    QString m_countError;
    QString m_filterOrigin;
    int m_errorOffset = -1;
    int m_errorLength = 0;
    int m_errorLine = 0;
    int m_errorColumn = 0;
    QElapsedTimer m_clock;
    QTimer m_tick;
    QueryMessagesModel m_messages;
    ResultTableModel m_results;
    TableEditModel m_edits; // after m_results: it connects to it
    PlanModel m_plan;
};

} // namespace quarry::ui
