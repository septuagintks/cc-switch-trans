cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED CCS_TRANS_SOURCE_ROOT)
    get_filename_component(CCS_TRANS_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
elseif(NOT IS_ABSOLUTE "${CCS_TRANS_SOURCE_ROOT}")
    get_filename_component(
        CCS_TRANS_SOURCE_ROOT "${CCS_TRANS_SOURCE_ROOT}" ABSOLUTE
        BASE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
endif()

set(gui_root "${CCS_TRANS_SOURCE_ROOT}/src/gui/windows")
if(NOT EXISTS "${gui_root}/CMakeLists.txt")
    message(FATAL_ERROR "Windows GUI source root is missing: ${gui_root}")
endif()

file(GLOB_RECURSE cpp_files
    "${gui_root}/*.cpp"
    "${gui_root}/*.hpp"
)
file(GLOB_RECURSE qml_files "${gui_root}/*.qml")

foreach(source IN LISTS cpp_files)
    file(STRINGS "${source}" lines)
    list(LENGTH lines line_count)
    if(line_count GREATER 600)
        file(RELATIVE_PATH relative "${CCS_TRANS_SOURCE_ROOT}" "${source}")
        message(FATAL_ERROR "Hand-written C++ file exceeds 600 lines: ${relative} (${line_count})")
    endif()
    file(READ "${source}" contents)
    if(contents MATCHES "#[ \t]*include[ \t]*[<\"](app|config|core|hosts|logging|presentation|protocols|routing|rules|runtime|server|storage|transport)/")
        file(RELATIVE_PATH relative "${CCS_TRANS_SOURCE_ROOT}" "${source}")
        message(FATAL_ERROR "Qt GUI layer includes a forbidden runtime layer: ${relative}")
    endif()
endforeach()

foreach(source IN LISTS qml_files)
    file(STRINGS "${source}" lines)
    list(LENGTH lines line_count)
    if(line_count GREATER 300)
        file(RELATIVE_PATH relative "${CCS_TRANS_SOURCE_ROOT}" "${source}")
        message(FATAL_ERROR "QML file exceeds 300 lines: ${relative} (${line_count})")
    endif()
    file(READ "${source}" contents)
    if(contents MATCHES "Qt[.]labs[.]settings|profiles[.]db|ConfigurationRepository|RuntimeSnapshot|named[ -]pipe")
        file(RELATIVE_PATH relative "${CCS_TRANS_SOURCE_ROOT}" "${source}")
        message(FATAL_ERROR "QML accesses a forbidden persistence/runtime concern: ${relative}")
    endif()
endforeach()

file(READ "${gui_root}/CMakeLists.txt" gui_cmake)
if(gui_cmake MATCHES "main_window[.]cpp|windows_theme[.]cpp|ccs-trans-core")
    message(FATAL_ERROR "Qt GUI target references the legacy window or runtime static library")
endif()

message(STATUS
    "GUI structure check passed (${cpp_files}; ${qml_files})")
