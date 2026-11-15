# libpq comes from vcpkg on every platform (docs/ARCHITECTURE.md section 8).
find_package(PostgreSQL REQUIRED)

# Windows: the loader does not look in the plugin's own folder for its dependencies, and
# ProviderRegistry::scan() would reject every native DLL it finds there as "not a plugin". So
# libpq and OpenSSL go next to the executables (the build root, where quarry.exe and the tests
# are), not next to the plugin. vcpkg's applocal step would copy them into the plugin folder.
set(VCPKG_APPLOCAL_DEPS OFF)

quarry_add_provider(postgres
    SOURCES
        postgres.json
        postgres_plugin.cpp
        postgres_provider.h
        postgres_provider.cpp
        postgres_connection.h
        postgres_connection.cpp
        postgres_introspection.cpp
        postgres_stream.h
        postgres_stream.cpp
        postgres_dialect.h
        postgres_dialect.cpp
        postgres_errors.h
        postgres_errors.cpp
        postgres_state.h
        postgres_support.h
        postgres_support.cpp
        postgres_text.h
    LIBS PostgreSQL::PostgreSQL)

if(WIN32)
    # The presets are single-config (Ninja), so the config is known here.
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND PostgreSQL_LIBRARY_DEBUG)
        set(_pg_lib "${PostgreSQL_LIBRARY_DEBUG}")
    else()
        set(_pg_lib "${PostgreSQL_LIBRARY_RELEASE}")
    endif()
    get_filename_component(_pg_lib_dir "${_pg_lib}" DIRECTORY)
    file(GLOB _pg_runtime_dlls
        "${_pg_lib_dir}/../bin/libpq*.dll"
        "${_pg_lib_dir}/../bin/libssl*.dll"
        "${_pg_lib_dir}/../bin/libcrypto*.dll")
    if(NOT _pg_runtime_dlls)
        message(FATAL_ERROR "libpq runtime DLLs not found next to ${_pg_lib}")
    endif()
    add_custom_command(TARGET quarry_provider_postgres POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_pg_runtime_dlls}
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        VERBATIM)
endif()
