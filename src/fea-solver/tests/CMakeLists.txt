find_package(Qt6 REQUIRED COMPONENTS Test)

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
