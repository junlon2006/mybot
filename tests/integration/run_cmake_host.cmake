if(NOT MYBOT_SOURCE_DIR OR NOT MYBOT_BINARY_DIR)
    message(FATAL_ERROR "MYBOT_SOURCE_DIR and MYBOT_BINARY_DIR are required")
endif()

file(REMOVE_RECURSE "${MYBOT_BINARY_DIR}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${MYBOT_SOURCE_DIR}/tests/integration/cmake_host"
            -B "${MYBOT_BINARY_DIR}" -DMYBOT_SOURCE_DIR=${MYBOT_SOURCE_DIR}
    RESULT_VARIABLE configure_result
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "CMake host fixture configure failed: ${configure_result}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${MYBOT_BINARY_DIR}" --target mybot_cmake_host_check
    RESULT_VARIABLE build_result
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "CMake host fixture build failed: ${build_result}")
endif()

execute_process(
    COMMAND "${MYBOT_BINARY_DIR}/mybot_cmake_host_check"
    RESULT_VARIABLE run_result
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "CMake host fixture failed: ${run_result}")
endif()
