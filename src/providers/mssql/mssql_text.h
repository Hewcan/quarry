#pragma once

#include <QCoreApplication>

namespace quarry::mssql {

// Translation context for every user-visible string of the SQL Server provider.
struct Text
{
    Q_DECLARE_TR_FUNCTIONS(quarry::mssql)
};

} // namespace quarry::mssql
