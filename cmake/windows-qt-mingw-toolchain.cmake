set(CCS_TRANS_QT_VERSION "6.10.3" CACHE STRING "Frozen Qt version")

if(DEFINED ENV{CCS_TRANS_QT_ROOT} AND NOT "$ENV{CCS_TRANS_QT_ROOT}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{CCS_TRANS_QT_ROOT}" CCS_TRANS_QT_ROOT)
else()
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}/Qt/${CCS_TRANS_QT_VERSION}/mingw_64"
        CCS_TRANS_QT_ROOT)
endif()

if(DEFINED ENV{CCS_TRANS_QT_MINGW_ROOT} AND
   NOT "$ENV{CCS_TRANS_QT_MINGW_ROOT}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{CCS_TRANS_QT_MINGW_ROOT}" CCS_TRANS_QT_MINGW_ROOT)
else()
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}/Qt/Tools/mingw1310_64"
        CCS_TRANS_QT_MINGW_ROOT)
endif()

foreach(required_path
        "${CCS_TRANS_QT_ROOT}/bin/qtpaths.exe"
        "${CCS_TRANS_QT_MINGW_ROOT}/bin/gcc.exe"
        "${CCS_TRANS_QT_MINGW_ROOT}/bin/g++.exe")
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR
            "Frozen Qt GUI toolchain is incomplete: ${required_path}. "
            "Set CCS_TRANS_QT_ROOT and CCS_TRANS_QT_MINGW_ROOT if Qt is installed elsewhere.")
    endif()
endforeach()

file(TO_NATIVE_PATH "${CCS_TRANS_QT_MINGW_ROOT}/bin" CCS_TRANS_QT_MINGW_BIN_NATIVE)
set(ENV{PATH} "${CCS_TRANS_QT_MINGW_BIN_NATIVE};$ENV{PATH}")

set(CMAKE_C_COMPILER "${CCS_TRANS_QT_MINGW_ROOT}/bin/gcc.exe"
    CACHE FILEPATH "Qt MinGW C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${CCS_TRANS_QT_MINGW_ROOT}/bin/g++.exe"
    CACHE FILEPATH "Qt MinGW C++ compiler" FORCE)
set(CMAKE_RC_COMPILER "${CCS_TRANS_QT_MINGW_ROOT}/bin/windres.exe"
    CACHE FILEPATH "Qt MinGW resource compiler" FORCE)
set(CMAKE_PREFIX_PATH "${CCS_TRANS_QT_ROOT}"
    CACHE PATH "Frozen Qt installation" FORCE)
