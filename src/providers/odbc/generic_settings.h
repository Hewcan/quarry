#pragma once

// Pure helpers of the generic ODBC provider: no driver manager calls, so unit tests can use them
// without a server.

#include <quarry/result.h>

#include <QString>
#include <QVariantMap>

namespace quarry::odbc {

struct GenericSettings
{
    QString dsn;
    QString driver; // registered name or path of the library
    QString extra; // "key=value;..." as typed, without the password
    QString user;
    QString password; // empty: whatever the data source or the extra text says
};

// `values` are the form fields (dsn, driver, connectionString, user); the password comes apart
// because it is never persisted. At least one of dsn, driver and connectionString is needed.
Result<GenericSettings> buildGenericSettings(const QVariantMap &values, const QString &password);

// "DSN=x;" or "Driver={x};", then the extra text, then UID and PWD={...} (the password always
// braced, with every } doubled).
QString buildGenericConnectionString(const GenericSettings &settings);

// What the user asked for, for the "not found" message: the data source, else the driver.
QString genericTarget(const GenericSettings &settings);

} // namespace quarry::odbc
