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
    main/platform/linux/linux_main.c
    main/mybot_app.c
    main/wifi/mybot_wifi_provisioning.c
    main/platform/linux/linux_wifi_host_network.c
    main/storage/mybot_kv_store.c
    main/platform/linux/linux_kv_store_file.c
    main/key_service/mybot_key_service.c
    main/lcd/mybot_lcd.c
    main/wake_words/mybot_wake_words.c
    main/platform/linux/linux_lcd_console.c
    main/platform/linux/linux_key_stdin.c
    components/ringbuf/mybot_ringbuf.c
    components/http_client/mybot_http_client.c
    components/json/mybot_json.c
    main/device/mybot_device_client.c
    main/audio/mybot_audio_device.c
    main/platform/linux/linux_audio_capture_alsa.c
    main/platform/linux/linux_audio_playback_alsa.c
    main/rtc/mybot_rtc_session.c
    main/device/mybot_device_lifecycle.c
)

add_executable(mybot ${MYBOT_SRC})

target_include_directories(mybot PRIVATE
    ${CMAKE_SOURCE_DIR}/main
    ${CMAKE_SOURCE_DIR}/main/audio
    ${CMAKE_SOURCE_DIR}/main/device
    ${CMAKE_SOURCE_DIR}/main/key_service
    ${CMAKE_SOURCE_DIR}/main/lcd
    ${CMAKE_SOURCE_DIR}/main/rtc
    ${CMAKE_SOURCE_DIR}/main/wake_words
    ${CMAKE_SOURCE_DIR}/main/wifi
    ${CMAKE_SOURCE_DIR}/components/ringbuf
    ${CMAKE_SOURCE_DIR}/components/http_client
    ${CMAKE_SOURCE_DIR}/components/json
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
include(CTest)
if(BUILD_TESTING)
    add_executable(mybot_device_lifecycle_test
        tests/device_lifecycle_test.c
        main/device/mybot_device_lifecycle.c
        main/storage/mybot_kv_store.c
    )
    target_include_directories(mybot_device_lifecycle_test PRIVATE
        ${CMAKE_SOURCE_DIR}/main
        ${AOSL_DIR}/include
        ${AOSL_DIR}/platform/include
    )
    target_link_libraries(mybot_device_lifecycle_test PRIVATE
        aosl
        pthread
        dl
        rt
        m
    )
    add_test(NAME mybot_device_lifecycle_test COMMAND mybot_device_lifecycle_test)

    add_executable(mybot_device_client_test
        tests/device_client_test.c
        main/device/mybot_device_client.c
        components/json/mybot_json.c
    )
    target_include_directories(mybot_device_client_test PRIVATE
        ${CMAKE_SOURCE_DIR}/main
        ${CMAKE_SOURCE_DIR}/components/http_client
        ${CMAKE_SOURCE_DIR}/components/json
        ${AOSL_DIR}/include
        ${AOSL_DIR}/platform/include
    )
    target_link_libraries(mybot_device_client_test PRIVATE
        aosl
        pthread
        dl
        rt
        m
    )
    add_test(NAME mybot_device_client_test COMMAND mybot_device_client_test)

    add_executable(mybot_http_client_test tests/http_client_test.c)
    target_include_directories(mybot_http_client_test PRIVATE
        ${AOSL_DIR}/include
        ${AOSL_DIR}/platform/include
    )
    target_link_libraries(mybot_http_client_test PRIVATE
        aosl
        pthread
        dl
        rt
        m
    )
    add_test(NAME mybot_http_client_test COMMAND mybot_http_client_test)

    add_executable(mybot_json_test
        tests/json_test.c
        components/json/mybot_json.c
    )
    target_include_directories(mybot_json_test PRIVATE ${CMAKE_SOURCE_DIR}/components/json)
    target_link_libraries(mybot_json_test PRIVATE m)
    add_test(NAME mybot_json_test COMMAND mybot_json_test)

    add_executable(mybot_key_service_test
        tests/key_service_test.c
        main/key_service/mybot_key_service.c
    )
    target_include_directories(mybot_key_service_test PRIVATE ${CMAKE_SOURCE_DIR}/main)
    add_test(NAME mybot_key_service_test COMMAND mybot_key_service_test)

    add_executable(mybot_key_stdin_test
        tests/key_stdin_test.c
        main/key_service/mybot_key_service.c
        main/platform/linux/linux_key_stdin.c
    )
    target_include_directories(mybot_key_stdin_test PRIVATE
        ${CMAKE_SOURCE_DIR}/main
        ${AOSL_DIR}/include
        ${AOSL_DIR}/platform/include
    )
    target_link_libraries(mybot_key_stdin_test PRIVATE
        aosl
        pthread
        dl
        rt
        m
    )
    add_test(NAME mybot_key_stdin_test COMMAND mybot_key_stdin_test)

    add_executable(mybot_lcd_test
        tests/lcd_test.c
        main/lcd/mybot_lcd.c
    )
    target_include_directories(mybot_lcd_test PRIVATE
        ${CMAKE_SOURCE_DIR}/main
        ${AOSL_DIR}/include
        ${AOSL_DIR}/platform/include
    )
    target_link_libraries(mybot_lcd_test PRIVATE
        aosl
        pthread
        dl
        rt
        m
    )
    add_test(NAME mybot_lcd_test COMMAND mybot_lcd_test)

    add_executable(mybot_lcd_console_test
        tests/lcd_console_test.c
        main/lcd/mybot_lcd.c
        main/platform/linux/linux_lcd_console.c
    )
    target_include_directories(mybot_lcd_console_test PRIVATE
        ${CMAKE_SOURCE_DIR}/main
        ${AOSL_DIR}/include
        ${AOSL_DIR}/platform/include
    )
    target_link_libraries(mybot_lcd_console_test PRIVATE
        aosl
        pthread
        dl
        rt
        m
    )
    add_test(NAME mybot_lcd_console_test COMMAND mybot_lcd_console_test)

    add_executable(mybot_wifi_provisioning_test
        tests/wifi_provisioning_test.c
        main/wifi/mybot_wifi_provisioning.c
    )
    target_include_directories(mybot_wifi_provisioning_test PRIVATE
        ${CMAKE_SOURCE_DIR}/main
        ${AOSL_DIR}/include
        ${AOSL_DIR}/platform/include
    )
    target_link_libraries(mybot_wifi_provisioning_test PRIVATE
        aosl
        pthread
        dl
        rt
        m
    )
    add_test(NAME mybot_wifi_provisioning_test COMMAND mybot_wifi_provisioning_test)

    add_executable(mybot_wake_words_test
        tests/wake_words_test.c
        main/wake_words/mybot_wake_words.c
    )
    target_include_directories(mybot_wake_words_test PRIVATE ${CMAKE_SOURCE_DIR}/main)
    add_test(NAME mybot_wake_words_test COMMAND mybot_wake_words_test)

    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        foreach(test_target IN ITEMS
            mybot_device_lifecycle_test
            mybot_device_client_test
            mybot_http_client_test
            mybot_json_test
            mybot_key_service_test
            mybot_key_stdin_test
            mybot_lcd_test
            mybot_lcd_console_test
            mybot_wifi_provisioning_test
            mybot_wake_words_test
        )
            target_compile_options(${test_target} PRIVATE -UNDEBUG)
        endforeach()
    endif()
endif()
