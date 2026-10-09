qt_add_executable(range-workflow-native native/main.cpp native/import_model.cpp native/import_model.h)
target_link_libraries(range-workflow-native PRIVATE range-model-lib range-solver-lib Qt6::Core)
set_target_properties(range-workflow-native PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
install(TARGETS range-workflow-native RUNTIME DESTINATION bin COMPONENT Application)
if(WIN32)
    qt_add_executable(range-workflow-isolation-launcher native/isolation_launcher.cpp native/isolation_launcher.h)
    target_compile_definitions(range-workflow-isolation-launcher PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    target_link_libraries(range-workflow-isolation-launcher PRIVATE Qt6::Core advapi32 userenv kernel32 shell32 ole32)
    set_target_properties(range-workflow-isolation-launcher PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
    install(TARGETS range-workflow-isolation-launcher RUNTIME DESTINATION bin COMPONENT Application)
endif()
install(DIRECTORY python/ DESTINATION share/range-workflow COMPONENT Application
    FILES_MATCHING PATTERN "*.py" PATTERN "*.json" PATTERN "__pycache__" EXCLUDE)
install(FILES ${CMAKE_SOURCE_DIR}/../docs/WORKFLOW_CONTRACT.md
    ${CMAKE_SOURCE_DIR}/../docs/CONTROL_WORKFLOW.md
    ${CMAKE_SOURCE_DIR}/../docs/AGENT_OPERATIONS.md
    ${CMAKE_SOURCE_DIR}/../docs/WORKFLOW_SDK.md
    ${CMAKE_SOURCE_DIR}/../docs/NATIVE_DOCUMENT_SDK.md
    ${CMAKE_SOURCE_DIR}/../docs/NATIVE_RESULTS.md
    ${CMAKE_SOURCE_DIR}/../docs/RECOVERY_OPERATIONS.md
      ${CMAKE_SOURCE_DIR}/../docs/RECOVERY_CONTRACT.md
      ${CMAKE_SOURCE_DIR}/../docs/CONVERSATIONS.md
      ${CMAKE_SOURCE_DIR}/../docs/TASK_PLANS.md
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
    foreach(_read_suite native_manifest read_query_lane read_transport mesh_client scientific_runtime runtime_verification scientific_input_inspection)
        add_test(NAME tst_workflow_${_read_suite} COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${_read_suite}.py" -v)
        set_tests_properties(tst_workflow_${_read_suite} PROPERTIES TIMEOUT 60)
    endforeach()
    foreach(_task_suite task_plan task_scheduler knowledge)
        add_test(NAME tst_workflow_${_task_suite} COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${_task_suite}.py" -v)
        set_tests_properties(tst_workflow_${_task_suite} PROPERTIES TIMEOUT 60)
    endforeach()
    add_test(NAME tst_workflow_task_runtime COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_task_runtime.py" -v)
    set_tests_properties(tst_workflow_task_runtime PROPERTIES TIMEOUT 60)
    add_test(NAME tst_workflow_task_scan_runtime COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_task_runtime.py"
        --solver "$<TARGET_FILE:fea-solver>" --native "$<TARGET_FILE:range-workflow-native>"
        --examples "${CMAKE_SOURCE_DIR}/fea-solver/examples/stage9"
        --python "${Python3_EXECUTABLE}" -v RealTaskScanAcceptance)
    set_tests_properties(tst_workflow_task_scan_runtime PROPERTIES TIMEOUT 300)
    if(WIN32)
        set_tests_properties(tst_workflow_task_scan_runtime PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    add_executable(range-workflow-fixture EXCLUDE_FROM_ALL tests/generate_beam.cpp)
    target_include_directories(range-workflow-fixture PRIVATE "${CMAKE_SOURCE_DIR}/range-solver-lib/tests")
    target_link_libraries(range-workflow-fixture PRIVATE range-model-lib Qt6::Core)
    add_executable(range-workflow-native-import-fixture tests/generate_native_import.cpp)
    target_link_libraries(range-workflow-native-import-fixture PRIVATE range-model-lib Qt6::Core)
    add_test(NAME tst_workflow_native_import COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_import.py"
        "$<TARGET_FILE:range-workflow-native>" "$<TARGET_FILE:range-workflow-native-import-fixture>" -v)
    add_test(NAME tst_workflow_model_sources COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_model_sources.py" -v)
    set_tests_properties(tst_workflow_native_import tst_workflow_model_sources PROPERTIES TIMEOUT 120)
    add_test(NAME tst_workflow_source_reference COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_source_reference.py" -v)
    add_test(NAME tst_workflow_native_results COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_results.py" -v)
    set_tests_properties(tst_workflow_native_results PROPERTIES TIMEOUT 60)
    add_test(NAME tst_workflow_imported_runtime COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_imported_workflow.py"
        --solver "$<TARGET_FILE:fea-solver>" --native "$<TARGET_FILE:range-workflow-native>"
        --examples "${CMAKE_SOURCE_DIR}/fea-solver/examples/stage9"
        --fixture "$<TARGET_FILE:range-workflow-native-import-fixture>"
        --python "${Python3_EXECUTABLE}" -v)
    set_tests_properties(tst_workflow_imported_runtime PROPERTIES TIMEOUT 600)
    if(WIN32)
        set_tests_properties(tst_workflow_imported_runtime PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    if(WIN32)
        set_tests_properties(tst_workflow_native_import PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
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
    add_test(NAME tst_workflow_recovery COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_recovery.py" -v)
    set_tests_properties(tst_workflow_recovery PROPERTIES TIMEOUT 60)
    add_test(NAME tst_workflow_recovery_runtime COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_recovery_runtime.py"
        --solver "$<TARGET_FILE:fea-solver>" --native "$<TARGET_FILE:range-workflow-native>"
        --examples "${CMAKE_SOURCE_DIR}/fea-solver/examples/stage9"
        --python "${Python3_EXECUTABLE}" -v)
    set_tests_properties(tst_workflow_recovery_runtime PROPERTIES TIMEOUT 300)
    if(WIN32)
        set_tests_properties(tst_workflow_recovery_runtime PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    add_test(NAME tst_workflow_extensions COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_extensions.py" -v)
    set_tests_properties(tst_workflow_extensions PROPERTIES TIMEOUT 120)
    if(RANGE_PRIVATE_PYTHON_DIRECTORY)
        add_test(NAME tst_workflow_scientific_contract COMMAND "${RANGE_PRIVATE_PYTHON_DIRECTORY}/python.exe" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scientific_contract.py" -v)
        add_test(NAME tst_workflow_scientific_extensions COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scientific_extensions.py" -v)
        set_tests_properties(tst_workflow_scientific_contract PROPERTIES TIMEOUT 60)
        set_tests_properties(tst_workflow_scientific_extensions PROPERTIES TIMEOUT 180
            ENVIRONMENT "RANGE_TEST_PRIVATE_PYTHON=${RANGE_PRIVATE_PYTHON_DIRECTORY}/python.exe")
        add_test(NAME tst_workflow_scientific_integration COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scientific_integration.py"
            --solver "$<TARGET_FILE:fea-solver>" --native "$<TARGET_FILE:range-workflow-native>"
            --examples "${CMAKE_SOURCE_DIR}/fea-solver/examples/stage9"
            --python "${RANGE_PRIVATE_PYTHON_DIRECTORY}/python.exe" -v)
        set_tests_properties(tst_workflow_scientific_integration PROPERTIES TIMEOUT 600)
        if(WIN32)
            set_tests_properties(tst_workflow_scientific_integration PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        endif()
    endif()
    add_test(NAME tst_workflow_telemetry COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_telemetry.py" -v)
    set_tests_properties(tst_workflow_telemetry PROPERTIES TIMEOUT 60)
    add_test(NAME tst_workflow_series_analysis COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_series_analysis.py" -v)
    add_test(NAME tst_workflow_runtime_parameters COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_runtime_parameters.py" -v)
    add_test(NAME tst_workflow_runtime_control COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_runtime_control.py" -v)
    add_test(NAME tst_workflow_runtime_reference COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_runtime_reference.py" -v)
    add_test(NAME tst_workflow_runtime_manager COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_runtime_manager.py" -v)
    add_test(NAME tst_workflow_runtime_control_integration COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_runtime_control_integration.py"
        --solver "$<TARGET_FILE:fea-solver>" --native "$<TARGET_FILE:range-workflow-native>"
        --examples "${CMAKE_SOURCE_DIR}/fea-solver/examples/stage9"
        --python "${Python3_EXECUTABLE}" -v)
    set_tests_properties(tst_workflow_runtime_control_integration PROPERTIES TIMEOUT 180)
    add_test(NAME tst_workflow_runtime_interrupt COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_runtime_interrupt.py"
        --solver "$<TARGET_FILE:fea-solver>" --native "$<TARGET_FILE:range-workflow-native>"
        --examples "${CMAKE_SOURCE_DIR}/fea-solver/examples/stage9"
        --python "${Python3_EXECUTABLE}" -v)
    set_tests_properties(tst_workflow_runtime_interrupt PROPERTIES TIMEOUT 240)
    foreach(runtime_story IN ITEMS failure final_receipt)
        add_test(NAME tst_workflow_runtime_${runtime_story} COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_runtime_${runtime_story}.py"
            --solver "$<TARGET_FILE:fea-solver>" --native "$<TARGET_FILE:range-workflow-native>"
            --examples "${CMAKE_SOURCE_DIR}/fea-solver/examples/stage9"
            --python "${Python3_EXECUTABLE}" -v)
        set_tests_properties(tst_workflow_runtime_${runtime_story} PROPERTIES TIMEOUT 180)
        if(WIN32)
            set_tests_properties(tst_workflow_runtime_${runtime_story} PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        endif()
    endforeach()
    if(WIN32)
        set_tests_properties(tst_workflow_runtime_control_integration tst_workflow_runtime_interrupt PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    set_tests_properties(tst_workflow_runtime_parameters tst_workflow_runtime_control PROPERTIES TIMEOUT 30)
    add_test(NAME tst_workflow_series_service COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_series_service.py" -v)
    add_test(NAME tst_workflow_series_workbench COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_series_workbench.py" -v)
    add_test(NAME tst_workflow_series_workbench_integration COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_series_workbench_integration.py"
        --solver "$<TARGET_FILE:fea-solver>" --native "$<TARGET_FILE:range-workflow-native>"
        --examples "${CMAKE_SOURCE_DIR}/fea-solver/examples/stage9" --python "${Python3_EXECUTABLE}" -v)
    set_tests_properties(tst_workflow_series_workbench_integration PROPERTIES TIMEOUT 180)
    if(WIN32)
        set_tests_properties(tst_workflow_series_workbench_integration PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    set_tests_properties(tst_workflow_series_analysis tst_workflow_series_service PROPERTIES TIMEOUT 60)
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
    add_test(NAME tst_native_document_sdk COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_document_client.py"
        --cli "$<TARGET_FILE:range-model-command>"
        --source "${CMAKE_SOURCE_DIR}/fea-solver/examples/stage9/stage9_truss4.ram" -v)
    set_tests_properties(tst_native_document_sdk PROPERTIES TIMEOUT 120)
    if(WIN32)
        set_tests_properties(tst_native_document_sdk PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    if(WIN32)
        set_tests_properties(tst_workflow_native tst_workflow_runtime PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
endif()
