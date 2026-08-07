add_executable(mybot main.c)
add_executable(mybot::linux_example ALIAS mybot)
target_link_libraries(mybot PRIVATE mybot::platform_linux aosl agora-rtc-sdk pthread dl rt m)
mybot_enable_sanitizers(mybot)
