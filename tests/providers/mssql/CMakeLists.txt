# mssql_support.cpp (pure helpers, no driver manager calls) is compiled in directly so the
# server-free unit tests can call it; everything else goes through the plugin, like the app does.
quarry_add_test(tst_mssql_provider
    SOURCES
        tst_mssql_provider.cpp
        ${CMAKE_SOURCE_DIR}/src/providers/mssql/mssql_support.cpp
    LIBS quarry_core quarry_provider_conformance quarry_odbc_common)
target_include_directories(tst_mssql_provider PRIVATE ${CMAKE_SOURCE_DIR}/src/providers/mssql)
target_compile_definitions(tst_mssql_provider PRIVATE
    QUARRY_PLUGIN_DIR="${QUARRY_PLUGIN_OUTPUT_DIR}")
add_dependencies(tst_mssql_provider quarry_provider_mssql)
# One database is created per conformance test.
set_tests_properties(tst_mssql_provider PROPERTIES TIMEOUT 600)
