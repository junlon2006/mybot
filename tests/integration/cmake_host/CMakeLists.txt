cmake_minimum_required(VERSION 3.16)
project(mybot_cmake_host_check LANGUAGES C)

if(NOT MYBOT_SOURCE_DIR)
    message(FATAL_ERROR "MYBOT_SOURCE_DIR must point to the mybot source tree")
endif()

set(MYBOT_BUILD_LINUX_PLATFORM OFF CACHE BOOL "" FORCE)
set(MYBOT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MYBOT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MYBOT_ENABLE_ASAN OFF CACHE BOOL "" FORCE)

# A real host integration supplies the target-architecture Agora package.
# Predeclare the target to verify that mybot does not require its bundled
# x86_64 paths when the host already owns this dependency.
add_library(agora-rtc-sdk SHARED IMPORTED GLOBAL)
set_target_properties(agora-rtc-sdk PROPERTIES
    IMPORTED_LOCATION "${MYBOT_SOURCE_DIR}/third_party/agora_rtsa_sdk/agora_sdk/lib/x86_64/libagora-rtc-sdk.so"
    INTERFACE_INCLUDE_DIRECTORIES "${MYBOT_SOURCE_DIR}/third_party/agora_rtsa_sdk/agora_sdk/include"
)

add_subdirectory("${MYBOT_SOURCE_DIR}" mybot)

add_executable(mybot_cmake_host_check main.c)
target_link_libraries(mybot_cmake_host_check PRIVATE mybot::sdk)
