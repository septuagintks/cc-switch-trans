cmake_minimum_required(VERSION 3.20)

project(ccs_trans VERSION 0.1.0 LANGUAGES CXX)

add_executable(ccs-trans
    src/main.cpp
    src/config.cpp
    src/header_filter.cpp
    src/logger.cpp
    src/proxy.cpp
    src/request_id.cpp
    src/server.cpp
)

target_compile_features(ccs-trans PRIVATE cxx_std_17)

if(WIN32)
    target_link_libraries(ccs-trans PRIVATE ws2_32 winhttp)
endif()

target_include_directories(ccs-trans PRIVATE src)
