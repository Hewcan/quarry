#pragma once

#include <quarry/explain_plan.h>

#include <QAbstractItemModel>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <vector>

namespace quarry::ui {

// The query plan of one editor tab (UX.md "Plan de consulta (H9)", ARCHITECTURE section 6.9): a
// tree of steps for the Plan tab, plus what the view says around it (state, engine, times,
// the raw text). QueryController owns it through QueryState and is the only one that changes it.
//
// The steps live in one flat vector in breadth-first order, so the children of a node are
// contiguous (firstChild, childCount) and data() never walks the tree. A root without a label (the
// wrapper of SQLite's top-level steps) is not a row: its children are the top level.
//
// The bar of a node is its own share, not its total: cost (or time, after ANALYZE with real times)
// minus what its children account for, over the total of the plan. An engine that reports neither
// (SQLite) gets no bar and no percentage: nothing is invented.
class PlanModel : public QAbstractItemModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by the editor tab's QueryState")

    // A plan was asked for in this tab: the Plan tab exists from then on.
    Q_PROPERTY(bool requested READ requested NOTIFY changed FINAL)
    // "idle" | "running" | "ready" | "failed" | "cancelled"
    Q_PROPERTY(QString state READ state NOTIFY changed FINAL)
    Q_PROPERTY(bool running READ running NOTIFY changed FINAL)
    // Why there is no plan ("failed"): plain language, and the engine's own text apart.
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY changed FINAL)
    Q_PROPERTY(QString errorDetail READ errorDetail NOTIFY changed FINAL)
    // A request that was refused (this engine cannot, ANALYZE of a write): shown above the plan,
    // which is kept.
    Q_PROPERTY(QString notice READ notice NOTIFY changed FINAL)
    Q_PROPERTY(QString rawText READ rawText NOTIFY changed FINAL)
    Q_PROPERTY(QString engine READ engine NOTIFY changed FINAL)
    Q_PROPERTY(bool analyzed READ analyzed NOTIFY changed FINAL)
    Q_PROPERTY(bool truncated READ truncated NOTIFY changed FINAL)
    Q_PROPERTY(int nodeCount READ nodeCount NOTIFY changed FINAL)
    // Milliseconds; -1 when the engine did not say.
    Q_PROPERTY(double planningTime READ planningTime NOTIFY changed FINAL)
    Q_PROPERTY(double executionTime READ executionTime NOTIFY changed FINAL)
    Q_PROPERTY(QString planningTimeText READ planningTimeText NOTIFY changed FINAL)
    Q_PROPERTY(QString executionTimeText READ executionTimeText NOTIFY changed FINAL)
    // What the bars measure: "none" | "cost" | "time".
    Q_PROPERTY(QString barBasis READ barBasis NOTIFY changed FINAL)
    Q_PROPERTY(bool hasBars READ hasBars NOTIFY changed FINAL)

public:
    // `depth` is TreeView's own delegate property, so the level of a step is `level`.
    enum Role {
        LabelRole = Qt::UserRole + 1,
        DetailsRole, // [{name, value}]
        CostTextRole, // "cost 12.5", "" when the engine gave none
        RowsTextRole, // "1,234 rows", "est. 1,234 rows", "" when none
        TimeTextRole, // "0.42 ms", "" without ANALYZE
        CostFractionRole, // 0..1; only meaningful with HasBarRole
        CostPercentTextRole, // "34%", "<1%", "" without a bar
        WarningsRole,
        LevelRole, // 0 = top level
        AccessibleDescriptionRole,
        HasBarRole,
        PositionRole, // 1-based among the siblings
        SetSizeRole,
        ShareTextRole, // "34% of the total execution time", "" without a bar
    };
    Q_ENUM(Role)

    explicit PlanModel(QObject *parent = nullptr);

    bool requested() const { return m_requested; }
    QString state() const;
    bool running() const { return m_state == State::Running; }
    QString errorMessage() const { return m_errorMessage; }
    QString errorDetail() const { return m_errorDetail; }
    QString notice() const { return m_notice; }
    QString rawText() const { return m_rawText; }
    QString engine() const { return m_engine; }
    bool analyzed() const { return m_analyzed; }
    bool truncated() const { return m_truncated; }
    int nodeCount() const { return static_cast<int>(m_nodes.size()); }
    double planningTime() const { return m_planningMs; }
    double executionTime() const { return m_executionMs; }
    QString planningTimeText() const;
    QString executionTimeText() const;
    QString barBasis() const;
    bool hasBars() const { return m_basis != Basis::None; }

    // Controller side.
    // A plan was asked for: the previous one goes, the tab appears, the state is "running".
    void begin(bool analyze);
    void setPlan(const quarry::ExplainPlan &plan);
    // `rawText` is whatever the engine returned, if anything: the Text view shows it.
    void setFailure(const QString &message, const QString &detail, const QString &rawText);
    void setCancelled();
    // The request was refused; the plan (if any) stays.
    void setNotice(const QString &notice);
    // Back to nothing (the tab's connection changed): the Plan tab goes.
    void clear();

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void changed();

private:
    enum class State { Idle, Running, Ready, Failed, Cancelled };
    enum class Basis { None, Cost, Time };

    struct Node
    {
        QString label;
        std::vector<quarry::ExplainProperty> details;
        QStringList warnings;
        QString costText;
        QString rowsText;
        QString timeText;
        QString percentText;
        QString shareText;
        double fraction = 0;
        qsizetype parent = -1;
        qsizetype firstChild = 0;
        int childCount = 0;
        int level = 0;
        int position = 1;
        int setSize = 1;
    };

    void reset(State state);
    qsizetype nodeOf(const QModelIndex &index) const;
    // Index in m_nodes of row `row` under `parent` (-1 = the top level).
    qsizetype childAt(qsizetype parent, int row) const;
    QString describe(const Node &node) const;

    bool m_requested = false;
    State m_state = State::Idle;
    QString m_errorMessage;
    QString m_errorDetail;
    QString m_notice;
    QString m_rawText;
    QString m_engine;
    bool m_analyzed = false;
    bool m_truncated = false;
    double m_planningMs = -1;
    double m_executionMs = -1;
    Basis m_basis = Basis::None;
    std::vector<Node> m_nodes;
    qsizetype m_topCount = 0; // the first m_topCount nodes are the top level
};

} // namespace quarry::ui
