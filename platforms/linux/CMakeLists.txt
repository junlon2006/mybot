add_library(mybot_platform_linux STATIC
    linux_platform.c
    linux_audio_capture_alsa.c
    linux_audio_playback_alsa.c
    linux_key_stdin.c
    linux_kv_store_file.c
    linux_lcd_console.c
    linux_wifi_host_network.c
)
add_library(mybot::platform_linux ALIAS mybot_platform_linux)

target_include_directories(mybot_platform_linux PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mybot_platform_linux
    PUBLIC mybot::sdk
    PRIVATE aosl asound pthread dl rt m
)
mybot_enable_sanitizers(mybot_platform_linux)
