# mysql_support.cpp and mysql_dialect.cpp (pure helpers, no libmariadb calls) are compiled in
# directly so the server-free unit tests can call them; everything else goes through the plugin,
# like the app does. The libmariadb target only supplies the headers (MYSQL_TYPE_* constants).
find_package(unofficial-libmariadb CONFIG REQUIRED)

quarry_add_test(tst_mysql_provider
    SOURCES
        tst_mysql_provider.cpp
        ${CMAKE_SOURCE_DIR}/src/providers/mysql/mysql_support.cpp
    LIBS quarry_core quarry_provider_conformance unofficial::libmariadb)
target_include_directories(tst_mysql_provider PRIVATE ${CMAKE_SOURCE_DIR}/src/providers/mysql)
target_compile_definitions(tst_mysql_provider PRIVATE
    QUARRY_PLUGIN_DIR="${QUARRY_PLUGIN_OUTPUT_DIR}")
add_dependencies(tst_mysql_provider quarry_provider_mysql)
# One database is created per conformance test.
set_tests_properties(tst_mysql_provider PROPERTIES TIMEOUT 600)

# The parsers are pure text: compiled in directly, no plugin and no driver needed.
quarry_add_test(tst_mysql_explain
    SOURCES
        tst_mysql_explain.cpp
        ${CMAKE_SOURCE_DIR}/src/providers/mysql/mysql_explain.cpp
        ${CMAKE_SOURCE_DIR}/src/providers/mysql/mariadb_explain.cpp
    LIBS quarry_core)
target_include_directories(tst_mysql_explain PRIVATE ${CMAKE_SOURCE_DIR}/src/providers/mysql)
