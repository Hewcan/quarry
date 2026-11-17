# SQL Server provider over ODBC. Everything shared with the generic provider lives in
# quarry_odbc_common (see its CMakeLists.txt for the deployment notes: Windows links odbc32 and
# needs nothing; Linux and macOS take libodbc and libodbcinst from vcpkg next to the executables).
# The Microsoft ODBC Driver 17/18 is installed by the user and found by name (SQLDrivers).
quarry_add_provider(mssql
    SOURCES
        mssql.json
        mssql_plugin.cpp
        mssql_provider.h
        mssql_provider.cpp
        mssql_connection.h
        mssql_connection.cpp
        mssql_introspection.cpp
        mssql_dialect.h
        mssql_dialect.cpp
        mssql_support.h
        mssql_support.cpp
        mssql_text.h
    LIBS quarry_odbc_common)
