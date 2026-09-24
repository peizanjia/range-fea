qt_add_executable(fea-solver
    src/application.cpp
    src/command_processor.cpp
    src/control_ndjson_transport.cpp
    src/execution_event_journal.cpp
    src/main.cpp
    src/main_task.cpp
    src/solver_input.cpp
    src/solver_task.cpp

    src/application.h
    src/command_processor.h
    src/control_ndjson_transport.h
    src/execution_event_journal.h
    src/main_task.h
    src/solver_input.h
    src/solver_task.h
)

add_dependencies(fea-solver range-base-lib range-model-lib range-solver-lib)

target_link_libraries(fea-solver
    PRIVATE
        range-solver-lib
        Qt6::Network
        common_defines
)

if(RANGE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
