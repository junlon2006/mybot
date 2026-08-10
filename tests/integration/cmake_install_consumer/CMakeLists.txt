cmake_minimum_required(VERSION 3.16)
project(mybot_install_consumer_check LANGUAGES C)

# Consume the installed package through find_package. The Agora RTSA package is
# supplied by the caller via MYBOT_AGORA_SDK_DIR / MYBOT_AGORA_RTC_LIBRARY.
find_package(mybot 1.0 CONFIG REQUIRED)

add_executable(mybot_install_consumer_check main.c)
target_link_libraries(mybot_install_consumer_check PRIVATE mybot::sdk)
