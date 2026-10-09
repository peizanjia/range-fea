qt_add_library(range-application-lib STATIC
    src/rmodel_command_service.cpp
    include/rmodel_command_service.h
    src/rmodel_command_session.cpp
    include/rmodel_command_session.h
    src/rproject_catalog.cpp
    include/rproject_catalog.h
    src/ragent_conversation_store.cpp
    include/ragent_conversation_store.h
    src/rnative_result_broker.cpp
    include/rnative_result_broker.h)
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
qt_add_executable(range-native-results tools/native_result_cli.cpp)
target_link_libraries(range-native-results PRIVATE range-application-lib)
set_target_properties(range-native-results PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
install(TARGETS range-native-results RUNTIME DESTINATION bin COMPONENT Application)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../docs/APPLICATION_SERVICE.md DESTINATION doc COMPONENT Application)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../docs/PROJECT_CATALOG.md DESTINATION doc COMPONENT Application)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../docs/NATIVE_MODEL_OPERATIONS.md DESTINATION doc COMPONENT Application)

if(RANGE_BUILD_TESTS)
    qt_add_executable(tst_agent_conversation_store tests/tst_agent_conversation_store.cpp)
    target_link_libraries(tst_agent_conversation_store PRIVATE range-application-lib Qt6::Test)
    add_test(NAME tst_agent_conversation_store COMMAND tst_agent_conversation_store -o -,txt)
    set_tests_properties(tst_agent_conversation_store PROPERTIES TIMEOUT 120)
    if(WIN32)
        set_tests_properties(tst_agent_conversation_store PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    qt_add_executable(tst_native_result_broker tests/tst_native_result_broker.cpp)
    target_link_libraries(tst_native_result_broker PRIVATE range-application-lib Qt6::Test)
    add_dependencies(tst_native_result_broker range-native-results)
    add_test(NAME tst_native_result_broker COMMAND tst_native_result_broker -o -,txt)
    if(WIN32)
        set_tests_properties(tst_native_result_broker PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
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
    qt_add_executable(tst_model_command_service tests/tst_model_command_service.cpp)
    target_link_libraries(tst_model_command_service PRIVATE range-application-lib Qt6::Test)
    add_test(NAME tst_model_command_service COMMAND tst_model_command_service -o -,txt)
    if(WIN32)
        set_tests_properties(tst_model_command_service PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
endif()
