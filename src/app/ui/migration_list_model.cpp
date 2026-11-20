#include "migration_list_model.h"

namespace quarry::ui {

void MigrationListModel::setEntries(std::vector<MigrationEntry> entries)
{
    beginResetModel();
    m_entries = std::move(entries);
    endResetModel();
}

int MigrationListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_entries.size());
}

QString MigrationListModel::stateText(const MigrationEntry &entry)
{
    const QString batch = entry.batch ? QString::number(*entry.batch) : tr("unknown");
    switch (entry.state) {
    case MigrationState::Pending:
        return tr("Pending");
    case MigrationState::Applied:
        return tr("Applied in batch %1").arg(batch);
    case MigrationState::Orphan:
        return tr("File missing (applied in batch %1)").arg(batch);
    }
    return {};
}

QVariant MigrationListModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid))
        return {};
    const MigrationEntry &e = m_entries[static_cast<size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return e.name;
    case StateRole:
        switch (e.state) {
        case MigrationState::Pending:
            return QStringLiteral("pending");
        case MigrationState::Applied:
            return QStringLiteral("applied");
        case MigrationState::Orphan:
            return QStringLiteral("orphan");
        }
        return {};
    case StateTextRole:
        return stateText(e);
    case BatchRole:
        return e.batch ? static_cast<int>(*e.batch) : -1;
    default:
        return {};
    }
}

QHash<int, QByteArray> MigrationListModel::roleNames() const
{
    return { { NameRole, "name" },
             { StateRole, "state" },
             { StateTextRole, "stateText" },
             { BatchRole, "batch" } };
}

} // namespace quarry::ui
