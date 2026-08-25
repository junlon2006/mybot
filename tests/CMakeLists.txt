set(MYBOT_TEST_COMMON_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/internal
    ${CMAKE_CURRENT_SOURCE_DIR}/support
)
set(MYBOT_TEST_SYSTEM_LIBS pthread dl rt m)

set(MYBOT_TEST_FEATURE_DEFINITIONS
    MYBOT_AUDIO_PTIME_MS=${MYBOT_AUDIO_PTIME_MS}
    MYBOT_CLOUD_AEC=$<BOOL:${MYBOT_CLOUD_AEC}>
    MYBOT_WAKE_WORDS=$<BOOL:${MYBOT_WAKE_WORDS}>
    MYBOT_AI_QOS=$<BOOL:${MYBOT_AI_QOS}>
    MYBOT_FAST_SEND_MULTIPLIER=${MYBOT_FAST_SEND_MULTIPLIER}
    MYBOT_ENABLE_HTTPS=$<BOOL:${MYBOT_ENABLE_HTTPS}>
    MYBOT_ALLOW_INSECURE_HTTP=$<BOOL:${MYBOT_ALLOW_INSECURE_HTTP}>
)

function(mybot_add_test target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE ${MYBOT_TEST_COMMON_INCLUDES})
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -UNDEBUG)
    endif()
    mybot_enable_sanitizers(${target})
    add_test(NAME ${target} COMMAND ${target})
endfunction()

mybot_add_test(mybot_device_lifecycle_test
    unit/device_lifecycle_test.c
    ../src/core/mybot_device_lifecycle.c
    ../src/platform/mybot_kv_store.c
)
target_link_libraries(mybot_device_lifecycle_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})
target_sources(mybot_device_lifecycle_test PRIVATE ../src/platform/mybot_platform.c)

mybot_add_test(mybot_device_client_test
    unit/device_client_test.c
    ../src/service/mybot_device_client.c
    ../src/support/mybot_json.c
)
target_link_libraries(mybot_device_client_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})

mybot_add_test(mybot_http_client_test
    unit/http_client_test.c
    ../src/platform/mybot_https.c
)
target_link_libraries(mybot_http_client_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})
target_sources(mybot_http_client_test PRIVATE ../src/platform/mybot_platform.c)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(mybot_http_client_test PRIVATE
        -Wl,--wrap=aosl_hal_malloc,--wrap=aosl_hal_realloc,--wrap=aosl_hal_free)
endif()

mybot_add_test(mybot_json_test unit/json_test.c ../src/support/mybot_json.c)
target_link_libraries(mybot_json_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})

mybot_add_test(mybot_key_service_test unit/key_service_test.c ../src/platform/mybot_key.c)
target_sources(mybot_key_service_test PRIVATE ../src/platform/mybot_platform.c)

mybot_add_test(mybot_lcd_test unit/lcd_test.c ../src/platform/mybot_lcd.c)
target_link_libraries(mybot_lcd_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})
target_sources(mybot_lcd_test PRIVATE ../src/platform/mybot_platform.c)

mybot_add_test(mybot_presenter_test
    unit/presenter_test.c
    ../src/core/mybot_presenter.c
    ../src/core/mybot_state_model.c
    ../src/platform/mybot_lcd.c
)
target_link_libraries(mybot_presenter_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})
target_sources(mybot_presenter_test PRIVATE ../src/platform/mybot_platform.c)

mybot_add_test(mybot_state_model_test
    unit/state_model_test.c
    ../src/core/mybot_state_model.c
)
target_link_libraries(mybot_state_model_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})

mybot_add_test(mybot_platform_registry_test
    unit/platform_registry_test.c
    ../src/platform/mybot_platform.c
)

mybot_add_test(mybot_wifi_test
    unit/wifi_provisioning_test.c
    ../src/platform/mybot_wifi.c
)
target_link_libraries(mybot_wifi_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})
target_sources(mybot_wifi_test PRIVATE ../src/platform/mybot_platform.c)

mybot_add_test(mybot_wake_words_test unit/wake_words_test.c ../src/media/mybot_wake_words.c)
target_sources(mybot_wake_words_test PRIVATE ../src/platform/mybot_platform.c)

mybot_add_test(mybot_audio_test unit/audio_test.c ../src/media/mybot_audio.c)
target_link_libraries(mybot_audio_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})
target_sources(mybot_audio_test PRIVATE ../src/platform/mybot_platform.c)

