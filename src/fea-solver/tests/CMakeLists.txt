find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(tst_native_progress_journal
    tst_native_progress_journal.cpp ../src/native_progress_journal.cpp)
target_include_directories(tst_native_progress_journal PRIVATE ../src)
target_link_libraries(tst_native_progress_journal PRIVATE range-base-lib Qt6::Test)
add_test(NAME tst_native_progress_journal COMMAND tst_native_progress_journal)
if(WIN32)
    set_tests_properties(tst_native_progress_journal PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

if(WIN32 AND CMAKE_VERSION VERSION_LESS 3.22)
    message(FATAL_ERROR "Windows QTest execution requires CMake 3.22 or newer for ENVIRONMENT_MODIFICATION")
endif()

set(CMAKE_AUTOMOC ON)

if(RANGE_REQUIRE_NUMERIC_TESTS)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
else()
    find_package(Python3 3.10 QUIET COMPONENTS Interpreter)
endif()
if(Python3_Interpreter_FOUND)
    add_test(NAME tst_control_tcp_bridge COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_control_tcp_bridge.py")
    add_test(NAME tst_control_learned COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_control_learned.py")
    add_test(NAME tst_control_velocity_cpp COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_control_velocity_cpp.py" "$<TARGET_FILE:range-control-velocity>")
    if(WIN32)
        set_tests_properties(tst_control_velocity_cpp PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import numpy,scipy"
        RESULT_VARIABLE stage9_python_dependencies OUTPUT_QUIET ERROR_VARIABLE stage9_python_dependency_error)
    if(stage9_python_dependencies EQUAL 0)
        qt_add_executable(tst_controller_adapters_cli tst_controller_adapters_cli.cpp)
        target_include_directories(tst_controller_adapters_cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../range-solver-lib/tests")
        target_link_libraries(tst_controller_adapters_cli PRIVATE range-solver-lib Qt6::Test)
        target_compile_definitions(tst_controller_adapters_cli PRIVATE
            RANGE_CONTROL_PYTHON="${Python3_EXECUTABLE}"
            RANGE_CONTROL_EXAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/../examples")
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import torch"
            RESULT_VARIABLE control_torch_available OUTPUT_QUIET ERROR_QUIET)
        if(control_torch_available EQUAL 0)
            target_compile_definitions(tst_controller_adapters_cli PRIVATE RANGE_CONTROL_TORCH_AVAILABLE=1)
        else()
            message(STATUS "PyTorch unavailable: learned policy training/FE integration not registered")
        endif()
        add_dependencies(tst_controller_adapters_cli fea-solver range-control-velocity)
        add_test(NAME tst_controller_adapters_cli COMMAND tst_controller_adapters_cli)
        if(WIN32)
            set_tests_properties(tst_controller_adapters_cli PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        endif()
        add_test(NAME tst_stage9_analysis COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_stage9_analysis.py")
        add_test(NAME tst_stage9_velocity_controller COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_stage9_velocity_controller.py")
        qt_add_executable(tst_stage9_cli tst_stage9_cli.cpp)
        target_include_directories(tst_stage9_cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../range-solver-lib/tests")
        target_link_libraries(tst_stage9_cli PRIVATE range-solver-lib Qt6::Test)
        target_compile_definitions(tst_stage9_cli PRIVATE
            RANGE_STAGE9_PYTHON="${Python3_EXECUTABLE}"
            RANGE_STAGE9_EXAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/../examples/stage9")
        add_dependencies(tst_stage9_cli fea-solver)
        add_test(NAME tst_stage9_cli COMMAND tst_stage9_cli)
        if(WIN32)
            set_tests_properties(tst_stage9_cli PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        endif()
    else()
        if(RANGE_REQUIRE_NUMERIC_TESTS)
            message(FATAL_ERROR
                "RANGE_REQUIRE_NUMERIC_TESTS=ON, but Python could not import NumPy and SciPy. "
                "Install examples/stage9/stage9-requirements.txt for ${Python3_EXECUTABLE}. "
                "Python reported: ${stage9_python_dependency_error}")
        endif()
        message(WARNING "Stage 9 Python tests require NumPy and SciPy; install examples/stage9/stage9-requirements.txt")
    endif()
else()
    message(WARNING "Stage 9 analysis/controller workflow requires Python 3.10+; integration tests not registered")
endif()

qt_add_executable(tst_fea_solver_input
    tst_solver_input.cpp
    ../src/solver_input.cpp
)
target_include_directories(tst_fea_solver_input PRIVATE ../src)
target_link_libraries(tst_fea_solver_input PRIVATE Qt6::Test)

add_test(NAME tst_fea_solver_input COMMAND tst_fea_solver_input)
if(WIN32)
    set_tests_properties(tst_fea_solver_input PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

qt_add_executable(tst_execution_event_journal
    tst_execution_event_journal.cpp
    ../src/execution_event_journal.cpp
)
target_include_directories(tst_execution_event_journal PRIVATE ../src)
target_link_libraries(tst_execution_event_journal PRIVATE Qt6::Test)
add_test(NAME tst_execution_event_journal COMMAND tst_execution_event_journal)
if(WIN32)
    set_tests_properties(tst_execution_event_journal PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

qt_add_executable(control_ndjson_fixture control_ndjson_fixture.cpp)
target_link_libraries(control_ndjson_fixture PRIVATE Qt6::Core)

qt_add_executable(tst_control_ndjson_transport
    tst_control_ndjson_transport.cpp
    ../src/control_ndjson_transport.cpp
)
target_include_directories(tst_control_ndjson_transport PRIVATE ../src)
target_link_libraries(tst_control_ndjson_transport PRIVATE Qt6::Test)
add_dependencies(tst_control_ndjson_transport control_ndjson_fixture)
add_test(NAME tst_control_ndjson_transport COMMAND tst_control_ndjson_transport)
if(WIN32)
    set_tests_properties(tst_control_ndjson_transport PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

qt_add_executable(tst_solver_audit_cli
    tst_solver_audit_cli.cpp
    ../src/execution_event_journal.cpp
)
target_include_directories(tst_solver_audit_cli PRIVATE ../src)
target_compile_definitions(tst_solver_audit_cli PRIVATE
    RANGE_PID_EXAMPLE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../examples/control_pid_python.py"
    RANGE_TYPED_CONTROL_EXAMPLE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../examples/control_typed_ndjson_python.py")
target_link_libraries(tst_solver_audit_cli PRIVATE range-solver-lib Qt6::Test)
add_dependencies(tst_solver_audit_cli fea-solver control_ndjson_fixture)
add_test(NAME tst_solver_audit_cli COMMAND tst_solver_audit_cli)
if(WIN32)
    set_tests_properties(tst_solver_audit_cli PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

qt_add_executable(tst_control_typed_ndjson_transport
    tst_control_typed_ndjson_transport.cpp
    ../src/control_typed_ndjson_transport.cpp)
target_include_directories(tst_control_typed_ndjson_transport PRIVATE ../src)
target_compile_definitions(tst_control_typed_ndjson_transport PRIVATE
    RANGE_TYPED_CONTROL_EXAMPLE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../examples/control_typed_ndjson_python.py")
target_link_libraries(tst_control_typed_ndjson_transport PRIVATE range-solver-lib Qt6::Test)
add_test(NAME tst_control_typed_ndjson_transport COMMAND tst_control_typed_ndjson_transport)
if(WIN32)
    set_tests_properties(tst_control_typed_ndjson_transport PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()
