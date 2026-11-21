# quarry_ui: static library with the C++ UI types (ThemeManager = QML singleton `Theme`) and the
# QML module Quarry.Controls (accessible controls, Icons singleton, icon font). Tests link it.
# Linking the quarry_uiplugin target registers the module in a static link.
# Warning flags on our own sources only: MSVC flags C4702 inside Qt headers as instantiated by the
# generated qmlcache code, which must not fail the build.
find_package(KF6SyntaxHighlighting CONFIG REQUIRED) # the SQL editor's highlighting (sql_document.cpp)

function(quarry_app_warnings)
    if(MSVC)
        set(flags /W4 /permissive- /utf-8 /Zc:__cplusplus $<$<BOOL:${QUARRY_WARNINGS_AS_ERRORS}>:/WX>)
    else()
        set(flags -Wall -Wextra -Wpedantic $<$<BOOL:${QUARRY_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()
    set_property(SOURCE ${ARGN} APPEND PROPERTY COMPILE_OPTIONS ${flags})
endfunction()

set(QUARRY_CONTROLS_QML
    FocusRing Heading Icon IconButton QButton QCheckBox QComboBox QDialog QMenu QMenuBar QMenuBarItem QMenuItem
    QMenuSeparator QScrollBar QSplitView QTabBar QTabButton QTextField QToolTip)

# Flat resource layout: /qt/qml/Quarry/Controls/<Name>.qml and .../lucide-subset.ttf
set(controls_qml_files)
foreach(name IN LISTS QUARRY_CONTROLS_QML)
    set(file qml/Quarry/Controls/${name}.qml)
    set_source_files_properties(${file} PROPERTIES QT_RESOURCE_ALIAS ${name}.qml)
    list(APPEND controls_qml_files ${file})
endforeach()
set_source_files_properties(qml/Quarry/Controls/Icons.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
    QT_RESOURCE_ALIAS Icons.qml)
set_source_files_properties(assets/icons/lucide-subset.ttf PROPERTIES
    QT_RESOURCE_ALIAS lucide-subset.ttf)

qt_add_library(quarry_ui STATIC)
qt_add_qml_module(quarry_ui
    URI Quarry.Controls
    VERSION 1.0
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Quarry/Controls
    SOURCES
        ui/about_info.cpp
        ui/about_info.h
        ui/command.cpp
        ui/command.h
        ui/command_registry.cpp
        ui/command_registry.h
        ui/completion_controller.cpp
        ui/completion_controller.h
        ui/completion_model.cpp
        ui/completion_model.h
        ui/connection_controller.cpp
        ui/connection_controller.h
        ui/connection_form_model.cpp
        ui/connection_form_model.h
        ui/connection_list_model.cpp
        ui/connection_list_model.h
        ui/connection_preflight.cpp
        ui/connection_preflight.h
        ui/editor_tabs_model.cpp
        ui/editor_tabs_model.h
        ui/export_controller.cpp
        ui/export_controller.h
        ui/history_model.cpp
        ui/history_model.h
        ui/language_manager.cpp
        ui/language_manager.h
        ui/layout_store.cpp
        ui/layout_store.h
        ui/migration_controller.cpp
        ui/migration_controller.h
        ui/migration_list_model.cpp
        ui/migration_list_model.h
        ui/plan_model.cpp
        ui/plan_model.h
        ui/query_controller.cpp
        ui/query_controller.h
        ui/query_messages_model.cpp
        ui/query_messages_model.h
        ui/query_state.cpp
        ui/query_state.h
        ui/result_table_model.cpp
        ui/result_table_model.h
        ui/saved_queries_model.cpp
        ui/saved_queries_model.h
        ui/schema_tree_model.cpp
        ui/schema_tree_model.h
        ui/sql_document.cpp
        ui/sql_document.h
        ui/table_edit_model.cpp
        ui/table_edit_model.h
        ui/theme_manager.cpp
        ui/theme_manager.h
        ui/theme_tokens.cpp
        ui/theme_tokens.h
    QML_FILES
        ${controls_qml_files}
        qml/Quarry/Controls/Icons.qml
    RESOURCES
        assets/icons/lucide-subset.ttf
    DEPENDENCIES QtQuick QtQuick.Templates
)
# ui/ too: the generated QML registration code includes the headers by bare file name.
target_include_directories(quarry_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/ui)
# KSyntaxHighlighting is dynamic (vcpkg, built against our Qt); it is found below.
target_link_libraries(quarry_ui PUBLIC quarry_core Qt6::Gui Qt6::Qml Qt6::Quick KF6::SyntaxHighlighting)
quarry_app_warnings(
    ui/about_info.cpp ui/command.cpp ui/command_registry.cpp ui/completion_controller.cpp ui/completion_model.cpp
    ui/connection_controller.cpp ui/connection_form_model.cpp ui/connection_preflight.cpp
    ui/connection_list_model.cpp ui/editor_tabs_model.cpp ui/export_controller.cpp ui/history_model.cpp ui/language_manager.cpp ui/layout_store.cpp
    ui/migration_controller.cpp ui/migration_list_model.cpp
    ui/plan_model.cpp ui/query_controller.cpp ui/query_messages_model.cpp ui/query_state.cpp ui/result_table_model.cpp
    ui/saved_queries_model.cpp
    ui/schema_tree_model.cpp ui/sql_document.cpp ui/table_edit_model.cpp
    ui/theme_manager.cpp ui/theme_tokens.cpp)

qt_add_executable(quarry main.cpp)

# One version for everything: project(VERSION) -> the app (About, applicationVersion), the Windows
# file properties and the installers.
target_compile_definitions(quarry PRIVATE QUARRY_VERSION="${PROJECT_VERSION}")
if(WIN32)
    set(QUARRY_ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/assets/app/quarry.ico")
    configure_file(quarry.rc.in "${CMAKE_CURRENT_BINARY_DIR}/quarry.rc" @ONLY)
    target_sources(quarry PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/quarry.rc")
endif()

qt_add_qml_module(quarry
    URI Quarry
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/MainWindow.qml
        qml/CommandPalette.qml
        qml/AppMenuBar.qml
        qml/SettingsDialog.qml
        qml/AboutDialog.qml
        qml/EmptyState.qml
        qml/Region.qml
        qml/ConnectionList.qml
        qml/PasswordDialog.qml
        qml/HostKeyDialog.qml
        qml/ConnectionDialog.qml
        qml/ColorPicker.qml
        qml/ConnectionTint.qml
        qml/DeleteConnectionDialog.qml
        qml/ProductionBadge.qml
        qml/FilePicker.qml
        qml/ErrorPanel.qml
        qml/EditorArea.qml
        qml/SqlEditor.qml
        qml/CompletionPopup.qml
        qml/SuggestionRow.qml
        qml/CloseTabDialog.qml
        qml/ConfirmRunDialog.qml
        qml/ConfirmAnalyzeDialog.qml
        qml/PlanView.qml
        qml/ResultsPanel.qml
        qml/QuarryGrid.qml
        qml/DataTab.qml
        qml/PreviewSqlDialog.qml
        qml/ApplyConfirmDialog.qml
        qml/DiscardChangesDialog.qml
        qml/ConfirmActionDialog.qml
        qml/NameDialog.qml
        qml/MigrationsDialog.qml
        qml/MoveToDialog.qml
        qml/HistoryView.qml
        qml/SavedView.qml
        qml/LibraryPanel.qml
        qml/ExportDialog.qml
        qml/ExportFilePicker.qml
    DEPENDENCIES QtQuick QtQuick.Controls.Basic QtQuick.Layouts QtQml Quarry.Controls
)

target_link_libraries(quarry PRIVATE quarry_core quarry_ui quarry_uiplugin Qt6::Quick Qt6::QuickControls2)

# The About dialog reads the licenses of what is shipped from :/licenses/THIRD_PARTY_LICENSES.txt
# (generated at configure time, cmake/ThirdPartyLicenses.cmake).
# Its own object library: lupdate scans the resource files of the SOURCE_TARGETS below, and a license
# text is not C++ (thousands of parse warnings).
add_library(quarry_licenses OBJECT)
qt_add_resources(quarry_licenses licenses
    PREFIX /licenses
    BASE "${CMAKE_BINARY_DIR}"
    FILES "${CMAKE_BINARY_DIR}/THIRD_PARTY_LICENSES.txt")
target_sources(quarry PRIVATE $<TARGET_OBJECTS:quarry_licenses>)
# The About dialog's picture, at the path AboutDialog.qml gives it relative to itself (qml/ -> ../assets).
qt_add_resources(quarry about_icon
    PREFIX /qt/qml/Quarry
    FILES assets/app/quarry-256.png)
quarry_app_warnings(main.cpp)
set_target_properties(quarry PROPERTIES WIN32_EXECUTABLE ON)

# Spanish UI strings. The .qm files are generated into the build tree and embedded as
# :/i18n/quarry_es.qm (loaded by LanguageManager); only the .ts is versioned.
# After changing any visible string: cmake --build <dir> --target update_translations
find_package(Qt6 REQUIRED COMPONENTS LinguistTools)
qt_add_translations(quarry_ui
    SOURCE_TARGETS quarry quarry_ui
    TS_FILES i18n/quarry_es.ts
    RESOURCE_PREFIX /i18n
    LUPDATE_OPTIONS -locations none
    NO_GENERATE_PLURALS_TS_FILE)


add_test(NAME quarry-smoke COMMAND quarry --smoke)
set_tests_properties(quarry-smoke PROPERTIES
    TIMEOUT 30
    # shortcut: on Linux vcpkg's libqt6keychain.so (RUNPATH=$ORIGIN) does not find libQt6DBus in the
    # aqt Qt dir, so a build-tree quarry needs LD_LIBRARY_PATH; H10 packaging must bundle/fix it.
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_QPA_PLATFORM=set:offscreen"
)
