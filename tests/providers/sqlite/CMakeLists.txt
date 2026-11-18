quarry_add_test(tst_sqlite_provider
    SOURCES tst_sqlite_provider.cpp
    LIBS quarry_core quarry_provider_conformance)
target_compile_definitions(tst_sqlite_provider PRIVATE
    QUARRY_PLUGIN_DIR="${QUARRY_PLUGIN_OUTPUT_DIR}")
add_dependencies(tst_sqlite_provider quarry_provider_sqlite)

# The parser is pure text: compiled in directly, no plugin needed.
quarry_add_test(tst_sqlite_explain
    SOURCES
        tst_sqlite_explain.cpp
        ${CMAKE_SOURCE_DIR}/src/providers/sqlite/sqlite_explain.cpp
    LIBS quarry_core)
target_include_directories(tst_sqlite_explain PRIVATE ${CMAKE_SOURCE_DIR}/src/providers/sqlite)
