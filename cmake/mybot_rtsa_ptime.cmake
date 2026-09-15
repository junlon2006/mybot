# SPDX-License-Identifier: Apache-2.0

function(mybot_check_rtsa_ptime sdk_dir expected_ptime_ms)
    if(NOT sdk_dir OR NOT expected_ptime_ms)
        message(FATAL_ERROR
            "An Agora RTSA SDK directory and MYBOT_AUDIO_PTIME_MS are required for ptime validation")
    endif()

    set(actual_ptime_ms "")
    foreach(config_file
            "${sdk_dir}/.config"
            "${sdk_dir}/include/global_config.cmake")
        if(EXISTS "${config_file}")
            file(STRINGS "${config_file}" ptime_lines
                 REGEX "^[ \t]*(set\\([ \t]*)?CONFIG_MINIMAL_TIMER_INTERVAL_MS([ \t]*=|[ \t]+)")
            foreach(ptime_line IN LISTS ptime_lines)
                if(ptime_line MATCHES
                   "^[ \t]*(set\\([ \t]*)?CONFIG_MINIMAL_TIMER_INTERVAL_MS[ \t]*(=|[ \t]+)[^0-9]*([0-9]+)")
                    set(actual_ptime_ms "${CMAKE_MATCH_3}")
                    break()
                endif()
            endforeach()
        endif()
        if(actual_ptime_ms)
            break()
        endif()
    endforeach()

    if(NOT actual_ptime_ms)
        message(FATAL_ERROR
            "Agora RTSA SDK at ${sdk_dir} does not expose "
            "CONFIG_MINIMAL_TIMER_INTERVAL_MS metadata; select a packaged SDK with "
            "matching build metadata")
    endif()
    if(NOT "${actual_ptime_ms}" STREQUAL "${expected_ptime_ms}")
        message(FATAL_ERROR
            "MYBOT_AUDIO_PTIME_MS=${expected_ptime_ms} does not match the Agora RTSA "
            "package timer cadence CONFIG_MINIMAL_TIMER_INTERVAL_MS=${actual_ptime_ms}. "
            "Select a matching RTSA SDK package.")
    endif()
endfunction()

function(mybot_check_rtsa_video sdk_dir enabled)
    if(NOT enabled)
        return()
    endif()
    if(NOT sdk_dir OR NOT EXISTS "${sdk_dir}/include/agora_rtc_api.h")
        message(FATAL_ERROR
            "MYBOT_ENABLE_VIDEO=ON requires an Agora RTSA package with agora_rtc_api.h")
    endif()

    file(STRINGS "${sdk_dir}/include/agora_rtc_api.h" video_api_lines
         REGEX "agora_rtc_send_video_data")
    if(NOT video_api_lines)
        message(FATAL_ERROR
            "MYBOT_ENABLE_VIDEO=ON requires an RTSA header exposing agora_rtc_send_video_data")
    endif()

    set(audio_only FALSE)
    foreach(config_file
            "${sdk_dir}/.config"
            "${sdk_dir}/include/global_config.cmake")
        if(EXISTS "${config_file}")
            file(STRINGS "${config_file}" audio_only_lines REGEX "CONFIG_AUDIO_ONLY")
            foreach(audio_only_line IN LISTS audio_only_lines)
                if(audio_only_line MATCHES
                   "CONFIG_AUDIO_ONLY[ \t]*(=|[ \t]+|\")[ \t\"]*(1|y|Y|on|ON|true|TRUE)")
                    set(audio_only TRUE)
                endif()
            endforeach()
        endif()
    endforeach()

    if(audio_only)
        message(FATAL_ERROR
            "MYBOT_ENABLE_VIDEO=ON cannot use an Agora RTSA package built with CONFIG_AUDIO_ONLY")
    endif()
endfunction()
