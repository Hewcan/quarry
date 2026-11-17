#pragma once

#include <QCoreApplication>

namespace quarry::odbc {

// Translation context for every user-visible string of the shared ODBC code and the generic
// provider.
struct Text
{
    Q_DECLARE_TR_FUNCTIONS(quarry::odbc)
};

} // namespace quarry::odbc
