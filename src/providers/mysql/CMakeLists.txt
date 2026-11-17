# MariaDB Connector/C comes from vcpkg on every platform (docs/ARCHITECTURE.md section 8).
find_package(unofficial-libmariadb CONFIG REQUIRED)

# Windows: the loader does not look in the plugin's own folder for its dependencies, and
# ProviderRegistry::scan() would reject every native DLL it finds there as "not a plugin". So
# libmariadb (and zlib) go next to the executables (the build root, where quarry.exe and the
# tests are), not next to the plugin. vcpkg's applocal step would copy them into the plugin
# folder.
set(VCPKG_APPLOCAL_DEPS OFF)

quarry_add_provider(mysql
    SOURCES
        mysql.json
        mysql_plugin.cpp
        mysql_provider.h
        mysql_provider.cpp
        mysql_connection.h
        mysql_connection.cpp
        mysql_introspection.cpp
        mysql_stream.h
        mysql_stream.cpp
        mysql_dialect.h
        mysql_dialect.cpp
        mysql_errors.h
        mysql_errors.cpp
        mysql_state.h
        mysql_state.cpp
        mysql_support.h
        mysql_support.cpp
        mysql_text.h
    LIBS unofficial::libmariadb)

# Connector/C authentication plugins (caching_sha2_password for MySQL 8.4, sha256_password,
# mysql_clear_password, client_ed25519, dialog) are shared libraries that libmariadb loads at run
# time from MYSQL_PLUGIN_DIR. The provider sets that option to <exe dir>/mariadb-plugins
# (mysql_support.cpp, pluginDirectory()), so they are copied there and never into plugins/quarry.
set(_maria_plugin_names caching_sha2_password sha256_password mysql_clear_password
    client_ed25519 dialog)

if(WIN32)
    # The presets are single-config (Ninja), so the config is known here.
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        get_target_property(_maria_lib unofficial::libmariadb IMPORTED_LOCATION_DEBUG)
    else()
        get_target_property(_maria_lib unofficial::libmariadb IMPORTED_LOCATION_RELEASE)
    endif()
    if(NOT _maria_lib)
        message(FATAL_ERROR "libmariadb runtime DLL not found for ${CMAKE_BUILD_TYPE}")
    endif()
    get_filename_component(_maria_bin_dir "${_maria_lib}" DIRECTORY)
    file(GLOB _maria_runtime_dlls
        "${_maria_bin_dir}/libmariadb*.dll"
        "${_maria_bin_dir}/z.dll"
        "${_maria_bin_dir}/zd.dll")
    set(_maria_plugins)
    foreach(_name IN LISTS _maria_plugin_names)
        set(_file "${_maria_bin_dir}/../plugins/libmariadb/${_name}.dll")
        if(NOT EXISTS "${_file}")
            message(FATAL_ERROR "MariaDB authentication plugin not found: ${_file}")
        endif()
        list(APPEND _maria_plugins "${_file}")
    endforeach()
    add_custom_command(TARGET quarry_provider_mysql POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_maria_runtime_dlls}
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/mariadb-plugins"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_maria_plugins}
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/mariadb-plugins"
        VERBATIM)
else()
    # Linux: libmariadb.so is found through the RPATH that vcpkg's toolchain gives to the
    # executables in the build tree; a packaged build needs RPATH $ORIGIN (macOS: @loader_path)
    # so it resolves next to the executable, with OpenSSL. Same layout for the plugins:
    # <exe dir>/mariadb-plugins/<name>.so (.dylib on macOS). Not verified here (CI): the
    # vcpkg port installs them below <prefix>/plugins/libmariadb or <prefix>/lib/mariadb/plugin.
    get_target_property(_maria_lib unofficial::libmariadb IMPORTED_LOCATION_RELEASE)
    if(NOT _maria_lib)
        get_target_property(_maria_lib unofficial::libmariadb IMPORTED_LOCATION_DEBUG)
    endif()
    if(_maria_lib)
        get_filename_component(_maria_lib_dir "${_maria_lib}" DIRECTORY)
        set(_maria_plugins)
        foreach(_name IN LISTS _maria_plugin_names)
            foreach(_dir "${_maria_lib_dir}/../plugins/libmariadb" "${_maria_lib_dir}/mariadb/plugin"
                         "${_maria_lib_dir}/plugin")
                foreach(_ext so dylib)
                    if(EXISTS "${_dir}/${_name}.${_ext}")
                        list(APPEND _maria_plugins "${_dir}/${_name}.${_ext}")
                    endif()
                endforeach()
            endforeach()
        endforeach()
        if(_maria_plugins)
            add_custom_command(TARGET quarry_provider_mysql POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/mariadb-plugins"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_maria_plugins}
                        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/mariadb-plugins"
                VERBATIM)
        else()
            message(WARNING "MariaDB authentication plugins not found: MySQL 8.4 logins "
                            "(caching_sha2_password) will not work in this build")
        endif()
    endif()
endif()
