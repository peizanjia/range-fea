qt_add_executable(range-workflow-native native/main.cpp)
target_link_libraries(range-workflow-native PRIVATE range-model-lib Qt6::Core)
set_target_properties(range-workflow-native PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
install(TARGETS range-workflow-native RUNTIME DESTINATION bin COMPONENT Application)
install(DIRECTORY python/ DESTINATION share/range-workflow COMPONENT Application
    FILES_MATCHING PATTERN "*.py" PATTERN "*.json" PATTERN "__pycache__" EXCLUDE)
install(FILES ${CMAKE_SOURCE_DIR}/../docs/WORKFLOW_CONTRACT.md
    ${CMAKE_SOURCE_DIR}/../docs/CONTROL_WORKFLOW.md DESTINATION doc COMPONENT Application)

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
    if(WIN32)
        set_tests_properties(tst_workflow_native tst_workflow_runtime PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
endif()
