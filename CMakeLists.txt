cmake_minimum_required(VERSION 3.20)

project(ccs_trans VERSION 0.4.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CCS_TRANS_PLATFORM_SOURCES)
if(WIN32)
    list(APPEND CCS_TRANS_PLATFORM_SOURCES
        src/hosts/windows/startup_registration.cpp
        src/hosts/windows/windows_error.cpp
        src/hosts/windows/windows_host_platform.cpp
        src/transport/windows/winhttp_transport.cpp
    )
endif()

add_library(ccs-trans-core STATIC
    src/app/application_controller.cpp
    src/app/app_service.cpp
    src/config/app_paths.cpp
    src/config/config_cli.cpp
    src/config/config_document.cpp
    src/config/config_store.cpp
    src/config/runtime_compiler.cpp
    src/core/cancellation.cpp
    src/core/request_id.cpp
    src/core/runtime_metrics.cpp
    src/core/url.cpp
    src/hosts/host_platform.cpp
    src/logging/logger.cpp
    src/protocols/chat_handler.cpp
    src/protocols/messages_handler.cpp
    src/protocols/protocol_handler.cpp
    src/protocols/protocol_registry.cpp
    src/protocols/responses_handler.cpp
    src/routing/route_table.cpp
    src/rules/generic_json_rules.cpp
    src/rules/remove_tool_rule.cpp
    src/rules/rule.cpp
    src/rules/rule_registry.cpp
    src/server/server.cpp
    src/transport/header_filter.cpp
    src/transport/upstream_transport.cpp
    ${CCS_TRANS_PLATFORM_SOURCES}
)

target_compile_features(ccs-trans-core PUBLIC cxx_std_20)
target_include_directories(ccs-trans-core PUBLIC src third_party)

if(WIN32)
    target_link_libraries(ccs-trans-core PRIVATE ws2_32 winhttp shell32 ole32 advapi32)
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

    add_executable(ccs-trans-route-table-tests
        tests/unit/route_table_tests.cpp
    )
    target_link_libraries(ccs-trans-route-table-tests PRIVATE ccs-trans-core)
    add_test(NAME ccs-trans-route-table-tests COMMAND ccs-trans-route-table-tests)

    add_executable(ccs-trans-protocol-tests
        tests/unit/protocol_tests.cpp
    )
    target_link_libraries(ccs-trans-protocol-tests PRIVATE ccs-trans-core)
    add_test(NAME ccs-trans-protocol-tests COMMAND ccs-trans-protocol-tests)

    add_executable(ccs-trans-rule-pipeline-tests
        tests/unit/rule_pipeline_tests.cpp
    )
    target_link_libraries(ccs-trans-rule-pipeline-tests PRIVATE ccs-trans-core)
    target_compile_definitions(ccs-trans-rule-pipeline-tests PRIVATE
        CCS_TRANS_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    add_test(NAME ccs-trans-rule-pipeline-tests COMMAND ccs-trans-rule-pipeline-tests)

    if(WIN32)
        add_executable(ccs-trans-application-controller-tests
            tests/unit/application_controller_tests.cpp
        )
        target_link_libraries(ccs-trans-application-controller-tests PRIVATE ccs-trans-core)
        add_test(
            NAME ccs-trans-application-controller-tests
            COMMAND ccs-trans-application-controller-tests
        )

        add_executable(ccs-trans-host-platform-tests
            tests/unit/host_platform_tests.cpp
        )
        target_link_libraries(ccs-trans-host-platform-tests PRIVATE ccs-trans-core)
        add_test(NAME ccs-trans-host-platform-tests COMMAND ccs-trans-host-platform-tests)

        add_executable(ccs-trans-reload-integration
            tests/integration/reload_integration.cpp
        )
        target_link_libraries(ccs-trans-reload-integration PRIVATE ccs-trans-core)
        add_test(NAME ccs-trans-reload-integration COMMAND ccs-trans-reload-integration)
        set_tests_properties(ccs-trans-reload-integration PROPERTIES TIMEOUT 30)
    endif()

    add_executable(ccs-trans-rule-pipeline-benchmark
        tests/benchmark/rule_pipeline_benchmark.cpp
    )
    target_link_libraries(ccs-trans-rule-pipeline-benchmark PRIVATE ccs-trans-core)
endif()
