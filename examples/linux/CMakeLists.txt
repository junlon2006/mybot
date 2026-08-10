add_executable(mybot main.c)
add_executable(mybot::linux_example ALIAS mybot)
target_link_libraries(mybot PRIVATE mybot::platform_linux aosl agora-rtc-sdk pthread dl rt m)
# Build-tree assets path, used by main.c as the MYBOT_ASSETS_DIR fallback.
target_compile_definitions(mybot PRIVATE MYBOT_EXAMPLE_ASSETS_DIR="${CMAKE_BINARY_DIR}/assets")
mybot_enable_sanitizers(mybot)
