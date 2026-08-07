set(MYBOT_TEST_COMMON_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/internal
)
set(MYBOT_TEST_SYSTEM_LIBS pthread dl rt m)

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

mybot_add_test(mybot_device_client_test
    unit/device_client_test.c
    ../src/service/mybot_device_client.c
    ../src/support/mybot_json.c
)
target_link_libraries(mybot_device_client_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})

mybot_add_test(mybot_http_client_test unit/http_client_test.c)
target_link_libraries(mybot_http_client_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})

mybot_add_test(mybot_json_test unit/json_test.c ../src/support/mybot_json.c)
target_link_libraries(mybot_json_test PRIVATE m)

mybot_add_test(mybot_key_service_test unit/key_service_test.c ../src/platform/mybot_key.c)

mybot_add_test(mybot_lcd_test unit/lcd_test.c ../src/platform/mybot_lcd.c)
target_link_libraries(mybot_lcd_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})

mybot_add_test(mybot_wifi_provisioning_test
    unit/wifi_provisioning_test.c
    ../src/platform/mybot_wifi.c
)
target_link_libraries(mybot_wifi_provisioning_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})

mybot_add_test(mybot_wake_words_test unit/wake_words_test.c ../src/media/mybot_wake_words.c)

mybot_add_test(mybot_audio_test unit/audio_test.c ../src/media/mybot_audio.c)

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

if(MYBOT_BUILD_LINUX_PLATFORM)
    mybot_add_test(mybot_key_stdin_test
        platform/linux/key_stdin_test.c
        ../src/platform/mybot_key.c
        ../platforms/linux/linux_key_stdin.c
    )
    target_include_directories(mybot_key_stdin_test PRIVATE ../platforms/linux)
    target_link_libraries(mybot_key_stdin_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})

    mybot_add_test(mybot_lcd_console_test
        platform/linux/lcd_console_test.c
        ../src/platform/mybot_lcd.c
        ../platforms/linux/linux_lcd_console.c
    )
    target_include_directories(mybot_lcd_console_test PRIVATE ../platforms/linux)
    target_link_libraries(mybot_lcd_console_test PRIVATE aosl ${MYBOT_TEST_SYSTEM_LIBS})
endif()
