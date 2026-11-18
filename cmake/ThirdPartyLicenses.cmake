# Writes ${CMAKE_BINARY_DIR}/THIRD_PARTY_LICENSES.txt at configure time: the licenses of what Quarry
# ships. The app embeds it as :/licenses/THIRD_PARTY_LICENSES.txt (About dialog) and the packages
# install it next to LICENSE and NOTICE. Nothing is written by hand: vcpkg's copyright file of every
# installed port is concatenated, so a new dependency shows up here without touching anything.
#
# Qt itself is not a vcpkg package here (shim ports, see vcpkg-overlays), so its notice and the
# Lucide icons' license are added explicitly.
set(_licenses_file "${CMAKE_BINARY_DIR}/THIRD_PARTY_LICENSES.txt")
set(_licenses_share "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")

# Build-time only ports and tooling: nothing of them reaches the package.
set(_licenses_skip ECM extra-cmake-modules qtbase qttools qlogging-categories6 doc man)

set(_licenses_text "Third-party software in Quarry\n")
string(APPEND _licenses_text "==============================\n\n")

string(APPEND _licenses_text "Qt 6\n---\n")
string(APPEND _licenses_text
    "Quarry uses the Qt framework (https://www.qt.io), licensed under the GNU Lesser General Public\n"
    "License version 3 (LGPLv3), and links it dynamically: the Qt libraries are separate files in the\n"
    "installation and can be replaced by another build of the same Qt version. The Qt source code is\n"
    "available at https://download.qt.io/official_releases/qt/ and the text of the license at\n"
    "https://www.gnu.org/licenses/lgpl-3.0.html.\n\n")

if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/lucide/LICENSE")
    file(READ "${CMAKE_SOURCE_DIR}/third_party/lucide/LICENSE" _lucide_license)
    string(APPEND _licenses_text "Lucide icons\n------------\n${_lucide_license}\n\n")
endif()

file(GLOB _licenses_ports LIST_DIRECTORIES true "${_licenses_share}/*")
list(SORT _licenses_ports)
foreach(_port_dir IN LISTS _licenses_ports)
    get_filename_component(_port "${_port_dir}" NAME)
    if(_port IN_LIST _licenses_skip OR NOT EXISTS "${_port_dir}/copyright")
        continue()
    endif()
    file(READ "${_port_dir}/copyright" _port_license)
    string(LENGTH "${_port}" _port_len)
    string(REPEAT "-" ${_port_len} _underline)
    string(APPEND _licenses_text "${_port}\n${_underline}\n${_port_license}\n\n")
endforeach()

file(WRITE "${_licenses_file}" "${_licenses_text}")
