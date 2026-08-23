if(NOT MYBOT_SOURCE_DIR OR NOT MYBOT_BINARY_DIR)
    message(FATAL_ERROR "MYBOT_SOURCE_DIR and MYBOT_BINARY_DIR are required")
endif()

set(source_build_dir "${MYBOT_BINARY_DIR}/install-source")
set(install_prefix "${MYBOT_BINARY_DIR}/install-check")
set(consumer_dir "${MYBOT_BINARY_DIR}/install-consumer")

file(REMOVE_RECURSE "${source_build_dir}")
file(REMOVE_RECURSE "${install_prefix}")
file(REMOVE_RECURSE "${consumer_dir}")

# Install from a clean non-sanitized build: the outer build may enable ASAN,
# and sanitizer flags do not propagate through the installed package.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${MYBOT_SOURCE_DIR}" -B "${source_build_dir}"
            -DMYBOT_BUILD_LINUX_PLATFORM=OFF
            -DMYBOT_BUILD_EXAMPLES=OFF
            -DMYBOT_BUILD_TESTS=OFF
            -DMYBOT_ENABLE_ASAN=OFF
    RESULT_VARIABLE source_configure_result
)
if(NOT source_configure_result EQUAL 0)
    message(FATAL_ERROR "install source configure failed: ${source_configure_result}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${source_build_dir}" --target mybot_sdk
    RESULT_VARIABLE source_build_result
)
if(NOT source_build_result EQUAL 0)
    message(FATAL_ERROR "install source build failed: ${source_build_result}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${source_build_dir}" --prefix "${install_prefix}"
    RESULT_VARIABLE install_result
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "cmake --install failed: ${install_result}")
endif()

foreach(required_document LICENSE THIRD_PARTY_NOTICES.md aosl-LICENSE)
    if(NOT EXISTS "${install_prefix}/share/doc/mybot/${required_document}")
        message(FATAL_ERROR "installed package is missing ${required_document}")
    endif()
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${MYBOT_SOURCE_DIR}/tests/integration/cmake_install_consumer"
            -B "${consumer_dir}"
            -DCMAKE_PREFIX_PATH=${install_prefix}
            -DMYBOT_AGORA_SDK_DIR=${MYBOT_SOURCE_DIR}/third_party/agora_rtsa_sdk/agora_sdk
            -DMYBOT_AGORA_RTC_LIBRARY=${MYBOT_SOURCE_DIR}/third_party/agora_rtsa_sdk/agora_sdk/lib/x86_64/libagora-rtc-sdk.a
    RESULT_VARIABLE configure_result
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "install consumer configure failed: ${configure_result}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${consumer_dir}"
    RESULT_VARIABLE build_result
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "install consumer build failed: ${build_result}")
endif()

execute_process(
    COMMAND "${consumer_dir}/mybot_install_consumer_check"
    RESULT_VARIABLE run_result
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "install consumer check failed: ${run_result}")
endif()
