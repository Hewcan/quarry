#pragma once

#include <quarry/completion.h>

#include <QAbstractListModel>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <vector>

namespace quarry::ui {

// The rows of the suggestion popup and of the palette's "@" search: one Suggestion each. The
// kind is also given as a word (kindText, translated), because the icon is never the only cue.
class CompletionModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by CompletionController")

    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)

public:
    enum Role {
        LabelRole = Qt::UserRole + 1,
        TextRole, // what is inserted
        KindRole, // "keyword" | "function" | "schema" | "table" | "view" | "materializedView" |
                  // "column"
        KindTextRole, // the same, as the user reads it
        DetailRole,
        PartsRole, // palette rows: the path of the table or view in the tree
    };
    Q_ENUM(Role)

    struct Item
    {
        quarry::Suggestion suggestion;
        QStringList parts;
        bool materialized = false;
    };

    explicit CompletionModel(QObject *parent = nullptr);

    int count() const { return static_cast<int>(m_items.size()); }
    void setItems(std::vector<Item> items);
    void setSuggestions(const std::vector<quarry::Suggestion> &suggestions);
    Q_INVOKABLE QString textAt(int row) const;
    Q_INVOKABLE QString labelAt(int row) const;
    Q_INVOKABLE QString kindTextAt(int row) const;
    Q_INVOKABLE QStringList partsAt(int row) const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();

private:
    static QString kindName(const Item &item);
    static QString kindText(const Item &item);

    std::vector<Item> m_items;
};

} // namespace quarry::ui
