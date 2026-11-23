add_library(mybot_platform_linux STATIC
    linux_platform.c
    linux_alsa_pcm.c
    linux_announce_file.c
    linux_audio_capture_alsa.c
    linux_audio_playback_alsa.c
    linux_audio_volume_alsa_mixer.c
    linux_key_stdin.c
    linux_kv_store_file.c
    linux_lcd_console.c
    linux_wifi_host_network.c
)
if(MYBOT_ENABLE_HTTPS)
    find_package(OpenSSL REQUIRED)
    target_sources(mybot_platform_linux PRIVATE linux_https_openssl.c)
endif()
add_library(mybot::platform_linux ALIAS mybot_platform_linux)

target_include_directories(mybot_platform_linux PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mybot_platform_linux
    PUBLIC mybot::sdk
    PRIVATE aosl asound pthread dl rt m
)
if(MYBOT_ENABLE_HTTPS)
    target_link_libraries(mybot_platform_linux PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
mybot_enable_sanitizers(mybot_platform_linux)

# Copy the pairing-code announcement PCM assets into the build tree so the
# Linux example can use them directly, wherever it is invoked from.
add_custom_command(TARGET mybot_platform_linux POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/locales"
            "${CMAKE_BINARY_DIR}/assets/locales"
    COMMENT "Copy pairing-code announcement PCM assets into the build tree")
