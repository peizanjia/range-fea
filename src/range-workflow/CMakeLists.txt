qt_add_executable(range-workflow-native native/main.cpp)
target_link_libraries(range-workflow-native PRIVATE range-model-lib Qt6::Core)
set_target_properties(range-workflow-native PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
install(TARGETS range-workflow-native RUNTIME DESTINATION bin COMPONENT Application)
install(DIRECTORY python/ DESTINATION share/range-workflow COMPONENT Application
    FILES_MATCHING PATTERN "*.py" PATTERN "*.json" PATTERN "__pycache__" EXCLUDE)
install(FILES ${CMAKE_SOURCE_DIR}/../docs/WORKFLOW_CONTRACT.md
    ${CMAKE_SOURCE_DIR}/../docs/CONTROL_WORKFLOW.md
    ${CMAKE_SOURCE_DIR}/../docs/AGENT_OPERATIONS.md
    ${CMAKE_SOURCE_DIR}/../docs/WORKFLOW_SDK.md
    ${CMAKE_SOURCE_DIR}/../docs/GUI_INTERACTION_DESIGN.md
    ${CMAKE_SOURCE_DIR}/../docs/GUI_UX_ACCEPTANCE.md
    ${CMAKE_SOURCE_DIR}/../docs/EXTENSION_SDK.md
    ${CMAKE_SOURCE_DIR}/../docs/STAGE12_ACCEPTANCE.md DESTINATION doc COMPONENT Application)
install(DIRECTORY examples/ DESTINATION share/range-workflow/examples COMPONENT Application FILES_MATCHING PATTERN "*.py")

set(RANGE_PRIVATE_PYTHON_DIRECTORY "" CACHE PATH "Verified private Python runtime assembled by scripts/packaging/build_private_python.py")
if(RANGE_PRIVATE_PYTHON_DIRECTORY)
    foreach(_runtime_file python.exe python313._pth runtime-lock.json runtime-files.json)
        if(NOT EXISTS "${RANGE_PRIVATE_PYTHON_DIRECTORY}/${_runtime_file}")
            message(FATAL_ERROR "Incomplete private Python runtime: ${_runtime_file}")
        endif()
    endforeach()
    install(DIRECTORY "${RANGE_PRIVATE_PYTHON_DIRECTORY}/" DESTINATION runtime/python COMPONENT Application
        PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE)
endif()

if(RANGE_BUILD_TESTS)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_executable(range-workflow-fixture EXCLUDE_FROM_ALL tests/generate_beam.cpp)
    target_include_directories(range-workflow-fixture PRIVATE "${CMAKE_SOURCE_DIR}/range-solver-lib/tests")
    target_link_libraries(range-workflow-fixture PRIVATE range-model-lib Qt6::Core)
    add_test(NAME tst_workflow_native COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native.py"
        "$<TARGET_FILE:range-workflow-native>"
        "${CMAKE_SOURCE_DIR}/fea-solver/examples/stage9/stage9_truss4.ram")
    add_test(NAME tst_workflow_runtime COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workflow_runtime.py"
        --solver "$<TARGET_FILE:fea-solver>" --native "$<TARGET_FILE:range-workflow-native>"
        --examples "${CMAKE_SOURCE_DIR}/fea-solver/examples/stage9"
        --python "${Python3_EXECUTABLE}" -v)
    set_tests_properties(tst_workflow_runtime PROPERTIES TIMEOUT 180)
    add_test(NAME tst_workflow_extensions COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_extensions.py" -v)
    set_tests_properties(tst_workflow_extensions PROPERTIES TIMEOUT 120)
    add_test(NAME tst_workflow_telemetry COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_telemetry.py" -v)
    set_tests_properties(tst_workflow_telemetry PROPERTIES TIMEOUT 60)
    add_test(NAME tst_workflow_project_catalog COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_project_catalog.py"
        --catalog-cli "$<TARGET_FILE:range-project>" -v)
    set_tests_properties(tst_workflow_project_catalog PROPERTIES TIMEOUT 120)
    if(WIN32)
        set_tests_properties(tst_workflow_project_catalog PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    add_test(NAME tst_private_python_packaging COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/../scripts/packaging/test_private_python.py" -v)
    if(WIN32)
        set_tests_properties(tst_workflow_native tst_workflow_runtime PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
endif()
