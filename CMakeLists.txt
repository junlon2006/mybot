cmake_minimum_required(VERSION 3.16)
project(mybot VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(MYBOT_ENABLE_ASAN "Enable AddressSanitizer for runtime memory diagnostics" ON)
if(MYBOT_ENABLE_ASAN)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address)
    else()
        message(FATAL_ERROR "MYBOT_ENABLE_ASAN requires a GNU or Clang compatible compiler")
    endif()
endif()

# ------------------------------------------------------------
# AOSL subsystem
# ------------------------------------------------------------
set(AOSL_DIR ${CMAKE_SOURCE_DIR}/components/aosl)
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_SOURCE_DIR}/cmake/toolchain_native.cmake)
set(AOSL_COMPILE_TEST OFF CACHE BOOL "Compile aosl_test")
add_subdirectory(components/aosl)

# ------------------------------------------------------------
# Agora RTSA SDK
# ------------------------------------------------------------
set(AGORA_SDK_DIR ${CMAKE_SOURCE_DIR}/components/agora_rtsa_sdk/agora_sdk)
set(AGORA_LIB_DIR ${AGORA_SDK_DIR}/lib/x86_64)

add_library(agora-rtc-sdk STATIC IMPORTED)
set_target_properties(agora-rtc-sdk PROPERTIES
    IMPORTED_LOCATION ${AGORA_LIB_DIR}/libagora-rtc-sdk.a
)

# ------------------------------------------------------------
# mybot executable
# ------------------------------------------------------------
set(MYBOT_SRC
    main/main.c
    main/app.c
    components/ringbuf/ringbuf.c
    components/http_client/http_client.c
    components/cJSON/cJSON.c
    main/protocols/device_api.c
    main/audio/audio_device.c
    main/board/linux/audio_capture_alsa.c
    main/board/linux/audio_playback_alsa.c
    main/protocols/rtc_session.c
    main/protocols/device_state.c
)

add_executable(mybot ${MYBOT_SRC})

target_include_directories(mybot PRIVATE
    ${CMAKE_SOURCE_DIR}/main
    ${CMAKE_SOURCE_DIR}/main/audio
    ${CMAKE_SOURCE_DIR}/main/protocols
    ${CMAKE_SOURCE_DIR}/components/ringbuf
    ${CMAKE_SOURCE_DIR}/components/http_client
    ${CMAKE_SOURCE_DIR}/components/cJSON
    ${AGORA_SDK_DIR}/include
    ${AOSL_DIR}/include
    ${AOSL_DIR}/platform/include
)

target_link_libraries(mybot PRIVATE
    aosl
    agora-rtc-sdk
    asound
    pthread
    dl
    rt
    m
)
