find_package(Qt6 6.10.3 EXACT REQUIRED COMPONENTS
    Core
    Gui
    Network
    Qml
    Quick
    QuickControls2
    QuickTest
    Test
)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
   NOT CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "13.1" OR
   CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "13.2")
    message(FATAL_ERROR
        "ccs-trans-gui requires the frozen Qt MinGW 13.1 toolchain; found "
        "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()

qt_standard_project_setup(REQUIRES 6.10.3)

add_library(ccs-trans-gui-foundation STATIC
    controllers/command_dispatcher.cpp
    controllers/command_dispatcher.hpp
    features/migration/migration_controller.cpp
    features/migration/migration_controller.hpp
    features/profiles/profiles_controller.cpp
    features/profiles/profiles_controller.hpp
    features/rules/rules_controller.cpp
    features/rules/rules_controller.hpp
    features/settings/settings_controller.cpp
    features/settings/settings_controller.hpp
    interaction/animation_policy.cpp
    interaction/animation_policy.hpp
    ipc/bootstrap_reader.cpp
    ipc/bootstrap_reader.hpp
    ipc/gui_ipc_client.cpp
    ipc/gui_ipc_client.hpp
    lifecycle/gui_window_controller.cpp
    lifecycle/gui_window_controller.hpp
    models/configuration_field_model.cpp
    models/configuration_field_model.hpp
    models/editable_field_model.cpp
    models/editable_field_model.hpp
    models/profile_summary_model.cpp
    models/profile_summary_model.hpp
    prototype/profile_list_model.cpp
    prototype/profile_list_model.hpp
    prototype/prototype_controller.cpp
    prototype/prototype_controller.hpp
    state/gui_state_store.cpp
    state/gui_state_store.hpp
)
target_compile_features(ccs-trans-gui-foundation PUBLIC cxx_std_20)
target_include_directories(ccs-trans-gui-foundation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(ccs-trans-gui-foundation PRIVATE UNICODE _UNICODE)
target_link_libraries(ccs-trans-gui-foundation PUBLIC
    ccs-trans-gui-ipc
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Quick
)

qt_add_executable(ccs-trans-gui WIN32
    app/gui_main.cpp
    diagnostics/frame_monitor.cpp
    diagnostics/frame_monitor.hpp
)
target_compile_definitions(ccs-trans-gui PRIVATE
    CCS_TRANS_GUI_VERSION="${CCS_TRANS_VERSION}"
    CCS_TRANS_GUI_SOURCE_COMMIT="${CCS_TRANS_SOURCE_COMMIT}"
    UNICODE
    _UNICODE
)
target_link_libraries(ccs-trans-gui PRIVATE
    ccs-trans-gui-foundation
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
)

set_source_files_properties(ui/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(ccs-trans-gui
    URI CcsTrans.Gui
    VERSION 1.0
    QML_FILES
        ui/Main.qml
        ui/PrototypeProbe.qml
        ui/Theme.qml
        ui/components/MotionButton.qml
        ui/components/MotionComboBox.qml
        ui/components/DecisionDialog.qml
        ui/components/FieldEditorRow.qml
        ui/components/ApplicationDialogs.qml
        ui/components/MotionScrollBar.qml
        ui/components/MotionSwitch.qml
        ui/components/MotionTextField.qml
        ui/components/NavigationTab.qml
        ui/components/ProfileDelegate.qml
        ui/components/ProfileSummaryDelegate.qml
        ui/features/ProfilesPage.qml
        ui/features/RulesPage.qml
        ui/features/SettingsPage.qml
)

add_custom_target(ccs-trans-gui-structure-check ALL
    COMMAND "${CMAKE_COMMAND}"
        -DCCS_TRANS_SOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P "${CMAKE_SOURCE_DIR}/tools/check_gui_structure.cmake"
    VERBATIM
)
add_dependencies(ccs-trans-gui ccs-trans-gui-structure-check)

if(BUILD_TESTING)
    function(ccs_trans_configure_qt_test test_name)
        set(test_environment_modification
            "PATH=path_list_prepend:${CCS_TRANS_QT_ROOT}/bin;PATH=path_list_prepend:${CCS_TRANS_QT_MINGW_ROOT}/bin")
        set_tests_properties(${test_name} PROPERTIES
            ENVIRONMENT_MODIFICATION "${test_environment_modification}"
        )
    endfunction()

    qt_add_executable(ccs-trans-gui-ipc-client-tests
        tests/gui_ipc_client_tests.cpp
    )
    set_target_properties(
        ccs-trans-gui-ipc-client-tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(ccs-trans-gui-ipc-client-tests PRIVATE
        ccs-trans-gui-foundation
        Qt6::Test
    )
    add_test(
        NAME ccs-trans-gui-ipc-client-tests
        COMMAND ccs-trans-gui-ipc-client-tests
    )
    set_tests_properties(
        ccs-trans-gui-ipc-client-tests PROPERTIES TIMEOUT 15)
    ccs_trans_configure_qt_test(ccs-trans-gui-ipc-client-tests)

    qt_add_executable(ccs-trans-gui-model-tests
        tests/profile_list_model_tests.cpp
    )
    target_link_libraries(ccs-trans-gui-model-tests PRIVATE
        ccs-trans-gui-foundation
        Qt6::Test
    )
    add_test(NAME ccs-trans-gui-model-tests COMMAND ccs-trans-gui-model-tests)
    ccs_trans_configure_qt_test(ccs-trans-gui-model-tests)

    qt_add_executable(ccs-trans-gui-feature-controller-tests
        tests/gui_feature_controller_tests.cpp
    )
    set_target_properties(
        ccs-trans-gui-feature-controller-tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(ccs-trans-gui-feature-controller-tests PRIVATE
        ccs-trans-gui-foundation
        Qt6::Test
    )
    add_test(
        NAME ccs-trans-gui-feature-controller-tests
        COMMAND ccs-trans-gui-feature-controller-tests
    )
    set_tests_properties(
        ccs-trans-gui-feature-controller-tests PROPERTIES TIMEOUT 15)
    ccs_trans_configure_qt_test(ccs-trans-gui-feature-controller-tests)

    qt_add_executable(ccs-trans-gui-qml-tests
        tests/qml_test_main.cpp
    )
    set_target_properties(ccs-trans-gui-qml-tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_compile_definitions(ccs-trans-gui-qml-tests PRIVATE
        CCS_TRANS_GUI_THEME_PATH="${CMAKE_CURRENT_SOURCE_DIR}/ui/Theme.qml"
    )
    target_link_libraries(ccs-trans-gui-qml-tests PRIVATE
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickTest
    )
    add_test(
        NAME ccs-trans-gui-qml-tests
        COMMAND ccs-trans-gui-qml-tests
            -input "${CMAKE_CURRENT_SOURCE_DIR}/tests/qml"
    )
    ccs_trans_configure_qt_test(ccs-trans-gui-qml-tests)

    add_test(
        NAME ccs-trans-gui-self-test
        COMMAND ccs-trans-gui --self-test --software-renderer
    )
    set_tests_properties(ccs-trans-gui-self-test PROPERTIES TIMEOUT 15)
    ccs_trans_configure_qt_test(ccs-trans-gui-self-test)

    add_test(
        NAME ccs-trans-gui-production-qml-probe
        COMMAND ccs-trans-gui --production-qml-probe --software-renderer
    )
    set_tests_properties(
        ccs-trans-gui-production-qml-probe PROPERTIES TIMEOUT 15)
    ccs_trans_configure_qt_test(ccs-trans-gui-production-qml-probe)
endif()
