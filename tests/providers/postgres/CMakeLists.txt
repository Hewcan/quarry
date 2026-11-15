# postgres_support.cpp (pure helpers, no libpq) is compiled in directly so the server-free unit
# tests can call it; everything else goes through the plugin, like the app does.
quarry_add_test(tst_postgres_provider
    SOURCES
        tst_postgres_provider.cpp
        ${CMAKE_SOURCE_DIR}/src/providers/postgres/postgres_support.cpp
    LIBS quarry_core quarry_provider_conformance)
target_include_directories(tst_postgres_provider PRIVATE ${CMAKE_SOURCE_DIR}/src/providers/postgres)
target_compile_definitions(tst_postgres_provider PRIVATE
    QUARRY_PLUGIN_DIR="${QUARRY_PLUGIN_OUTPUT_DIR}")
add_dependencies(tst_postgres_provider quarry_provider_postgres)
# One database is created per conformance test.
set_tests_properties(tst_postgres_provider PROPERTIES TIMEOUT 600)
