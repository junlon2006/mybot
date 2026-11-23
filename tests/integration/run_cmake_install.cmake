include("${CMAKE_CURRENT_LIST_DIR}/run_checked.cmake")

if(NOT MYBOT_SOURCE_DIR OR NOT MYBOT_BINARY_DIR OR NOT MYBOT_TEST_CONFIG_DIR)
    message(FATAL_ERROR "MYBOT_SOURCE_DIR, MYBOT_BINARY_DIR and MYBOT_TEST_CONFIG_DIR are required")
endif()

set(source_build_dir "${MYBOT_BINARY_DIR}/install-source")
set(install_prefix "${MYBOT_BINARY_DIR}/install-check")
set(consumer_dir "${MYBOT_BINARY_DIR}/install-consumer")

file(REMOVE_RECURSE "${source_build_dir}")
file(REMOVE_RECURSE "${install_prefix}")
file(REMOVE_RECURSE "${consumer_dir}")

# Install from a clean non-sanitized build: the outer build may enable ASAN,
# and sanitizer flags do not propagate through the installed package.
run_checked("install source configure"
    "${CMAKE_COMMAND}" -S "${MYBOT_SOURCE_DIR}" -B "${source_build_dir}"
    -C "${MYBOT_TEST_CONFIG_DIR}/config.cmake"
    -DMYBOT_BUILD_LINUX_PLATFORM=OFF
    -DMYBOT_BUILD_EXAMPLES=OFF
    -DMYBOT_BUILD_TESTS=OFF
    -DMYBOT_ENABLE_ASAN=OFF
)

run_checked("install source build"
    "${CMAKE_COMMAND}" --build "${source_build_dir}" --target mybot_sdk
)

run_checked("cmake --install"
    "${CMAKE_COMMAND}" --install "${source_build_dir}" --prefix "${install_prefix}"
)

foreach(required_document LICENSE THIRD_PARTY_NOTICES.md aosl-LICENSE)
    if(NOT EXISTS "${install_prefix}/share/doc/mybot/${required_document}")
        message(FATAL_ERROR "installed package is missing ${required_document}")
    endif()
endforeach()

file(GLOB_RECURSE installed_pkg_config "${install_prefix}/mybot.pc")
if(installed_pkg_config)
    message(FATAL_ERROR "CMake package installation must not install mybot.pc")
endif()

run_checked("install consumer configure"
    "${CMAKE_COMMAND}" -S "${MYBOT_SOURCE_DIR}/tests/integration/cmake_install_consumer"
    -C "${MYBOT_TEST_CONFIG_DIR}/config.cmake"
    -B "${consumer_dir}"
    -DCMAKE_PREFIX_PATH=${install_prefix}
)

run_checked("install consumer build"
    "${CMAKE_COMMAND}" --build "${consumer_dir}"
)

run_checked("install consumer check"
    "${consumer_dir}/mybot_install_consumer_check"
)
