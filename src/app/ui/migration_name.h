#pragma once

#include <QString>

namespace quarry::ui {

// A Laravel migration file is named `2024_01_01_000000_create_users_table`: the lists show it
// without the leading date (`000000_create_users_table`), where the full name is still in the
// tooltip and in what the screen reader says. A name that does not start with `YYYY_MM_DD_` (or
// that is only that prefix) is returned as it is.
QString shortMigrationName(const QString &name);

} // namespace quarry::ui
