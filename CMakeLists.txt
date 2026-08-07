cmake_minimum_required(VERSION 3.16)
project(mybot VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(MYBOT_TOP_LEVEL ON)
else()
    set(MYBOT_TOP_LEVEL OFF)
endif()

if(MYBOT_TOP_LEVEL AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(MYBOT_LINUX_DEFAULT ON)
else()
    set(MYBOT_LINUX_DEFAULT OFF)
endif()

option(MYBOT_BUILD_LINUX_PLATFORM "Build the bundled Linux platform backend" ${MYBOT_LINUX_DEFAULT})
option(MYBOT_BUILD_EXAMPLES "Build mybot example applications" ${MYBOT_TOP_LEVEL})
option(MYBOT_BUILD_TESTS "Build mybot tests" ${MYBOT_TOP_LEVEL})
option(MYBOT_ENABLE_ASAN "Enable AddressSanitizer for mybot targets" ${MYBOT_TOP_LEVEL})

set(AOSL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/aosl" CACHE PATH
    "Path to the AOSL source tree")
set(AGORA_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/agora_rtsa_sdk/agora_sdk" CACHE PATH
    "Path to the Agora RTSA SDK package")
set(AGORA_RTC_LIBRARY "${AGORA_SDK_DIR}/lib/x86_64/libagora-rtc-sdk.a" CACHE FILEPATH
    "Path to the Agora RTSA library for the target architecture")

if(NOT TARGET aosl)
    set(AOSL_DECLARE_PROJECT OFF CACHE BOOL "Build AOSL as a standalone project" FORCE)
    set(AOSL_COMPILE_TEST OFF CACHE BOOL "Build AOSL tests" FORCE)
    include("${AOSL_DIR}/CMakeLists.txt")
    add_library(aosl STATIC ${AOSL_ADD_SOURCES})
    target_include_directories(aosl
        PUBLIC ${AOSL_ADD_INCLUDES_PUBLIC}
        PRIVATE ${AOSL_ADD_INCLUDES_PRIVATE}
    )
    target_compile_options(aosl PRIVATE ${AOSL_ADD_OPTIONS})
    target_compile_definitions(aosl PRIVATE ${AOSL_ADD_DEFINITIONS})
endif()

if(NOT TARGET agora-rtc-sdk)
    add_library(agora-rtc-sdk STATIC IMPORTED GLOBAL)
    set_target_properties(agora-rtc-sdk PROPERTIES
        IMPORTED_LOCATION "${AGORA_RTC_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${AGORA_SDK_DIR}/include"
    )
endif()

function(mybot_enable_sanitizers target)
    if(MYBOT_ENABLE_ASAN AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
        target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
endfunction()

add_library(mybot_sdk STATIC
    src/core/mybot_app.c
    src/core/mybot_device_lifecycle.c
    src/media/mybot_audio.c
    src/media/mybot_wake_words.c
    src/platform/mybot_key.c
    src/platform/mybot_kv_store.c
    src/platform/mybot_lcd.c
    src/platform/mybot_wifi.c
    src/rtc/mybot_agora_rtc.c
    src/service/mybot_device_client.c
    src/support/mybot_http_client.c
    src/support/mybot_json.c
    src/support/mybot_ringbuf.c
)
add_library(mybot::sdk ALIAS mybot_sdk)
set_target_properties(mybot_sdk PROPERTIES EXPORT_NAME sdk)

target_include_directories(mybot_sdk
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
)
target_link_libraries(mybot_sdk PUBLIC aosl agora-rtc-sdk)
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(mybot_sdk PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(mybot_sdk PUBLIC rt)
endif()
mybot_enable_sanitizers(mybot_sdk)

if(MYBOT_BUILD_LINUX_PLATFORM)
    add_subdirectory(platforms/linux)
endif()

if(MYBOT_BUILD_EXAMPLES)
    if(NOT MYBOT_BUILD_LINUX_PLATFORM)
        message(FATAL_ERROR "MYBOT_BUILD_EXAMPLES currently requires MYBOT_BUILD_LINUX_PLATFORM=ON")
    endif()
    add_subdirectory(examples/linux)
endif()

if(MYBOT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS mybot_sdk
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/mybot DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
