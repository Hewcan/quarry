# Install layout and Qt deployment (ARCHITECTURE.md section 15).
#
#   <prefix>/bin/quarry[.exe]
#   <prefix>/bin/plugins/quarry/quarry_provider_*      the app looks in <exe dir>/plugins/quarry
#   <prefix>/bin/mariadb-plugins/                       Connector/C authentication plugins
#   <prefix>/bin/<native DLLs>                          Windows: libpq, OpenSSL, libmariadb, libssh2...
#   Qt libraries, plugins and QML modules, qt.conf      written by the Qt deploy script below
#   <prefix>/{LICENSE,NOTICE,THIRD_PARTY_LICENSES.txt}
#
# Nothing here signs or notarizes: that belongs to the release workflow (docs/ARCHITECTURE.md 15).
include(GNUInstallDirs)

set(_quarry_providers sqlite postgres mysql odbc mssql)

install(TARGETS quarry
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    BUNDLE DESTINATION .)

foreach(_id IN LISTS _quarry_providers)
    install(TARGETS quarry_provider_${_id}
        LIBRARY DESTINATION "${CMAKE_INSTALL_BINDIR}/plugins/quarry"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}/plugins/quarry")
endforeach()

if(WIN32)
    # The Windows loader does not look in plugins/quarry for the providers' dependencies, so they
    # sit next to quarry.exe (the same reason the build tree does it, see the provider CMake files).
    # file(GET_RUNTIME_DEPENDENCIES) follows the imports of quarry.exe and of every provider through
    # vcpkg's bin directory (libpq, OpenSSL, libmariadb, libssh2 and what they import: zlib...).
    # Qt's own DLLs (Qt6*.dll; vcpkg's qt6keychain.dll is lowercase) are left to windeployqt; the
    # Windows system ones are not copied.
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_quarry_vcpkg_bin "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin")
    else()
        set(_quarry_vcpkg_bin "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    endif()
    set(_quarry_modules "")
    foreach(_id IN LISTS _quarry_providers)
        string(APPEND _quarry_modules " \"$<TARGET_FILE:quarry_provider_${_id}>\"")
    endforeach()
    install(CODE "
        if(POLICY CMP0207)
            cmake_policy(SET CMP0207 NEW) # normalized paths, so the regexes see forward slashes
        endif()
        file(GET_RUNTIME_DEPENDENCIES
            EXECUTABLES \"$<TARGET_FILE:quarry>\"
            MODULES ${_quarry_modules}
            RESOLVED_DEPENDENCIES_VAR _quarry_resolved
            UNRESOLVED_DEPENDENCIES_VAR _quarry_unresolved
            CONFLICTING_DEPENDENCIES_PREFIX _quarry_conflict
            DIRECTORIES \"${_quarry_vcpkg_bin}\"
            PRE_EXCLUDE_REGEXES \"^api-ms-\" \"^ext-ms-\" \"^Qt6\"
            POST_EXCLUDE_REGEXES \"[Ss]ystem32\")
        # Qt6*.dll sit in Qt's bin directory, which is not searched here: windeployqt copies them.
        list(FILTER _quarry_unresolved EXCLUDE REGEX \"^Qt6\")
        if(_quarry_unresolved)
            message(FATAL_ERROR \"Unresolved DLL dependencies: \${_quarry_unresolved}\")
        endif()
        # The build tree root holds copies of the same DLLs (the providers' post-build steps): the
        # first of each conflicting pair is as good as the other.
        foreach(_quarry_name IN LISTS _quarry_conflict_FILENAMES)
            list(GET _quarry_conflict_\${_quarry_name} 0 _quarry_first)
            list(APPEND _quarry_resolved \"\${_quarry_first}\")
        endforeach()
        file(INSTALL \${_quarry_resolved} DESTINATION \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
    ")

    # The Visual C++ runtime next to the executable (app-local), not the vc_redist installer The Universal CRT
    # is part of Windows 10, so it is not copied.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
    include(InstallRequiredSystemLibraries)
endif()

# Created by the mysql provider's post-build step on every platform that has them.
install(DIRECTORY "${CMAKE_BINARY_DIR}/mariadb-plugins"
    DESTINATION "${CMAKE_INSTALL_BINDIR}" OPTIONAL)

install(FILES LICENSE NOTICE "${CMAKE_BINARY_DIR}/THIRD_PARTY_LICENSES.txt" DESTINATION .)

# Qt: libraries, platform and QML plugins, the QML modules the app imports (QtQuick.Dialogs
# included, found by the QML import scanner) and qt.conf. Translations stay out: the app ships its
# own .qm files inside the executable.
if(WIN32)
    # windeployqt on quarry.exe fails on qt6keychain.dll (see packaging/windows/deploy_probe.cpp), so
    # it scans a probe with the same Qt modules and copies next to quarry.exe. Build the probe with
    # `cmake --build <dir> --target quarry_deploy_probe` before `cmake --install`.
    add_executable(quarry_deploy_probe EXCLUDE_FROM_ALL packaging/windows/deploy_probe.cpp)
    target_link_libraries(quarry_deploy_probe PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
    set(_windeployqt "$<TARGET_FILE_DIR:Qt6::Core>/windeployqt.exe")
    install(CODE "
        execute_process(
            COMMAND \"${_windeployqt}\" --dir \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\"
                    --release --no-translations --no-opengl-sw --no-system-d3d-compiler
                    --no-compiler-runtime --skip-plugin-types iconengines,qmltooling,networkinformation,tls,generic
                    --no-system-dxc-compiler --qmldir \"${CMAKE_CURRENT_SOURCE_DIR}/src/app/qml\"
                    \"$<TARGET_FILE:quarry_deploy_probe>\"
            RESULT_VARIABLE _quarry_windeployqt_result)
        if(NOT _quarry_windeployqt_result EQUAL 0)
            message(FATAL_ERROR \"windeployqt failed: \${_quarry_windeployqt_result}\")
        endif()
    ")
    # The app sets the Basic style itself; the other Quick Controls styles windeployqt copies would
    # add about 15 MB for nothing.
    install(CODE "
        set(_quarry_bin \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
        foreach(_style FluentWinUI3 Fusion Imagine Material Universal Windows Native)
            file(GLOB _quarry_style_dlls \"\${_quarry_bin}/Qt6QuickControls2\${_style}*.dll\")
            file(REMOVE \${_quarry_style_dlls})
            file(REMOVE_RECURSE \"\${_quarry_bin}/qml/QtQuick/Controls/\${_style}\")
        endforeach()
    ")
else()
    # shortcut: macOS and Linux install the layout only. The relocatable bundles (macdeployqt for the
    # .app, linuxdeploy for the AppImage) belong to the macOS and Linux phases of H10, and a tree
    # installed here still finds Qt and the vcpkg libraries through the build's RPATH.
endif()
