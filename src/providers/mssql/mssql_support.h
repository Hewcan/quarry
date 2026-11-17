#pragma once

// Pure helpers of the SQL Server provider: no driver manager calls, so unit tests can use them
// without a server.

#include <quarry/result.h>
#include <quarry/value_type.h>

#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <optional>

namespace quarry::mssql {

// "Encrypt" of the Microsoft driver: yes, no or strict (TDS 8, Driver 18 only).
enum class EncryptMode { Mandatory, Optional, Strict };

struct ConnSettings
{
    QString host;
    QString instance; // empty: the default instance, found by port
    int port = 1433; // ignored when there is an instance
    QString database; // empty: the default database of the login
    QString user;
    QString password;
    bool windowsAuthentication = false;
    EncryptMode encrypt = EncryptMode::Mandatory;
    bool trustServerCertificate = false;
    QString driver; // empty: the newest Microsoft driver that is installed
};

// `values` are the form fields (server, port, database, user, authentication, encrypt,
// trustservercertificate, driver); the password comes apart because it is never persisted.
Result<ConnSettings> buildConnSettings(const QVariantMap &values, const QString &password);

// The driver to use: the requested one as is, else the first of "ODBC Driver 18 for SQL Server"
// and "ODBC Driver 17 for SQL Server" among `installed` (as the driver manager spells it).
// nullopt: none of them is installed.
std::optional<QString> pickDriver(const QString &requested, const QStringList &installed);

// Whether the driver understands Encrypt=strict: its name carries a version of 18 or more.
bool supportsStrictEncryption(const QString &driver);

// "host,port" or "host\instance".
QString serverSpec(const ConnSettings &settings);

// "Driver={...};Server=...;Database=...;UID=...;PWD={...};Encrypt=...;..." (PWD always braced,
// with every } doubled). Fails for Encrypt=strict with a driver older than 18.
Result<QString> buildConnectionString(const ConnSettings &settings, const QString &driver);

// sys.types name ("nvarchar", "uniqueidentifier"...) -> normalized type.
ValueType valueTypeForTypeName(QStringView typeName);

// "nvarchar(255)", "varbinary(max)", "decimal(10,2)", "datetime2(7)" from sys.columns values
// (max_length is in bytes, -1 for max).
QString nativeTypeName(const QString &typeName, int maxLength, int precision, int scale);

} // namespace quarry::mssql
