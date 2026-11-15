#include <quarry/change_set.h>

#include <QCoreApplication>

namespace quarry {

namespace {

Error internal(const QString &message)
{
    return Error { ErrorKind::Internal, message, {}, {}, {} };
}

QString tr(const char *text)
{
    return QCoreApplication::translate("quarry::ChangeSqlBuilder", text);
}

} // namespace

Result<std::vector<PlannedStatement>> ChangeSqlBuilder::build(const ISqlDialect &dialect,
                                                              const EditTarget &target,
                                                              const std::vector<RowEdit> &edits)
{
    const QString table = dialect.qualifiedName(target.table);
    std::vector<PlannedStatement> out;
    out.reserve(edits.size());

    for (size_t i = 0; i < edits.size(); ++i) {
        const RowEdit &edit = edits[i];
        const int number = static_cast<int>(i) + 1;
        const bool needsKey = edit.kind != RowEdit::Kind::Insert;
        if (needsKey) {
            if (target.key.columns.isEmpty())
                return fail(internal(tr("The table has no key to identify rows.")));
            if (edit.key.size() != target.key.columns.size())
                return fail(internal(
                    tr("Change %1: the row key does not match the key columns.").arg(number)));
            for (const QVariant &value : edit.key)
                if (value.isNull())
                    return fail(
                        internal(tr("Change %1: the row key has a NULL value.").arg(number)));
        }
        if (edit.kind == RowEdit::Kind::Update && edit.cells.empty())
            return fail(
                internal(tr("Change %1: there are no cell changes to update.").arg(number)));

        PlannedStatement planned;
        planned.editIndex = static_cast<int>(i);
        planned.expectOneRow = needsKey;
        QVariantList &params = planned.statement.parameters;
        int next = 1;

        QStringList names;
        QStringList markers;
        for (const CellChange &cell : edit.cells) {
            if (cell.column < 0 || cell.column >= target.columns.size())
                return fail(internal(tr("Change %1: column index %2 is out of range.")
                                         .arg(number)
                                         .arg(cell.column)));
            names << dialect.quoteIdentifier(target.columns.at(cell.column));
            markers << dialect.placeholder(next++);
            params.push_back(cell.value);
        }

        QString where;
        if (needsKey) {
            QStringList conditions;
            for (qsizetype k = 0; k < target.key.columns.size(); ++k) {
                conditions << dialect.quoteIdentifier(target.key.columns.at(k))
                        + QStringLiteral(" = ") + dialect.placeholder(next++);
                params.push_back(edit.key.at(k));
            }
            where = QStringLiteral(" WHERE ") + conditions.join(QStringLiteral(" AND "));
        }

        QString &sql = planned.statement.sql;
        switch (edit.kind) {
        case RowEdit::Kind::Update: {
            QStringList sets;
            for (qsizetype c = 0; c < names.size(); ++c)
                sets << names.at(c) + QStringLiteral(" = ") + markers.at(c);
            sql = QStringLiteral("UPDATE %1 SET %2%3")
                      .arg(table, sets.join(QStringLiteral(", ")), where);
            break;
        }
        case RowEdit::Kind::Insert:
            if (names.isEmpty())
                sql = QStringLiteral("INSERT INTO %1 DEFAULT VALUES").arg(table);
            else
                sql = QStringLiteral("INSERT INTO %1 (%2) VALUES (%3)")
                          .arg(table, names.join(QStringLiteral(", ")),
                               markers.join(QStringLiteral(", ")));
            break;
        case RowEdit::Kind::Delete:
            sql = QStringLiteral("DELETE FROM %1%2").arg(table, where);
            break;
        }
        out.push_back(std::move(planned));
    }
    return out;
}

} // namespace quarry
