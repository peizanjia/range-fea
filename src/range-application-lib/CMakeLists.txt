qt_add_library(range-application-lib STATIC
    src/rmodel_command_session.cpp
    include/rmodel_command_session.h)
target_include_directories(range-application-lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(range-application-lib PUBLIC range-model-lib Qt6::Core)

if(RANGE_BUILD_TESTS)
    qt_add_executable(tst_model_command_session tests/tst_model_command_session.cpp)
    target_link_libraries(tst_model_command_session PRIVATE range-application-lib Qt6::Test)
    add_test(NAME tst_model_command_session COMMAND tst_model_command_session -o -,txt)
    if(WIN32)
        set_tests_properties(tst_model_command_session PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
endif()
