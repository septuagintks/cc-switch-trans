cmake_minimum_required(VERSION 3.20)

if(CMAKE_HOST_APPLE)
    if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Supported macOS architecture")
    endif()
    if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "Supported macOS deployment target")
    endif()
    if(NOT DEFINED CMAKE_OSX_SYSROOT OR CMAKE_OSX_SYSROOT STREQUAL "")
        set(CMAKE_OSX_SYSROOT "macosx" CACHE STRING "Selected macOS SDK" FORCE)
    endif()
endif()

project(ccs_trans VERSION 0.7.0 LANGUAGES C CXX)

set(CCS_TRANS_VERSION_SUFFIX "")
set(CCS_TRANS_VERSION "${PROJECT_VERSION}${CCS_TRANS_VERSION_SUFFIX}")
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE CCS_TRANS_GIT_RESULT
    OUTPUT_VARIABLE CCS_TRANS_SOURCE_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT CCS_TRANS_GIT_RESULT EQUAL 0)
    set(CCS_TRANS_SOURCE_COMMIT "unknown")
endif()
execute_process(
    COMMAND git status --porcelain
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE CCS_TRANS_GIT_STATUS_RESULT
    OUTPUT_VARIABLE CCS_TRANS_GIT_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(CCS_TRANS_GIT_STATUS_RESULT EQUAL 0 AND CCS_TRANS_GIT_STATUS STREQUAL "")
    set(CCS_TRANS_SOURCE_DIRTY false)
else()
    set(CCS_TRANS_SOURCE_DIRTY true)
endif()

if(APPLE)
    enable_language(OBJCXX)
endif()

include(CTest)

if(NOT WIN32 AND NOT APPLE)
    message(FATAL_ERROR "ccs-trans supports only Windows and macOS")
endif()

if(APPLE)
    if(NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        message(FATAL_ERROR "macOS builds require an Apple Silicon host")
    endif()
    if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
        message(FATAL_ERROR "CMAKE_OSX_ARCHITECTURES must be exactly arm64")
    endif()
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "26.0")
        message(FATAL_ERROR "CMAKE_OSX_DEPLOYMENT_TARGET must be exactly 26.0")
    endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(CCS_TRANS_WARNINGS_AS_ERRORS "Enable strict C++ warnings and treat them as errors" OFF)
option(CCS_TRANS_BUILD_RUNTIME "Build the CLI and native tray/menu runtime" ON)
option(CCS_TRANS_BUILD_QT_GUI "Build the independent Windows Qt Quick GUI" OFF)

if(CCS_TRANS_BUILD_RUNTIME AND CCS_TRANS_BUILD_QT_GUI)
    message(FATAL_ERROR
        "Runtime and Qt GUI targets require independent build trees; enable exactly one")
endif()
if(NOT CCS_TRANS_BUILD_RUNTIME AND NOT CCS_TRANS_BUILD_QT_GUI)
    message(FATAL_ERROR "Enable one of CCS_TRANS_BUILD_RUNTIME or CCS_TRANS_BUILD_QT_GUI")
endif()
if(CCS_TRANS_BUILD_QT_GUI AND NOT WIN32)
    message(FATAL_ERROR "The Qt Quick sidecar is a Windows-only target")
endif()
if(CCS_TRANS_WARNINGS_AS_ERRORS)
    if(MSVC)
        add_compile_options(
            $<$<COMPILE_LANGUAGE:CXX>:/W4>
            $<$<COMPILE_LANGUAGE:CXX>:/WX>
            $<$<COMPILE_LANGUAGE:CXX>:/permissive->
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$")
        add_compile_options(
            $<$<COMPILE_LANGUAGE:CXX>:-Wall>
            $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
            $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>
            $<$<COMPILE_LANGUAGE:CXX>:-Werror>
            $<$<COMPILE_LANGUAGE:OBJCXX>:-Wall>
            $<$<COMPILE_LANGUAGE:OBJCXX>:-Wextra>
            $<$<COMPILE_LANGUAGE:OBJCXX>:-Wpedantic>
            $<$<COMPILE_LANGUAGE:OBJCXX>:-Werror>
        )
    else()
        message(FATAL_ERROR "CCS_TRANS_WARNINGS_AS_ERRORS is unsupported by this compiler")
    endif()
endif()

set(CCS_TRANS_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    src/core/version.hpp.in
    "${CCS_TRANS_GENERATED_DIR}/include/core/version.hpp"
    @ONLY
)

if(CCS_TRANS_BUILD_RUNTIME)
add_library(ccs-trans-sqlite3 STATIC
    third_party/sqlite/sqlite3.c
)
target_include_directories(ccs-trans-sqlite3 PUBLIC
    third_party/sqlite
)
target_compile_definitions(ccs-trans-sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DQS=0
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_ENABLE_API_ARMOR
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_LOAD_EXTENSION
)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(ccs-trans-sqlite3 PRIVATE -Wno-stringop-overread)
endif()

set(CCS_TRANS_PLATFORM_SOURCES)
if(WIN32)
    enable_language(RC)
    list(APPEND CCS_TRANS_PLATFORM_SOURCES
        src/hosts/windows/instance_coordinator.cpp
        src/hosts/windows/startup_registration.cpp
        src/hosts/windows/windows_error.cpp
        src/hosts/windows/windows_host_platform.cpp
        src/server/platform/windows/local_socket.cpp
        src/transport/windows/winhttp_transport.cpp
    )
elseif(APPLE)
    list(APPEND CCS_TRANS_PLATFORM_SOURCES
        src/server/platform/posix/local_socket.cpp
        src/transport/macos/curl_transport.cpp
    )
endif()

if(APPLE)
    set(CCS_TRANS_MACOS_ICON_DIR "${CCS_TRANS_GENERATED_DIR}/icons/macos")
    set(CCS_TRANS_MACOS_APP_ICON "${CCS_TRANS_MACOS_ICON_DIR}/ccs-trans.icns")
    set(CCS_TRANS_MACOS_STATUS_ICON
        "${CCS_TRANS_MACOS_ICON_DIR}/ccs-trans-statusTemplate.png")
    add_custom_command(
        OUTPUT "${CCS_TRANS_MACOS_APP_ICON}" "${CCS_TRANS_MACOS_STATUS_ICON}"
        COMMAND /bin/sh "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_macos_icons.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/ccs-trans-512.png"
            "${CCS_TRANS_MACOS_ICON_DIR}"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/ccs-trans-512.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_macos_icons.sh"
        VERBATIM
    )
    set_source_files_properties(
        "${CCS_TRANS_MACOS_APP_ICON}"
        "${CCS_TRANS_MACOS_STATUS_ICON}"
        PROPERTIES
            GENERATED TRUE
            MACOSX_PACKAGE_LOCATION Resources
    )
    configure_file(
        packaging/macos/Info.plist.in
        "${CCS_TRANS_GENERATED_DIR}/macos/Info.plist"
        @ONLY
    )

    add_executable(ccs-trans-menu MACOSX_BUNDLE
        src/hosts/macos/instance_coordinator.mm
        src/hosts/macos/main_window.mm
        src/hosts/macos/macos_host_platform.mm
        src/hosts/macos/menu_app.mm
        src/hosts/macos/menu_main.mm
        src/hosts/macos/startup_registration.mm
        "${CCS_TRANS_MACOS_APP_ICON}"
        "${CCS_TRANS_MACOS_STATUS_ICON}"
    )
    find_library(CCS_TRANS_APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(CCS_TRANS_SERVICE_MANAGEMENT_FRAMEWORK ServiceManagement REQUIRED)
    target_compile_options(ccs-trans-menu PRIVATE -fobjc-arc)
    target_link_libraries(ccs-trans-menu PRIVATE
        ccs-trans-core
        "${CCS_TRANS_APPKIT_FRAMEWORK}"
        "${CCS_TRANS_SERVICE_MANAGEMENT_FRAMEWORK}"
    )
    set_target_properties(ccs-trans-menu PROPERTIES
        OUTPUT_NAME ccs-trans
        MACOSX_BUNDLE_INFO_PLIST "${CCS_TRANS_GENERATED_DIR}/macos/Info.plist"
    )
endif()

add_library(ccs-trans-core STATIC
    src/app/application_controller.cpp
    src/app/app_service.cpp
    src/app/control_executor.cpp
    src/config/app_paths.cpp
    src/config/application_config.cpp
    src/config/config_cli.cpp
    src/config/composite_config_repository.cpp
    src/config/configuration_editor.cpp
    src/config/configuration_snapshot.cpp
    src/config/config_document.cpp
    src/config/config_editing_service.cpp
    src/config/config_store.cpp
    src/config/field_descriptor.cpp
    src/config/rules_text.cpp
    src/config/runtime_compiler.cpp
    src/core/cancellation.cpp
    src/core/inflight_memory_budget.cpp
    src/core/request_id.cpp
    src/core/runtime_metrics.cpp
    src/core/sha256.cpp
    src/core/url.cpp
    src/hosts/host_platform.cpp
    src/logging/logger.cpp
    src/protocols/chat_handler.cpp
    src/protocols/messages_handler.cpp
    src/protocols/protocol_handler.cpp
    src/protocols/protocol_registry.cpp
    src/protocols/responses_handler.cpp
    src/presentation/main_window_contract.cpp
    src/presentation/main_window_view_model.cpp
    src/presentation/ui_preferences.cpp
    src/presentation/ui_preferences_store.cpp
    src/routing/route_table.cpp
    src/rules/generic_json_rules.cpp
    src/rules/remove_tool_rule.cpp
    src/rules/rule.cpp
    src/rules/rule_json.cpp
    src/rules/rule_registry.cpp
    src/server/server.cpp
    src/storage/sqlite_profile_store.cpp
    src/transport/header_filter.cpp
    src/transport/upstream_transport.cpp
    ${CCS_TRANS_PLATFORM_SOURCES}
)

target_compile_features(ccs-trans-core PUBLIC cxx_std_20)
target_include_directories(ccs-trans-core PUBLIC
    src
    third_party
    "${CCS_TRANS_GENERATED_DIR}/include"
)

if(WIN32)
    target_link_libraries(ccs-trans-core PRIVATE
        ccs-trans-sqlite3
        ws2_32
        winhttp
        shell32
        ole32
        advapi32
    )
elseif(APPLE)
    find_library(
        CCS_TRANS_SYSTEM_CURL
        NAMES curl
        PATHS "${CMAKE_OSX_SYSROOT}/usr/lib"
        NO_DEFAULT_PATH
        REQUIRED
    )
    if(NOT CCS_TRANS_SYSTEM_CURL MATCHES "/[^/]*MacOSX[^/]*\\.sdk/usr/lib/libcurl")
        message(FATAL_ERROR "macOS must link libcurl from the selected Xcode SDK")
    endif()
    target_link_libraries(ccs-trans-core PRIVATE
        ccs-trans-sqlite3
        "${CCS_TRANS_SYSTEM_CURL}"
    )
endif()

add_executable(ccs-trans
    src/hosts/cli_main.cpp
)

target_link_libraries(ccs-trans PRIVATE ccs-trans-core)

if(WIN32)
    set(CCS_TRANS_WINDOWS_ICON "${CCS_TRANS_GENERATED_DIR}/icons/ccs-trans.ico")
    add_custom_command(
        OUTPUT "${CCS_TRANS_WINDOWS_ICON}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CCS_TRANS_GENERATED_DIR}/icons"
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_icons.ps1"
            -SourcePath "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/ccs-trans-512.png"
            -OutputPath "${CCS_TRANS_WINDOWS_ICON}"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/ccs-trans-512.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_icons.ps1"
        VERBATIM
    )
    string(REPLACE "\\" "/" CCS_TRANS_ICON_RC_PATH "${CCS_TRANS_WINDOWS_ICON}")
    configure_file(
        packaging/windows/ccs-trans-tray.rc.in
        "${CCS_TRANS_GENERATED_DIR}/ccs-trans-tray.rc"
        @ONLY
    )
    set_source_files_properties(
        "${CCS_TRANS_GENERATED_DIR}/ccs-trans-tray.rc"
        PROPERTIES OBJECT_DEPENDS "${CCS_TRANS_WINDOWS_ICON}"
    )

    add_executable(ccs-trans-tray WIN32
        src/hosts/windows/main_window.cpp
        src/hosts/windows/tray_app.cpp
        src/hosts/windows/tray_main.cpp
        src/hosts/windows/windows_theme.cpp
        "${CCS_TRANS_GENERATED_DIR}/ccs-trans-tray.rc"
    )
    target_compile_definitions(ccs-trans-tray PRIVATE UNICODE _UNICODE)
    target_link_libraries(ccs-trans-tray PRIVATE
        ccs-trans-core
        comctl32
        dwmapi
        gdiplus
        advapi32
        shell32
        ole32
        user32
        uxtheme
    )
    if(MINGW)
        target_link_options(ccs-trans-tray PRIVATE -municode)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(ccs-trans-sqlite-dependency-tests
        tests/unit/sqlite_dependency_tests.cpp
    )
    target_link_libraries(ccs-trans-sqlite-dependency-tests PRIVATE ccs-trans-sqlite3)
    add_test(
        NAME ccs-trans-sqlite-dependency-tests
        COMMAND ccs-trans-sqlite-dependency-tests
    )

    add_executable(ccs-trans-sqlite-profile-store-tests
        tests/unit/sqlite_profile_store_tests.cpp
    )
    target_link_libraries(ccs-trans-sqlite-profile-store-tests PRIVATE
        ccs-trans-core
        ccs-trans-sqlite3
    )
    add_test(
        NAME ccs-trans-sqlite-profile-store-tests
        COMMAND ccs-trans-sqlite-profile-store-tests
    )

    add_executable(ccs-trans-core-tests
        tests/unit/core_tests.cpp
    )
    target_link_libraries(ccs-trans-core-tests PRIVATE ccs-trans-core)
    target_include_directories(ccs-trans-core-tests PRIVATE third_party)
    add_test(NAME ccs-trans-core-tests COMMAND ccs-trans-core-tests)

    add_executable(ccs-trans-control-executor-tests
        tests/unit/control_executor_tests.cpp
    )
    target_link_libraries(ccs-trans-control-executor-tests PRIVATE ccs-trans-core)
    add_test(NAME ccs-trans-control-executor-tests COMMAND ccs-trans-control-executor-tests)

    add_executable(ccs-trans-inflight-memory-budget-tests
        tests/unit/inflight_memory_budget_tests.cpp
    )
    target_link_libraries(ccs-trans-inflight-memory-budget-tests PRIVATE ccs-trans-core)
    add_test(
        NAME ccs-trans-inflight-memory-budget-tests
        COMMAND ccs-trans-inflight-memory-budget-tests
    )

    add_executable(ccs-trans-local-socket-tests
        tests/unit/local_socket_tests.cpp
    )
    target_link_libraries(ccs-trans-local-socket-tests PRIVATE ccs-trans-core)
    add_test(NAME ccs-trans-local-socket-tests COMMAND ccs-trans-local-socket-tests)

    add_executable(ccs-trans-config-document-tests
        tests/unit/config_document_tests.cpp
    )
    target_link_libraries(ccs-trans-config-document-tests PRIVATE ccs-trans-core)
    target_compile_definitions(ccs-trans-config-document-tests PRIVATE
        CCS_TRANS_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    add_test(NAME ccs-trans-config-document-tests COMMAND ccs-trans-config-document-tests)

    add_executable(ccs-trans-application-config-tests
        tests/unit/application_config_tests.cpp
    )
    target_link_libraries(ccs-trans-application-config-tests PRIVATE ccs-trans-core)
    add_test(NAME ccs-trans-application-config-tests COMMAND ccs-trans-application-config-tests)

    add_executable(ccs-trans-configuration-snapshot-tests
        tests/unit/configuration_snapshot_tests.cpp
    )

    add_executable(ccs-trans-composite-config-repository-tests
        tests/unit/composite_config_repository_tests.cpp
    )
    target_link_libraries(ccs-trans-composite-config-repository-tests PRIVATE
        ccs-trans-core
        ccs-trans-sqlite3
    )
    add_test(
        NAME ccs-trans-composite-config-repository-tests
        COMMAND ccs-trans-composite-config-repository-tests
    )
    target_link_libraries(ccs-trans-configuration-snapshot-tests PRIVATE ccs-trans-core)
    add_test(
        NAME ccs-trans-configuration-snapshot-tests
        COMMAND ccs-trans-configuration-snapshot-tests
    )

    add_executable(ccs-trans-sha256-tests
        tests/unit/sha256_tests.cpp
    )
    target_link_libraries(ccs-trans-sha256-tests PRIVATE ccs-trans-core)
    add_test(NAME ccs-trans-sha256-tests COMMAND ccs-trans-sha256-tests)

    add_executable(ccs-trans-config-cli-tests
        tests/unit/config_cli_tests.cpp
    )
    target_link_libraries(ccs-trans-config-cli-tests PRIVATE ccs-trans-core)
    add_test(NAME ccs-trans-config-cli-tests COMMAND ccs-trans-config-cli-tests)

    add_executable(ccs-trans-config-editing-service-tests
        tests/unit/config_editing_service_tests.cpp
    )
    target_link_libraries(ccs-trans-config-editing-service-tests PRIVATE ccs-trans-core)
    add_test(
        NAME ccs-trans-config-editing-service-tests
        COMMAND ccs-trans-config-editing-service-tests
    )

    add_executable(ccs-trans-configuration-editor-tests
        tests/unit/configuration_editor_tests.cpp
    )
    target_link_libraries(ccs-trans-configuration-editor-tests PRIVATE ccs-trans-core)
    add_test(
        NAME ccs-trans-configuration-editor-tests
        COMMAND ccs-trans-configuration-editor-tests
    )

    add_executable(ccs-trans-rules-text-tests
        tests/unit/rules_text_tests.cpp
    )
    target_link_libraries(ccs-trans-rules-text-tests PRIVATE ccs-trans-core)
    add_test(NAME ccs-trans-rules-text-tests COMMAND ccs-trans-rules-text-tests)

    add_executable(ccs-trans-presentation-contract-tests
        tests/unit/presentation_contract_tests.cpp
    )
    target_link_libraries(ccs-trans-presentation-contract-tests PRIVATE ccs-trans-core)
    add_test(
        NAME ccs-trans-presentation-contract-tests
        COMMAND ccs-trans-presentation-contract-tests
    )

    add_executable(ccs-trans-ui-preferences-store-tests
        tests/unit/ui_preferences_store_tests.cpp
    )
    target_link_libraries(ccs-trans-ui-preferences-store-tests PRIVATE ccs-trans-core)
    add_test(
        NAME ccs-trans-ui-preferences-store-tests
        COMMAND ccs-trans-ui-preferences-store-tests
    )

    add_executable(ccs-trans-main-window-view-model-tests
        tests/unit/main_window_view_model_tests.cpp
    )
    target_link_libraries(ccs-trans-main-window-view-model-tests PRIVATE ccs-trans-core)
    add_test(
        NAME ccs-trans-main-window-view-model-tests
        COMMAND ccs-trans-main-window-view-model-tests
    )

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

    if(WIN32)
        add_executable(ccs-trans-instance-coordinator-tests
            tests/unit/instance_coordinator_tests.cpp
        )
        target_link_libraries(ccs-trans-instance-coordinator-tests PRIVATE ccs-trans-core)
        add_test(
            NAME ccs-trans-instance-coordinator-tests
            COMMAND ccs-trans-instance-coordinator-tests
        )

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
endif()

if(CCS_TRANS_BUILD_QT_GUI)
    add_subdirectory(src/gui/windows)
endif()

if(BUILD_TESTING)
    add_test(
        NAME ccs-trans-gui-structure-check
        COMMAND "${CMAKE_COMMAND}"
            -DCCS_TRANS_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_gui_structure.cmake"
    )
endif()
