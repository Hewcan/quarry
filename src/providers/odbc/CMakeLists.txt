# Generic ODBC provider: any driver or data source. Everything shared with the SQL Server
# provider lives in quarry_odbc_common (see its CMakeLists.txt for the deployment notes: Windows
# links odbc32 and needs nothing; Linux and macOS take libodbc and libodbcinst from vcpkg next to
# the executables).
quarry_add_provider(odbc
    SOURCES
        odbc.json
        odbc_plugin.cpp
        odbc_provider.h
        odbc_provider.cpp
        generic_connection.h
        generic_connection.cpp
        generic_dialect.h
        generic_dialect.cpp
        generic_settings.h
        generic_settings.cpp
    LIBS quarry_odbc_common)
