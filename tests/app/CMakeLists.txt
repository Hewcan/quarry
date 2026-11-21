quarry_add_test(tst_command_registry SOURCES tst_command_registry.cpp LIBS quarry_ui)
quarry_add_test(tst_connection_list_model SOURCES tst_connection_list_model.cpp LIBS quarry_ui)
quarry_add_test(tst_connection_controller SOURCES tst_connection_controller.cpp LIBS quarry_ui)
quarry_add_test(tst_connection_form_model SOURCES tst_connection_form_model.cpp LIBS quarry_ui)
quarry_add_test(tst_connection_preflight SOURCES tst_connection_preflight.cpp LIBS quarry_ui)
quarry_add_test(tst_editor_tabs_model SOURCES tst_editor_tabs_model.cpp LIBS quarry_ui)
quarry_add_test(tst_migration_name SOURCES tst_migration_name.cpp LIBS quarry_ui)
quarry_add_test(tst_query_controller SOURCES tst_query_controller.cpp LIBS quarry_ui)
quarry_add_test(tst_plan_model SOURCES tst_plan_model.cpp LIBS quarry_ui)
quarry_add_test(tst_migration_controller SOURCES tst_migration_controller.cpp
    LIBS quarry_ui Qt6::Test unofficial::sqlite3::sqlite3)
quarry_add_test(tst_explain_controller SOURCES tst_explain_controller.cpp LIBS quarry_ui)
quarry_add_test(tst_export_controller SOURCES tst_export_controller.cpp LIBS quarry_ui)
quarry_add_test(tst_history_model SOURCES tst_history_model.cpp LIBS quarry_ui)
quarry_add_test(tst_saved_queries_model SOURCES tst_saved_queries_model.cpp LIBS quarry_ui)
quarry_add_test(tst_completion_controller SOURCES tst_completion_controller.cpp LIBS quarry_ui)
quarry_add_test(tst_result_table_model SOURCES tst_result_table_model.cpp LIBS quarry_ui)
quarry_add_test(tst_table_edit_model SOURCES tst_table_edit_model.cpp LIBS quarry_ui)
quarry_add_test(tst_data_editing SOURCES tst_data_editing.cpp LIBS quarry_ui)
find_package(Qt6 REQUIRED COMPONENTS QuickPrivate) # the grid accessibility spike derives from QAccessibleQuickItem
quarry_add_test(tst_grid_accessible SOURCES tst_grid_accessible.cpp LIBS quarry_ui Qt6::Quick Qt6::QuickPrivate)
quarry_add_test(tst_sql_document SOURCES tst_sql_document.cpp LIBS quarry_ui)
quarry_add_test(tst_schema_tree_model SOURCES tst_schema_tree_model.cpp
    LIBS quarry_ui unofficial::sqlite3::sqlite3)
foreach(t tst_connection_list_model tst_connection_controller tst_migration_controller tst_schema_tree_model tst_editor_tabs_model tst_query_controller tst_explain_controller tst_export_controller tst_data_editing tst_completion_controller tst_history_model tst_saved_queries_model)
    # The real SQLite plugin, like tests/core/tst_connection_tester.cpp.
    target_compile_definitions(${t} PRIVATE QUARRY_PLUGIN_DIR="${QUARRY_PLUGIN_OUTPUT_DIR}")
    add_dependencies(${t} quarry_provider_sqlite)
endforeach()
# The migration runner tests run the fake `php` of tests/core (never a real PHP or a real migration).
target_compile_definitions(tst_migration_controller PRIVATE QUARRY_FAKE_PHP="$<TARGET_FILE:fake_php>")
add_dependencies(tst_migration_controller fake_php)
# FakeDB (an engine that can analyze) plus the real SQLite for the end-to-end case.
add_subdirectory(fake_explain)
target_compile_definitions(tst_explain_controller PRIVATE
    QUARRY_FAKE_PLUGIN_DIR="${QUARRY_FAKE_EXPLAIN_PLUGIN_OUTPUT_DIR}")
add_dependencies(tst_explain_controller quarry_provider_fake_explain)
# FakeTree: several databases, a schema, users and indexes, for the object tree v2 (H12 phase A).
add_subdirectory(fake_tree)
target_compile_definitions(tst_schema_tree_model PRIVATE
    QUARRY_FAKE_TREE_DIR="${QUARRY_FAKE_TREE_PLUGIN_OUTPUT_DIR}")
add_dependencies(tst_schema_tree_model quarry_provider_fake_tree)
# It takes about a minute (10 000 tables, the search, the migrations folder): a loaded runner went over 60 s.
set_tests_properties(tst_schema_tree_model PROPERTIES TIMEOUT 180)
# The PostgreSQL variants of tst_data_editing load the real plugin (they skip without the URL).
add_dependencies(tst_data_editing quarry_provider_postgres)
add_dependencies(tst_completion_controller quarry_provider_postgres)
quarry_add_test(tst_language_manager SOURCES tst_language_manager.cpp LIBS quarry_ui)
quarry_add_test(tst_theme_contrast SOURCES tst_theme_contrast.cpp LIBS quarry_ui)
quarry_add_test(tst_theme_manager SOURCES tst_theme_manager.cpp LIBS quarry_ui)
quarry_add_test(tst_about_info SOURCES tst_about_info.cpp LIBS quarry_ui)

# Qt Quick Test: loads tst_controls.qml (and src/app/qml/Main.qml) against the static modules.
# The result also goes to a file: on Windows CI this test has failed now and then without leaving any
# output in the log, and the CI uploads the file when the job fails.
quarry_add_test(tst_controls SOURCES tst_controls_main.cpp
    LIBS quarry_ui quarry_uiplugin Qt6::QuickTest unofficial::sqlite3::sqlite3
    ARGS -o "${CMAKE_BINARY_DIR}/tst_controls.log,txt" -o "-,txt")
target_compile_definitions(tst_controls PRIVATE
    QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    QUARRY_PLUGIN_DIR="${QUARRY_PLUGIN_OUTPUT_DIR}"
    QUARRY_FAKE_PHP="$<TARGET_FILE:fake_php>")
add_dependencies(tst_controls quarry_provider_sqlite fake_php)
# Three QML test files, one of them (tst_connections) really saves and tests connections.
set_tests_properties(tst_controls PROPERTIES TIMEOUT 480)
if(WIN32)
    # Qt's offscreen platform finds no fonts on Windows, which collapses all text metrics to zero.
    set_property(TEST tst_controls APPEND PROPERTY ENVIRONMENT_MODIFICATION "QT_QPA_FONTDIR=set:$ENV{WINDIR}/Fonts")
endif()
