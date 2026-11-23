include("${CMAKE_CURRENT_LIST_DIR}/run_checked.cmake")

if(NOT MYBOT_SOURCE_DIR OR NOT MYBOT_BINARY_DIR OR NOT MYBOT_TEST_CONFIG_DIR)
    message(FATAL_ERROR "MYBOT_SOURCE_DIR, MYBOT_BINARY_DIR and MYBOT_TEST_CONFIG_DIR are required")
endif()

file(REMOVE_RECURSE "${MYBOT_BINARY_DIR}")
run_checked("CMake host fixture configure"
    "${CMAKE_COMMAND}" -S "${MYBOT_SOURCE_DIR}/tests/integration/cmake_host"
    -C "${MYBOT_TEST_CONFIG_DIR}/config.cmake"
    -B "${MYBOT_BINARY_DIR}" -DMYBOT_SOURCE_DIR=${MYBOT_SOURCE_DIR}
)

run_checked("CMake host fixture build"
    "${CMAKE_COMMAND}" --build "${MYBOT_BINARY_DIR}" --target mybot_cmake_host_check
)

run_checked("CMake host C++ fixture build"
    "${CMAKE_COMMAND}" --build "${MYBOT_BINARY_DIR}" --target mybot_cmake_host_cpp_check
)

run_checked("CMake host fixture"
    "${MYBOT_BINARY_DIR}/mybot_cmake_host_check"
)

run_checked("CMake host C++ fixture"
    "${MYBOT_BINARY_DIR}/mybot_cmake_host_cpp_check"
)

# The selected RTSA package must reject a different packet duration.
include("${MYBOT_TEST_CONFIG_DIR}/config.cmake")
if(MYBOT_AUDIO_PTIME_MS EQUAL 20)
    set(mismatched_ptime 60)
else()
    set(mismatched_ptime 20)
endif()
set(ptime_mismatch_dir "${MYBOT_BINARY_DIR}/ptime-mismatch")
file(REMOVE_RECURSE "${ptime_mismatch_dir}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${MYBOT_SOURCE_DIR}" -B "${ptime_mismatch_dir}"
            -C "${MYBOT_TEST_CONFIG_DIR}/config.cmake"
            -DCONFIG_PLATFORM=linux
            -DMYBOT_BUILD_LINUX_PLATFORM=OFF
            -DMYBOT_BUILD_EXAMPLES=OFF
            -DMYBOT_BUILD_TESTS=OFF
            -DMYBOT_ENABLE_HTTPS=OFF
            -DMYBOT_ALLOW_INSECURE_HTTP=ON
            -DMYBOT_AUDIO_PTIME_MS=${mismatched_ptime}
            RESULT_VARIABLE ptime_mismatch_result
            OUTPUT_VARIABLE ptime_mismatch_output
            ERROR_VARIABLE ptime_mismatch_error
)
if(ptime_mismatch_result EQUAL 0)
    message(FATAL_ERROR "CMake accepted an RTSA/ptime mismatch for the selected SDK")
endif()
set(ptime_mismatch_diagnostic "${ptime_mismatch_output}${ptime_mismatch_error}")
if(NOT ptime_mismatch_diagnostic MATCHES "MYBOT_AUDIO_PTIME_MS=.*CONFIG_MINIMAL_TIMER_INTERVAL_MS")
    message(FATAL_ERROR
        "CMake rejected ptime mismatch for an unexpected reason: ${ptime_mismatch_diagnostic}")
endif()
