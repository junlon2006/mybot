cmake_minimum_required(VERSION 2.4...3.5)
project(hello_rtm)

include_directories(
    ${UTILITY})

aux_source_directory(${UTILITY} COMMON_FILES)
add_executable(hello_rtm hello_rtm.c ${COMMON_FILES})
target_link_libraries(hello_rtm agora-rtc-sdk ${LIBS})

add_executable(hello_rtm_file hello_rtm_file.c ${COMMON_FILES})
target_link_libraries(hello_rtm_file agora-rtc-sdk ${LIBS})
