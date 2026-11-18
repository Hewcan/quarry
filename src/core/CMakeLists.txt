# Provider plugin API: contract between core and providers/* (ARCHITECTURE.md section 4).
# STATIC because providers need ColumnBuffer's implementation; PIC because it is linked into
# MODULE plugins on Linux.
add_library(quarry_provider_api STATIC
    api/include/quarry/cancellation.h
    api/include/quarry/column_buffer.h
    api/include/quarry/explain_plan.h
    api/include/quarry/object_path.h
    api/include/quarry/provider_api.h
    api/include/quarry/result.h
    api/include/quarry/result_batch.h
    api/include/quarry/value_type.h
    api/src/column_buffer.cpp
)
target_include_directories(quarry_provider_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/api/include)
target_link_libraries(quarry_provider_api PUBLIC Qt6::Core)
set_target_properties(quarry_provider_api PROPERTIES POSITION_INDEPENDENT_CODE ON)
quarry_set_warnings(quarry_provider_api)
quarry_assert_no_ui_deps(quarry_provider_api)

# GLOBAL: consumers of the static quarry_core need these targets resolvable in their own scope.
find_package(Qt6Keychain CONFIG REQUIRED GLOBAL)
find_package(unofficial-sqlite3 CONFIG REQUIRED GLOBAL)
find_package(libssh2 CONFIG REQUIRED GLOBAL)

# Core: no UI dependencies, enforced below.
qt_add_library(quarry_core STATIC
    include/quarry/cell_formatter.h
    include/quarry/change_set.h
    include/quarry/connection_profile.h
    include/quarry/connection_store.h
    include/quarry/connection_tester.h
    include/quarry/completion.h
    include/quarry/credential_store.h
    include/quarry/editor_tab.h
    include/quarry/editor_tab_store.h
    include/quarry/export_format.h
    include/quarry/export_options.h
    include/quarry/exporter.h
    include/quarry/history_entry.h
    include/quarry/history_query.h
    include/quarry/metadata_cache.h
    include/quarry/provider_registry.h
    include/quarry/query_history_store.h
    include/quarry/saved_folder.h
    include/quarry/saved_query.h
    include/quarry/saved_query_store.h
    include/quarry/session.h
    include/quarry/sql_context.h
    include/quarry/sql_safety.h
    include/quarry/ssh_host_key.h
    include/quarry/ssh_tunnel_config.h
    src/app_database.h
    src/app_database.cpp
    src/cell_formatter.cpp
    src/change_set.cpp
    src/connection_store.cpp
    src/connection_store_worker.h
    src/connection_store_worker.cpp
    src/connection_tester.cpp
    src/completion.cpp
    src/credential_store.cpp
    src/editor_tab_store.cpp
    src/editor_tab_store_worker.h
    src/editor_tab_store_worker.cpp
    src/export_sink.h
    src/export_sink.cpp
    src/export_text.h
    src/export_text.cpp
    src/export_writer.h
    src/export_writers.cpp
    src/exporter.cpp
    src/exporter_worker.h
    src/exporter_worker.cpp
    src/metadata_cache.cpp
    src/provider_registry.cpp
    src/query_history_store.cpp
    src/query_history_store_worker.h
    src/query_history_store_worker.cpp
    src/saved_query_store.cpp
    src/saved_query_store_worker.h
    src/saved_query_store_worker.cpp
    src/session.cpp
    src/session_worker.h
    src/session_worker.cpp
    src/sql_lexer.h
    src/sql_lexer.cpp
    src/sql_context.cpp
    src/sql_safety.cpp
    src/known_hosts_store.cpp
    src/ssh_host_key_checker.cpp
    src/ssh_tunnel_config.cpp
    src/ssh_tunnelled_connection.h
    src/ssh_tunnelled_connection.cpp
    src/ssh/ssh_client.h
    src/ssh/ssh_client.cpp
    src/ssh/ssh_net.h
    src/ssh/ssh_net.cpp
    src/ssh/ssh_tunnel.h
    src/ssh/ssh_tunnel.cpp
)
target_include_directories(quarry_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(quarry_core PUBLIC quarry_provider_api Qt6::Core
    PRIVATE Qt6Keychain::Qt6Keychain unofficial::sqlite3::sqlite3 libssh2::libssh2)
if(WIN32)
    # Raw sockets for the SSH tunnel (connect, accept, WSAPoll).
    target_link_libraries(quarry_core PRIVATE ws2_32)
    target_compile_definitions(quarry_core PRIVATE NOMINMAX)
endif()
quarry_set_warnings(quarry_core)
quarry_assert_no_ui_deps(quarry_core)

# Milestone 1 dependency: resolved now so the vcpkg wiring is proven from H0 on.
find_package(PostgreSQL REQUIRED)
