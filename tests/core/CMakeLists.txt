quarry_add_test(tst_provider_registry SOURCES tst_provider_registry.cpp LIBS quarry_core)
target_compile_definitions(tst_provider_registry PRIVATE
    QUARRY_TEST_PLUGIN_DIR="${QUARRY_TEST_PLUGIN_OUTPUT_DIR}"
    QUARRY_PLUGIN_DIR="${QUARRY_PLUGIN_OUTPUT_DIR}")
# tcp_test_server.h uses core's private socket helpers.
target_include_directories(tst_provider_registry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/src)
add_dependencies(tst_provider_registry quarry_provider_dummy quarry_provider_dummy_future
    quarry_provider_sqlite quarry_provider_postgres)

quarry_add_test(tst_sql_safety SOURCES tst_sql_safety.cpp LIBS quarry_core)
quarry_add_test(tst_sql_context SOURCES tst_sql_context.cpp LIBS quarry_core)
quarry_add_test(tst_completion SOURCES tst_completion.cpp LIBS quarry_core)
quarry_add_test(tst_change_set SOURCES tst_change_set.cpp LIBS quarry_core)
quarry_add_test(tst_table_query SOURCES tst_table_query.cpp LIBS quarry_core)
quarry_add_test(tst_ddl_builder SOURCES tst_ddl_builder.cpp LIBS quarry_core)
quarry_add_test(tst_cell_formatter SOURCES tst_cell_formatter.cpp LIBS quarry_core)
quarry_add_test(tst_result SOURCES tst_result.cpp LIBS quarry_provider_api)
quarry_add_test(tst_column_buffer SOURCES tst_column_buffer.cpp LIBS quarry_provider_api)
quarry_add_test(tst_session SOURCES tst_session.cpp LIBS quarry_core)
quarry_add_test(tst_connection_store SOURCES tst_connection_store.cpp LIBS quarry_core unofficial::sqlite3::sqlite3)
quarry_add_test(tst_editor_tab_store SOURCES tst_editor_tab_store.cpp LIBS quarry_core unofficial::sqlite3::sqlite3)
quarry_add_test(tst_query_history_store SOURCES tst_query_history_store.cpp LIBS quarry_core unofficial::sqlite3::sqlite3)
quarry_add_test(tst_saved_query_store SOURCES tst_saved_query_store.cpp LIBS quarry_core unofficial::sqlite3::sqlite3)
quarry_add_test(tst_credential_store SOURCES tst_credential_store.cpp LIBS quarry_core)
quarry_add_test(tst_connection_tester SOURCES tst_connection_tester.cpp LIBS quarry_core)
target_compile_definitions(tst_connection_tester PRIVATE
    QUARRY_PLUGIN_DIR="${QUARRY_PLUGIN_OUTPUT_DIR}"
    QUARRY_SLOW_PLUGIN_DIR="${QUARRY_SLOW_PLUGIN_OUTPUT_DIR}")
add_dependencies(tst_connection_tester quarry_provider_sqlite quarry_provider_slow)
quarry_add_test(tst_schema_children SOURCES tst_schema_children.cpp LIBS quarry_core unofficial::sqlite3::sqlite3)
target_compile_definitions(tst_schema_children PRIVATE QUARRY_PLUGIN_DIR="${QUARRY_PLUGIN_OUTPUT_DIR}")
add_dependencies(tst_schema_children quarry_provider_sqlite)
quarry_add_test(tst_session_describe SOURCES tst_session_describe.cpp LIBS quarry_core unofficial::sqlite3::sqlite3)
target_compile_definitions(tst_session_describe PRIVATE QUARRY_PLUGIN_DIR="${QUARRY_PLUGIN_OUTPUT_DIR}")
add_dependencies(tst_session_describe quarry_provider_sqlite)
quarry_add_test(tst_metadata_cache SOURCES tst_metadata_cache.cpp LIBS quarry_core unofficial::sqlite3::sqlite3)
target_compile_definitions(tst_metadata_cache PRIVATE QUARRY_PLUGIN_DIR="${QUARRY_PLUGIN_OUTPUT_DIR}")
add_dependencies(tst_metadata_cache quarry_provider_sqlite)
quarry_add_test(tst_exporter SOURCES tst_exporter.cpp LIBS quarry_core unofficial::sqlite3::sqlite3)
target_compile_definitions(tst_exporter PRIVATE QUARRY_PLUGIN_DIR="${QUARRY_PLUGIN_OUTPUT_DIR}")
add_dependencies(tst_exporter quarry_provider_sqlite)
quarry_add_test(tst_migrations SOURCES tst_migrations.cpp LIBS quarry_core unofficial::sqlite3::sqlite3)
target_compile_definitions(tst_migrations PRIVATE QUARRY_PLUGIN_DIR="${QUARRY_PLUGIN_OUTPUT_DIR}")
add_dependencies(tst_migrations quarry_provider_sqlite)

quarry_add_test(tst_ai_settings SOURCES tst_ai_settings.cpp LIBS quarry_core unofficial::sqlite3::sqlite3)

# A stand-in for `php` (never installed): the runner tests point MigrationSettings::phpPath at it.
qt_add_executable(fake_php fake_php/fake_php.cpp)
target_link_libraries(fake_php PRIVATE Qt6::Core)
quarry_set_warnings(fake_php)
quarry_add_test(tst_migration_runner SOURCES tst_migration_runner.cpp LIBS quarry_core)
target_compile_definitions(tst_migration_runner PRIVATE QUARRY_FAKE_PHP="$<TARGET_FILE:fake_php>")
add_dependencies(tst_migration_runner fake_php)

# Dump and restore (H14). fake_pg_tool stands in for pg_dump, psql and pg_restore (never run).
qt_add_executable(fake_pg_tool fake_pg_tool/fake_pg_tool.cpp)
target_link_libraries(fake_pg_tool PRIVATE Qt6::Core)
quarry_set_warnings(fake_pg_tool)
quarry_add_test(tst_dump SOURCES tst_dump.cpp LIBS quarry_core unofficial::sqlite3::sqlite3
    ARGS -o "${CMAKE_BINARY_DIR}/tst_dump.log,txt" -o "-,txt")
target_compile_definitions(tst_dump PRIVATE QUARRY_FAKE_PG_TOOL="$<TARGET_FILE:fake_pg_tool>")
add_dependencies(tst_dump fake_pg_tool)

# SSH tunnels (H8). tst_ssh_tunnel reaches into src/core/src for the private socket helpers.
quarry_add_test(tst_ssh_tunnel_config SOURCES tst_ssh_tunnel_config.cpp LIBS quarry_core)
quarry_add_test(tst_known_hosts_store SOURCES tst_known_hosts_store.cpp LIBS quarry_core)
quarry_add_test(tst_ssh_tunnel SOURCES tst_ssh_tunnel.cpp tcp_test_server.h LIBS quarry_core)
target_include_directories(tst_ssh_tunnel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/src)
