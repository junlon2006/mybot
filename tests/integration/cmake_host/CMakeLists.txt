cmake_minimum_required(VERSION 3.16)
project(mybot_cmake_host_check LANGUAGES C)

if(NOT MYBOT_SOURCE_DIR)
    message(FATAL_ERROR "MYBOT_SOURCE_DIR must point to the mybot source tree")
endif()

set(MYBOT_BUILD_LINUX_PLATFORM OFF CACHE BOOL "" FORCE)
set(MYBOT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MYBOT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MYBOT_ENABLE_ASAN OFF CACHE BOOL "" FORCE)

add_subdirectory("${MYBOT_SOURCE_DIR}" mybot)

add_executable(mybot_cmake_host_check main.c)
target_link_libraries(mybot_cmake_host_check PRIVATE mybot::sdk)
