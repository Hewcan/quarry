#include "ai_record_model.h"

namespace quarry::ui {

AiRecordModel::AiRecordModel(QStringList roles, QObject *parent)
    : QAbstractListModel(parent)
    , m_roles(std::move(roles))
{ }

void AiRecordModel::append(const QVariantMap &row)
{
    const int at = count();
    beginInsertRows({}, at, at);
    m_rows.append(row);
    endInsertRows();
    emit countChanged();
}

void AiRecordModel::trim(int limit)
{
    const int extra = count() - qMax(0, limit);
    if (extra <= 0)
        return;
    beginRemoveRows({}, 0, extra - 1);
    m_rows.remove(0, extra);
    endRemoveRows();
    emit countChanged();
}

void AiRecordModel::clear()
{
    if (m_rows.isEmpty())
        return;
    beginResetModel();
    m_rows.clear();
    endResetModel();
    emit countChanged();
}

QVariantMap AiRecordModel::get(int row) const
{
    return row >= 0 && row < count() ? m_rows.at(row) : QVariantMap();
}

int AiRecordModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant AiRecordModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= count() || role < Qt::UserRole + 1
        || role - Qt::UserRole - 1 >= m_roles.size())
        return {};
    return m_rows.at(index.row()).value(m_roles.at(role - Qt::UserRole - 1));
}

QHash<int, QByteArray> AiRecordModel::roleNames() const
{
    QHash<int, QByteArray> names;
    for (qsizetype i = 0; i < m_roles.size(); ++i)
        names.insert(Qt::UserRole + 1 + static_cast<int>(i), m_roles.at(i).toUtf8());
    return names;
}

} // namespace quarry::ui
