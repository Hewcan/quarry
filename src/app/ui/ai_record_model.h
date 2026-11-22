#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QList>
#include <QStringList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

namespace quarry::ui {

// A small list model of records (the messages of a chat, the requests of the "Context sent"
// panel): every row is a map and every key of `roles` is a role. In memory only; the chat is never
// stored (H13 phase C).
class AiRecordModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ANONYMOUS

    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)

public:
    explicit AiRecordModel(QStringList roles, QObject *parent = nullptr);

    int count() const { return static_cast<int>(m_rows.size()); }
    const QList<QVariantMap> &rows() const { return m_rows; }
    void append(const QVariantMap &row);
    // Keeps only the last `limit` rows.
    void trim(int limit);
    void clear();
    Q_INVOKABLE QVariantMap get(int row) const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();

private:
    QStringList m_roles;
    QList<QVariantMap> m_rows;
};

} // namespace quarry::ui
