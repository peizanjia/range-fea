find_package(Qt6 REQUIRED COMPONENTS Test)

if(WIN32 AND CMAKE_VERSION VERSION_LESS 3.22)
    message(FATAL_ERROR "Windows QTest execution requires CMake 3.22 or newer for ENVIRONMENT_MODIFICATION")
endif()

set(CMAKE_AUTOMOC ON)

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
    RANGE_PID_EXAMPLE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../examples/control_pid_python.py")
target_link_libraries(tst_solver_audit_cli PRIVATE range-solver-lib Qt6::Test)
add_dependencies(tst_solver_audit_cli fea-solver control_ndjson_fixture)
add_test(NAME tst_solver_audit_cli COMMAND tst_solver_audit_cli)
if(WIN32)
    set_tests_properties(tst_solver_audit_cli PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()
