cmake_minimum_required(VERSION 3.24)

# Before project(): exports QT_ROOT_DIR for the vcpkg manifest install (Qt shim ports).
include(cmake/QtLocate.cmake)

project(quarry VERSION 0.0.1 LANGUAGES CXX)
if(WIN32)
    enable_language(RC) # quarry.rc: icon and file version
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# All executables at the build root on every platform: the app finds providers in
# <exe dir>/plugins/quarry, and src/app/quarry would clash with the QML module dir
# src/app/Quarry on case-insensitive file systems (macOS).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

find_package(Qt6 ${QUARRY_QT_VERSION} EXACT REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 QuickTemplates2 QuickTest Test)
qt_standard_project_setup(REQUIRES ${QUARRY_QT_VERSION})

include(cmake/QuarryHelpers.cmake)
include(cmake/ThirdPartyLicenses.cmake)

enable_testing()

add_subdirectory(src/core)
add_subdirectory(src/providers)
add_subdirectory(src/app)
include(cmake/QuarryInstall.cmake)
add_subdirectory(tests)
add_subdirectory(tools/env-check)
add_subdirectory(tools/bench)
