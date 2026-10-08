qt_add_executable(fea-solver
    src/application.cpp
    src/command_processor.cpp
    src/control_ndjson_transport.cpp
    src/control_typed_ndjson_transport.cpp
    src/typed_control_task.cpp
    src/execution_event_journal.cpp
    src/main.cpp
    src/main_task.cpp
    src/linear_model_task.cpp
    src/native_progress_journal.cpp
    src/native_result_journal.cpp
    src/solver_input.cpp
    src/solver_task.cpp

    src/application.h
    src/command_processor.h
    src/control_ndjson_transport.h
    src/control_typed_ndjson_transport.h
    src/typed_control_task.h
    src/execution_event_journal.h
    src/main_task.h
    src/linear_model_task.h
    src/native_progress_journal.h
    src/native_result_journal.h
    src/solver_input.h
    src/solver_task.h
)

add_dependencies(fea-solver range-base-lib range-model-lib range-solver-lib)

qt_add_executable(range-control-velocity examples/control_velocity_cpp.cpp)
target_link_libraries(range-control-velocity PRIVATE Qt6::Core)
install(TARGETS range-control-velocity RUNTIME DESTINATION bin COMPONENT Application)

install(DIRECTORY examples/ DESTINATION examples/control COMPONENT Application
    FILES_MATCHING PATTERN "*.py" PATTERN "*.json" PATTERN "*.ram" PATTERN "*.m" PATTERN "*.cpp"
    PATTERN "__pycache__" EXCLUDE)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/examples/stage9/stage9-requirements.txt"
    DESTINATION examples/control/stage9 COMPONENT Application)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/examples/control-learning-requirements.txt"
    DESTINATION examples/control COMPONENT Application)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/CONTROLLER_ADAPTERS.md"
    DESTINATION doc COMPONENT Application)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/TYPED_FE_CONTROL.md"
    DESTINATION doc COMPONENT Application)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/STAGE9_CONTROL_ANALYSIS.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../TECHNICAL_ROUTE_STAGE9.md"
    DESTINATION doc COMPONENT Application)

target_link_libraries(fea-solver
    PRIVATE
        range-solver-lib
        Qt6::Network
        common_defines
)

if(RANGE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
