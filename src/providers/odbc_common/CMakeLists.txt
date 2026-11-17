# Code shared by the ODBC providers (odbc and mssql): environment and connection handles, the
# result stream, type and error maps, cancellation (docs/ARCHITECTURE.md section 6.7). A static
# library: each plugin carries its own copy, so a plugin never depends on the other.
#
# ODBC itself: Windows links odbc32 from the SDK (nothing to deploy). Linux and macOS use
# unixODBC from vcpkg (libodbc, libodbcinst), which is dynamic on purpose: the drivers load their
# own libodbcinst, so there can only be one copy in the process. A packaged build puts libodbc
# and libodbcinst next to the executables (RPATH $ORIGIN, macOS @loader_path), as it does with
# libpq; in the build tree vcpkg's toolchain gives the executables the RPATH. Not verified here
# (CI). The driver manager looks for odbcinst.ini under its vcpkg prefix: drivers registered by
# name in /etc need ODBCSYSINI=/etc, DRIVER={path} does not.
find_package(ODBC REQUIRED)

add_library(quarry_odbc_common STATIC
    odbc_headers.h
    odbc_text.h
    odbc_support.h
    odbc_support.cpp
    odbc_diag.h
    odbc_diag.cpp
    odbc_state.h
    odbc_state.cpp
    odbc_data.h
    odbc_data.cpp
    odbc_stream.h
    odbc_stream.cpp
    odbc_connection.h
    odbc_connection.cpp)
target_include_directories(quarry_odbc_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(quarry_odbc_common PUBLIC quarry_provider_api Qt6::Core ODBC::ODBC)
set_target_properties(quarry_odbc_common PROPERTIES POSITION_INDEPENDENT_CODE ON)
quarry_set_warnings(quarry_odbc_common)
