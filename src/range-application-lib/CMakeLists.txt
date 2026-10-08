qt_add_library(range-application-lib STATIC
    src/rmodel_command_session.cpp
    include/rmodel_command_session.h
    src/rproject_catalog.cpp
    include/rproject_catalog.h)
target_include_directories(range-application-lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(range-application-lib PUBLIC range-model-lib Qt6::Core)

qt_add_executable(range-model-command tools/model_command_cli.cpp)
target_link_libraries(range-model-command PRIVATE range-application-lib)
set_target_properties(range-model-command PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
install(TARGETS range-model-command RUNTIME DESTINATION bin COMPONENT Application)
qt_add_executable(range-project tools/project_catalog_cli.cpp)
target_link_libraries(range-project PRIVATE range-application-lib)
set_target_properties(range-project PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
install(TARGETS range-project RUNTIME DESTINATION bin COMPONENT Application)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../docs/APPLICATION_SERVICE.md DESTINATION doc COMPONENT Application)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../docs/PROJECT_CATALOG.md DESTINATION doc COMPONENT Application)

if(RANGE_BUILD_TESTS)
    qt_add_executable(tst_model_command_cli tests/tst_model_command_cli.cpp)
    target_link_libraries(tst_model_command_cli PRIVATE range-application-lib Qt6::Test)
    add_dependencies(tst_model_command_cli range-model-command)
    add_test(NAME tst_model_command_cli COMMAND tst_model_command_cli -o -,txt)
    if(WIN32)
        set_tests_properties(tst_model_command_cli PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    qt_add_executable(tst_project_catalog tests/tst_project_catalog.cpp)
    target_link_libraries(tst_project_catalog PRIVATE range-application-lib Qt6::Test)
    add_dependencies(tst_project_catalog range-project)
    add_test(NAME tst_project_catalog COMMAND tst_project_catalog -o -,txt)
    if(WIN32)
        set_tests_properties(tst_project_catalog PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    qt_add_executable(tst_model_command_session tests/tst_model_command_session.cpp)
    target_link_libraries(tst_model_command_session PRIVATE range-application-lib Qt6::Test)
    add_test(NAME tst_model_command_session COMMAND tst_model_command_session -o -,txt)
    if(WIN32)
        set_tests_properties(tst_model_command_session PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
endif()
