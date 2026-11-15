#pragma once

#include <QCoreApplication>

namespace quarry::postgres {

// Translation context for every user-visible string of this provider.
struct Text
{
    Q_DECLARE_TR_FUNCTIONS(quarry::postgres)
};

} // namespace quarry::postgres
