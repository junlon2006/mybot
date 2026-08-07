add_library(mybot_platform_linux STATIC
    linux_platform.c
    linux_audio_capture_alsa.c
    linux_audio_playback_alsa.c
    linux_key_stdin.c
    linux_kv_store_file.c
    linux_lcd_console.c
    linux_wifi_host_network.c
)
option(MYBOT_LINUX_HTTPS_OPENSSL "Use OpenSSL for the Linux HTTPS transport" ${MYBOT_ENABLE_HTTPS})
if(MYBOT_LINUX_HTTPS_OPENSSL AND NOT MYBOT_ENABLE_HTTPS)
    message(FATAL_ERROR "MYBOT_LINUX_HTTPS_OPENSSL requires MYBOT_ENABLE_HTTPS=ON")
endif()
if(MYBOT_LINUX_HTTPS_OPENSSL)
    find_package(OpenSSL REQUIRED)
    target_sources(mybot_platform_linux PRIVATE linux_https_openssl.c)
endif()
add_library(mybot::platform_linux ALIAS mybot_platform_linux)

target_include_directories(mybot_platform_linux PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mybot_platform_linux
    PUBLIC mybot::sdk
    PRIVATE aosl asound pthread dl rt m
)
target_compile_definitions(mybot_platform_linux PRIVATE
    MYBOT_LINUX_HTTPS_OPENSSL=$<BOOL:${MYBOT_LINUX_HTTPS_OPENSSL}>
)
if(MYBOT_LINUX_HTTPS_OPENSSL)
    target_link_libraries(mybot_platform_linux PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
mybot_enable_sanitizers(mybot_platform_linux)
