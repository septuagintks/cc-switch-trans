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

add_library(ccs-trans-gui-prototype STATIC
    interaction/animation_policy.cpp
    interaction/animation_policy.hpp
    prototype/profile_list_model.cpp
    prototype/profile_list_model.hpp
    prototype/prototype_controller.cpp
    prototype/prototype_controller.hpp
)
target_compile_features(ccs-trans-gui-prototype PUBLIC cxx_std_20)
target_include_directories(ccs-trans-gui-prototype PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(ccs-trans-gui-prototype PRIVATE UNICODE _UNICODE)
target_link_libraries(ccs-trans-gui-prototype PUBLIC Qt6::Core Qt6::Gui)
target_link_libraries(ccs-trans-gui-prototype PUBLIC ccs-trans-gui-ipc)

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
    ccs-trans-gui-prototype
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
)

qt_add_qml_module(ccs-trans-gui
    URI CcsTrans.Gui
    VERSION 1.0
    QML_FILES
        ui/Main.qml
        ui/components/MotionButton.qml
        ui/components/MotionSwitch.qml
        ui/components/ProfileDelegate.qml
)

add_custom_target(ccs-trans-gui-structure-check ALL
    COMMAND "${CMAKE_COMMAND}"
        -DCCS_TRANS_SOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P "${CMAKE_SOURCE_DIR}/tools/check_gui_structure.cmake"
    VERBATIM
)
add_dependencies(ccs-trans-gui ccs-trans-gui-structure-check)

if(BUILD_TESTING)
    qt_add_executable(ccs-trans-gui-model-tests
        tests/profile_list_model_tests.cpp
    )
    target_link_libraries(ccs-trans-gui-model-tests PRIVATE
        ccs-trans-gui-prototype
        Qt6::Test
    )
    add_test(NAME ccs-trans-gui-model-tests COMMAND ccs-trans-gui-model-tests)

    qt_add_executable(ccs-trans-gui-qml-tests
        tests/qml_test_main.cpp
    )
    set_target_properties(ccs-trans-gui-qml-tests PROPERTIES WIN32_EXECUTABLE FALSE)
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

    add_test(
        NAME ccs-trans-gui-self-test
        COMMAND ccs-trans-gui --self-test --software-renderer
    )
    set_tests_properties(ccs-trans-gui-self-test PROPERTIES TIMEOUT 15)
endif()
