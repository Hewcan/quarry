#pragma once

#include <quarry/migration_source.h>

#include <QAbstractListModel>
#include <QtQml/qqmlregistration.h>

#include <vector>

namespace quarry::ui {

// Rows of the migrations view (one per MigrationEntry). The state goes out as a word and as
// text, never only as a color (UX §2).
class MigrationListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by MigrationController")

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        StateRole, // "pending" | "applied" | "orphan"
        StateTextRole, // "Pending", "Applied in batch 2", "File missing (batch 1)"
        BatchRole, // -1 = none
    };
    Q_ENUM(Role)

    using QAbstractListModel::QAbstractListModel;

    void setEntries(std::vector<quarry::MigrationEntry> entries);
    void clear() { setEntries({}); }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    static QString stateText(const quarry::MigrationEntry &entry);

private:
    std::vector<quarry::MigrationEntry> m_entries;
};

} // namespace quarry::ui
