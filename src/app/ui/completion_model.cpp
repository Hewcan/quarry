#include "completion_model.h"

namespace quarry::ui {

CompletionModel::CompletionModel(QObject *parent)
    : QAbstractListModel(parent)
{ }

void CompletionModel::setItems(std::vector<Item> items)
{
    const int before = count();
    beginResetModel();
    m_items = std::move(items);
    endResetModel();
    if (before != count())
        emit countChanged();
}

void CompletionModel::setSuggestions(const std::vector<quarry::Suggestion> &suggestions)
{
    std::vector<Item> items;
    items.reserve(suggestions.size());
    for (const quarry::Suggestion &s : suggestions)
        items.push_back(Item { s, {}, false });
    setItems(std::move(items));
}

QString CompletionModel::kindName(const Item &item)
{
    switch (item.suggestion.kind) {
    case SuggestionKind::Keyword:
        return QStringLiteral("keyword");
    case SuggestionKind::Function:
        return QStringLiteral("function");
    case SuggestionKind::Schema:
        return QStringLiteral("schema");
    case SuggestionKind::Table:
        return QStringLiteral("table");
    case SuggestionKind::View:
        return item.materialized ? QStringLiteral("materializedView") : QStringLiteral("view");
    case SuggestionKind::Column:
        break;
    }
    return QStringLiteral("column");
}

QString CompletionModel::kindText(const Item &item)
{
    switch (item.suggestion.kind) {
    case SuggestionKind::Keyword:
        return tr("keyword");
    case SuggestionKind::Function:
        return tr("function");
    case SuggestionKind::Schema:
        return tr("schema");
    case SuggestionKind::Table:
        return tr("table");
    case SuggestionKind::View:
        return item.materialized ? tr("materialized view") : tr("view");
    case SuggestionKind::Column:
        break;
    }
    return tr("column");
}

QString CompletionModel::textAt(int row) const
{
    return row >= 0 && row < count() ? m_items[static_cast<size_t>(row)].suggestion.text
                                     : QString();
}

QString CompletionModel::labelAt(int row) const
{
    return row >= 0 && row < count() ? m_items[static_cast<size_t>(row)].suggestion.label
                                     : QString();
}

QString CompletionModel::kindTextAt(int row) const
{
    return row >= 0 && row < count() ? kindText(m_items[static_cast<size_t>(row)]) : QString();
}

QStringList CompletionModel::partsAt(int row) const
{
    return row >= 0 && row < count() ? m_items[static_cast<size_t>(row)].parts : QStringList();
}

int CompletionModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant CompletionModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid))
        return {};
    const Item &item = m_items[static_cast<size_t>(index.row())];
    switch (role) {
    case LabelRole:
    case Qt::DisplayRole:
        return item.suggestion.label;
    case TextRole:
        return item.suggestion.text;
    case KindRole:
        return kindName(item);
    case KindTextRole:
        return kindText(item);
    case DetailRole:
        return item.suggestion.detail;
    case PartsRole:
        return item.parts;
    default:
        return {};
    }
}

QHash<int, QByteArray> CompletionModel::roleNames() const
{
    return {
        { LabelRole, "label" },       { TextRole, "text" },     { KindRole, "kind" },
        { KindTextRole, "kindText" }, { DetailRole, "detail" }, { PartsRole, "parts" },
    };
}

} // namespace quarry::ui
