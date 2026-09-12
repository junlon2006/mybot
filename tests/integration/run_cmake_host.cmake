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

# The bundled RTSA package is built with a 60 ms timer cadence. A different
# packet duration must be rejected unless the caller supplies a matching SDK.
set(ptime_mismatch_dir "${MYBOT_BINARY_DIR}/ptime-mismatch")
file(REMOVE_RECURSE "${ptime_mismatch_dir}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${MYBOT_SOURCE_DIR}" -B "${ptime_mismatch_dir}"
            -DCONFIG_PLATFORM=linux
            -DMYBOT_BUILD_LINUX_PLATFORM=OFF
            -DMYBOT_BUILD_EXAMPLES=OFF
            -DMYBOT_BUILD_TESTS=OFF
            -DMYBOT_ENABLE_HTTPS=OFF
            -DMYBOT_ALLOW_INSECURE_HTTP=ON
            -DMYBOT_AUDIO_PTIME_MS=20
    RESULT_VARIABLE ptime_mismatch_result
)
if(ptime_mismatch_result EQUAL 0)
    message(FATAL_ERROR "CMake accepted an RTSA/ptime mismatch for the bundled SDK")
endif()
