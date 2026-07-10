cmake_minimum_required(VERSION 3.20)

project(ccs_trans VERSION 0.1.0 LANGUAGES CXX)

add_library(ccs-trans-core STATIC
    src/config/config.cpp
    src/core/request_id.cpp
    src/logging/logger.cpp
    src/server/server.cpp
    src/transport/header_filter.cpp
    src/transport/proxy.cpp
)

target_compile_features(ccs-trans-core PUBLIC cxx_std_17)
target_include_directories(ccs-trans-core PUBLIC src)

if(WIN32)
    target_link_libraries(ccs-trans-core PRIVATE ws2_32 winhttp)
endif()

add_executable(ccs-trans
    src/hosts/cli_main.cpp
)

target_link_libraries(ccs-trans PRIVATE ccs-trans-core)
