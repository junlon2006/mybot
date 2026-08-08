cmake_minimum_required(VERSION 3.16)

# Single source of truth for the SDK version. include/mybot/mybot_version.h is
# generated from mybot_version.h.in at configure time, so the code can never
# drift from the version declared here.
set(MYBOT_VERSION_MAJOR 0)
set(MYBOT_VERSION_MINOR 1)
set(MYBOT_VERSION_PATCH 0)
set(MYBOT_VERSION_PRERELEASE "rc.1")
if(MYBOT_VERSION_PRERELEASE)
    set(MYBOT_VERSION_STRING
        "${MYBOT_VERSION_MAJOR}.${MYBOT_VERSION_MINOR}.${MYBOT_VERSION_PATCH}-${MYBOT_VERSION_PRERELEASE}")
else()
    set(MYBOT_VERSION_STRING
        "${MYBOT_VERSION_MAJOR}.${MYBOT_VERSION_MINOR}.${MYBOT_VERSION_PATCH}")
endif()

project(mybot VERSION ${MYBOT_VERSION_MAJOR}.${MYBOT_VERSION_MINOR}.${MYBOT_VERSION_PATCH}
        LANGUAGES C)

include(GNUInstallDirs)

configure_file(include/mybot/mybot_version.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/include/mybot/mybot_version.h @ONLY)

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
option(MYBOT_BUILD_EXAMPLES "Build mybot example applications" ${MYBOT_LINUX_DEFAULT})
option(MYBOT_BUILD_TESTS "Build mybot tests" ${MYBOT_LINUX_DEFAULT})
option(MYBOT_ENABLE_ASAN "Enable AddressSanitizer for mybot targets" OFF)

set(MYBOT_AUDIO_PTIME_MS "60" CACHE STRING "Audio packet duration: 20, 40, or 60 ms")
set_property(CACHE MYBOT_AUDIO_PTIME_MS PROPERTY STRINGS 20 40 60)
option(MYBOT_CLOUD_AEC "Enable server-side acoustic echo cancellation" ON)
option(MYBOT_WAKE_WORDS "Enable the platform local-ASR wake-word backend" OFF)
option(MYBOT_AI_QOS "Enable Agora AI-driven QoS optimization" ON)
set(MYBOT_FAST_SEND_MULTIPLIER "3" CACHE STRING "Agora fast-send multiplier: 1 through 5")
set_property(CACHE MYBOT_FAST_SEND_MULTIPLIER PROPERTY STRINGS 1 2 3 4 5)
option(MYBOT_SHOW_TRANSCRIPT "Receive real-time transcripts over the data stream" OFF)
option(MYBOT_ENABLE_HTTPS "Enable HTTPS device-service transport" ON)
option(MYBOT_ALLOW_INSECURE_HTTP "Allow plaintext HTTP for local development" OFF)

if(NOT MYBOT_ENABLE_HTTPS AND NOT MYBOT_ALLOW_INSECURE_HTTP)
    message(FATAL_ERROR
        "Enable MYBOT_ENABLE_HTTPS or explicitly set MYBOT_ALLOW_INSECURE_HTTP=ON")
endif()

if(NOT MYBOT_AUDIO_PTIME_MS MATCHES "^(20|40|60)$")
    message(FATAL_ERROR "MYBOT_AUDIO_PTIME_MS must be 20, 40, or 60")
endif()
if(NOT MYBOT_FAST_SEND_MULTIPLIER MATCHES "^[1-5]$")
    message(FATAL_ERROR "MYBOT_FAST_SEND_MULTIPLIER must be an integer from 1 through 5")
endif()

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
    if(NOT EXISTS "${AGORA_SDK_DIR}/include/agora_rtc_api.h")
        message(FATAL_ERROR
            "Agora RTSA headers were not found under AGORA_SDK_DIR=${AGORA_SDK_DIR}")
    endif()
    if(NOT EXISTS "${AGORA_RTC_LIBRARY}")
        message(FATAL_ERROR "Agora RTSA library was not found: ${AGORA_RTC_LIBRARY}")
    endif()

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
    src/core/mybot_version.c
    src/media/mybot_audio.c
    src/media/mybot_wake_words.c
    src/platform/mybot_key.c
    src/platform/mybot_https_transport.c
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
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
)
target_compile_definitions(mybot_sdk PUBLIC
    MYBOT_AUDIO_PTIME_MS=${MYBOT_AUDIO_PTIME_MS}
    MYBOT_CLOUD_AEC=$<BOOL:${MYBOT_CLOUD_AEC}>
    MYBOT_WAKE_WORDS=$<BOOL:${MYBOT_WAKE_WORDS}>
    MYBOT_AI_QOS=$<BOOL:${MYBOT_AI_QOS}>
    MYBOT_FAST_SEND_MULTIPLIER=${MYBOT_FAST_SEND_MULTIPLIER}
    MYBOT_SHOW_TRANSCRIPT=$<BOOL:${MYBOT_SHOW_TRANSCRIPT}>
    MYBOT_ENABLE_HTTPS=$<BOOL:${MYBOT_ENABLE_HTTPS}>
    MYBOT_ALLOW_INSECURE_HTTP=$<BOOL:${MYBOT_ALLOW_INSECURE_HTTP}>
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
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/mybot/mybot_version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mybot)
install(DIRECTORY include/mybot DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        PATTERN "*.in" EXCLUDE)