mybot_add_test(mybot_ringbuf_test unit/ringbuf_test.c ../src/support/mybot_ringbuf.c)
target_link_libraries(mybot_ringbuf_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})

mybot_add_test(mybot_announce_test unit/announce_test.c ../src/platform/mybot_announce.c)
target_link_libraries(mybot_announce_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})
target_sources(mybot_announce_test PRIVATE ../src/platform/mybot_platform.c)

mybot_add_test(mybot_agora_rtc_test unit/agora_rtc_test.c ../src/rtc/mybot_agora_rtc.c)
target_include_directories(mybot_agora_rtc_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/agora_rtsa_sdk/agora_sdk/include)
target_compile_definitions(mybot_agora_rtc_test PRIVATE ${MYBOT_TEST_FEATURE_DEFINITIONS})
target_link_libraries(mybot_agora_rtc_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})

mybot_add_test(mybot_app_test
    unit/app_test.c
    ../src/core/mybot_app.c
    ../src/core/mybot_state_model.c
    ../src/core/mybot_presenter.c
    ../src/media/mybot_media_pipeline.c
    ../src/support/mybot_ringbuf.c
)
target_compile_definitions(mybot_app_test PRIVATE ${MYBOT_TEST_FEATURE_DEFINITIONS})
target_link_libraries(mybot_app_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})

mybot_add_test(mybot_public_headers_test integration/public_headers_test.c)
target_link_libraries(mybot_public_headers_test PRIVATE mybot::sdk)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mybot_public_headers_test PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME mybot_cmake_host_integration
    COMMAND ${CMAKE_COMMAND}
        -DMYBOT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/..
        -DMYBOT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/cmake_host
        -P ${CMAKE_CURRENT_SOURCE_DIR}/integration/run_cmake_host.cmake
)

add_test(NAME mybot_install_consumer_check
    COMMAND ${CMAKE_COMMAND}
        -DMYBOT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/..
        -DMYBOT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/..
        -P ${CMAKE_CURRENT_SOURCE_DIR}/integration/run_cmake_install.cmake
)

if(MYBOT_BUILD_LINUX_PLATFORM)
    mybot_add_test(mybot_kv_store_file_test
        platform/linux/kv_store_file_test.c
        ../src/platform/mybot_kv_store.c
        ../platforms/linux/linux_kv_store_file.c
    )
    target_include_directories(mybot_kv_store_file_test PRIVATE ../platforms/linux)
    target_link_libraries(mybot_kv_store_file_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})
    target_sources(mybot_kv_store_file_test PRIVATE ../src/platform/mybot_platform.c)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(mybot_kv_store_file_test PRIVATE -Wl,--wrap=fsync)
    endif()

    mybot_add_test(mybot_key_stdin_test
        platform/linux/key_stdin_test.c
        ../src/platform/mybot_key.c
        ../platforms/linux/linux_key_stdin.c
    )
    target_include_directories(mybot_key_stdin_test PRIVATE ../platforms/linux)
    target_link_libraries(mybot_key_stdin_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})
    target_sources(mybot_key_stdin_test PRIVATE ../src/platform/mybot_platform.c)

    mybot_add_test(mybot_lcd_console_test
        platform/linux/lcd_console_test.c
        ../src/platform/mybot_lcd.c
        ../platforms/linux/linux_lcd_console.c
    )
    target_include_directories(mybot_lcd_console_test PRIVATE ../platforms/linux)
    target_link_libraries(mybot_lcd_console_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})
    target_sources(mybot_lcd_console_test PRIVATE ../src/platform/mybot_platform.c)

    mybot_add_test(mybot_announce_file_test
        platform/linux/announce_file_test.c
        ../src/platform/mybot_announce.c
        ../platforms/linux/linux_announce_file.c
    )
    target_include_directories(mybot_announce_file_test PRIVATE ../platforms/linux)
    target_link_libraries(mybot_announce_file_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})
    target_sources(mybot_announce_file_test PRIVATE ../src/platform/mybot_platform.c)

    mybot_add_test(mybot_linux_platform_descriptor_test
        platform/linux/platform_descriptor_test.c
    )
    target_include_directories(mybot_linux_platform_descriptor_test PRIVATE ../platforms/linux)
    target_link_libraries(mybot_linux_platform_descriptor_test PRIVATE mybot::platform_linux)
    target_compile_definitions(mybot_linux_platform_descriptor_test PRIVATE
        MYBOT_LINUX_HTTPS_OPENSSL=$<BOOL:${MYBOT_LINUX_HTTPS_OPENSSL}>)
endif()
