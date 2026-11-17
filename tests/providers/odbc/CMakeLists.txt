# generic_settings.cpp (pure helpers, no driver manager calls) is compiled in directly so the
# server-free unit tests can call it; everything else goes through the plugin, like the app does.
# quarry_odbc_common brings the ODBC headers (SQL_* constants) and odbc_support.
quarry_add_test(tst_odbc_provider
    SOURCES
        tst_odbc_provider.cpp
        ${CMAKE_SOURCE_DIR}/src/providers/odbc/generic_settings.cpp
    LIBS quarry_core quarry_provider_conformance quarry_odbc_common)
target_include_directories(tst_odbc_provider PRIVATE ${CMAKE_SOURCE_DIR}/src/providers/odbc)
target_compile_definitions(tst_odbc_provider PRIVATE
    QUARRY_PLUGIN_DIR="${QUARRY_PLUGIN_OUTPUT_DIR}")
add_dependencies(tst_odbc_provider quarry_provider_odbc)
# One schema is created per conformance test.
set_tests_properties(tst_odbc_provider PROPERTIES TIMEOUT 600)
