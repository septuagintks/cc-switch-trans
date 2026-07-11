cmake_minimum_required(VERSION 3.20)

project(ccs_trans VERSION 0.4.0 LANGUAGES CXX)

include(CTest)

add_library(ccs-trans-core STATIC
    src/config/app_paths.cpp
    src/config/config.cpp
    src/config/config_cli.cpp
    src/config/config_document.cpp
    src/config/config_store.cpp
    src/config/profile_store.cpp
    src/core/app_service.cpp
    src/core/cancellation.cpp
    src/core/request_id.cpp
    src/core/runtime_metrics.cpp
    src/core/task.cpp
    src/core/task_router.cpp
    src/core/transform.cpp
    src/core/url.cpp
    src/logging/logger.cpp
    src/server/server.cpp
    src/transport/header_filter.cpp
    src/transport/proxy.cpp
    src/transforms/findcg_responses_transform.cpp
)

target_compile_features(ccs-trans-core PUBLIC cxx_std_17)
target_include_directories(ccs-trans-core PUBLIC src third_party)

if(WIN32)
    target_link_libraries(ccs-trans-core PRIVATE ws2_32 winhttp shell32 ole32)
endif()

add_executable(ccs-trans
    src/hosts/cli_main.cpp
)

target_link_libraries(ccs-trans PRIVATE ccs-trans-core)

if(BUILD_TESTING)
    add_executable(ccs-trans-core-tests
        tests/unit/core_tests.cpp
    )
    target_link_libraries(ccs-trans-core-tests PRIVATE ccs-trans-core)
    target_include_directories(ccs-trans-core-tests PRIVATE third_party)
    target_compile_definitions(ccs-trans-core-tests PRIVATE
        CCS_TRANS_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    add_test(NAME ccs-trans-core-tests COMMAND ccs-trans-core-tests)

    add_executable(ccs-trans-config-document-tests
        tests/unit/config_document_tests.cpp
    )
    target_link_libraries(ccs-trans-config-document-tests PRIVATE ccs-trans-core)
    target_compile_definitions(ccs-trans-config-document-tests PRIVATE
        CCS_TRANS_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    add_test(NAME ccs-trans-config-document-tests COMMAND ccs-trans-config-document-tests)

    add_executable(ccs-trans-config-cli-tests
        tests/unit/config_cli_tests.cpp
    )
    target_link_libraries(ccs-trans-config-cli-tests PRIVATE ccs-trans-core)
    add_test(NAME ccs-trans-config-cli-tests COMMAND ccs-trans-config-cli-tests)

    if(WIN32)
        add_executable(ccs-trans-reload-integration
            tests/integration/reload_integration.cpp
        )
        target_link_libraries(ccs-trans-reload-integration PRIVATE ccs-trans-core)
        add_test(NAME ccs-trans-reload-integration COMMAND ccs-trans-reload-integration)
        set_tests_properties(ccs-trans-reload-integration PROPERTIES TIMEOUT 30)
    endif()

    add_executable(ccs-trans-transform-benchmark
        tests/benchmark/transform_benchmark.cpp
    )
    target_link_libraries(ccs-trans-transform-benchmark PRIVATE ccs-trans-core)
endif()
